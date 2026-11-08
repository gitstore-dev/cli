#!/usr/bin/env python3
"""Prepare local immutable release subjects/ledger; never creates tags or releases."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/packaging'))
from release_candidate import advance, propose, register, write_new


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asset-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    commands = parser.add_subparsers(dest='command', required=True)
    initialize = commands.add_parser('propose')
    initialize.add_argument('--version', required=True)
    initialize.add_argument('--revision', required=True)
    initialize.add_argument('--subjects', required=True, type=Path,
                            help='JSON rows: filename, kind, os, arch, installInventory, unpackedBytes')
    initialize.add_argument('--extra-asset', action='append', default=[])
    transition = commands.add_parser('advance')
    transition.add_argument('--candidate', required=True, type=Path)
    transition.add_argument('--state', required=True)
    transition.add_argument('--reviewed-tag')
    transition.add_argument('--reviewed-revision')
    transition.add_argument('--receipt', action='append', default=[])
    transition.add_argument('--trusted-root', type=Path)
    args = parser.parse_args()
    if not args.asset_dir.is_absolute() or not args.output.is_absolute():
        parser.error('select absolute asset and new ledger paths')
    if args.output.exists() or args.output.is_symlink():
        raise FileExistsError('ledger output already exists; select the next immutable ledger path')
    if args.command == 'propose':
        value = propose(args.version, args.revision, args.extra_asset)
        for row in json.loads(args.subjects.read_text()):
            register(value, args.asset_dir, row['filename'], row['kind'], row['os'], row['arch'],
                     install_inventory=row['installInventory'], unpacked_bytes=row['unpackedBytes'])
    else:
        value = json.loads(args.candidate.read_text())
        if args.receipt:
            if value['state'] != 'packaged' or args.state != 'validated':
                raise ValueError('qualification receipts are selected only at packaged → validated')
            value['receipts'] = args.receipt
        value = advance(value, args.state, args.asset_dir, reviewed_tag=args.reviewed_tag,
                        reviewed_revision=args.reviewed_revision, trusted_root=args.trusted_root)
    write_new(args.output, json.dumps(value, indent=2).encode() + b'\n')
    print(json.dumps(dict(state=value['state'], tag=value['tag'], subjects=len(value['artifacts']))))


if __name__ == '__main__':
    main()
