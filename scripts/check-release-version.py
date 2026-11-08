#!/usr/bin/env python3
"""Reject unsynchronized or unexpected alpha proposals before their merge."""
import argparse
import json
from pathlib import Path
import sys
import tomllib
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/packaging'))
from versions import validate_alpha_proposal


def check(root, previous):
    root = Path(root)
    version = (root / 'version.txt').read_text().strip()
    validate_alpha_proposal(previous, version)
    cargo = tomllib.loads((root / 'Cargo.toml').read_text())
    lock = tomllib.loads((root / 'Cargo.lock').read_text())
    manifest = json.loads((root / '.release-please-manifest.json').read_text())
    versions = [cargo['workspace']['package']['version'], manifest['.']]
    versions += [p['version'] for p in lock['package'] if p['name'] == 'gs']
    if len(versions) != 3 or any(v != version for v in versions):
        raise ValueError('application, package lock and release manifest versions disagree')
    return version


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--previous', required=True)
    args = parser.parse_args()
    print(check(args.root, args.previous))
