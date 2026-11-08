#!/usr/bin/env python3
"""Build one production tuple from committed reviewed closure/license inputs.

Missing reviews fail before building. This produces read-only candidates, never
host qualification, signatures, tags, release uploads or package registry writes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime/packaging'))
from inventory import relative, snapshot_file
from notices import generate, read_file
from stage import inspect_layout, archive_layout
from formats import npm, deb, rpm
from release_candidate import platforms, slots, slot_filename, safe_name


def run(*args):
    return subprocess.run(args, cwd=ROOT, check=True, capture_output=False)


def fetch(url, destination, digest):
    if not url.startswith('https://') or destination.exists() or destination.is_symlink():
        raise ValueError('select a reviewed HTTPS source and new destination')
    with urllib.request.urlopen(url, timeout=60) as response:
        if not response.url.startswith('https://'):
            raise ValueError('source download redirected outside HTTPS')
        payload = response.read(512 * 1024 * 1024 + 1)
    if len(payload) > 512 * 1024 * 1024 or hashlib.sha256(payload).hexdigest() != digest:
        raise ValueError('reviewed source material digest mismatch')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if subprocess.check_output(['git','status','--porcelain','--untracked-files=normal'],cwd=ROOT):
        raise ValueError('production release inputs must come from a clean committed checkout; use disposable smoke tooling for working-tree experiments')
    os_name = 'darwin' if sys.platform == 'darwin' else 'linux'
    arch = {'aarch64':'arm64', 'arm64':'arm64', 'x86_64':'x64'}.get(platform.machine())
    row = next(r for r in platforms() if (r['os'], r['arch']) == (os_name, arch))
    reviewed = ROOT / 'packaging/release-inputs' / f'{os_name}-{arch}'
    # These files must be reviewed and committed, never generated from guessed
    # license names in the release job. T015 supplies their actual contents.
    manifest = json.loads((reviewed / 'notices.json').read_text())
    closure = json.loads((reviewed / 'closure.json').read_text())
    materials = json.loads((reviewed / 'materials.json').read_text())
    if not closure:
        raise ValueError('nonempty reviewed library closure required')
    output = args.output
    if os.geteuid() == 0 or not output.is_absolute() or output.exists() or output.is_symlink():
        raise ValueError('select a non-root producer and new absolute output directory')
    version = (ROOT / 'version.txt').read_text().strip()
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    output.mkdir(mode=0o700)
    inputs = output / 'reviewed-materials'
    inputs.mkdir(mode=0o700)
    for entry in materials:
        relative(entry['path'])
        fetch(entry['url'], inputs / entry['path'], entry['sha256'])
    os.environ['LIBZ_SYS_STATIC'] = '1'
    if os_name == 'darwin':
        os.environ['MACOSX_DEPLOYMENT_TARGET'] = '15.0'
    run('cargo', 'build', '--release', '--workspace', '--locked')
    support = output / 'share'
    run('sh', str(ROOT / 'scripts/generate-cli-reference.sh'), str(ROOT / 'target/release/gs'), str(support))
    packages = []
    manifests = ['Cargo.toml'] + (['daemon/image-backend/Cargo.toml'] if os_name == 'linux' else [])
    for cargo in manifests:
        metadata = json.loads(subprocess.check_output(['cargo', 'metadata', '--locked', '--format-version', '1', '--manifest-path', cargo], cwd=ROOT))
        packages += metadata['packages']
    generate(inputs, manifest, {'packages': packages}, closure, support / 'notices')
    command = ['sh', str(ROOT / 'scripts/build-release-candidate.sh'), '--build-dir', str(output / 'native'),
               '--prefix', str(output / 'prefix'), '--share-dir', str(support)]
    if os_name == 'linux':
        source_cache = output / 'curl-sources'
        source_cache.mkdir()
        lock = json.loads((ROOT / 'packaging/curl-source.json').read_text())
        for name, digest in lock['files'].items():
            fetch('https://archive.ubuntu.com/ubuntu/pool/main/c/curl/' + name, source_cache / name, digest)
        curl = output / 'static-curl'
        run(sys.executable, str(ROOT / 'scripts/build-static-curl.py'), '--source-cache', str(source_cache),
            '--build-dir', str(output / 'curl-build'), '--prefix', str(curl))
        command += ['--static-curl-prefix', str(curl)]
    run(*command)
    prefix = output / 'prefix'
    inventory = inspect_layout(prefix, version, revision, os_name, arch, row['abiFloor'], library_closure=closure)
    assets = output / 'assets'
    assets.mkdir(mode=0o700)
    inventory_name = slot_filename(version, 'manifest', os_name, arch)
    (assets / inventory_name).write_text(json.dumps(inventory, indent=2) + '\n')
    subjects = []
    for kind, selected_os, selected_arch in slots():
        if (selected_os, selected_arch) != (os_name, arch):
            continue
        name = slot_filename(version, kind, os_name, arch)
        target = assets / name
        if kind == 'archive':
            archive_layout(prefix, inventory, target)
        elif kind in ('npm', 'deb', 'rpm'):
            {'npm':npm, 'deb':deb, 'rpm':rpm}[kind](prefix, inventory, target)
        elif kind in ('gs', 'gitstored'):
            shutil.copyfile(prefix / 'bin' / kind, target)
            target.chmod(0o755)
        subjects.append(dict(filename=name, kind=kind, os=os_name, arch=arch,
            installInventory=inventory_name, unpackedBytes=sum(f['bytes'] for f in inventory['files'])))
    extra = {}
    for component in manifest['components']:
        if 'LGPL' in component['license']:
            for field, kind in [('correspondingSource', 'source'), ('relinkKit', 'relink')]:
                entry = component[field]
                name = safe_name(entry['releaseAsset'])
                payload = read_file(inputs, entry)
                if name in extra and extra[name] != (kind, payload):
                    raise ValueError('conflicting source/relink material names')
                extra[name] = (kind, payload)
    for name, (kind, payload) in extra.items():
        (assets / name).write_bytes(payload)
        subjects.append(dict(filename=name, kind=kind, os='any', arch='any',
            installInventory='release-inventory.json', unpackedBytes=len(payload)))
    for entry in subjects:
        snapshot_file(assets, entry['filename'])
    (assets / 'subjects.json').write_text(json.dumps(subjects, indent=2) + '\n')


if __name__ == '__main__':
    main()
