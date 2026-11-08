#!/usr/bin/env python3
"""Build explicitly UNQUALIFIED lifecycle fixtures from real local executables.

Intentionally incomplete third-party notices; never publish or qualify these
files. Names cannot be registered in the canonical release subject slots.
"""
import argparse
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'runtime/packaging'))
from formats import npm, deb, rpm
from notices import DISCLAIMER
from stage import inspect_layout, archive_layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pair', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--channels', required=True, nargs='+', choices=['npm','deb','rpm','archive'])
    args = parser.parse_args()
    if not args.pair.is_absolute() or not args.output.is_absolute() or args.output.exists():
        parser.error('select an absolute ordinary binary pair and new output directory')
    args.output.mkdir(mode=0o700)
    prefix = args.output / 'prefix'
    (prefix / 'bin').mkdir(parents=True)
    for name in ('gs', 'gitstored'):
        if (args.pair / name).is_symlink():
            parser.error('select ordinary executable copies')
        shutil.copyfile(args.pair / name, prefix / 'bin' / name)
        (prefix / 'bin' / name).chmod(0o755)
    subprocess.run(['sh', str(ROOT / 'scripts/generate-cli-reference.sh'), str(prefix / 'bin/gs'),
                    str(prefix / 'share')], check=True)
    notices = prefix / 'share/notices'
    notices.mkdir()
    (notices / 'UNQUALIFIED.txt').write_text('INCOMPLETE THIRD-PARTY NOTICES. LOCAL TEST ONLY. NEVER PUBLISH.\n' + DISCLAIMER)
    (notices / 'components.json').write_text(json.dumps(dict(components=[dict(
        id='UNQUALIFIED-INTEGRATION-FIXTURE', license='LicenseRef-Integration-Fixture')])) + '\n')
    for path in notices.iterdir():
        path.chmod(0o644)
    os_name = 'darwin' if sys.platform == 'darwin' else 'linux'
    arch = {'arm64':'arm64', 'aarch64':'arm64', 'x86_64':'x64'}[platform.machine()]
    value = inspect_layout(prefix, args.version, args.revision, os_name, arch,
        'TEST ONLY; unqualified', library_closure=[])
    (args.output / 'UNQUALIFIED.install.json').write_text(json.dumps(value, indent=2) + '\n')
    for channel in args.channels:
        name = f'UNQUALIFIED-ONLY-{args.version}-{os_name}-{arch}.' + {'npm':'tgz','deb':'deb','rpm':'rpm','archive':'tar.gz'}[channel]
        output = args.output / name
        {'npm':npm,'deb':deb,'rpm':rpm,'archive':archive_layout}[channel](prefix, value, output)
    print(json.dumps(dict(output=str(args.output), qualification='UNQUALIFIED; incomplete notices; never publish')))


if __name__ == '__main__':
    main()
