"""Explicit disposable public-lifecycle fixture. Missing setup is an error, not a pass.

Tests execute the public dispatcher with the selected native libraries. They do
not substitute native probes for CLI acceptance or touch personal credentials.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import socket
import struct
import threading
import sqlite3
import tempfile
import time
from urllib.parse import urlsplit


class Fixture:
    def __init__(self, offline=None, unsupported=False, oidc=False):
        if os.geteuid() == 0 or not Path("/proc/self/ns/user").exists():
            raise RuntimeError("Select an explicitly owned, non-root disposable Linux fixture")
        source = Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).resolve(strict=True)
        settings = json.loads(source.read_text())
        if settings.get("disposableUid") != os.geteuid() or settings.get("dedicatedTestAccount") is not True:
            raise RuntimeError("Select this dedicated disposable test account explicitly")
        parent = Path(settings["fixtureParent"]).resolve(strict=True)
        if parent.stat().st_uid != os.geteuid() or parent.stat().st_mode & 0o022:
            raise RuntimeError("Fixture parent must be owned and not group/other writable")
        self.root = Path(tempfile.mkdtemp(prefix="gs-public-runtime-", dir=parent))
        self.root.chmod(0o700)
        self.deployments = set()
        self.ready_timeout = 60
        for name in ("bin", "home", "config", "runtime", "state", "cache", "credentials"):
            (self.root / name).mkdir(mode=0o700)
        self.gs = self.root / "bin/gs"
        self.daemon = self.root / "bin/gitstored"
        for name, destination in (("gs", self.gs), ("daemon", self.daemon)):
            source_binary = Path(settings["unsupportedDaemon"] if name == "daemon" and unsupported else settings[name]).resolve(strict=True)
            shutil.copyfile(source_binary, destination)
            destination.chmod(0o500)
            if self.digest(source_binary) != self.digest(destination):
                raise RuntimeError("Fixture installation copy mismatch")
        self.environment = {"PATH": "/nonexistent", "HOME": str(self.root / "home"),
                            "XDG_CONFIG_HOME": str(self.root / "config"),
                            "XDG_STATE_HOME": str(self.root / "state"),
                            "XDG_CACHE_HOME": str(self.root / "cache"),
                            "XDG_RUNTIME_DIR": f"/run/user/{os.geteuid()}",
                            "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{os.geteuid()}/bus",
                            "LANG": "C", "NO_COLOR": "1"}
        # Owner settings, not wire-controlled paths. Dispatcher must parse and
        # validate these before any credential or asset operation is enabled.
        self.settings = {"schemaVersion": 1, "bootstrapCredentialSource": {
            "kind": "protected-directory", "path": str(self.root / "credentials")}}
        if oidc:
            (self.root / "oidc-credentials").mkdir(mode=0o700)
            self.settings["oidcCredentialSource"] = {"kind": "protected-directory", "path": str(self.root / "oidc-credentials")}
            self.ready_timeout = 240
        self.offline_layouts = settings.get("offlineLayouts")
        self.wrong_platform_layouts = settings.get("wrongPlatformLayouts")
        if offline == "persistent":
            self.settings["offlineLayouts"] = {
                name: settings[key] for name, key in
                (("api", "offlineApiLayout"), ("git", "offlineGitLayout"),
                 ("controller", "offlineControllerLayout"), ("scylladb", "offlineScyllaLayout"))}
        elif offline in ("oidc", "oidc-persistent"):
            layouts = settings.get("offlineOidcLayouts")
            providers = {"oidc-smtp", "oidc-hydra", "oidc-kratos", "oidc-ui", "oidc-bridge"}
            if not isinstance(layouts, dict) or not providers.issubset(layouts):
                raise RuntimeError("Select all five pinned disposable OIDC offline layouts")
            self.settings["offlineLayouts"] = dict(layouts)
            if offline == "oidc-persistent":
                self.settings["offlineLayouts"]["scylladb"] = settings["offlineScyllaLayout"]
        elif offline == "verified" and self.offline_layouts:
            self.settings["offlineLayouts"] = self.offline_layouts
        elif offline == "wrong-platform" and self.wrong_platform_layouts:
            self.settings["offlineLayouts"] = self.wrong_platform_layouts
        elif offline == "truncated" and self.offline_layouts:
            original = Path(self.offline_layouts["api"]).resolve(strict=True)
            files = list(original.rglob("*"))
            if any(p.is_symlink() for p in files) or sum(p.stat().st_size for p in files if p.is_file()) > 134217728:
                raise RuntimeError("Select a bounded verified ordinary OCI layout fixture")
            partial = self.root / "truncated-api"
            shutil.copytree(original, partial, symlinks=True)
            partial.chmod(0o700)
            blobs = [p for p in (partial / "blobs/sha256").iterdir() if p.is_file() and p.stat().st_size > 1024]
            if not blobs:
                raise RuntimeError("Offline fixture has no complete layer blob")
            blob = max(blobs, key=lambda p: p.stat().st_size)
            blob.chmod(0o600)
            with blob.open("r+b") as stream:
                stream.truncate(blob.stat().st_size // 2)
            self.settings["offlineLayouts"] = {**self.offline_layouts, "api": str(partial)}
        self.write_settings()
        self.socket = self.root / "runtime/gitstored.sock"
        self.start_manager()

    def start_manager(self):
        if hasattr(self,"process") and self.process.poll() is None:
            raise RuntimeError("The fixture manager is already running")
        self.process = subprocess.Popen([str(self.daemon), "--runtime-dir", str(self.root / "runtime"),
                                         "--state-dir", str(self.root / "state")],
                                        env=self.environment, stdin=subprocess.DEVNULL,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 10
        while True:
            if self.process.poll() is not None or time.monotonic() >= deadline:
                if self.process.poll() is None:
                    self.process.terminate()
                    try:
                        self.process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.process.kill()
                        self.process.wait(timeout=5)
                raise RuntimeError(f"Manager did not become available; retained fixture: {self.root}")
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                    probe.connect(str(self.socket))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            time.sleep(0.01)

    @staticmethod
    def digest(path):
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()

    def write_settings(self):
        path = self.root / "state/settings.json"
        path.write_text(json.dumps(self.settings))
        path.chmod(0o600)

    def command(self, *arguments, socket=None):
        return [str(self.gs), "--daemon-socket", str(socket or self.socket), "-o", "json", *arguments]

    def call(self, *arguments, success=True, timeout=90):
        result = subprocess.run(self.command(*arguments), env=self.environment,
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout)
        if success and result.returncode:
            # Raw application/config responses must never enter an assertion log.
            safe_code = "unavailable"
            safe_stage = "unavailable"
            for line in reversed(result.stderr.splitlines()):
                try:
                    diagnostic = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    continue
                if isinstance(diagnostic, dict):
                    code, stage = diagnostic.get("code"), diagnostic.get("stage")
                    if code in {"INVALID_ARGUMENT", "LOCAL_VALIDATION_FAILED", "LOCAL_FAILURE", "NOT_FOUND", "PROVISIONING_REQUIRED", "UNSUPPORTED_CAPABILITY", "UNSUPPORTED_HOST", "IDEMPOTENCY_CONFLICT", "OWNERSHIP_UNCERTAIN", "OPERATION_FAILED", "DEADLINE_EXCEEDED"}:
                        safe_code = code
                    if stage in {"validate", "parse", "resolve", "negotiate", "transport", "runtime", "observe", "cleanup"}:
                        safe_stage = stage
                    break
            raise AssertionError(f"Public command failed (exit {result.returncode}, {safe_code}/{safe_stage}); fixture {self.root}")
        if not success:
            if result.returncode == 0:
                raise AssertionError("Expected explicit pre-effect failure")
            return result
        try:
            return json.loads(result.stdout)
        except ValueError:
            raise AssertionError("Public command did not return exactly one JSON document") from None

    def start(self, name, *, provision=True, key=None, datastore="memdb"):
        self.deployments.add(name)
        arguments = ["--non-interactive", "serve", "--deployment", name, "--datastore", datastore,
                     "--detach", "--idempotency-key", key or f"start-{name}"]
        if provision:
            arguments.append("--allow-provision")
        operation = self.call(*arguments)
        if operation.get("deploymentId") != name or not operation.get("operationId"):
            raise AssertionError("Acceptance must preserve deployment and operation identity")
        return operation

    def ready(self, name, operation):
        result = self.call("--timeout", f"{self.ready_timeout}s", "--wait", "ready", "deployment", "inspect", name,
                           "--operation", operation["operationId"], timeout=self.ready_timeout + 10)
        deployment = result["deployment"]
        if (deployment["readiness"] != "ready" or
                deployment["observedGeneration"] != deployment["generation"]):
            raise AssertionError("Stale or pending generation satisfied readiness")
        return deployment

    def stop(self, name, key=None):
        self.call("deployment", "stop", name, "--idempotency-key", key or f"stop-{name}")

    def assets(self):
        # Only immutable asset payloads, never credentials/configuration/volumes.
        root = self.root / "state/assets"
        return {str(p.relative_to(root)): self.digest(p) for p in root.rglob("*")
                if p.is_file() and not p.is_symlink() and
                (p.name in ("manifest.json", "config.json") or "blobs" in p.parts)}

    def credential_path(self, deployment):
        with sqlite3.connect((self.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
            store, = database.execute("SELECT store_id FROM query_meta").fetchone()
        identity = {"installation": store, "issuer": "gitstore-local/core/1",
                    "account": deployment, "purpose": "bootstrap-material"}
        digest = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        return self.root / "credentials" / f"bootstrap-{digest}.json"

    def active_components(self):
        with sqlite3.connect((self.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
            rows = database.execute("SELECT descriptor FROM assets WHERE state='active'").fetchall()
        return {image["component"] for (descriptor,) in rows for image in json.loads(descriptor)["images"]}

    def failed_start(self, name, *, provision=False, key=None):
        self.deployments.add(name)
        arguments = ["--non-interactive", "serve", "--deployment", name, "--detach",
                     "--idempotency-key", key or f"failed-{name}"]
        if provision:
            arguments.append("--allow-provision")
        result = subprocess.run(self.command(*arguments), env=self.environment,
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=20)
        if result.returncode:
            if not any(code in result.stderr for code in (b"OPERATION_FAILED", b"INVALID_ARGUMENT")):
                raise AssertionError("Asset scenario did not reach the expected verification failure")
            return
        operation = json.loads(result.stdout)
        # Durable acceptance is valid even if a later bounded verification worker
        # fails. Require the recorded unsuccessful outcome, not a synchronous hash.
        terminal = self.call("--timeout", "60s", "--wait", "terminal", "deployment", "inspect", name,
                             "--operation", operation["operationId"], success=False)
        record = json.loads(terminal.stdout)
        if record["operation"]["state"] != "failed" or record["deployment"]["readiness"] == "ready":
            raise AssertionError("Invalid asset did not produce a recorded failed unready start")

    @staticmethod
    def endpoints(deployment):
        endpoints = deployment["endpoints"]
        if set(endpoints) != {"api", "git"}:
            raise AssertionError("Only API and Git may have public forwards")
        for endpoint in endpoints.values():
            url = urlsplit(endpoint)
            if url.scheme != "http" or url.hostname != "127.0.0.1" or not url.port or url.port < 1024:
                raise AssertionError("Endpoint must use an owned high loopback port")
            if url.username or url.password:
                raise AssertionError("Endpoint leaked credentials")
        return set(endpoints.values())

    def close(self):
        # A failed stop retains its ledger/data. Only our own manager child is
        # terminated here; no broad process matching or recursive data deletion.
        for name in self.deployments:
            try:
                self.stop(name, key=f"fixture-cleanup-{hashlib.sha256(name.encode()).hexdigest()[:8]}-{time.time_ns()}")
            except (AssertionError, subprocess.TimeoutExpired):
                pass
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        print(f"Retained public runtime fixture: {self.root}")


class DropStartAck:
    """Forward a durable start, discard exactly its ACK and retain only its ID."""
    def __init__(self, fixture):
        self.fixture = fixture
        self.path = fixture.root / "runtime/drop-ack.sock"
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(self.path))
        self.inode = self.path.stat().st_ino
        self.path.chmod(0o600)
        self.listener.listen(1)
        self.listener.settimeout(15)
        self.operation = None
        self.error = None
        self.finished = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    @staticmethod
    def exact(stream, length):
        buffer = bytearray()
        while len(buffer) < length:
            chunk = stream.recv(length - len(buffer))
            if not chunk:
                raise EOFError()
            buffer.extend(chunk)
        return bytes(buffer)

    @classmethod
    def frame(cls, stream):
        prefix = cls.exact(stream, 4)
        length = struct.unpack(">I", prefix)[0]
        if not 0 < length <= 1048576:
            raise ValueError("Private proxy frame bound exceeded")
        body = cls.exact(stream, length)
        return prefix + body, json.loads(body)

    def run(self):
        try:
            client, _ = self.listener.accept()
            with client, socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as upstream:
                client.settimeout(15)
                upstream.settimeout(15)
                upstream.connect(str(self.fixture.socket))
                for _ in range(8):
                    request_bytes, request = self.frame(client)
                    upstream.sendall(request_bytes)
                    response_bytes, response = self.frame(upstream)
                    if (request.get("method") == "deployment.start" and
                            request.get("requestId") == response.get("requestId")):
                        self.operation = response.get("result", {}).get("operationId")
                        return  # Close client before delivering this exact ACK.
                    client.sendall(response_bytes)
        except Exception as error:
            self.error = type(error).__name__  # Never raw frames or credentials.
        finally:
            self.listener.close()
            self.finished.set()

    def close(self):
        self.listener.close()
        self.thread.join(timeout=16)
        if self.path.exists() and self.path.lstat().st_ino == self.inode:
            self.path.unlink()
