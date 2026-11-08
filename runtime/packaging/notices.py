"""Generate notices from reviewed, byte-identified license/source obligations.

Fail closed on missing Cargo packages, native libraries or LGPL relink material.
SPDX identifiers alone are not complete license notices.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

DISCLAIMER = ('Early-alpha software: no upgrade compatibility, data preservation '
    'or recovery guarantees. Upstream GitStore is also early alpha and its schemas '
    'continue changing incompatibly. Keep independent backups and do not use '
    'irreplaceable data. Attestations are available for optional consumer verification.\n')
ROOT = Path(__file__).resolve().parents[2]


def read_file(root, entry):
    name = entry['path']
    path = Path(name)
    if path.is_absolute() or '..' in path.parts or not path.parts:
        raise ValueError('notice paths must remain inside the reviewed input root')
    current = root
    for part in path.parts:
        current /= part
        if current.is_symlink():
            raise ValueError('notice input must not follow symbolic links')
    payload = current.read_bytes()
    if not payload or hashlib.sha256(payload).hexdigest() != entry['sha256']:
        raise ValueError('notice input digest mismatch')
    return payload


def generate(root, manifest, cargo_metadata, closure, output):
    root, output = Path(root), Path(output)
    if not root.is_absolute() or root.is_symlink() or not output.is_absolute() or output.exists():
        raise ValueError('select an absolute input root and new output directory')
    expected = {f"{p['name']}@{p['version']}" for p in cargo_metadata['packages']
                if p.get('source') is not None}
    # Both independent Rust workspaces must be included in producer metadata.
    native = {item['name'] for item in closure if item['delivery'] != 'system'}
    components = manifest['components']
    ids = [item['id'] for item in components]
    if len(ids) != len(set(ids)) or set(ids) != expected | native:
        raise ValueError('reviewed notices must cover the exact Rust and native closure')
    payloads = {'PRERELEASE.txt': DISCLAIMER.encode(), 'LICENSE.gitstore': (ROOT / 'LICENSE').read_bytes()}
    for component in components:
        identity = component['id']
        if not re.fullmatch(r'[A-Za-z0-9_.@+\-]+', identity):
            raise ValueError('invalid component identity')
        if not component['source'].startswith('https://') or not component['license']:
            raise ValueError('each component requires reviewed source and license identity')
        if not component['licenses']:
            raise ValueError('full license notices required')
        for index, entry in enumerate(component['licenses']):
            payloads[f'{identity}/LICENSE-{index}.txt'] = read_file(root, entry)
        if 'LGPL' in component['license']:
            # Reviewed source + relink kit are separate release subjects. Do not
            # pretend a link to upstream alone satisfies static-link obligations.
            for field in ['correspondingSource', 'relinkKit']:
                entry = component[field]
                read_file(root, entry)
                if not re.fullmatch(r'[A-Za-z0-9_.~-]+', entry.get('releaseAsset', '')):
                    raise ValueError('LGPL material must name its release asset')
    payloads['components.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    output.mkdir(mode=0o755)
    for name, payload in payloads.items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        target.chmod(0o644)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['input-root', 'manifest', 'cargo-metadata', 'closure', 'output']:
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    generate(args.input_root, json.loads(args.manifest.read_text()),
             json.loads(args.cargo_metadata.read_text()), json.loads(args.closure.read_text()), args.output)


if __name__ == '__main__':
    main()
