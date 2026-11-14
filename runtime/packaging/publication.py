"""Idempotent draft promotion over an explicit backend; never replaces assets."""
import hashlib
from pathlib import Path

from inventory import snapshot_file
from release_candidate import DISCLAIMER, validate_bytes, safe_name, public_artifacts


def publish(value, root, backend, *, update_tap=False):
    if value['state'] != 'producer-verified':
        raise ValueError('only producer-verified candidates may be promoted')
    validate_bytes(value, root)
    assets = {}
    for artifact in public_artifacts(value):
        for name in (artifact['filename'], safe_name(artifact['bundle'])):
            snapshot_file(root, name)
            assets[name] = (Path(root) / name).read_bytes()
    state = backend.inspect()
    if (state['tag'], state['revision'], state['prerelease']) != (
            value['tag'], value['sourceRevision'], True):
        raise ValueError('remote release tag/source/prerelease identity conflict')
    existing = state['assets']
    if set(existing) - set(assets) or any(assets[n] != payload for n, payload in existing.items()):
        raise ValueError('remote assets conflict; never clobber or delete them')
    if not state['draft'] and set(existing) != set(assets):
        raise ValueError('published release is incomplete; manual recovery required')
    if not state['body'].startswith(DISCLAIMER + '\n\n'):
        if not state['draft']:
            raise ValueError('published release lacks the mandatory prerelease notice; manual correction required')
        backend.ensure_notice(DISCLAIMER)
    for name in sorted(set(assets) - set(existing)):
        backend.upload(name, assets[name])
    observed = backend.inspect()
    if (observed['tag'], observed['revision'], observed['prerelease']) != (
            value['tag'], value['sourceRevision'], True) or observed['assets'] != assets:
        raise ValueError('uploaded draft does not exactly match the verified candidate')
    if not observed['body'].startswith(DISCLAIMER + '\n\n'):
        raise ValueError('draft release notes lack the required no-guarantees/backup notice')
    if state['draft']:
        backend.promote()
    observed = backend.inspect()
    if observed['draft'] or observed['assets'] != assets or not observed['body'].startswith(DISCLAIMER + '\n\n'):
        raise ValueError('release promotion was not observed')
    tap = backend.update_tap(value['tag'], assets['gitstore.rb']) if update_tap else None
    return dict(tag=value['tag'], revision=value['sourceRevision'], published=True, tap=tap,
                assets={n: hashlib.sha256(b).hexdigest() for n, b in assets.items()})
