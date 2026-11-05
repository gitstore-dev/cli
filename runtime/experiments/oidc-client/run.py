#!/usr/bin/env python3
"""Opt-in controlled OIDC HTTP worker checks; not a native provider graph pass."""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import stat
import subprocess
import tempfile
import threading
import time
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--fixture", required=True, type=Path)
args = parser.parse_args()
uid = os.geteuid()
if not uid or not Path("/proc/self/ns/user").exists():
    parser.error("Select an explicitly dedicated non-root Linux fixture")
source = args.fixture
info = source.lstat()
if (not source.is_absolute() or not stat.S_ISREG(info.st_mode) or info.st_uid != uid or
        stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or info.st_size > 65536):
    parser.error("Select an owned protected ordinary fixture manifest")
settings = json.loads(source.read_bytes())
if settings.get("disposableUid") != uid or settings.get("dedicatedTestAccount") is not True:
    parser.error("Explicit dedicated account selection is required")
parent = Path(settings["fixtureParent"])
if (not parent.is_absolute() or parent.resolve() != parent or parent.stat().st_uid != uid or
        parent.stat().st_mode & 0o022):
    parser.error("Select an owned ordinary disposable parent")
inputs = {}
for key in ("oidcHttpProbe", "postman"):
    path = Path(settings[key])
    metadata = path.lstat()
    if (not path.is_absolute() or not stat.S_ISREG(metadata.st_mode) or
            metadata.st_uid not in (0, uid) or metadata.st_mode & 0o022 or
            not metadata.st_mode & 0o111 or metadata.st_size > 268435456):
        parser.error("Select trusted ordinary test executables")
    with path.open("rb") as stream:
        inputs[key] = {"path": str(path), "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}
root = Path(tempfile.mkdtemp(prefix="oidc-http-", dir=parent))
root.chmod(0o700)
repo = Path(__file__).resolve().parents[3]
probe = root / "oidc_http_probe"
shutil.copyfile(settings["oidcHttpProbe"], probe)
probe.chmod(0o500)
if hashlib.sha256(probe.read_bytes()).hexdigest() != inputs["oidcHttpProbe"]["sha256"]:
    parser.error("Probe changed during snapshot")
home = root / "home"
home.mkdir(mode=0o700)
environment = {"HOME": str(home), "XDG_CONFIG_HOME": str(home), "PATH": "/usr/bin:/bin",
               "LANG": "C", "HTTP_PROXY": "http://127.0.0.1:9",
               "HTTPS_PROXY": "http://127.0.0.1:9", "NO_PROXY": "127.0.0.1,localhost"}
record = {"schemaVersion": 1, "scope": "Controlled HTTP and private worker only; OIDC profile remains disabled",
          "inputs": inputs, "uid": uid, "status": "running", "cases": []}
known = ["synthetic-oidc-registered-token", "synthetic-mismatched-secret-value",
         "synthetic-unexpected-public-secret"]


def write_private(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w") as stream:
        json.dump(value, stream, indent=2)
        stream.flush()
        os.fsync(stream.fileno())


def save():
    path = root / "result.json"
    if path.exists():
        with path.open("w") as stream:
            json.dump(record, stream, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
    else:
        write_private(path, record)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def request(path, value=None):
    data = None if value is None else json.dumps(value).encode()
    message = urllib.request.Request("http://127.0.0.1:4445" + path, data=data,
                                     headers={"Content-Type": "application/json"})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(message, timeout=5) as response:
        return json.load(response)


def invoke(action, suffix, expected, callback="http://127.0.0.1:3000/callback"):
    secret = secrets.token_urlsafe(32)
    known.append(secret)  # Register before any request or CLI log can contain it.
    path = root / (action + "-" + suffix + ".json")
    write_private(path, {"clientId": "gitstore-oidc-fixture-" + suffix, "audience": "gitstore",
                         "redirectUri": callback, "clientSecret": secret})
    env = {"HOME": str(home), "PATH": "/nonexistent", "LANG": "C",
           "HTTP_PROXY": "http://127.0.0.1:4446", "http_proxy": "http://127.0.0.1:4446",
           "ALL_PROXY": "http://127.0.0.1:4446", "NO_PROXY": ""}
    result = subprocess.run([str(probe), action, str(path)], env=env, capture_output=True,
                             text=True, timeout=12)
    check(all(item not in result.stdout + result.stderr for item in known),
          "Probe disclosed sensitive fixture material")
    check(result.returncode == 0, "Probe failed; inspect protected fixture")
    value = json.loads(result.stdout)
    check(set(value) == {"status"} and isinstance(value["status"], int), "Unexpected worker receipt shape")
    record["cases"].append({"action": action, "case": suffix, "expectedStatus": expected,
                            "status": value["status"], "typedReceiptOnly": True})
    save()
    check(value == {"status": expected}, "Unexpected typed HTTP worker result")


class Trap(http.server.BaseHTTPRequestHandler):
    count = 0

    def log_message(self, *unused):
        pass

    def do_GET(self):
        Trap.count += 1
        self.send_error(501)

    def do_POST(self):
        Trap.count += 1
        self.send_error(501)


mock = None
trap = None
reader = None
started = threading.Event()
start = time.monotonic()
save()
try:
    # Refuse a preexisting provider before starting anything. The mock's own
    # startup event and live PID are checked before sending any mutation.
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 4445))
    trap = http.server.ThreadingHTTPServer(("127.0.0.1", 4446), Trap)
    threading.Thread(target=trap.serve_forever, daemon=True).start()
    mock = subprocess.Popen([settings["postman"], "mock", "run",
                             str(repo / "postman/mocks/oidc-hydra"), "--port", "4445",
                             "--output", "ndjson", "--no-history"], cwd=repo,
                            env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            start_new_session=True)

    def capture():
        descriptor = os.open(root / "mock.log", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as output:
            while True:
                line = mock.stdout.readline(1048577)
                if not line:
                    return
                if len(line) > 1048576:
                    while line and not line.endswith(b"\n"):
                        line = mock.stdout.readline(1048577)
                    output.write(b"[oversized CLI log suppressed]\n")
                    continue
                for item in list(known):
                    line = line.replace(item.encode(), b"[REDACTED]")
                output.write(line)
                output.flush()
                if b"Mock server started: oidc-hydra at http://localhost:4445" in line:
                    started.set()

    reader = threading.Thread(target=capture, daemon=True)
    reader.start()
    check(started.wait(25) and mock.poll() is None, "Owned Postman mock did not start")
    invoke("ready", "ready", 1)
    request("/__fixture/ready", {"code": 503, "status": "unavailable"})
    invoke("ready", "unready", 0)
    request("/__fixture/ready", {"code": 200, "status": "warming"})
    invoke("ready", "not-ready-body", -1)
    request("/__fixture/ready", {"code": 200, "status": "ok"})
    invoke("create", "success", 2)
    invoke("inspect", "success", 3)
    for case in ("disconnect", "delay", "redirect", "bad-json", "oversized",
                 "wrong-client", "wrong-secret", "bad-type"):
        invoke("create", case, -2)
    # The provider may have committed despite a lost response. A matching GET
    # has a different typed status and never retries/acknowledges that CREATE.
    invoke("inspect", "disconnect", 3)
    invoke("create", "wrong-scope", 2)
    invoke("inspect", "wrong-scope", -1)
    invoke("create", "expose-secret", 2)
    invoke("inspect", "expose-secret", -1)
    before = request("/__fixture/calls")
    invoke("create", "invalid-callback", -1, callback="http://example.test/callback")
    check(request("/__fixture/calls") == before, "Invalid callback reached the provider")
    history = request("/__fixture/calls")
    check(len(history) == 11 and len({item["clientId"] for item in history}) == 11,
          "Worker repeated a mutation after uncertain submission")
    check(Trap.count == 0, "Redirect or proxy received a credential-bearing request")
    collection_secret = secrets.token_urlsafe(32)
    known.append(collection_secret)
    values = {"baseUrl": "http://127.0.0.1:4445", "ownedDisposableOidcFixture": "true",
              "clientId": "gitstore-oidc-fixture-collection", "audience": "gitstore",
              "redirectUri": "http://127.0.0.1:3000/callback", "clientSecret": collection_secret}
    selected_environment = root / "collection.environment.json"
    write_private(selected_environment, {"name": "Owned synthetic OIDC fixture", "values": [
        {"key": key, "value": value, "enabled": True,
         "type": "secret" if key == "clientSecret" else "default"}
        for key, value in values.items()]})
    collection = subprocess.run(
        [settings["postman"], "collection", "run", str(repo / "postman/collections/oidc-provider"),
         "-e", str(selected_environment), "--no-report-events", "--ignore-redirects"],
        cwd=repo, env=environment, capture_output=True, text=True, timeout=45)
    output = collection.stdout + collection.stderr
    check(all(item not in output for item in known), "Collection disclosed sensitive fixture material")
    log = root / "collection.log"
    descriptor = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w") as stream:
        stream.write(output)
    check(collection.returncode == 0, "Saved OIDC collection failed; inspect protected fixture")
    record.update(status="passed", mutationAttempts=len(history),
                  redirectAndProxyTrapRequests=Trap.count,
                  savedCollection={"exitCode": collection.returncode, "requests": 3,
                                   "assertionBlocks": 3, "target": "controlled Postman mock"},
                  elapsedSeconds=round(time.monotonic() - start, 3))
except Exception:
    record.update(status="failed", reason="Controlled HTTP assertion failed; inspect protected fixture",
                  elapsedSeconds=round(time.monotonic() - start, 3))
    raise
finally:
    if mock and mock.poll() is None:
        os.killpg(mock.pid, signal.SIGTERM)
        try:
            mock.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(mock.pid, signal.SIGKILL)
            mock.wait(timeout=5)
    if reader:
        reader.join(timeout=5)
    if trap:
        trap.shutdown()
        trap.server_close()
    for key, identity in inputs.items():
        with Path(identity["path"]).open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != identity["sha256"]:
                record.update(status="failed", reason="Selected input changed during run")
    save()
    print(json.dumps({"status": record["status"], "record": str(root / "result.json"),
                      "scope": record["scope"]}), flush=True)
if record["status"] != "passed":
    raise SystemExit(1)
