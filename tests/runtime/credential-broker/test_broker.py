#!/usr/bin/env python3
"""Explicit Linux C launcher -> installed gs -> synthetic private-file check."""
import argparse
import os
import shutil
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--probe", required=True, type=Path)
parser.add_argument("--gs", required=True, type=Path)
args = parser.parse_args()
probe, gs = args.probe.resolve(strict=True), args.gs.resolve(strict=True)
with tempfile.TemporaryDirectory(prefix="gitstored-broker-") as root:
    os.chmod(root, 0o700)
    # Cargo hard-links its development executable to deps/. Installation supplies
    # a private single-link copy; do not weaken the launcher's file identity rule.
    installed = Path(root) / "gs"
    shutil.copyfile(gs, installed)
    installed.chmod(0o700)
    secret = Path(root) / "synthetic-secret"
    secret.write_text("private-fixture-value", encoding="utf-8")
    secret.chmod(0o600)
    env = {"PATH": "/nonexistent", "HOME": root, "DOCKER_CONFIG": root,
           "GITSTORE_CONFIG": str(Path(root) / "missing-config")}
    for mode, success in [(0o600, True), (0o644, False)]:
        secret.chmod(mode)
        result = subprocess.run([str(probe), str(installed), str(secret)], env=env,
                                capture_output=True, timeout=10, cwd=root)
        if (result.returncode == 0) != success or result.stdout or result.stderr:
            raise SystemExit("FAIL: private descriptor broker/protected-file policy")
print("PASS: C launcher -> gs broker; private output and explicit file permissions")
