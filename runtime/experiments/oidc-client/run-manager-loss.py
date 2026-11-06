#!/usr/bin/env python3
"""Kill one owned test manager after readiness, then prove recovery and owned stop.

Maintainer fixture only. Supply an already configured disposable private state
root, installed synthetic credential broker, and native oidc_deployment_probe.
Never points at the user's normal manager/configuration. No provider process is
signalled by this driver; the production recovery/cleanup actors handle them.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import stat
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("probe", "broker", "root", "output-parent"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--deployment", required=True)
    parser.add_argument("--combined", action="store_true")
    args = parser.parse_args()
    for path in (args.probe, args.broker, args.root, args.output_parent):
        if not path.is_absolute() or not path.exists():
            parser.error("paths must be absolute and exist")
    info = args.root.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        parser.error("fixture root must be a private owned directory")
    if not args.deployment.startswith("oidc-") or len(args.deployment) > 40 or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.deployment):
        parser.error("use a dedicated oidc-prefixed fixture deployment name")
    result_root = Path(tempfile.mkdtemp(prefix="oidc-manager-loss-", dir=args.output_parent))
    os.chmod(result_root, 0o700)
    command = [str(args.probe), str(args.root), str(args.broker), args.deployment,
               "combined" if args.combined else "standalone"]
    started = time.monotonic()
    manager = None
    pidfd = None
    with (result_root / "manager.stderr").open("wb") as diagnostics:
        try:
            manager = subprocess.Popen(command + ["leave"], stdout=subprocess.PIPE,
                                       stderr=diagnostics, start_new_session=True)
            pidfd = os.pidfd_open(manager.pid)
            ready = False
            deadline = time.monotonic() + 240
            while time.monotonic() < deadline:
                if manager.poll() is not None:
                    raise RuntimeError("owned manager exited before readiness")
                if not select.select([manager.stdout], [], [], 1)[0]:
                    continue
                line = manager.stdout.readline()
                if not line:
                    raise RuntimeError("owned manager readiness pipe closed")
                value = json.loads(line)
                if value == {"managerReady": True, "providerServices": 5}:
                    ready = True
                    break
                raise RuntimeError("unexpected private fixture reply")
            if not ready:
                raise RuntimeError("owned manager readiness deadline")
            signal.pidfd_send_signal(pidfd, signal.SIGKILL)
            if manager.wait(timeout=10) != -signal.SIGKILL:
                raise RuntimeError("owned manager death was not observed")
        finally:
            if manager is not None and manager.poll() is None:
                manager.kill()
                manager.wait(timeout=10)
            if pidfd is not None:
                os.close(pidfd)
    recovery = subprocess.run(command + ["recover"], capture_output=True, timeout=90, check=False)
    (result_root / "recovery.stdout").write_bytes(recovery.stdout)
    (result_root / "recovery.stderr").write_bytes(recovery.stderr)
    result = {"scope": "owned native manager loss and production recovery/stop", "exitCode": recovery.returncode,
              "elapsedSeconds": round(time.monotonic() - started, 3), "deployment": args.deployment,
              "root": str(args.root), "combined": args.combined,
              "probeSha256": hashlib.sha256(args.probe.read_bytes()).hexdigest()}
    if recovery.returncode == 0:
        result["result"] = json.loads(recovery.stdout)
    (result_root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({**result, "evidenceRoot": str(result_root)}))
    return recovery.returncode


if __name__ == "__main__":
    raise SystemExit(main())
