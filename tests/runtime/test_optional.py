"""Public optional graphs against explicitly selected native disposable inputs."""
import json
import os
from pathlib import Path
import selectors
import signal
import sqlite3
import subprocess
import time
import unittest
from urllib.request import urlopen
from support import Fixture, DropStartAck


class Optional(unittest.TestCase):
    def test_unavailable_components_fail_before_runtime_effects(self):
        f = Fixture()
        self.addCleanup(f.close)
        before = f.call("deployment", "list")
        for component, prerequisite in (("admin", b"Admin UI"), ("storefront", b"publication reads")):
            for prefix in ((component, "serve"), ("serve", "--add", component)):
                with self.subTest(component=component, prefix=prefix):
                    result = f.call("--non-interactive", *prefix, "--deployment", "optional",
                                    "--allow-provision", "--detach", success=False)
                    self.assertEqual(result.returncode, 6)
                    self.assertIn(prerequisite, result.stderr)
                    self.assertIn(b"UNSUPPORTED", result.stderr)
                    self.assertEqual(f.assets(), {})
                    self.assertEqual(list((f.root / "credentials").iterdir()), [])
                    self.assertEqual(f.call("deployment", "list"), before)

    def test_duplicate_selection_is_usage_error_before_capability_failure(self):
        f = Fixture()
        self.addCleanup(f.close)
        for prefix in (("serve", "--add", "oidc", "--add", "oidc"),
                       ("oidc", "serve", "--add", "oidc")):
            result = f.call(*prefix, success=False)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(f.assets(), {})

    def test_oidc_requires_explicit_asset_consent(self):
        f = Fixture(oidc=True)
        self.addCleanup(f.close)
        before = f.call("deployment", "list")
        result = f.call("--non-interactive", "oidc", "serve", "--deployment", "no-consent",
                        "--detach", success=False)
        self.assertIn(b"PROVISIONING_REQUIRED", result.stderr)
        self.assertEqual(f.assets(), {})
        self.assertEqual(list((f.root / "oidc-credentials").iterdir()), [])
        self.assertEqual(f.call("deployment", "list"), before)

    @staticmethod
    def database(f):
        return sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True)

    def start(self, f, name, key, combined=False):
        f.deployments.add(name)
        prefix = ("serve", "--add", "oidc") if combined else ("oidc", "serve")
        return f.call("--non-interactive", *prefix, "--deployment", name, "--detach",
                      "--idempotency-key", key)


    def assert_endpoints(self, deployment, combined):
        names = {"oidcIssuer", "oidcBridge", "identity", "identityUi", "mail"}
        if combined:
            names |= {"api", "git"}
        self.assertEqual(set(deployment["endpoints"]), names)
        issuer = deployment["endpoints"]["oidcIssuer"]
        with urlopen(issuer + ".well-known/openid-configuration", timeout=5) as response:
            discovery = json.load(response)
        self.assertEqual(discovery["issuer"], issuer)
        self.assertEqual(discovery["jwks_uri"], issuer + ".well-known/jwks.json")
        # Dispatch only to the selected fixed issuer, never to a response URL.
        with urlopen(issuer + ".well-known/jwks.json", timeout=5) as response:
            keys = json.load(response)["keys"]
        self.assertTrue(keys)
        self.assertTrue(all(k.get("kty") == "RSA" and k.get("alg") == "RS256" for k in keys))

    def assert_logs(self, f, name, selector):
        process = subprocess.Popen(f.command("deployment", "logs", name, "--component", selector),
                                   env=f.environment, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            with selectors.DefaultSelector() as poll:
                poll.register(process.stdout, selectors.EVENT_READ)
                self.assertTrue(poll.select(15), "No provider log event")
            first = process.stdout.readline(1048577)
            record = json.loads(first)
            self.assertTrue(record["payload"]["component"].startswith("oidc-"))
            if selector != "oidc":
                self.assertEqual(record["payload"]["component"], selector)
            process.send_signal(signal.SIGINT)
            rest, diagnostics = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0)
            self.assertFalse(diagnostics)
            for path in (f.root / "oidc-credentials").glob("*.json"):
                material = json.loads(path.read_bytes())
                for value in material.values():
                    if isinstance(value, str) and len(value) > 16:
                        self.assertFalse(value.encode() in first + rest, "Provider secret in public logs")
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)

    def test_public_standalone_retained_restart_and_manager_loss(self):
        f = Fixture(offline="oidc", oidc=True)
        self.addCleanup(f.close)
        operation = self.start(f, "identity", "fresh")
        deployment = f.ready("identity", operation)
        self.assert_endpoints(deployment, False)
        self.assertEqual(f.active_components(), {"oidc-smtp", "oidc-hydra", "oidc-kratos", "oidc-ui", "oidc-bridge"})
        with self.database(f) as db:
            volumes = db.execute("SELECT * FROM oidc_provider_volumes").fetchall()
            self.assertEqual(len(volumes), 2)
            self.assertEqual(db.execute("SELECT count(*) FROM repository_volumes").fetchone()[0], 0)
        material = {p.name: f.digest(p) for p in (f.root / "oidc-credentials").glob("*.json")}
        self.assertTrue(material)
        assets = f.assets()
        self.assert_logs(f, "identity", "oidc")
        self.assert_logs(f, "identity", "oidc-bridge")
        f.stop("identity", key="first-stop")
        proxy = DropStartAck(f)
        self.addCleanup(proxy.close)
        subprocess.run(f.command("--timeout", "10s", "--non-interactive", "oidc", "serve",
                       "--deployment", "identity", "--detach", "--idempotency-key", "retained",
                       socket=proxy.path), env=f.environment, stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        self.assertTrue(proxy.finished.wait(16))
        self.assertIsNone(proxy.error)
        self.assertTrue(proxy.operation)
        replay = self.start(f, "identity", "retained")
        self.assertEqual(replay["operationId"], proxy.operation)
        current = f.ready("identity", replay)
        self.assertEqual(current["endpoints"], deployment["endpoints"])
        self.assertEqual(f.assets(), assets)
        self.assert_endpoints(current, False)
        self.assertEqual({p.name: f.digest(p) for p in (f.root / "oidc-credentials").glob("*.json")}, material)
        with self.database(f) as db:
            self.assertEqual(db.execute("SELECT * FROM oidc_provider_volumes").fetchall(), volumes)
            native = [json.loads(raw)["native"] for raw, in db.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' AND ownership_state='held' AND resource_id LIKE 'run-%'")]
        self.assertEqual(len(native), 5)
        terminal = f.call("deployment", "inspect", "identity", "--operation", replay["operationId"])["operation"]
        manager = os.pidfd_open(f.process.pid)
        try:
            signal.pidfd_send_signal(manager, signal.SIGKILL)
            f.process.wait(timeout=5)
        finally:
            os.close(manager)
        f.start_manager()
        recovered = f.call("deployment", "inspect", "identity")
        self.assertNotEqual(recovered["readiness"], "ready")
        self.assertFalse(recovered.get("endpoints"))
        result = f.call("--non-interactive", "oidc", "serve", "--deployment", "identity",
                        "--detach", "--idempotency-key", "unsafe-replacement", success=False)
        self.assertTrue(b"OWNERSHIP_UNCERTAIN" in result.stderr or b"DEPLOYMENT_CONFLICT" in result.stderr)
        f.stop("identity", key="recovered-stop")
        self.assertEqual(f.call("deployment", "inspect", "identity", "--operation", replay["operationId"])["operation"], terminal)
        self.assertEqual(f.call("deployment", "inspect", "identity")["writerOwnership"], "released")

    def pause_bridge_during_core_start(self, f, operation):
        """A previous provider sweep cannot satisfy readiness after slow core startup."""
        deadline = time.monotonic() + f.ready_timeout
        while True:
            with self.database(f) as db:
                rows = [(rid, json.loads(raw) if raw else {}) for rid, raw in db.execute(
                    "SELECT resource_id,backend_identity FROM resource_ownership "
                    "WHERE operation_id=? AND resource_type='workload'", (operation,))]
            with self.database(f) as db:
                state, = db.execute("SELECT state FROM operations WHERE operation_id=?", (operation,)).fetchone()
            self.assertNotIn(state, {"failed", "cancelled"}, "Start failed before the final readiness check")
            bridge = next((v.get("native") for rid, v in rows if rid.startswith("run-oidc-bridge-")), None)
            core = next((v.get("native") for rid, v in rows if rid.startswith("run-scylladb-")), None)
            if bridge and core and bridge.get("pid", 0) > 0 and core.get("pid", 0) > 0:
                break
            self.assertLess(time.monotonic(), deadline, "Core startup never followed provider readiness")
            time.sleep(0.02)
        fd = os.pidfd_open(bridge["pid"])
        paused = False
        try:
            raw = (Path("/proc") / str(bridge["pid"]) / "stat").read_bytes()
            self.assertEqual(int(raw[raw.rindex(b")") + 2:].split()[19]), bridge["startTime"])
            signal.pidfd_send_signal(fd, signal.SIGSTOP)
            paused = True
            while True:
                with self.database(f) as db:
                    row = db.execute("SELECT observed_state FROM components WHERE component='controller'").fetchone()
                self.assertNotEqual(f.call("deployment", "inspect", "combined")["readiness"], "ready")
                if row and row[0] == "running":
                    break
                self.assertLess(time.monotonic(), deadline, "Core services did not start with a stalled bridge")
                time.sleep(0.1)
            until = time.monotonic() + 10
            while time.monotonic() < until:
                self.assertNotEqual(f.call("deployment", "inspect", "combined")["readiness"], "ready",
                                    "Stale provider checks satisfied combined readiness")
                time.sleep(0.1)
        finally:
            if paused:
                signal.pidfd_send_signal(fd, signal.SIGCONT)
            os.close(fd)

    def test_public_combined_graph(self):
        self.combined_graph("memdb")

    def test_public_combined_persistent_graph(self):
        self.combined_graph("scylladb")

    def combined_graph(self, datastore):
        f = Fixture(offline="oidc-persistent" if datastore == "scylladb" else "oidc", oidc=True)
        self.addCleanup(f.close)
        f.deployments.add("combined")
        process = subprocess.Popen(f.command("--non-interactive", "serve", "--add", "oidc",
                                   "--deployment", "combined", "--datastore", datastore, "--idempotency-key", "combined"),
                                   env=f.environment, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        deadline = time.monotonic() + 15
        while True:
            self.assertIsNone(process.poll(), "Foreground start exited before readiness")
            try:
                accepted = f.call("deployment", "inspect", "combined")
                if accepted.get("lastOperationId"):
                    break
            except AssertionError:
                pass
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.05)
        if datastore == "scylladb":
            self.pause_bridge_during_core_start(f, accepted["lastOperationId"])
        deployment = f.ready("combined", {"operationId": accepted["lastOperationId"]})
        self.assert_endpoints(deployment, True)
        self.assertEqual(f.active_components(), {"api", "git", "controller", "oidc-smtp", "oidc-hydra", "oidc-kratos", "oidc-ui", "oidc-bridge"} | ({"scylladb"} if datastore == "scylladb" else set()))
        self.assertTrue(list((f.root / "credentials").glob("*.json")))
        self.assertTrue(list((f.root / "oidc-credentials").glob("*.json")))
        process.send_signal(signal.SIGINT)
        process.wait(timeout=60)
        stopped = f.call("deployment", "inspect", "combined")
        self.assertEqual(stopped["desiredState"], "stopped")
        self.assertEqual(stopped["writerOwnership"], "released")



if __name__ == "__main__":
    unittest.main()
