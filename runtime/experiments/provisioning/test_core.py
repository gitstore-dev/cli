#!/usr/bin/env python3
"""Opt-in native Linux core probe; assets must be verified private materializations.

This is development evidence, not clean-install or public lifecycle acceptance.
Every synthetic fixture is retained, including credentials, ledger and repository
volume; no broad cleanup or automatic signal to an unverified process is allowed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("probe", type=Path)
parser.add_argument("broker", type=Path)
parser.add_argument("assets", type=Path, help="parent of api/git/controller-execution/rootfs")
parser.add_argument("--fixture-parent", required=True, type=Path)
parser.add_argument("--sanitized", action="store_true")
args = parser.parse_args()
if os.geteuid() == 0 or not Path("/proc/self/ns/user").exists():
    raise SystemExit("Select a non-root disposable Linux fixture")
parent = args.fixture_parent.resolve(strict=True)
if parent.stat().st_uid != os.geteuid() or parent.stat().st_mode & 0o022:
    raise SystemExit("Fixture parent must be owned and not group/other writable")
roots = [(args.assets / f"{name}-execution/rootfs").resolve(strict=True)
         for name in ("api", "git", "controller")]
probe = args.probe.resolve(strict=True)
source_broker = args.broker.resolve(strict=True)
root = Path(tempfile.mkdtemp(prefix="core-live-", dir=parent))
root.chmod(0o700)
broker = root / "gs"
shutil.copyfile(source_broker, broker)
broker.chmod(0o500)
with source_broker.open("rb") as source, broker.open("rb") as installed:
    if hashlib.file_digest(source, "sha256").digest() != hashlib.file_digest(installed, "sha256").digest():
        raise SystemExit(f"Broker copy mismatch; fixture retained: {root}")
run = root / "run"
run.mkdir(mode=0o700)
uid = os.geteuid()
environment = {"HOME": pwd.getpwuid(uid).pw_dir, "PATH": "/nonexistent", "LANG": "C",
               "XDG_RUNTIME_DIR": f"/run/user/{uid}",
               "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{uid}/bus"}
if args.sanitized:
    environment.update(ASAN_OPTIONS="handle_segv=0:detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
print(json.dumps({"fixture": str(root), "sanitized": args.sanitized}), flush=True)
try:
    result = subprocess.run([str(probe), str(run), *(str(p) for p in roots), str(broker)],
                            env=environment, timeout=240, check=False)
except subprocess.TimeoutExpired:
    raise SystemExit(f"Probe deadline expired; inspect retained ownership before cleanup: {root}") from None
print(json.dumps({"exit": result.returncode, "fixture": str(root),
                  "scope": "private native core bootstrap and stop; public dispatcher unverified"}), flush=True)
raise SystemExit(result.returncode)
