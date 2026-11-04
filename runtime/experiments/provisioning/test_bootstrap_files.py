#!/usr/bin/env python3
"""Explicit Linux synthetic credential/file fixture; never reads the user's store."""
import argparse
import base64
import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("probe", type=Path)
parser.add_argument("broker", type=Path)
parser.add_argument("--sanitized", action="store_true")
args = parser.parse_args()
if os.geteuid() == 0 or not Path("/proc/self/ns/user").exists():
    raise SystemExit("Select a non-root disposable Linux fixture")
root = Path(tempfile.mkdtemp(prefix="gitstore-bootstrap-files-"))
root.chmod(0o700)
environment = {"PATH": "/usr/bin:/bin", "HOME": str(root), "LANG": "C"}
if args.sanitized:
    environment.update(ASAN_OPTIONS="handle_segv=0:detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")
source_broker = args.broker.resolve(strict=True)
installed_broker = root / "gs"
shutil.copyfile(source_broker, installed_broker)
installed_broker.chmod(0o500)
with source_broker.open("rb") as source, installed_broker.open("rb") as installed:
    if hashlib.file_digest(source, "sha256").digest() != hashlib.file_digest(installed, "sha256").digest():
        raise SystemExit(f"broker installation copy failed; fixture retained: {root}")
result = subprocess.run([str(args.probe.resolve(strict=True)), str(root), str(installed_broker)], env=environment,
                        capture_output=True, text=True, timeout=35)
if result.returncode:
    print(json.dumps({"root": str(root), "exit": result.returncode,
                      "output": result.stdout.strip(), "stderr": result.stderr.strip()}))
    raise SystemExit(result.returncode)
try:
    scope = root / "scope"
    material = json.loads((root / "credentials/bootstrap.json").read_text())
    def config(name):
        with (scope / name).open("rb") as source:
            return tomllib.load(source)
    api = config("api-validator.toml")
    git = config("git.toml")
    controller = config("controller-base.toml")
    assert api["api"]["auth"]["serviceaccount"]["signing_key"] == material["apiIssuerPEM"]
    assert api["grpc_auth"]["hmac_secret"] == git["grpc_auth"]["hmac_secret"] == material["hmacSecret"]
    assert "api" not in controller and "grpc_auth" not in controller
    assert "api" not in git
    record = json.loads((scope / "controller-signing.json").read_text())
    assert base64.b64decode(record["values"]["privateKey"]).decode() == material["controllerPEM"]
    assert base64.b64decode(record["values"]["keyID"]) == b"controller-key"
    assert material["apiIssuerPEM"] != material["controllerPEM"]
    assert all((file.stat().st_mode & 0o7777) == 0o600 for file in scope.iterdir())
except Exception:
    # Do not include a secret-bearing assertion, parser diagnostic or local value.
    raise SystemExit(f"private config assertion failed; synthetic fixture retained: {root}") from None
shutil.rmtree(root)
print(json.dumps({"exit": 0, "sanitized": args.sanitized,
                  "checks": "actual installed broker add-only file flow; stable reuse; TOML parsing; API-only issuer and controller-only record",
                  "scope": "synthetic credential/file fixture; no service or OS-store claim"}))
