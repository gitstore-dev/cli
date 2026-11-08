#!/usr/bin/env python3
"""Materialize code-only system packaging layout in a new explicit DESTDIR."""
import argparse
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/packaging'))
from formats import materialize, system_layout

parser = argparse.ArgumentParser(description=__doc__)
for name in ['prefix', 'inventory', 'destdir']:
    parser.add_argument('--' + name, required=True, type=Path)
args = parser.parse_args()
if not args.destdir.is_absolute() or args.destdir.exists() or args.destdir.is_symlink():
    parser.error('select a new absolute DESTDIR; this never installs on the running host')
value = json.loads(args.inventory.read_text())
payload, links = system_layout(args.prefix, value)
args.destdir.mkdir(mode=0o755)
materialize(args.destdir, payload, links)
