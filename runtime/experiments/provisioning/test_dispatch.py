#!/usr/bin/env python3
"""Explicit disposable Linux public Unix-protocol integration; retain all state.
Uses already prepared, verified offline OCI inputs. This bypasses CLI serve UX.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import socket
import struct
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("daemon", type=Path)
parser.add_argument("gs", type=Path)
parser.add_argument("layouts", type=Path, help="parent of offline-api/git/controller")
parser.add_argument("--fixture-parent", type=Path, required=True)
args = parser.parse_args()
uid = os.geteuid()
if not uid or not Path("/proc/self/ns/user").exists():
    raise SystemExit("Select a non-root disposable Linux fixture")
parent = args.fixture_parent.resolve(strict=True)
if parent.stat().st_uid != uid or parent.stat().st_mode & 0o022:
    raise SystemExit("Fixture parent must be owned and not group/other writable")
os.umask(0o077)
root = Path(tempfile.mkdtemp(prefix="dispatch-", dir=parent))
for name in ("bin", "state", "runtime", "credentials"):
    (root / name).mkdir(mode=0o700)
for name, original in (("gs", args.gs), ("gitstored", args.daemon)):
    installed = root / "bin" / name
    shutil.copyfile(original, installed)
    installed.chmod(0o500)
    with original.open("rb") as first, installed.open("rb") as second:
        if hashlib.file_digest(first, "sha256").digest() != hashlib.file_digest(second, "sha256").digest():
            raise SystemExit("Installed fixture copy mismatch")
(root / "state/settings.json").write_text(json.dumps({"schemaVersion": 1,
    "bootstrapCredentialSource": {"kind": "protected-directory", "path": str(root / "credentials")},
    "offlineLayouts": {c: str((args.layouts / f"offline-{c}").resolve(strict=True)) for c in ("api", "git", "controller")}}))
environment = {"HOME": pwd.getpwuid(uid).pw_dir, "PATH": "/nonexistent", "LANG": "C",
               "XDG_RUNTIME_DIR": f"/run/user/{uid}",
               "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{uid}/bus"}
print(json.dumps({"fixture": str(root)}), flush=True)
diagnostics = (root / "manager.stderr").open("wb")
process = subprocess.Popen([str(root / "bin/gitstored"), "--runtime-dir", str(root / "runtime"),
                            "--state-dir", str(root / "state")], env=environment,
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=diagnostics)
connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
connection.settimeout(10)
request_number = 0

def receive(length):
    output = b""
    while len(output) < length:
        chunk = connection.recv(length - len(output))
        if not chunk:
            raise RuntimeError("Manager disconnected; inspect retained owned state")
        output += chunk
    return output

def call(method, params, key=None):
    global request_number
    request_number += 1
    request = {"version": {"major": 1, "minor": 0}, "requestId": f"r-{request_number}",
               "method": method, "params": params}
    if key:
        request["idempotencyKey"] = key
    payload = json.dumps(request).encode()
    connection.sendall(struct.pack("!I", len(payload)) + payload)
    length, = struct.unpack("!I", receive(4))
    if not 0 < length <= 1048576:
        raise RuntimeError("Invalid response bound")
    response = json.loads(receive(length))
    if "error" in response:
        raise RuntimeError(f"Public method {method} failed with {response['error'].get('code')}")
    return response["result"]

def terminal(operation):
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        started = time.monotonic()
        observed = call("operation.inspect", {"operationId": operation})
        if time.monotonic() - started > 2:
            raise RuntimeError("Control inspection blocked behind runtime work")
        if observed["state"] in ("succeeded", "failed", "cancelled"):
            return observed
        time.sleep(0.1)
    raise RuntimeError("Operation deadline expired; retained ownership requires inspection")

started_operation = None
stopped = False
try:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Manager exited before socket readiness")
        try:
            connection.connect(str(root / "runtime/gitstored.sock"))
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.01)
    else:
        raise RuntimeError("Manager socket deadline")
    capabilities = call("system.negotiate", {"supported": [{"major": 1, "minor": 0}],
        "requiredCapabilities": []})
    if not {"deployment.start", "deployment.stop", "operation.read"}.issubset(capabilities["capabilities"]):
        raise RuntimeError(f"Lifecycle capabilities unavailable: {capabilities['capabilities']}")
    params = {"deploymentId": "fixture", "profileId": "core", "profileVersion": "1", "datastore": "memdb"}
    accepted = call("deployment.start", params, "dispatch-start")
    started_operation = accepted["operationId"]
    replay = call("deployment.start", params, "dispatch-start")
    assert replay["operationId"] == started_operation and replay["replayed"]
    observed = terminal(started_operation)
    if observed["state"] != "succeeded":
        raise RuntimeError("Accepted deployment did not reach readiness")
    deployment = call("deployment.inspect", {"deploymentId": "fixture"})
    assert deployment["readiness"] == "ready" and deployment["generation"] == deployment["observedGeneration"]
    assert deployment["writerOwnership"] == "held"
    assert set(deployment["endpoints"]) == {"api", "git"}
    stop = call("deployment.stop", {"deploymentId": "fixture"}, "dispatch-stop")
    assert terminal(stop["operationId"])["state"] == "succeeded"
    stopped = True
    deployment = call("deployment.inspect", {"deploymentId": "fixture"})
    assert deployment["desiredState"] == "stopped" and deployment["writerOwnership"] == "released"
    assert "endpoints" not in deployment
    again = call("deployment.stop", {"deploymentId": "fixture"}, "dispatch-stop-again")
    assert terminal(again["operationId"])["state"] == "succeeded"
    credential_files = [p for p in (root / "credentials").iterdir() if p.suffix == ".json"]
    assert len(credential_files) == 1
    with credential_files[0].open("rb") as stream:
        credential_digest = hashlib.file_digest(stream, "sha256").digest()
    restarted = call("deployment.start", params, "dispatch-restart")
    started_operation = restarted["operationId"]
    stopped = False
    assert terminal(started_operation)["state"] == "succeeded"
    deployment = call("deployment.inspect", {"deploymentId": "fixture"})
    assert deployment["generation"] == 3 and deployment["readiness"] == "ready" and deployment["writerOwnership"] == "held"
    with credential_files[0].open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").digest() == credential_digest
    stopped_operation = call("deployment.stop", {"deploymentId": "fixture"}, "dispatch-restart-stop")
    assert terminal(stopped_operation["operationId"])["state"] == "succeeded"
    stopped = True
    print(json.dumps({"publicDispatch": "passed", "readiness": "verified", "stop": "observed",
                      "dedupe": "same operation", "controlResponsiveness": "under 2 seconds"}), flush=True)
finally:
    if started_operation and not stopped and process.poll() is None:
        try:
            stop = call("deployment.stop", {"deploymentId": "fixture"}, "failure-stop")
            stopped = terminal(stop["operationId"])["state"] == "succeeded"
        except Exception:
            print(json.dumps({"cleanup": "uncertain", "fixture": str(root)}), flush=True)
    connection.close()
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
