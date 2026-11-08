"""Render GitHub-hosted packages from an exact checked production inventory."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

from inventory import validate_tree
from notices import DISCLAIMER
from versions import encode

ROOT = Path(__file__).resolve().parents[2]
ARCH = {'x64': {'deb': 'amd64', 'rpm': 'x86_64'},
        'arm64': {'deb': 'arm64', 'rpm': 'aarch64'}}


def checked_payload(prefix, value):
    validate_tree(value, prefix)
    total = sum(item['bytes'] for item in value['files'])
    if total > 512 * 1024 * 1024:
        raise ValueError('package payload exceeds 512 MiB')
    payload = {}
    for item in value['files']:
        data = (Path(prefix) / item['path']).read_bytes()
        if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
            raise ValueError('payload changed after inventory')
        payload[item['path']] = (data, item['mode'])
    payload['share/gitstore/install.json'] = ((json.dumps(value, indent=2) + '\n').encode(), 0o644)
    payload['share/notices/PRERELEASE.txt'] = (DISCLAIMER.encode(), 0o644)
    return payload


def tar_bytes(payload, links=None):
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode='wb', filename='', mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as archive:
            for name, (data, mode) in sorted(payload.items()):
                entry = tarfile.TarInfo(name)
                entry.mode, entry.size = mode, len(data)
                archive.addfile(entry, io.BytesIO(data))
            for name, target in sorted((links or {}).items()):
                entry = tarfile.TarInfo(name)
                entry.type, entry.linkname, entry.mode = tarfile.SYMTYPE, target, 0o777
                archive.addfile(entry)
    return buffer.getvalue()


def write_new(output, data):
    output = Path(output)
    if not output.is_absolute():
        raise ValueError('select an absolute new artifact path')
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def npm(prefix, value, output):
    payload = checked_payload(prefix, value)
    metadata = json.loads((ROOT / 'packaging/npm/package.json.in').read_text())
    metadata.update(name=f"@gitstore/cli-{value['os']}-{value['arch']}",
                    version=encode(value['applicationVersion'])['npm'],
                    os=[value['os']], cpu=[value['arch']])
    payload['package.json'] = ((json.dumps(metadata, indent=2) + '\n').encode(), 0o644)
    write_new(output, tar_bytes({'package/' + name: entry for name, entry in payload.items()}))


def system_layout(prefix, value):
    if value['os'] != 'linux':
        raise ValueError('native system packages require Linux payloads')
    payload = {'usr/libexec/gitstore/' + name: entry for name, entry in checked_payload(prefix, value).items()}
    # Unit installation is inert. There are intentionally no package scriptlets.
    unit = (ROOT / 'runtime/packaging/systemd/gitstored.service.in').read_bytes().replace(
        b'@CMAKE_INSTALL_FULL_BINDIR@', b'/usr/libexec/gitstore/bin')
    payload['usr/lib/systemd/user/gitstored.service'] = (unit, 0o644)
    links = {f'usr/bin/{name}': f'../libexec/gitstore/bin/{name}' for name in ['gs', 'gitstored']}
    return payload, links


def materialize(root, payload, links):
    for name, (data, mode) in payload.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
    for name, target in links.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(target)


def deb(prefix, value, output):
    payload, links = system_layout(prefix, value)
    versions = encode(value['applicationVersion'])
    control = (ROOT / 'packaging/deb/control.in').read_text().replace('@VERSION@', versions['debian']).replace('@ARCH@', ARCH[value['arch']]['deb'])
    with tempfile.TemporaryDirectory(prefix='gitstore-deb-') as temporary:
        root = Path(temporary) / 'root'
        materialize(root, payload, links)
        (root / 'DEBIAN').mkdir()
        (root / 'DEBIAN/control').write_text(control)
        artifact = Path(temporary) / 'package.deb'
        subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(root), str(artifact)], check=True, timeout=120)
        write_new(output, artifact.read_bytes())


def rpm(prefix, value, output):
    payload, links = system_layout(prefix, value)
    versions = encode(value['applicationVersion'])
    spec = (ROOT / 'packaging/rpm/gitstore.spec.in').read_text()
    components = json.loads((Path(prefix) / 'share/notices/components.json').read_text())['components']
    licenses = sorted({component['license'] for component in components})
    if not licenses or any(not re.fullmatch(r'[A-Za-z0-9 .+()\-]+', license) for license in licenses):
        raise ValueError('RPM requires reviewed component license expressions')
    spec = spec.replace('@LICENSE@', 'MIT AND ' + ' AND '.join('(' + license + ')' for license in licenses))
    for key, replacement in [('VERSION', versions['rpmVersion']), ('RELEASE', versions['rpmRelease']), ('ARCH', ARCH[value['arch']]['rpm'])]:
        spec = spec.replace('@' + key + '@', replacement)
    with tempfile.TemporaryDirectory(prefix='gitstore-rpm-') as temporary:
        root = Path(temporary)
        for name in ['SOURCES', 'SPECS', 'BUILD', 'BUILDROOT', 'RPMS', 'SRPMS']:
            (root / name).mkdir()
        (root / 'SOURCES/payload.tar.gz').write_bytes(tar_bytes(payload, links))
        (root / 'SPECS/gitstore.spec').write_text(spec)
        subprocess.run(['rpmbuild', '--define', '_topdir ' + str(root),
                        '--target', ARCH[value['arch']]['rpm'], '-bb',
                        str(root / 'SPECS/gitstore.spec')], check=True, timeout=180)
        artifacts = list((root / 'RPMS').rglob('*.rpm'))
        if len(artifacts) != 1:
            raise ValueError('expected one binary RPM')
        write_new(output, artifacts[0].read_bytes())


def main(channel):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['prefix', 'inventory', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    value = json.loads(args.inventory.read_text())
    {'deb': deb, 'rpm': rpm, 'npm': npm}[channel](args.prefix, value, args.output)
