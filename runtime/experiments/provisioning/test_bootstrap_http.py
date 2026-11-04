#!/usr/bin/env python3
"""Controlled HTTP evidence for the actual C client; not a Postman or live-service pass."""
import argparse
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from cryptography.hazmat.primitives import serialization

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("broker", type=Path)
parser.add_argument("--sanitized", action="store_true")
args = parser.parse_args()
if os.geteuid() == 0 or not Path("/proc/self/ns/user").exists():
    raise SystemExit("Select a non-root disposable Linux fixture")
build = args.build.resolve(strict=True)
root = Path(tempfile.mkdtemp(prefix="gitstore-bootstrap-http-"))
root.chmod(0o700)
environment = {"PATH": "/usr/bin:/bin", "HOME": str(root), "LANG": "C"}
if args.sanitized:
    environment.update(ASAN_OPTIONS="handle_segv=0:detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")
installed = root / "gs"
source_broker = args.broker.resolve(strict=True)
shutil.copyfile(source_broker, installed)
installed.chmod(0o500)
with source_broker.open("rb") as source, installed.open("rb") as copy:
    if hashlib.file_digest(source, "sha256").digest() != hashlib.file_digest(copy, "sha256").digest():
        raise SystemExit(f"broker copy mismatch; fixture retained: {root}")
prepared = subprocess.run([str(build / "bootstrap_provision_probe"), str(root), str(installed)],
                          env=environment, capture_output=True, text=True, timeout=35)
if prepared.returncode:
    raise SystemExit(f"private material fixture failed; retained: {root}")
material_path = root / "credentials/bootstrap.json"
material = json.loads(material_path.read_text())
key = serialization.load_pem_private_key(material["controllerPEM"].encode(), password=None)
public_key = key.public_key().public_bytes(serialization.Encoding.PEM,
                                          serialization.PublicFormat.SubjectPublicKeyInfo).decode()
repository = Path(__file__).resolve().parents[3]
login_query = (repository / "runtime/profiles/bootstrap-login.graphql").read_text()
enroll_query = (repository / "runtime/profiles/bootstrap-enroll.graphql").read_text()
fixture_token = "synthetic.bootstrap.token"
fixture_uid = "019a4e28-7680-7000-8000-000000000001"
api_health = {"status": "healthy", "version": "0.1.0-alpha.8", "timestamp": "2026-11-04T00:00:00Z",
              "checks": {"datastore": {"status": "healthy"}, "resource_watch": {"status": "healthy"}}}
controller_health = {"status": "ok", "version": "0.1.0-alpha.8", "ready": True,
                     "credentialReady": True, "kinds": {name: {"registered": True, "recovering": False,
                     "stalled": False, "poisonItems": 0} for name in
                     ("Namespace", "Repository", "CategoryTaxonomy", "Product")}}
advertisement = b"001e# service=git-upload-pack\n0000"
attacker_hits = []

class State:
    def __init__(self, case):
        self.case, self.login, self.create, self.invalid = case, 0, 0, False

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *unused):
        pass
    def reply(self, status, value, kind="application/json", location=None):
        body = value if isinstance(value, bytes) else json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        if location:
            self.send_header("Location", location)
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass
        self.close_connection = True
    def do_GET(self):
        state = self.server.state
        case = state.case
        if self.server.server_port == attacker.server_port:
            attacker_hits.append(True)
            self.reply(200, {})
        elif self.path == "/ready":
            value = json.loads(json.dumps(api_health))
            if case == "api-degraded": value["status"] = "degraded"
            if case == "api-version": value["version"] = "old"
            if case == "api-503": return self.reply(503, value)
            if case == "api-malformed": return self.reply(200, b"{not-json")
            if case == "api-oversized": return self.reply(200, b"x" * (1048576 + 1))
            if case == "api-redirect": return self.reply(302, {}, location=attacker_url)
            if case == "api-slow": time.sleep(3)
            self.reply(200, value)
        elif self.path == "/health":
            value = json.loads(json.dumps(controller_health))
            if case == "controller-credential": value["credentialReady"] = False
            if case == "controller-recovering": value["kinds"]["Repository"]["recovering"] = True
            if case == "controller-poison": value["kinds"]["Product"]["poisonItems"] = 1
            if case == "controller-unregistered": value["kinds"]["Namespace"]["registered"] = False
            if case == "controller-liveness": value = {"status": "ok", "version": "0.1.0-alpha.8"}
            self.reply(200, value)
        elif self.path == "/gitstore-system/gitstore-system/info/refs?service=git-upload-pack":
            expected = "Basic " + base64.b64encode(("admin:" + material["password"]).encode()).decode()
            if self.headers.get("Authorization") != expected:
                state.invalid = True
                return self.reply(401, {})
            if case == "git-redirect": return self.reply(302, {}, location=attacker_url)
            if case == "git-type": return self.reply(200, advertisement)
            self.reply(200, advertisement if case != "git-malformed" else b"not-git",
                       "application/x-git-upload-pack-advertisement")
        else:
            state.invalid = True
            self.reply(404, {})
    def do_POST(self):
        if self.server.server_port == attacker.server_port:
            attacker_hits.append(True)
            return self.reply(200, {})
        state = self.server.state
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if self.path != "/graphql" or not 0 < length < 8192: raise ValueError()
            body = json.loads(self.rfile.read(length))
            if body["query"] == login_query:
                state.login += 1
                if body["variables"] != {"username": "admin", "password": material["password"]}: raise ValueError()
                if state.case == "login-denied": return self.reply(401, {"errors": [{"message": "denied"}]})
                if state.case == "login-redirect": return self.reply(302, {}, location=attacker_url)
                token = "unsafe\r\nheader" if state.case == "token-injection" else fixture_token
                return self.reply(200, {"data": {"login": {"token": {"accessToken": token}}}})
            if body["query"] != enroll_query or self.headers.get("Authorization") != "Bearer " + fixture_token: raise ValueError()
            if body["variables"] != {"name": "gitstore-controller-manager", "keyID": "controller-key",
                                     "publicKey": public_key}: raise ValueError()
            state.create += 1
            if state.case == "enroll-lost-ack":
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                self.close_connection = True
                return
            if state.case == "enroll-redirect": return self.reply(302, {}, location=attacker_url)
            uid = "invalid uid" if state.case == "enroll-bad-uid" else fixture_uid
            value = {"data": {"createServiceAccount": {"serviceAccount": {"metadata": {"uid": uid}}}}}
            if state.case == "enroll-partial": value["errors"] = [{"message": "partial fixture error"}]
            self.reply(500 if state.case == "enroll-500" else 200, value)
        except Exception:
            state.invalid = True
            self.reply(400, {"errors": [{"message": "controlled request rejected"}]})

servers = []
running = []
current_case = "bind-owned-fixture-ports"
try:
    attacker = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    attacker.state = State("attacker")
    attacker_url = f"http://127.0.0.1:{attacker.server_port}/sink"
    servers.append(attacker)
    for port in (4000, 9000, 5001):
        server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        server.state = State("initial")
        servers.append(server)
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        running.append(server)
    cases = [("api", name, status, "") for name, status in
             [("api-ready", 0), ("api-degraded", 1), ("api-503", 1), ("api-version", -1),
              ("api-malformed", -1), ("api-oversized", -1), ("api-redirect", -1), ("api-slow", 1)]]
    cases += [("git", name, status, "") for name, status in
              [("git-ready", 0), ("git-type", -1), ("git-malformed", -1), ("git-redirect", -1)]]
    cases += [("controller", name, status, "") for name, status in
              [("controller-ready", 0), ("controller-credential", 1), ("controller-recovering", 1),
               ("controller-poison", 1), ("controller-unregistered", 1), ("controller-liveness", -1)]]
    cases += [("enroll", "enroll-ready", 0, fixture_uid), ("enroll", "login-denied", -1, ""),
              ("enroll", "login-redirect", -2, ""), ("enroll", "token-injection", -2, ""),
              ("enroll", "enroll-lost-ack", -2, ""), ("enroll", "enroll-redirect", -2, ""),
              ("enroll", "enroll-partial", -2, fixture_uid), ("enroll", "enroll-500", -2, fixture_uid),
              ("enroll", "enroll-bad-uid", -2, "")]
    environment.update(http_proxy=attacker_url, HTTP_PROXY=attacker_url,
                       ALL_PROXY=attacker_url, NO_PROXY="", no_proxy="")
    for action, name, status, uid in cases:
        current_case = name
        state = State(name)
        for server in servers[1:]: server.state = state
        result = subprocess.run([str(build / "bootstrap_http_probe"), action, str(material_path)],
                                env=environment, capture_output=True, text=True, timeout=10)
        try:
            actual = json.loads(result.stdout)
            expected_count = 0 if name in ("login-denied", "login-redirect", "token-injection") else 1
            valid = result.returncode == 0 and not result.stderr and actual == {"status": status, "uid": uid}
            valid = valid and not state.invalid and not attacker_hits
            if action == "enroll": valid = valid and state.login == 1 and state.create == expected_count
            valid = valid and all(secret not in result.stdout + result.stderr for key, secret in material.items()
                                  if key not in ("schemaVersion", "format", "username"))
        except Exception:
            valid = False
        if not valid:
            raise RuntimeError(name)
    print(json.dumps({"cases": len(cases), "exit": 0, "sanitized": args.sanitized,
                      "checks": "readiness semantics; matching controller public key; no proxy/redirect; bounded responses; no blind mutation retries; known partial UID retained",
                      "scope": "controlled HTTP fixture, not Postman simulation or real GitStore"}))
except Exception:
    raise SystemExit(f"controlled HTTP case {current_case} failed; private fixture retained: {root}") from None
finally:
    for server in servers:
        if server in running:
            server.shutdown()
        server.server_close()
shutil.rmtree(root)
