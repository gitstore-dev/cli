#!/usr/bin/env python3
"""Assemble exactly four trusted build tuples; freeze inventory and checksums."""
import argparse
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime/packaging'))
from release_candidate import advance, propose, register, safe_name, write_new
from inventory import snapshot_file
from homebrew import render


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--builds', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--revision', required=True)
    args = parser.parse_args()
    if not args.output.is_absolute() or args.output.exists() or args.output.is_symlink():
        raise ValueError('select a new absolute output directory')
    groups = list(args.builds.glob('*/subjects.json'))
    if len(groups) != 4:
        raise ValueError('exactly four trusted platform build groups required')
    entries = {}
    for group in groups:
        for row in json.loads(group.read_text()):
            name = safe_name(row['filename'])
            identity = snapshot_file(group.parent, name)
            if name in entries:
                previous, previous_path = entries[name]
                if row != previous or snapshot_file(previous_path.parent, name)['sha256'] != identity['sha256']:
                    raise ValueError('build groups contain conflicting asset identities')
            entries[name] = (row, group.parent / name)
    extras = [name for name, (row, _) in entries.items() if row['kind'] in ('source', 'relink')]
    value = propose(args.version, args.revision, extras)
    args.output.mkdir(mode=0o700)
    archives = []
    for name, (row, source) in entries.items():
        shutil.copyfile(source, args.output / name)
        (args.output / name).chmod(0o755 if row['kind'] in ('gs', 'gitstored') else 0o644)
        register(value, args.output, name, row['kind'], row['os'], row['arch'],
                 install_inventory=row['installInventory'], unpacked_bytes=row['unpackedBytes'])
        if row['kind'] == 'archive':
            archives.append(dict(os=row['os'], arch=row['arch'],
                url=f'https://github.com/gitstore-dev/cli/releases/download/{value["tag"]}/{name}',
                sha256=snapshot_file(args.output, name)['sha256']))
    formula = render(args.version, archives).encode()
    write_new(args.output / 'gitstore.rb', formula)
    register(value, args.output, 'gitstore.rb', 'formula', 'any', 'any',
             install_inventory='release-inventory.json', unpacked_bytes=len(formula))
    for target in ('version-reviewed', 'built', 'packaged'):
        value = advance(value, target, args.output, reviewed_tag=value['tag'], reviewed_revision=args.revision)
    write_new(args.output / 'candidate.json', json.dumps(value, indent=2).encode() + b'\n')


if __name__ == '__main__':
    main()
