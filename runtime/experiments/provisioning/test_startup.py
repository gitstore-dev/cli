#!/usr/bin/env python3
"""Installed per-user startup probe on an explicitly dedicated disposable UID.
Uses the canonical runtime directory once; retain that directory and all state.
No deployment is authorized and no image/network/credential effect is expected.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import select
import shutil
import signal
import socket
import sqlite3
import struct
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("daemon", type=Path)
parser.add_argument("gs", type=Path)
parser.add_argument("--fixture-parent", type=Path, required=True)
parser.add_argument("--dedicated-test-account", action="store_true", required=True)
args = parser.parse_args()
uid = os.geteuid()
runtime = Path(f"/run/user/{uid}/gitstore")
if not uid or not Path("/proc/self/ns/user").exists() or runtime.exists():
    raise SystemExit("Select a dedicated non-root Linux account with no existing canonical GitStore runtime directory")
parent = args.fixture_parent.resolve(strict=True)
if parent.stat().st_uid != uid or parent.stat().st_mode & 0o022:
    raise SystemExit("Fixture parent must be owned and not group/other writable")
os.umask(0o077)
root = Path(tempfile.mkdtemp(prefix="installed-startup-", dir=parent))
(root / "bin").mkdir(mode=0o700)
for name, original in (("gs", args.gs), ("gitstored", args.daemon)):
    installed = root / "bin" / name
    shutil.copyfile(original, installed)
    installed.chmod(0o500)
    with original.open("rb") as first, installed.open("rb") as second:
        assert hashlib.file_digest(first, "sha256").digest() == hashlib.file_digest(second, "sha256").digest()
environment = {"HOME": pwd.getpwuid(uid).pw_dir, "PATH": "/nonexistent", "LANG": "C",
               "XDG_RUNTIME_DIR": f"/run/user/{uid}", "XDG_STATE_HOME": str(root / "state"),
               "XDG_CONFIG_HOME": str(root / "config"),
               "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{uid}/bus"}
print(json.dumps({"fixture": str(root), "runtime": str(runtime)}), flush=True)
result = subprocess.run([str(root / "bin/gs"), "--non-interactive", "-o", "json", "serve",
                         "--deployment", "startup-fixture", "--detach"], env=environment,
                        capture_output=True, timeout=20)
expected = [str(root / "bin/gitstored"), "--runtime-dir", str(runtime), "--state-dir",
            str(root / "state/gitstore"), "--ready-fd", "3"]
manager = None
for entry in Path("/proc").iterdir():
    if not entry.name.isdigit():
        continue
    try:
        if entry.stat().st_uid != uid:
            continue
        command = (entry / "cmdline").read_bytes().rstrip(b"\0").split(b"\0")
        if command == [s.encode() for s in expected]:
            if manager is not None:
                raise RuntimeError("Duplicate installed managers")
            manager = int(entry.name)
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        pass
if manager is None:
    raise RuntimeError(f"Installed manager was not ready; CLI exit {result.returncode}; fixture retained")
pid_fd = os.pidfd_open(manager)
try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stream:
        stream.settimeout(2)
        stream.connect(str(runtime / "gitstored.sock"))
        payload = json.dumps({"version": {"major": 1, "minor": 0}, "requestId": "probe",
            "method": "system.negotiate", "params": {"supported": [{"major": 1, "minor": 0}],
            "requiredCapabilities": ["deployment.start"]}}).encode()
        stream.sendall(struct.pack("!I", len(payload)) + payload)
        def receive(size):
            value = b""
            while len(value) < size:
                block = stream.recv(size-len(value))
                if not block:
                    raise RuntimeError("Readiness channel lost")
                value += block
            return value
        size, = struct.unpack("!I", receive(4))
        assert 0 < size <= 1048576
        response = json.loads(receive(size))
        assert response["result"]["implementationVersion"] == "0.1.0"
    db = sqlite3.connect((root / "state/gitstore/state.db").as_uri() + "?mode=ro", uri=True)
    assert db.execute("SELECT count(*) FROM operations").fetchone() == (0,)
    db.close()
    assert not (root / "state/gitstore/assets").exists()
    assert not (root / "state/gitstore/volumes").exists()
    print(json.dumps({"installedStartup": "passed", "privateReadiness": "verified",
                      "effects": "no operations, assets or volumes", "managerPid": manager}), flush=True)
finally:
    # This exact own manager was selected by UID plus all original invocation
    # arguments under the newly allocated private installation, then pinned.
    signal.pidfd_send_signal(pid_fd, signal.SIGTERM)
    poller = select.poll()
    poller.register(pid_fd, select.POLLIN)
    if not poller.poll(10000):
        raise RuntimeError("Owned manager shutdown remained uncertain")
    os.close(pid_fd)
