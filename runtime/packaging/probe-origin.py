#!/usr/bin/env python3
"""Disposable Linux experiment: distinguish real private lookup from system fallback.

Developer tooling only. Never mutate a selected product executable or library.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', required=True, type=Path)
    parser.add_argument('--fixture-parent', required=True, type=Path)
    args = parser.parse_args()
    if os.geteuid() == 0 or not Path('/proc/self/ns/user').exists():
        parser.error('select the dedicated non-root Linux fixture')
    for path in [args.daemon, args.fixture_parent]:
        if not path.is_absolute() or path.is_symlink() or path.stat().st_uid != os.geteuid():
            parser.error('select absolute ordinary paths owned by the test account')
    if args.fixture_parent.stat().st_mode & 0o077:
        parser.error('fixture parent must be mode 0700')
    baseline = subprocess.run([str(args.daemon), '--version'], capture_output=True,
                              check=True, timeout=10, env={'PATH': '/nonexistent'})
    dependencies = subprocess.run(['/usr/bin/ldd', str(args.daemon)],
        capture_output=True, check=True, timeout=10, env={'PATH': '/usr/bin:/bin'})
    match = re.search(rb'libcap\.so\.2 => (/[^\s]+)', dependencies.stdout)
    if not match:
        parser.error('select the production libcrun-linked daemon with libcap')
    library = Path(os.fsdecode(match[1])).resolve(strict=True)
    info = library.stat()
    if info.st_uid != 0 or info.st_mode & 0o022 or not stat.S_ISREG(info.st_mode):
        parser.error('system library identity is not trusted')
    root = Path(tempfile.mkdtemp(prefix='origin-reexec-', dir=args.fixture_parent))
    root.chmod(0o700)
    (root / 'bin').mkdir(mode=0o700)
    (root / 'lib').mkdir(mode=0o700)
    binary = root / 'bin/gitstored'
    private = root / 'lib/libgstd-probe-cap.so.2'
    shutil.copyfile(args.daemon, binary)
    binary.chmod(0o755)
    shutil.copyfile(library, private)
    private.chmod(0o644)
    subprocess.run(['/usr/bin/patchelf', '--replace-needed', 'libcap.so.2', private.name,
                    str(binary)], check=True, timeout=10, capture_output=True)
    subprocess.run(['/usr/bin/patchelf', '--force-rpath', '--set-rpath', '$ORIGIN/../lib',
                    str(binary)], check=True, timeout=10, capture_output=True)
    relative = subprocess.run([str(binary), '--version'], capture_output=True,
                             timeout=10, env={'PATH': '/nonexistent'})
    subprocess.run(['/usr/bin/patchelf', '--force-rpath', '--set-rpath', str(root / 'lib'),
                    str(binary)], check=True, timeout=10, capture_output=True)
    absolute = subprocess.run([str(binary), '--version'], capture_output=True,
                             timeout=10, env={'PATH': '/nonexistent'})
    result = {'fixture': str(root), 'baselineExit': 0,
        'relativeOriginExit': relative.returncode, 'fixedPathExit': absolute.returncode,
        'relativePrivateLibraryMissing': private.name.encode() in relative.stderr,
        'fixedPathVersionMatches': absolute.stdout == baseline.stdout,
        'scope': 'renamed private libcap prevents accidental system fallback; copies only'}
    print(json.dumps(result))
    if absolute.returncode != 0 or absolute.stdout != baseline.stdout:
        raise SystemExit('fixed-path control did not pass; retained experiment')


if __name__ == '__main__':
    main()
