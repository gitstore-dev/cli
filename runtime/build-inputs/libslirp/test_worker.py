#!/usr/bin/env python3
"""Opt-in Linux fixture checks for private persistent network workers, not gs start."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("probe", type=Path)
parser.add_argument("--sanitized", action="store_true")
parser.add_argument("--keep", action="store_true")
args = parser.parse_args()
if os.geteuid() == 0 or not Path("/proc/self/ns/user").exists():
    raise SystemExit("Select an explicit non-root disposable Linux fixture")
probe = args.probe.resolve(strict=True)
jobs = []
for scenario in ("normal", "lost-ack"):
    root = Path(tempfile.mkdtemp(prefix="gitstore-network-worker-"))
    root.chmod(0o700)
    environment = {"PATH": "/usr/bin:/bin", "HOME": str(root), "LANG": "C"}
    if args.sanitized:
        environment.update(ASAN_OPTIONS="handle_segv=0:detect_leaks=1",
                           UBSAN_OPTIONS="halt_on_error=1")
    child = subprocess.Popen([str(probe), str(root), *([scenario] if scenario != "normal" else [])],
                             env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True)
    jobs.append((scenario, root, child))
failed = False
for scenario, root, child in jobs:
    try:
        output, errors = child.communicate(timeout=35)
    except subprocess.TimeoutExpired:
        # This is our test child only. A detached worker is retained with its
        # committed owner identity for explicit reconciliation, never guessed dead.
        child.kill()
        output, errors = child.communicate()
    result = {"scenario": scenario, "root": str(root), "exit": child.returncode,
              "sanitized": args.sanitized, "output": output.strip(), "stderr": errors.strip()}
    owner_path = root / "scope/owner.json"
    if child.returncode == 0 and owner_path.stat().st_size <= 4096:
        owner = json.loads(owner_path.read_text())
        ready_path = root / "scope/ready.json"
        if ready_path.stat().st_size > 4096:
            raise SystemExit(f"oversized fixture evidence retained: {root}")
        observed = json.loads(ready_path.read_text())["observed"]
        result["identity"] = {key: owner[key] for key in ("bootId", "pid", "startTime")}
        result["namespace"] = {key: observed[key] for key in ("userInode", "networkInode")}
        result["ports"] = {key: observed[key] for key in ("apiPort", "gitPort")}
        if not args.keep:
            # Successful probe has observed actual worker/keeper exit and port
            # closure. Delete only this runner's exact transient synthetic root.
            shutil.rmtree(root)
    else:
        failed = True
    print(json.dumps(result), flush=True)
raise SystemExit(1 if failed else 0)
