#!/usr/bin/env python3
"""Install/replace/remove explicitly selected native package candidates.

These controlled lanes do not qualify native Linux workloads.
All artifacts and fixture roots must be explicitly selected; no registry access.
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
import tarfile
import tempfile

from support import macos_runtime_boundary, owned_file


def inspect(path, digest, version):
    owned_file(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError('selected npm artifact digest changed')
    with tarfile.open(path, 'r:gz') as archive:
        entries = archive.getmembers()
        if sum(entry.size for entry in entries) > 512 * 1024 * 1024:
            raise ValueError('package exceeds bounded size')
        names = [entry.name for entry in entries]
        if len(names) != len(set(names)):
            raise ValueError('duplicate package paths')
        for entry in entries:
            relative = Path(entry.name)
            if not entry.isfile() or relative.is_absolute() or '..' in relative.parts or relative.parts[0] != 'package':
                raise ValueError('unexpected npm member identity')
        metadata = json.load(archive.extractfile('package/package.json'))
        os_name = 'darwin' if sys.platform == 'darwin' else 'linux'
        arch = {'arm64':'arm64','aarch64':'arm64','x86_64':'x64'}.get(platform.machine())
        name = f'@gitstore/cli-{os_name}-{arch}'
        if (metadata['name'] != name or metadata['version'] != version or
                metadata['os'] != [os_name] or metadata['cpu'] != [arch] or
                metadata['bin'] != {'gs':'bin/gs','gitstored':'bin/gitstored'} or
                metadata.get('scripts') or metadata.get('dependencies') or
                metadata.get('optionalDependencies') or metadata.get('peerDependencies')):
            raise ValueError('unexpected platform, version, bin map or registry/lifecycle dependency')
        for binary in ['gs', 'gitstored']:
            if archive.getmember('package/bin/' + binary).mode != 0o755:
                raise ValueError('ordinary executable mode required')
    return name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--channel', choices=['archive','npm','deb','rpm','homebrew'], required=True)
    parser.add_argument('--system-fixture', type=Path)
    parser.add_argument('--formula', type=Path)
    parser.add_argument('--formula-sha256')
    parser.add_argument('--replacement-formula', type=Path)
    parser.add_argument('--replacement-formula-sha256')
    parser.add_argument('--brew-source', type=Path)
    parser.add_argument('--brew-revision')
    parser.add_argument('--artifact', type=Path, required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--fixture-parent', type=Path, required=True)
    parser.add_argument('--replacement', type=Path)
    parser.add_argument('--replacement-sha256')
    parser.add_argument('--replacement-version')
    args = parser.parse_args()
    if bool(args.replacement) != bool(args.replacement_sha256) or bool(args.replacement) != bool(args.replacement_version):
        parser.error('replacement requires all three exact identity arguments')
    if args.channel == 'archive':
        from archive_lifecycle import run
        return run(args)
    if args.channel == 'homebrew':
        from homebrew_lifecycle import run
        return run(args)
    if args.channel in ('deb','rpm'):
        from system_packages import run
        return run(args)
    parent = args.fixture_parent
    if (not parent.is_absolute() or parent.is_symlink() or parent.stat().st_uid != os.geteuid()
            or parent.stat().st_mode & 0o077):
        parser.error('select the owned mode-0700 fixture parent')
    if bool(args.replacement) != bool(args.replacement_sha256) or bool(args.replacement) != bool(args.replacement_version):
        parser.error('replacement requires all three exact identity arguments')
    selections = [(args.artifact, args.sha256, args.version)]
    if args.replacement:
        selections.append((args.replacement, args.replacement_sha256, args.replacement_version))
    names = [inspect(*selection) for selection in selections]
    if len(set(names)) != 1:
        parser.error('replacement must select the same scoped platform package')
    npm, node = shutil.which('npm'), shutil.which('node')
    if not npm or not node:
        parser.error('select a fixture with Node/npm; installed binaries do not require Node')
    root = Path(tempfile.mkdtemp(prefix='npm-lifecycle-', dir=parent)).resolve()
    root.chmod(0o700)
    for directory in ['home','config','state','cache','runtime','prefix with spaces','npm-cache']:
        (root / directory).mkdir(mode=0o700)
    empty = root / 'empty-npmrc'
    empty.touch(mode=0o600)
    config = root / 'config/config.toml'
    config.touch(mode=0o600)
    env = {'HOME':str(root / 'home'), 'XDG_CONFIG_HOME':str(root / 'config'),
        'XDG_STATE_HOME':str(root / 'state'), 'XDG_CACHE_HOME':str(root / 'cache'),
        'XDG_RUNTIME_DIR':str(root / 'runtime'), 'GITSTORE_CONFIG':str(config),
        'NPM_CONFIG_CACHE':str(root / 'npm-cache'), 'NPM_CONFIG_PREFIX':str(root / 'prefix with spaces'),
        'NPM_CONFIG_USERCONFIG':str(empty), 'NPM_CONFIG_GLOBALCONFIG':str(root / 'global-npmrc'),
        'NPM_CONFIG_OFFLINE':'true', 'NPM_CONFIG_UPDATE_NOTIFIER':'false',
        'PATH':str(Path(node).parent)+':/usr/bin:/bin', 'LANG':'C'}
    receipt = {'channel':'npm','fixture':str(root),'qualification':'controlled package lifecycle only',
        'host':platform.platform(),'artifacts':[],'consumerVerifier':'absent from command PATH'}
    print(json.dumps({'fixture':str(root),'phase':'selected'}), flush=True)
    for artifact, digest, version in selections:
        subprocess.run([npm,'install','--global','--ignore-scripts','--offline','--no-audit','--no-fund',str(artifact)],
            # npm chmods command targets from its process umask, independently
            # of the archive mode. Keep a permissive hosted-account umask from
            # turning the private fixture's executables group writable.
            check=True, env=env, cwd=root, timeout=120, umask=0o022)
        binary_dir = root / 'prefix with spaces/bin'
        siblings = []
        before = {str(path.relative_to(root)) for directory in ['config','state','cache','runtime'] for path in (root / directory).rglob('*')}
        for binary in ['gs','gitstored']:
            link = binary_dir / binary
            if not link.is_symlink():
                raise ValueError('expected npm public command link')
            real = link.resolve(strict=True)
            owned_file(real, executable=True)
            siblings.append(real.parent)
            command_env = {**env,'PATH':'/nonexistent'}
            for flag in ['--help','--version']:
                reply = subprocess.run([str(link),flag], check=True, env=command_env, cwd=root,
                    capture_output=True, timeout=15)
                if reply.stderr or not reply.stdout or flag == '--version' and version.encode() not in reply.stdout:
                    raise ValueError('installed executable identity/help failed')
        if siblings[0] != siblings[1]:
            raise ValueError('real executables must remain siblings')
        if sys.platform == 'darwin':
            macos_runtime_boundary(binary_dir/'gs', {**env,'PATH':'/nonexistent'}, root)
        after = {str(path.relative_to(root)) for directory in ['config','state','cache','runtime'] for path in (root / directory).rglob('*')}
        if before != after:
            raise ValueError('help/version created configuration or runtime state')
        receipt['artifacts'].append({'sha256':digest,'applicationVersion':version})
    subprocess.run([npm,'uninstall','--global','--ignore-scripts','--offline','--no-audit','--no-fund',names[0]],
        check=True, env=env, cwd=root, timeout=120)
    if any((root / 'prefix with spaces/bin' / name).exists() or (root / 'prefix with spaces/bin' / name).is_symlink() for name in ['gs','gitstored']):
        raise ValueError('npm removal left public command links')
    if any((directory / name).exists() for directory in siblings for name in ['gs','gitstored']):
        raise ValueError('npm removal left native payloads')
    receipt['removed'] = True
    (root / 'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
