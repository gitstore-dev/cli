"""Relocate exact archive payloads in an owned private fixture, without installers."""
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile

from homebrew_lifecycle import checked_archive
from inventory import validate_tree
from versions import order_key


def unpack(artifact, digest, version, destination):
    inventory = checked_archive(artifact, digest, version)
    payload = artifact.read_bytes()
    if hashlib.sha256(payload).hexdigest() != digest:
        raise ValueError('archive changed before extraction')
    destination.mkdir(mode=0o700)
    # checked_archive permits only unique, bounded ordinary relative files.
    # Read the checked bytes directly; never follow archive links or use extractall.
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as archive:
        for entry in archive.getmembers():
            target = destination / entry.name
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('xb') as output:
                shutil.copyfileobj(archive.extractfile(entry), output)
            target.chmod(entry.mode)
    validate_tree(inventory, destination)


def run(args):
    parent = args.fixture_parent
    if (os.geteuid() == 0 or not parent.is_absolute() or parent.is_symlink()
            or parent.stat().st_uid != os.geteuid() or parent.stat().st_mode & 0o077):
        raise ValueError('select a non-root producer and owned private fixture parent')
    selections = [(args.artifact, args.sha256, args.version)]
    if args.replacement:
        if order_key(args.version) >= order_key(args.replacement_version):
            raise ValueError('archive replacement must advance the prerelease')
        selections.append((args.replacement, args.replacement_sha256, args.replacement_version))
    for artifact, digest, version in selections:
        checked_archive(artifact, digest, version)
    root = Path(tempfile.mkdtemp(prefix='archive-lifecycle-', dir=parent)).resolve()
    root.chmod(0o700)
    env = {'PATH': '/nonexistent', 'LANG': 'C', 'GIT_CONFIG_NOSYSTEM': '1'}
    for name, variable in [('home','HOME'), ('config','XDG_CONFIG_HOME'),
            ('state','XDG_STATE_HOME'), ('cache','XDG_CACHE_HOME'), ('runtime','XDG_RUNTIME_DIR')]:
        (root/name).mkdir(mode=0o700)
        env[variable] = str(root/name)
    config = root/'config/config.toml'
    config.touch(mode=0o600)
    env['GITSTORE_CONFIG'] = str(config)
    receipt = dict(channel='archive', fixture=str(root), host=platform.platform(), artifacts=[],
        qualification='controlled archive lifecycle only; not release qualification')
    prefix = root/'prefix with spaces'
    for artifact, digest, version in selections:
        if prefix.exists():
            shutil.rmtree(prefix)
        unpack(artifact, digest, version, prefix)
        # Move the complete pair again to exercise relocation without loader overrides.
        relocated = root/'relocated prefix with spaces'
        prefix.rename(relocated)
        before = {str(p.relative_to(root)) for name in ('config','state','runtime') for p in (root/name).rglob('*')}
        for binary in ('gs','gitstored'):
            for flag in ('--help','--version'):
                reply = subprocess.run([str(relocated/'bin'/binary), flag], cwd=root,
                    env=env, capture_output=True, check=True, timeout=15)
                if reply.stderr or not reply.stdout or flag == '--version' and version.encode() not in reply.stdout:
                    raise ValueError('relocated archive command identity/help failed')
        if sys.platform == 'darwin':
            reply = subprocess.run([str(relocated/'bin/gs'), 'deployment','list','-o','json'],
                cwd=root, env=env, capture_output=True, timeout=15)
            if reply.returncode != 6 or reply.stdout:
                raise ValueError('macOS archive must refuse unavailable runtime explicitly')
        after = {str(p.relative_to(root)) for name in ('config','state','runtime') for p in (root/name).rglob('*')}
        if before != after:
            raise ValueError('archive help/version initialized runtime state')
        relocated.rename(prefix)
        receipt['artifacts'].append(dict(sha256=digest, applicationVersion=version))
    shutil.rmtree(prefix)
    receipt['removed'] = not prefix.exists()
    target = root/'receipt.json'
    target.write_text(json.dumps(receipt, indent=2)+'\n')
    target.chmod(0o600)
    print(json.dumps(receipt))
