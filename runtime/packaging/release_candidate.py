"""Fail-closed local producer ledger. Structural receipts are not signatures."""
import copy
import hashlib
import json
from pathlib import Path
import re
import tarfile

from inventory import ROOT, snapshot_file, validate_install, validate_release
from versions import encode, parse
from command_measurements import validate_performance

STATES = ('proposed', 'version-reviewed', 'built', 'packaged', 'validated',
          'attested', 'producer-verified', 'published')
DISCLAIMER = ('GitStore and this CLI are early alpha. Upstream schemas continue changing '
              'incompatibly. No upgrade compatibility, data preservation or recovery is '
              'guaranteed. Use, upgrade, migration or removal may cause data loss. Keep '
              'independent backups; do not use prereleases for irreplaceable data.')
GENERATED = {'release-inventory.json', 'SHA256SUMS'}
SHARED_BUNDLE = 'release.sigstore.jsonl'


def bundle_name(value, filename):
    # Missing selection is the original schema-v1 layout, retained for immutable
    # alpha releases. Never infer a layout from files present on disk.
    return (SHARED_BUNDLE if value.get('attestationLayout', 'per-subject-v1') ==
            'shared-v1' else safe_name(filename + '.sigstore.jsonl'))


def public_artifacts(value):
    """Keep qualification identities in the ledger, select downloads explicitly."""
    if value.get('publicationLayout', 'all-inputs-v1') == 'all-inputs-v1':
        return value['artifacts']
    internal = {slot_filename(value['version'], kind, row['os'], row['arch'])
                for row in platforms() for kind in ('gs', 'gitstored', 'manifest')}
    return [a for a in value['artifacts'] if a['filename'] not in internal]


def platforms():
    return json.loads((ROOT / 'packaging/platforms.json').read_text())['rows']


def slots():
    result = []
    for row in platforms():
        for kind in ('archive', 'npm', 'manifest', 'gs', 'gitstored'):
            result.append((kind, row['os'], row['arch']))
        if row['os'] == 'linux':
            result.extend((kind, row['os'], row['arch']) for kind in ('deb', 'rpm'))
    return result + [('formula', 'any', 'any')]


def slot_filename(version, kind, os_name, arch):
    identity = encode(version)
    base = f'gitstore-{version}-{os_name}-{arch}'
    if kind == 'formula':
        return 'gitstore.rb'
    if kind in ('gs', 'gitstored'):
        return f'{kind}-{version}-{os_name}-{arch}'
    if kind == 'manifest':
        return base + '.install.json'
    if kind == 'npm':
        return f'gitstore-npm-{version}-{os_name}-{arch}.tgz'
    if kind == 'deb':
        return f'gitstore_{identity["debian"]}_{"amd64" if arch == "x64" else "arm64"}.deb'
    if kind == 'rpm':
        return f'gitstore-{identity["rpmVersion"]}-{identity["rpmRelease"]}.{ "x86_64" if arch == "x64" else "aarch64"}.rpm'
    return base + '.tar.gz'


def qualification_rows():
    result = set()
    for row in platforms():
        channels = row['fixtureChannels']
        if set(channels) != set(row['fixtures']) or set().union(*map(set, channels.values())) != set(row['channels']):
            raise ValueError('each selected host and channel requires an explicit fixture mapping')
        for host, selected in channels.items():
            if not selected or len(selected) != len(set(selected)):
                raise ValueError('empty or duplicate fixture channel mapping')
            result.update((row['os'],row['arch'],host,channel) for channel in selected)
    return result


def safe_name(name):
    if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.~-]*', name):
        raise ValueError('expected an ordinary release asset basename')
    return name


def propose(version, revision, extra_assets=()):
    if parse(version)[3] is None:
        raise ValueError('this release policy only permits explicit alpha prereleases')
    required = [slot_filename(version, *slot) for slot in slots()] + sorted(GENERATED)
    required += [safe_name(name) for name in extra_assets]
    value = dict(schemaVersion=1, attestationLayout='shared-v1', publicationLayout='packages-v1', version=version, tag='v' + version, sourceRevision=revision,
                 workflow='gitstore-dev/cli/.github/workflows/release.yml', state='proposed',
                 disclaimer=DISCLAIMER, requiredArtifacts=sorted(required), artifacts=[], receipts=[])
    validate_release(value)
    return value


def register(value, root, filename, kind, os_name, arch, *, install_inventory, unpacked_bytes):
    if value['state'] not in ('proposed', 'version-reviewed') or filename in GENERATED:
        raise ValueError('subjects must be registered before freezing the built ledger')
    safe_name(filename)
    if filename not in value['requiredArtifacts'] or any(a['filename'] == filename for a in value['artifacts']):
        raise ValueError('unexpected or duplicate subject')
    extra = kind in ('source', 'relink') and (os_name, arch) == ('any', 'any')
    if not extra and ((kind, os_name, arch) not in slots() or filename != slot_filename(value['version'], kind, os_name, arch)):
        raise ValueError('subject does not match its reviewed platform/format slot')
    identity = snapshot_file(root, filename)
    version = encode(value['version'])
    package_version = version['debian'] if kind == 'deb' else (
        version['rpmVersion'] + '-' + version['rpmRelease'] if kind == 'rpm' else value['version'])
    floor = next((r['abiFloor'] for r in platforms() if (r['os'], r['arch']) == (os_name, arch)), 'not executable')
    value['artifacts'].append(dict(filename=filename, format='binary' if kind in ('gs', 'gitstored') else kind,
        os=os_name, arch=arch, abiFloor=floor, packageVersion=package_version,
        applicationVersion=value['version'], sourceRevision=value['sourceRevision'],
        sha256=identity['sha256'], bytes=identity['bytes'], unpackedBytes=unpacked_bytes,
        installInventory=install_inventory, bundle=None, receipt=None))
    validate_release(value)


def validate_bytes(value, root):
    if not Path(root).is_absolute():
        raise ValueError('select an absolute asset root')
    validate_release(value)
    if value['disclaimer'] != DISCLAIMER:
        raise ValueError('mandatory prerelease disclaimer changed or absent')
    expected = {slot_filename(value['version'], *s) for s in slots()} | GENERATED
    if not expected.issubset(set(value['requiredArtifacts'])):
        raise ValueError('required release matrix was reduced')
    allowed = {slot_filename(value['version'], *s): s for s in slots()}
    for artifact in value['artifacts']:
        name = safe_name(artifact['filename'])
        actual = snapshot_file(root, name)
        if (actual['sha256'], actual['bytes']) != (artifact['sha256'], artifact['bytes']):
            raise ValueError('final subject bytes changed: ' + name)
        if name in allowed:
            kind, os_name, arch = allowed[name]
            if (artifact['format'], artifact['os'], artifact['arch']) != (
                    'binary' if kind in ('gs', 'gitstored') else kind, os_name, arch):
                raise ValueError('subject platform or format mismatch')
    required_now = expected - GENERATED if STATES.index(value['state']) < 3 else expected
    if STATES.index(value['state']) >= STATES.index('built') and not required_now.issubset({a['filename'] for a in value['artifacts']}):
        raise ValueError('incomplete required platform/format matrix')
    if STATES.index(value['state']) >= STATES.index('built'):
        by_name = {a['filename']: a for a in value['artifacts']}
        for row in platforms():
            os_name, arch = row['os'], row['arch']
            inventory_name = slot_filename(value['version'], 'manifest', os_name, arch)
            manifest = json.loads((Path(root) / inventory_name).read_bytes())
            validate_install(manifest)
            if (manifest['applicationVersion'], manifest['sourceRevision'], manifest['os'], manifest['arch']) != (
                    value['version'], value['sourceRevision'], os_name, arch):
                raise ValueError('install inventory source/platform/version mismatch')
            for name in ('gs', 'gitstored'):
                binary = by_name[slot_filename(value['version'], name, os_name, arch)]
                if manifest['executables'][name]['sha256'] != binary['sha256']:
                    raise ValueError('loose executable differs from the install inventory')
            if value.get('publicationLayout') == 'packages-v1':
                archive_name = slot_filename(value['version'], 'archive', os_name, arch)
                with tarfile.open(Path(root) / archive_name, 'r:gz') as archive:
                    members = archive.getmembers()
                    by_path = {m.name: m for m in members}
                    if len(by_path) != len(members):
                        raise ValueError('duplicate archive member')
                    for path, expected_hash in [('share/gitstore/install.json', None)] + [
                            ('bin/' + binary, manifest['executables'][binary]['sha256'])
                            for binary in ('gs', 'gitstored')]:
                        member = by_path.get(path)
                        if member is None or not member.isfile() or member.size > 1024 * 1024 * 1024:
                            raise ValueError('archive lacks qualified executable/inventory')
                        stream = archive.extractfile(member)
                        if expected_hash is None:
                            if member.size > 16 * 1024 * 1024 or json.load(stream) != manifest:
                                raise ValueError('archive install inventory differs from qualification input')
                        elif hashlib.file_digest(stream, 'sha256').hexdigest() != expected_hash:
                            raise ValueError('archive executable differs from qualification input')
            for artifact in value['artifacts']:
                if (artifact['os'], artifact['arch']) == (os_name, arch) and artifact['installInventory'] != inventory_name:
                    raise ValueError('platform subject references the wrong install inventory')


def write_new(path, payload):
    with Path(path).open('xb') as stream:
        stream.write(payload)


def freeze(value, root):
    """Inventory describes payloads; checksums include inventory, never themselves.

    The mutable producer ledger is not a signed subject, avoiding a self-hash or
    bundle/receipt reference cycle. Both immutable generated subjects are signed.
    """
    root = Path(root)
    if any((root / n).exists() or (root / n).is_symlink() for n in GENERATED):
        raise FileExistsError('generated subjects already exist; resume the saved packaged ledger')
    inventories = {}
    if value.get('publicationLayout') == 'packages-v1':
        inventories = {slot_filename(value['version'], 'manifest', r['os'], r['arch']):
                       json.loads((root / slot_filename(value['version'], 'manifest', r['os'], r['arch'])).read_bytes())
                       for r in platforms()}
    payload = json.dumps(dict(schemaVersion=1, tag=value['tag'], version=value['version'],
        sourceRevision=value['sourceRevision'], disclaimer=DISCLAIMER,
        **({'installationInventories': inventories} if inventories else {}),
        artifacts=sorted(public_artifacts(value), key=lambda a: a['filename'])), indent=2).encode() + b'\n'
    for name, kind in [('release-inventory.json', 'manifest'), ('SHA256SUMS', 'checksums')]:
        if kind == 'checksums':
            payload = ''.join(a['sha256'] + '  ' + a['filename'] + '\n'
                for a in sorted(public_artifacts(value), key=lambda a: a['filename'])).encode()
        write_new(root / name, payload)
        value['artifacts'].append(dict(filename=name, format=kind, os='any', arch='any',
            abiFloor='not executable', packageVersion=value['version'], applicationVersion=value['version'],
            sourceRevision=value['sourceRevision'], sha256=hashlib.sha256(payload).hexdigest(),
            bytes=len(payload), unpackedBytes=len(payload), installInventory='release-inventory.json',
            bundle=None, receipt=None))


def validate_receipts(value, root):
    required = qualification_rows()
    seen = set()
    subjects = {a['filename']: a['sha256'] for a in value['artifacts']}
    for name in value['receipts']:
        safe_name(name)
        entry = snapshot_file(root, name)
        if entry['bytes'] > 1024 * 1024:
            raise ValueError('qualification receipt is too large')
        receipt = json.loads((Path(root) / name).read_bytes())
        selection = tuple(receipt['selection'])
        if (receipt['schemaVersion'] != 1 or receipt['result'] != 'pass' or
                receipt['tag'] != value['tag'] or receipt['sourceRevision'] != value['sourceRevision'] or
                receipt['subjects'] != subjects or selection not in required or selection in seen):
            raise ValueError('qualification receipt does not bind this candidate/selection')
        performance = receipt.get('performance')
        if not isinstance(performance,dict):
            raise ValueError('qualification requires recorded reference-host performance')
        validate_performance(performance,selection[0] == 'linux')
        expected_inputs = {}
        for binary in ('gs','gitstored'):
            filename = slot_filename(value['version'],binary,selection[0],selection[1])
            artifact = next(a for a in value['artifacts'] if a['filename'] == filename)
            expected_inputs[binary] = {key:artifact[key] for key in ('sha256','bytes')}
        if performance.get('sourceInputs') != expected_inputs:
            raise ValueError('performance was measured on a different candidate executable pair')
        seen.add(selection)
    if seen != required:
        raise ValueError('missing selected host/channel qualification receipts')


def advance(value, target, root, *, reviewed_tag=None, reviewed_revision=None, trusted_root=None):
    value = copy.deepcopy(value)
    validate_bytes(value, root)
    if target not in STATES or STATES.index(target) != STATES.index(value['state']) + 1:
        raise ValueError('release states must advance exactly one step')
    if target == 'published':
        raise ValueError('publication requires observed remote promotion, not a local state edit')
    if target == 'version-reviewed' and (reviewed_tag, reviewed_revision) != (value['tag'], value['sourceRevision']):
        raise ValueError('explicit reviewed tag and full source revision must agree')
    if target == 'packaged':
        freeze(value, root)
    if STATES.index(target) >= STATES.index('validated'):
        validate_receipts(value, root)
        for artifact in value['artifacts']:
            artifact['receipt'] = ','.join(value['receipts'])
    if STATES.index(target) >= STATES.index('attested'):
        for artifact in public_artifacts(value):
            bundle = bundle_name(value, artifact['filename'])
            snapshot_file(root, bundle)
            artifact['bundle'] = bundle
    if target == 'producer-verified':
        if trusted_root is None:
            raise ValueError('select the reviewed GitHub/Sigstore trusted root')
        from release_verification import verify
        for artifact in public_artifacts(value):
            verify(Path(root) / artifact['filename'], Path(root) / artifact['bundle'],
                   trusted_root, 'refs/tags/' + value['tag'], value['sourceRevision'], artifact['sha256'])
    value['state'] = target
    validate_bytes(value, root)
    return value
