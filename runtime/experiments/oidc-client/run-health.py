#!/usr/bin/env python3
"""Run the fixed health consumer collection against owned local Postman mocks.

Maintainer-only controlled HTTP evidence. No provider images, credentials or
cloud publication; this does not establish native/public lifecycle readiness.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--postman", required=True, type=Path)
    parser.add_argument("--mock", required=True, type=Path)
    parser.add_argument("--collection", required=True, type=Path)
    parser.add_argument("--output-parent", required=True, type=Path)
    args = parser.parse_args()
    for path in (args.postman, args.mock, args.collection, args.output_parent):
        if not path.is_absolute() or not path.exists():
            parser.error("all paths must be absolute and exist")
    # The same fixed listeners used by the C worker, inside a disposable Linux
    # fixture. Never terminate a borrowed process or silently choose new ports.
    for port in (4445, 4434, 24002, 24004, 24005):
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", port))
    root = Path(tempfile.mkdtemp(prefix="oidc-health-collection-", dir=args.output_parent))
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root), NO_COLOR="1")
    server = subprocess.Popen(
        [str(args.postman), "mock", "run", str(args.mock), "--port", "4445", "--no-history"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
    )
    try:
        for _ in range(100):
            if server.poll() is not None:
                raise RuntimeError("owned mock failed to start")
            try:
                with socket.create_connection(("127.0.0.1", 4445), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("owned mock startup timeout")
        result = subprocess.run(
            [str(args.postman), "collection", "run", str(args.collection), "--report-events=false"],
            env=env, capture_output=True, text=True, timeout=45, check=False,
        )
        (root / "collection.txt").write_text(result.stdout + result.stderr)
        receipt = {"scope": "controlled health consumer requests only", "exitCode": result.returncode,
                   "root": str(root), "collection": str(args.collection)}
        (root / "result.json").write_text(json.dumps(receipt, indent=2) + "\n")
        print(result.stdout + result.stderr, end="")
        print(json.dumps(receipt))
        return result.returncode
    finally:
        if server.poll() is None:
            os.killpg(server.pid, signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(server.pid, signal.SIGKILL)
                server.wait()


if __name__ == "__main__":
    raise SystemExit(main())
