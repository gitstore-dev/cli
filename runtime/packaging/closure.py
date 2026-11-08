"""Inspect final native dependencies and exercise Linux descriptor re-execution.

Producer checks only. These checks do not replace service lifecycle acceptance.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

LINUX_SYSTEM = {'libc.so.6', 'libm.so.6', 'libgcc_s.so.1', 'libdl.so.2',
                'libpthread.so.0', 'librt.so.1', 'ld-linux-aarch64.so.1',
                'ld-linux-x86-64.so.2'}


def inspect_macho(binary):
    libraries = subprocess.run(['otool', '-L', str(binary)], capture_output=True,
                               text=True, check=True, timeout=15).stdout
    needed = [line.strip().split(' (compatibility version ', 1)[0]
              for line in libraries.splitlines()[1:] if line.strip()]
    if not needed or any(not name.startswith(('/usr/lib/', '/System/Library/Frameworks/')) for name in needed):
        raise ValueError('macOS control candidate depends on unqualified private/Homebrew libraries')
    commands = subprocess.run(['otool', '-l', str(binary)], capture_output=True,
                              text=True, check=True, timeout=15).stdout
    versions = re.findall(r'cmd LC_BUILD_VERSION\b(?:(?!Load command).)*?\bminos ([0-9.]+)', commands, re.S)
    versions += re.findall(r'cmd LC_VERSION_MIN_MACOSX\b(?:(?!Load command).)*?\bversion ([0-9.]+)', commands, re.S)
    if not versions or any(tuple((list(map(int, version.split('.'))) + [0, 0])[:2]) > (15, 0) for version in versions):
        raise ValueError('macOS candidate exceeds the declared deployment floor')
    return {'needed': needed, 'deploymentTargets': versions, 'runtime': 'control-only'}


def inspect_elf(binary):
    reply = subprocess.run(['readelf', '-d', str(binary)], capture_output=True,
                           text=True, check=True, timeout=15)
    needed = sorted(set(re.findall(r'\(NEEDED\).*\[([^\]]+)\]', reply.stdout)))
    if set(needed) - LINUX_SYSTEM:
        raise ValueError('unqualified dynamic closure: ' + ', '.join(set(needed) - LINUX_SYSTEM))
    if re.search(r'\((?:RPATH|RUNPATH)\)', reply.stdout):
        raise ValueError('private-static candidate must not depend on library search paths')
    versions = subprocess.run(['readelf', '--version-info', str(binary)],
        capture_output=True, text=True, check=True, timeout=15).stdout
    glibc = sorted({tuple(map(int, v.split('.'))) for v in
                    re.findall(r'Name: GLIBC_([0-9.]+)', versions)})
    if glibc and glibc[-1] > (2, 39):
        raise ValueError('candidate exceeds declared glibc 2.39 floor')
    return {'needed': needed, 'glibcRequired': '.'.join(map(str, glibc[-1])) if glibc else None}


def reexecute(binary):
    binary = Path(binary)
    info = binary.lstat()
    if not binary.is_absolute() or binary.is_symlink() or info.st_nlink != 1:
        raise ValueError('select an ordinary final executable')
    env = {'PATH': '/nonexistent', 'LANG': 'C'}

    def invoke(path, inherited=()):
        result = subprocess.run([str(path), '--version'], env=env,
            pass_fds=inherited, capture_output=True, check=True, timeout=15)
        if result.stderr or len(result.stdout) > 1024:
            raise ValueError('unexpected re-execution version response')
        return result.stdout

    ordinary = invoke(binary)
    pinned = os.open(binary, os.O_RDONLY | os.O_NOFOLLOW)
    memory = os.memfd_create('gitstore-release-closure', os.MFD_ALLOW_SEALING)
    try:
        with binary.open('rb') as source:
            while chunk := source.read(1024 * 1024):
                remaining = memoryview(chunk)
                while remaining:
                    remaining = remaining[os.write(memory, remaining):]
        fcntl.fcntl(memory, fcntl.F_ADD_SEALS,
                    fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW |
                    fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
        for descriptor in [pinned, memory]:
            if invoke(f'/proc/self/fd/{descriptor}', (descriptor,)) != ordinary:
                raise ValueError('descriptor re-execution changed version identity')
    finally:
        os.close(pinned)
        os.close(memory)
    return {'ordinary': True, 'pinnedFd': True, 'sealedMemfd': True,
            'sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    args = parser.parse_args()
    if sys.platform == 'darwin':
        binary = args.binary
        if not binary.is_absolute() or binary.is_symlink() or binary.stat().st_nlink != 1:
            raise ValueError('select an ordinary final executable')
        identity = inspect_macho(binary)
        result = subprocess.run([str(binary), '--version'], check=True, capture_output=True,
            timeout=15, env={'PATH':'/nonexistent', 'LANG':'C'})
        if result.stderr or not result.stdout or len(result.stdout) > 1024:
            raise ValueError('unexpected macOS executable version output')
        execution = {'ordinary':True, 'sha256':hashlib.sha256(binary.read_bytes()).hexdigest()}
    else:
        identity, execution = inspect_elf(args.binary), reexecute(args.binary)
    print(json.dumps({'binary': str(args.binary), **identity, 'execution': execution}, indent=2))


if __name__ == '__main__':
    main()
