#!/usr/bin/env python3
"""Explicit disposable Linux probe: libcrun joins one owned user/network namespace."""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('probe', type=Path)
parser.add_argument('payload', type=Path)
parser.add_argument('--seccomp', required=True, type=Path)
args = parser.parse_args()
if os.getuid() == 0 or not hasattr(os, 'unshare'):
    raise SystemExit('Requires an explicitly selected non-root Linux fixture')
uid, gid = os.getuid(), os.getgid()
ready_r, ready_w = os.pipe()
life_r, life_w = os.pipe()
keeper = os.fork()
if not keeper:
    os.close(ready_r)
    os.close(life_w)
    try:
        os.unshare(os.CLONE_NEWUSER)
        Path('/proc/self/uid_map').write_text(f'0 {uid} 1\n')
        Path('/proc/self/setgroups').write_text('deny\n')
        Path('/proc/self/gid_map').write_text(f'0 {gid} 1\n')
        os.unshare(os.CLONE_NEWNET)
        os.write(ready_w, b'R')
        os.close(ready_w)
        os.read(life_r, 1)  # parent death closes its only writer, too
        os.close(life_r)
        os._exit(0)
    except BaseException:
        os._exit(1)
os.close(ready_w)
os.close(life_r)
try:
    ready = select.poll()
    ready.register(ready_r, select.POLLIN)
    if not ready.poll(5000) or os.read(ready_r, 1) != b'R':
        raise SystemExit('Owned namespace preparation failed')
    os.close(ready_r)
    print(json.dumps({'scope': 'owned shared namespaces, synthetic payload only',
                      'keeper': keeper,
                      'userNamespace': os.stat(f'/proc/{keeper}/ns/user').st_ino,
                      'networkNamespace': os.stat(f'/proc/{keeper}/ns/net').st_ino}), flush=True)
    runner = Path(__file__).with_name('run.py')
    command = [sys.executable, str(runner), str(args.probe.resolve(strict=True)),
               str(args.payload.resolve(strict=True)), '--delegated', '--seccomp',
               str(args.seccomp.resolve(strict=True)), '--namespace-owner', str(keeper)]
    result = subprocess.run(command, timeout=65, check=False)
    if result.returncode:
        raise SystemExit(result.returncode)
finally:
    os.close(life_w)
    _, status = os.waitpid(keeper, 0)
    if status:
        print('Namespace keeper failed; native failure roots remain for reconciliation', file=sys.stderr)
