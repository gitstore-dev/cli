"""Candidate distribution acceptance, with actual native services and public APIs."""
import json
import fcntl
import os
from pathlib import Path
import subprocess
import sqlite3
import time
import unittest
import urllib.error
import urllib.request
from support import Fixture
import test_assets as assets_tests
import test_core as core_tests


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *arguments, **keywords):
        return None


class CleanInstall(unittest.TestCase):
    def select(self, *, offline="verified", unsupported=False):
        self.fixture = Fixture(offline=offline, unsupported=unsupported)
        self.fixture.ready_timeout = 600
        self.addCleanup(self.fixture.close)
        self.assertEqual({p.name for p in (self.fixture.root / "bin").iterdir()}, {"gs", "gitstored"})
        self.assertEqual(self.fixture.environment["PATH"], "/nonexistent")
        return self.fixture

    def test_cold_interactive_approval(self):
        self.select(offline=None)
        core_tests.Core.test_interactive_cold_start_requires_displayed_asset_approval(self)

    def test_automation_denial_and_verified_offline_start(self):
        self.select(offline=None)
        assets_tests.Assets.test_no_input_without_permission_downloads_nothing(self)
        self.select()
        assets_tests.Assets.test_offline_layouts_authorize_only_the_selected_verified_bytes(self)

    def test_warm_cache_reuse(self):
        self.select()
        assets_tests.Assets.test_warm_restart_reuses_assets_and_existing_private_material(self)

    def test_concurrent_isolation(self):
        self.select()
        core_tests.Core.test_deployments_have_separate_volumes_names_and_loopback_ports(self)

    def test_foreground_and_detached_lifetimes(self):
        self.select()
        core_tests.Core.test_foreground_interrupt_requests_owned_cleanup(self)
        self.select()
        core_tests.Core.test_detached_exit_and_same_key_reconnect_preserve_operation(self)

    def test_missing_runtime_and_optional_mapping(self):
        self.select(offline=None, unsupported=True)
        core_tests.Core.test_missing_execution_capability_fails_before_effects(self)
        self.select(offline=None)
        core_tests.Core.test_unavailable_optional_mapping_has_no_asset_side_effect(self)

    def test_public_graphql_git_and_retained_restart(self):
        f = self.select()
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_text())
        helper = settings["repositoryProbe"]
        protected = self.protected
        protected(f.root / ".gitstored-disposable-test", "gitstore-disposable-v1\n")
        started = time.monotonic()
        first = f.start("public", provision=False)
        deployment = f.ready("public", first)
        first_ready = time.monotonic() - started
        material_path = f.credential_path("public")
        material_hash = f.digest(material_path)
        material = json.loads(material_path.read_text())
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        repository_root = Path(__file__).resolve().parents[2]
        login_document = (repository_root / "runtime/profiles/bootstrap-login.graphql").read_text()
        token = None

        def login(current):
            body = json.dumps({"query": login_document, "variables": {
                "username": material["username"], "password": material["password"]}}).encode()
            request = urllib.request.Request(current["endpoints"]["api"] + "/graphql", body,
                                             {"Content-Type": "application/json"})
            try:
                with opener.open(request, timeout=10) as response:
                    raw = response.read(1048577)
                self.assertLessEqual(len(raw), 1048576)
                value = json.loads(raw)
                self.assertFalse(value.get("errors"), "Public login rejected fixture credentials")
                result = value["data"]["login"]["token"]["accessToken"]
                self.assertIsInstance(result, str)
                return result
            except (OSError, ValueError, KeyError):
                raise AssertionError("Public fixture login failed") from None

        def configure(current):
            nonlocal token
            token = login(current)
            protected(f.root / "public-token", token, replace=True)
            protected(f.root / "public-git.json", json.dumps({
                "username": material["username"], "password": material["password"]}), replace=True)
            protected(f.root / "public.toml", f'''version = 1
current_context = "candidate"
[contexts.candidate]
endpoint = "{current['endpoints']['api']}/graphql"
git_endpoint = "{current['endpoints']['git']}"
allow_loopback_http = true
contract_profile = "m0-2026-10-27"
account = "admin"
namespace = "candidate-native"
[contexts.candidate.accounts.admin]
issuer = "candidate"
[contexts.candidate.git_auth]
mechanism = "basic"
account = "admin"
issuer = "candidate"
credential_file = {json.dumps(str(f.root / 'public-git.json'))}
''', replace=True)

        def cli(*arguments, cwd=None, expected_code=0):
            result = subprocess.run([str(f.gs), "--config", str(f.root / "public.toml"),
                                     "--credential-file", str(f.root / "public-token"),
                                     "--non-interactive", "-o", "json", *arguments],
                                    cwd=cwd or f.root, env=f.environment,
                                    capture_output=True, timeout=40)
            self.assertEqual(result.returncode, expected_code, "Candidate public CLI command failed")
            self.assertNotIn(token.encode(), result.stderr)
            return json.loads(result.stdout) if result.stdout else None

        def mutation(name, document, resource):
            path = f.root / f"{name}.json"
            protected(path, json.dumps({"query": document, "variables": {"input": resource}}))
            value = cli("api", "graphql", "--input", str(path))
            self.assertFalse(value.get("errors"), "Public authoring mutation was rejected")

        def git_available(current, name):
            deadline = time.monotonic() + 90
            while True:
                request = urllib.request.Request(
                    f"{current['endpoints']['git']}/candidate-native/{name}.git/info/refs?service=git-upload-pack",
                    headers={"Authorization": "Bearer " + token})
                try:
                    with opener.open(request, timeout=5) as response:
                        if response.status == 200:
                            self.assertIn("git-upload-pack", response.headers.get("Content-Type", ""))
                            return
                except urllib.error.HTTPError as error:
                    self.assertIn(error.code, (404, 409, 503), "Public Git readiness failed")
                except OSError:
                    pass
                self.assertLess(time.monotonic(), deadline, "Public Git repository did not become available")
                time.sleep(0.2)

        def revision(mode, path):
            result = subprocess.run([helper, mode, str(path)], env=f.environment,
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, "Owned Git library fixture failed")
            value = result.stdout.decode().strip()
            self.assertRegex(value, r"\A[0-9a-f]{40}\Z")
            return value

        configure(deployment)
        self.assertEqual(cli("api", "graphql", "-f", "query={__typename}")["data"]["__typename"], "Query")
        mutation("namespace", "mutation($input:CreateNamespaceInput!){createNamespace(input:$input){namespace{id}}}", {
            "apiVersion": "gitstore.dev/v1beta1", "kind": "Namespace",
            "metadata": {"name": "candidate-native"}, "spec": {"title": "Candidate fixture", "tier": "ORGANIZATION"}})
        git_available(deployment, "gitstore-system")
        mutation("repository", "mutation($input:CreateRepositoryInput!){createRepository(input:$input){repository{id}}}", {
            "apiVersion": "gitstore.dev/v1beta1", "kind": "Repository",
            "metadata": {"namespace": "candidate-native", "name": "catalog"},
            "spec": {"defaultBranch": "main", "visibility": "PRIVATE", "storageClass": "standard"}})
        git_available(deployment, "catalog")
        clone = f.root / "first-clone"
        cli("repository", "clone", "catalog", str(clone))
        revision("seed", clone)
        revision("seed-remote", clone)
        tracked = f.root / "tracked-clone"
        cli("repository", "clone", "catalog", str(tracked))
        expected = revision("advance", tracked)
        cli("repository", "sync", "catalog", cwd=tracked)
        self.assertEqual(revision("head", tracked), expected)
        copy = f.root / "second-clone"
        cli("repository", "clone", "catalog", str(copy))
        self.assertEqual(revision("head", copy), expected)
        self.assertEqual((copy / "candidate-marker.txt").read_bytes(), b"candidate advanced fixture\n")
        f.stop("public")
        second = f.start("public", provision=False, key="public-restart")
        restarted = f.ready("public", second)
        self.assertGreater(restarted["generation"], deployment["generation"])
        self.assertEqual(f.digest(material_path), material_hash)
        configure(restarted)
        # memdb identities/checkpoints intentionally reset. Retained Git objects
        # do not manufacture an admitted catalogue or silently restore identity.
        reset = cli("api", "graphql", "-f", 'query={namespace(by:{name:"candidate-native"}){id}}', expected_code=4)
        self.assertIsNone((reset.get("data") or {}).get("namespace"))
        self.assertIn("NOT_FOUND", {error.get("extensions", {}).get("code") for error in reset["errors"]})
        status = (Path("/proc") / str(f.process.pid) / "status").read_text().splitlines()
        memory = {line.split(":")[0]: line.split(":")[1].strip() for line in status
                  if line.startswith(("VmRSS:", "VmHWM:"))}
        costs = {"coldOfflineReadySeconds": round(first_ready, 3),
                 "managerMemoryOnly": memory,
                 "stateLogicalBytes": sum(p.stat().st_size for p in (f.root / "state").rglob("*")
                                   if p.is_file() and not p.is_symlink()),
                 "assetLogicalBytes": sum(p.stat().st_size for p in (f.root / "state/assets").rglob("*")
                                   if p.is_file() and not p.is_symlink())}
        f.stop("public", key="public-final-stop")
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
            volume, device, inode = database.execute(
                "SELECT volume_id,data_device,data_inode FROM repository_volumes WHERE allocated=1").fetchone()
        self.assertRegex(volume, r"\Arepo-[0-9a-f]{56}\Z")
        data = f.root / f"state/volumes/volume-{volume}/data"
        self.assertEqual((data.stat().st_dev, data.stat().st_ino), (int(device), int(inode)))
        lease = os.open(f.root / f"state/volumes/writer-{volume}.lock", os.O_RDWR | os.O_NOFOLLOW)
        try:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            retained = subprocess.run([helper, "retained", str(f.root), expected],
                                      env=f.environment, capture_output=True, timeout=15)
            self.assertEqual(retained.returncode, 0, "Owned stopped volume lost the known Git commit")
            self.assertEqual(retained.stdout.decode().strip(), expected)
        finally:
            os.close(lease)
        costs["retentionEvidence"] = "Known commit/tree/blob verified with libgit2 under stopped-volume lease"
        costs["memdbBoundary"] = "Catalogue identity reset; no automatic public resource hydration"
        protected(f.root / "candidate-runtime-costs.json", json.dumps(costs, indent=2) + "\n")

    @staticmethod
    def protected(path, value, replace=False):
        flags = os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | (os.O_TRUNC if replace else os.O_EXCL)
        fd = os.open(path, flags, 0o600)
        with os.fdopen(fd, "w") as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())


if __name__ == "__main__":
    unittest.main()
