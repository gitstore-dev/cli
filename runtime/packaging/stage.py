"""Inventory and archive an explicitly selected production staging directory."""
import argparse
import gzip
import io
import json
import os
from pathlib import Path
import re
import subprocess
import struct
import tarfile

from inventory import relative, snapshot_file, validate_install, validate_tree
from versions import encode

ALLOWED = ('share/notices/', 'share/reference/', 'share/completions/',
           'share/gitstore/systemd/', 'lib/')
FORBIDDEN = re.compile(r'(?:secret|credential|password|token|\.env|\.key$|\.pem$|probe|rootfs)', re.I)
LICENSE_NOTICE = re.compile(r'share/notices/[A-Za-z0-9_.@+\-]+/LICENSE-[0-9]+\.txt')


def reported_version(path):
    reply = subprocess.run([str(path), '--version'], capture_output=True,
        timeout=10, check=True, env={'PATH': '/nonexistent', 'LANG': 'C'})
    if reply.stderr or len(reply.stdout) > 1024:
        raise ValueError('unexpected executable version response')
    match = re.fullmatch(rb'(?:gs|gitstored) ([0-9]+\.[0-9]+\.[0-9]+(?:-alpha\.[0-9]+)?)(?: \(protocol [0-9.]+; production build\))?\n', reply.stdout)
    if not match:
        raise ValueError('unrecognized application version')
    return match[1].decode('ascii')


def inspect_layout(prefix, version, revision, os_name, arch, abi_floor,
                   version_reader=reported_version, library_closure=None):
    prefix = Path(prefix)
    if not prefix.is_absolute() or prefix.is_symlink():
        raise ValueError('select an absolute ordinary production staging directory')
    encode(version)
    if not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('source revision must be a full lowercase SHA')
    files = []
    names = []
    for path in sorted(prefix.rglob('*')):
        name = path.relative_to(prefix).as_posix()
        if path.is_symlink():
            raise ValueError('staging symlinks are not permitted')
        if path.is_dir():
            continue
        relative(name)
        if (FORBIDDEN.search(name) and not LICENSE_NOTICE.fullmatch(name)) or not (name in ('bin/gs', 'bin/gitstored') or name.startswith(ALLOWED)):
            raise ValueError(f'not an allowed distribution payload: {name}')
        entry = snapshot_file(prefix, name)
        if entry['mode'] != (0o755 if name.startswith('bin/') else 0o644):
            raise ValueError('unexpected staging file mode')
        names.append(name)
        files.append(entry)
    executables = {}
    for name in ['gs', 'gitstored']:
        key = f'bin/{name}'
        if key not in names:
            raise ValueError('matching executable pair required')
        binary = prefix / key
        with binary.open('rb') as stream:
            header = stream.read(64)
        if os_name == 'linux':
            if (len(header) < 20 or header[:5] != b'\x7fELF\x02' or
                    header[5] not in (1, 2)):
                raise ValueError('64-bit ELF executable required')
            machine = struct.unpack(('<' if header[5] == 1 else '>') + 'H', header[18:20])[0]
            if machine != {'x64': 62, 'arm64': 183}.get(arch):
                raise ValueError('ELF architecture does not match selected package')
        elif os_name == 'darwin':
            if len(header) < 8 or header[:4] not in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf'):
                raise ValueError('64-bit Mach-O executable required')
            machine = struct.unpack(('<' if header[0] == 0xcf else '>') + 'I', header[4:8])[0]
            if machine != {'x64': 0x1000007, 'arm64': 0x100000c}.get(arch):
                raise ValueError('Mach-O architecture does not match selected package')
        else:
            raise ValueError('unsupported platform')
        actual = version_reader(binary)
        if actual != version:
            raise ValueError('application versions disagree')
        executables[name] = {'version': actual,
            'sha256': next(item['sha256'] for item in files if item['path'] == key)}
    value = {'schemaVersion': 1, 'applicationVersion': version, 'sourceRevision': revision,
        'os': os_name, 'arch': arch, 'abiFloor': abi_floor,
        'runtime': 'native-linux' if os_name == 'linux' else 'control-only',
        'executables': executables, 'files': files, 'libraryClosure': library_closure or [],
        'notices': [name for name in names if name.startswith('share/notices/')],
        'reference': [name for name in names if name.startswith('share/reference/')],
        'completions': [name for name in names if name.startswith('share/completions/')]}
    private_hashes = {item['sha256'] for item in value['libraryClosure'] if item['delivery'] == 'private'}
    if any(item['path'].startswith('lib/') and item['sha256'] not in private_hashes for item in files):
        raise ValueError('private libraries require reviewed closure/license records')
    validate_install(value)
    return value


def archive_layout(prefix, value, output):
    validate_tree(value, prefix)
    output = Path(output)
    if not output.is_absolute():
        raise ValueError('select an absolute new archive path')
    # Snapshot into memory bounded by the checked inventory, then recheck hashes.
    import hashlib
    payloads = {}
    total = sum(item['bytes'] for item in value['files'])
    if total > 512 * 1024 * 1024:
        raise ValueError('distribution staging exceeds 512 MiB')
    for item in value['files']:
        payload = (Path(prefix) / item['path']).read_bytes()
        if len(payload) != item['bytes'] or hashlib.sha256(payload).hexdigest() != item['sha256']:
            raise ValueError('staging changed after inventory')
        payloads[item['path']] = (payload, item['mode'])
    payloads['share/gitstore/install.json'] = ((json.dumps(value, indent=2) + '\n').encode(), 0o644)
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as raw:
        with gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w') as archive:
                for name, (payload, mode) in sorted(payloads.items()):
                    entry = tarfile.TarInfo(name)
                    entry.mode, entry.size, entry.mtime = mode, len(payload), 0
                    archive.addfile(entry, io.BytesIO(payload))
        raw.flush()
        os.fsync(raw.fileno())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--os', required=True, choices=['linux', 'darwin'])
    parser.add_argument('--arch', required=True, choices=['x64', 'arm64'])
    parser.add_argument('--abi-floor', required=True)
    parser.add_argument('--library-closure', required=True, type=Path)
    args = parser.parse_args()
    closure = json.loads(args.library_closure.read_text())
    if not isinstance(closure, list) or not closure:
        parser.error('select the reviewed nonempty native library closure')
    value = inspect_layout(args.prefix, args.version, args.revision, args.os, args.arch,
                           args.abi_floor, library_closure=closure)
    archive_layout(args.prefix, value, args.output)
    print(json.dumps({'archive': str(args.output), 'qualification': 'unqualified candidate',
                      'version': args.version, 'files': len(value['files'])}))


if __name__ == '__main__':
    main()
