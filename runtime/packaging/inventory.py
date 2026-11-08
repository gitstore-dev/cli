"""Local distribution inventory validation; no network, execution or publication."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[2]


def validate_schema(value, name):
    schema = json.loads((ROOT / 'packaging/schemas' / f'{name}.schema.json').read_text())
    Draft202012Validator.check_schema(schema)
    errors = sorted(Draft202012Validator(schema).iter_errors(value), key=lambda e: str(e.path))
    if errors:
        error = errors[0]
        raise ValueError(f'{name}: {list(error.path)}: {error.message}')


def unique(values, label):
    if len(values) != len(set(values)):
        raise ValueError(f'duplicate {label}')


def relative(value):
    path = PurePosixPath(value)
    if (not value or path.is_absolute() or '\\' in value or
            any(part in ('', '.', '..') for part in value.split('/'))):
        raise ValueError('inventory path must be normalized and relative')
    return path


def validate_capabilities(value):
    validate_schema(value, 'capability-evidence')
    ids = [item['id'] for item in value['commands']]
    unique(ids, 'command ID')
    if set(ids) != {f'CMD-{n:03}' for n in range(1, 85)}:
        raise ValueError('expected exact CMD-001 through CMD-084 coverage')
    sources = [item['path'] for item in value['sources']]
    unique(sources, 'source')
    for source in sources:
        relative(source)
    for item in value['commands']:
        relative(item['clientPath'])
        if not set(item['sources']).issubset(sources):
            raise ValueError('command references an unreviewed source')
        if item['result'] != 'source-reviewed':
            raise ValueError('source-only receipt cannot claim conformance results')


def validate_install(value):
    validate_schema(value, 'install')
    files = {item['path']: item for item in value['files']}
    unique([item['path'] for item in value['files']], 'installed file')
    for path in files:
        relative(path)
    for name in ('gs', 'gitstored'):
        executable = value['executables'][name]
        if executable['version'] != value['applicationVersion']:
            raise ValueError('installed executable versions disagree')
        entry = files.get(f'bin/{name}')
        if not entry or entry['mode'] != 0o755 or entry['sha256'] != executable['sha256']:
            raise ValueError('missing or mismatched executable inventory')
    for key in ('notices', 'reference', 'completions'):
        for path in value[key]:
            if path not in files or not path.startswith('share/'):
                raise ValueError(f'missing {key} file')
    if value['os'] == 'darwin' and value['runtime'] != 'control-only':
        raise ValueError('macOS native runtime is unqualified')


def snapshot_file(root, name):
    """Hash a single-link file without following any path component symlinks."""
    parts = relative(name).parts
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
    directory = os.open(root, flags | os.O_DIRECTORY)
    try:
        for part in parts[:-1]:
            child = os.open(part, flags | os.O_DIRECTORY, dir_fd=directory)
            os.close(directory)
            directory = child
        fd = os.open(parts[-1], flags, dir_fd=directory)
        with os.fdopen(fd, 'rb') as stream:
            before = os.fstat(stream.fileno())
            if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                    before.st_mode & 0o022 or before.st_uid not in (0, os.geteuid())):
                raise ValueError('unsafe inventory file identity or mode')
            if not 0 < before.st_size <= 1024 * 1024 * 1024:
                raise ValueError('inventory file size outside bounds')
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            after = os.fstat(stream.fileno())
            if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                    after.st_size, after.st_mtime_ns, after.st_ctime_ns):
                raise ValueError('inventory file changed while hashing')
            return {'path': name, 'sha256': digest, 'bytes': before.st_size,
                    'mode': stat.S_IMODE(before.st_mode)}
    finally:
        os.close(directory)


def validate_tree(value, root):
    validate_install(value)
    if not Path(root).is_absolute():
        raise ValueError('select an absolute staging root')
    for expected in value['files']:
        if snapshot_file(root, expected['path']) != expected:
            raise ValueError(f'inventory content mismatch: {expected["path"]}')


def validate_release(value):
    validate_schema(value, 'release')
    if value['tag'] != 'v' + value['version']:
        raise ValueError('tag/version mismatch')
    required = value['requiredArtifacts']
    unique(required, 'required artifact')
    names = [item['filename'] for item in value['artifacts']]
    unique(names, 'artifact name')
    for item in value['artifacts']:
        if (item['applicationVersion'] != value['version'] or
                item['sourceRevision'] != value['sourceRevision']):
            raise ValueError('artifact source/version mismatch')
    states = ['proposed', 'version-reviewed', 'built', 'packaged', 'validated',
              'attested', 'producer-verified', 'published']
    phase = states.index(value['state'])
    if phase >= states.index('packaged') and set(names) != set(required):
        raise ValueError('incomplete or unexpected artifact set')
    if phase >= states.index('validated'):
        if not value['receipts'] or any(not item['receipt'] for item in value['artifacts']):
            raise ValueError('missing qualification receipts')
    if phase >= states.index('attested') and any(not item['bundle'] for item in value['artifacts']):
        raise ValueError('missing attestation bundle')
    # Structural validation is not a cryptographic verification or promotion receipt.
