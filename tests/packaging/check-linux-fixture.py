#!/usr/bin/env python3
"""Read-only prerequisites for the dedicated disposable native Linux account."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import socket
import stat
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', required=True, type=Path)
    args = parser.parse_args()
    uid = os.geteuid()
    if uid == 0 or not Path('/proc/self/ns/user').exists():
        parser.error('select the dedicated non-root Linux test account')
    info = args.fixture.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != uid or
            info.st_nlink != 1 or info.st_mode & 0o077):
        parser.error('fixture manifest must be an owned private single-link file')
    value = json.loads(args.fixture.read_text())
    if value.get('disposableUid') != uid or value.get('dedicatedTestAccount') is not True:
        parser.error('fixture does not select this dedicated account')
    runtime = Path(f'/run/user/{uid}')
    if runtime.stat().st_uid != uid or stat.S_IMODE(runtime.stat().st_mode) != 0o700:
        parser.error('private user runtime directory missing')
    cgroup = Path(f'/sys/fs/cgroup/user.slice/user-{uid}.slice/user@{uid}.service')
    if not {'cpu', 'memory', 'pids'} <= set((cgroup / 'cgroup.controllers').read_text().split()):
        parser.error('required cgroup controllers are not delegated')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as bus:
        bus.settimeout(3)
        bus.connect(str(runtime / 'bus'))
    fd = os.open('/dev/net/tun', os.O_RDWR | os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        if not stat.S_ISCHR(os.fstat(fd).st_mode):
            parser.error('TUN is not a character device')
    finally:
        os.close(fd)
    subprocess.run(['unshare', '--user', '--map-root-user', '--net', 'true'],
                   check=True, timeout=5, capture_output=True)
    fd = os.pidfd_open(os.getpid())
    os.close(fd)
    root = os.open(value['fixtureParent'], os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        class OpenHow(ctypes.Structure):
            _fields_ = [('flags', ctypes.c_uint64), ('mode', ctypes.c_uint64),
                        ('resolve', ctypes.c_uint64)]
        libc = ctypes.CDLL(None, use_errno=True)
        how = OpenHow(os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC, 0, 0x08 | 0x04)
        fd = libc.syscall(437, root, b'.', ctypes.byref(how), ctypes.sizeof(how))
        if fd < 0:
            raise OSError(ctypes.get_errno(), 'openat2 prerequisite failed')
        os.close(fd)
    finally:
        os.close(root)
    print(json.dumps({'fixture': value['fixtureName'], 'uid': uid,
        'prerequisites': 'pass', 'workloadAcceptance': 'not-run'}))


if __name__ == '__main__':
    main()
