"""Homebrew lifecycle in a new private checkout/prefix; never touches the user's tap/Cellar."""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import shutil
import subprocess
import sys
import tarfile
import tempfile

from support import owned_file
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
from homebrew import render
from inventory import relative, validate_install, validate_tree
from versions import order_key


def checked_formula(path, digest, version, archive_digest):
    owned_file(path)
    payload = path.read_bytes()
    if hashlib.sha256(payload).hexdigest() != digest:
        raise ValueError('selected formula digest mismatch')
    text = payload.decode()
    pairs = re.findall(r'url "([^"]+)"\s+sha256 "([0-9a-f]{64})"', text)
    archives = []
    for url, sha256 in pairs:
        match = re.fullmatch(r'https://github.com/gitstore-dev/cli/releases/download/v' + re.escape(version) +
            '/gitstore-' + re.escape(version) + r'-(linux|darwin)-(x64|arm64)\.tar\.gz', url)
        if not match:
            raise ValueError('formula URL is not an exact canonical GitHub release asset')
        archives.append(dict(os=match[1], arch=match[2], url=url, sha256=sha256))
    if render(version, archives) != text:
        raise ValueError('formula differs from the reviewed binary-only template')
    os_name = 'darwin' if sys.platform == 'darwin' else 'linux'
    arch = {'aarch64':'arm64','arm64':'arm64','x86_64':'x64'}.get(platform.machine())
    selected = next(a for a in archives if (a['os'],a['arch']) == (os_name,arch))
    if selected['sha256'] != archive_digest:
        raise ValueError('formula does not select the exact local native archive')
    return payload


def checked_archive(path, digest, version):
    owned_file(path)
    if path.stat().st_size > 512 * 1024 * 1024 or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError('selected Homebrew archive digest mismatch')
    with tarfile.open(path, 'r:gz') as archive:
        members = archive.getmembers()
        if len({m.name for m in members}) != len(members) or sum(m.size for m in members) > 512 * 1024 * 1024:
            raise ValueError('duplicate or oversized Homebrew archive')
        for member in members:
            relative(member.name)
            if (not member.isfile() or member.name.startswith('/') or '..' in member.name.split('/') or
                    not (member.name in ('bin/gs','bin/gitstored') or member.name.startswith(('share/','lib/')))):
                raise ValueError('unexpected Homebrew archive path/type')
        value = json.load(archive.extractfile('share/gitstore/install.json'))
        validate_install(value)
        expected = {entry['path']: entry for entry in value['files']}
        if set(expected) | {'share/gitstore/install.json'} != {m.name for m in members}:
            raise ValueError('archive differs from the exact install inventory')
        for member in members:
            if member.name == 'share/gitstore/install.json':
                if member.mode != 0o644:
                    raise ValueError('install metadata must have ordinary read-only publication mode')
                continue
            entry = expected[member.name]
            if (member.mode != entry['mode'] or member.size != entry['bytes'] or
                    hashlib.sha256(archive.extractfile(member).read()).hexdigest() != entry['sha256']):
                raise ValueError('archive content/mode differs from the install inventory')
    os_name = 'darwin' if sys.platform == 'darwin' else 'linux'
    arch = {'aarch64':'arm64','arm64':'arm64','x86_64':'x64'}.get(platform.machine())
    if (value['applicationVersion'], value['os'], value['arch']) != (version,os_name,arch):
        raise ValueError('Homebrew archive version/platform mismatch')
    return value


def run(args):
    if os.geteuid() == 0 or args.brew_source is None or args.brew_revision is None:
        raise ValueError('select a non-root producer and explicit local Homebrew checkout/revision')
    if not re.fullmatch('[0-9a-f]{40}', args.brew_revision):
        raise ValueError('select a full reviewed Homebrew tooling revision')
    selections = [(args.artifact,args.sha256,args.version,args.formula,args.formula_sha256)]
    if args.replacement:
        selections.append((args.replacement,args.replacement_sha256,args.replacement_version,
                           args.replacement_formula,args.replacement_formula_sha256))
        if order_key(args.version) >= order_key(args.replacement_version):
            raise ValueError('Homebrew replacement must advance the prerelease')
    if any(formula is None or digest is None for _,_,_,formula,digest in selections):
        raise ValueError('each archive requires its explicitly selected formula and digest')
    for artifact,digest,version,formula,formula_digest in selections:
        checked_archive(artifact,digest,version)
        checked_formula(formula,formula_digest,version,digest)
    parent = args.fixture_parent
    if not parent.is_absolute() or parent.is_symlink() or parent.stat().st_uid != os.geteuid() or parent.stat().st_mode & 0o077:
        raise ValueError('select an owned private fixture parent')
    if sys.platform == 'darwin' and any(parent.resolve().is_relative_to(Path(p)) for p in (
            '/private/tmp','/private/var/tmp','/private/var/folders')):
        raise ValueError('macOS Homebrew fixture prefix must be outside system temporary directories; select a private ignored build directory')
    root = Path(tempfile.mkdtemp(prefix='brew-lifecycle-', dir=parent)).resolve()
    root.chmod(0o700)
    for name in ('home','cache','logs','config','state','runtime','temp','empty-git-template'):
        (root/name).mkdir(mode=0o700)
    config = root/'config/config.toml'
    config.touch(mode=0o600)
    env = dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin', HOME=str(root/'home'), LANG='C',
        XDG_CONFIG_HOME=str(root/'config'), XDG_STATE_HOME=str(root/'state'), XDG_RUNTIME_DIR=str(root/'runtime'),
        GITSTORE_CONFIG=str(config), GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null',
        GIT_TEMPLATE_DIR=str(root/'empty-git-template'), HOMEBREW_CACHE=str(root/'cache'),
        HOMEBREW_LOGS=str(root/'logs'), HOMEBREW_NO_AUTO_UPDATE='1', HOMEBREW_NO_ANALYTICS='1',
        HOMEBREW_TEMP=str(root/'temp'), TMPDIR=str(root/'temp'),
        HOMEBREW_NO_INSTALL_CLEANUP='1', HOMEBREW_NO_ENV_HINTS='1')

    command_number = 0
    def command(*arguments):
        nonlocal command_number
        command_number += 1
        output_path = root/'logs'/f'command-{command_number:03}.stdout.log'
        error_path = root/'logs'/f'command-{command_number:03}.stderr.log'
        with output_path.open('wb') as output, error_path.open('wb') as error:
            process = subprocess.Popen(arguments, env=env, cwd=root, stdin=subprocess.DEVNULL,
                stdout=output, stderr=error, start_new_session=True)
            try:
                process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
                raise RuntimeError('owned Homebrew command timed out; inspect ' + str(error_path)) from None
        if process.returncode:
            raise RuntimeError('owned Homebrew fixture command failed; inspect ' + str(error_path))
        if output_path.stat().st_size > 4 * 1024 * 1024:
            raise ValueError('Homebrew command output exceeded fixture bounds')
        return output_path.read_text().strip()

    source = args.brew_source.resolve(strict=True)
    if command('git','-C',str(source),'rev-parse','HEAD') != args.brew_revision:
        raise ValueError('local Homebrew source does not match the selected revision')
    command('git','-C',str(source),'diff','--quiet','HEAD','--')
    prefix = root/'prefix'
    command('git','clone','--local','--shared','--no-checkout',str(source),str(prefix))
    command('git','-C',str(prefix),'checkout','--detach',args.brew_revision)
    # Copy only local build-tool runtime, never installed formulae or taps. All
    # user configuration, Cellar, credentials, global cache and Git hooks stay out.
    for name in ('portable-ruby','bundle','gems'):
        vendor = source/'Library/Homebrew/vendor'/name
        if vendor.is_dir():
            target = prefix/'Library/Homebrew/vendor'/name
            if target.is_symlink():
                target.unlink()
            elif target.exists():
                shutil.rmtree(target)
            shutil.copytree(vendor,target,symlinks=True)
    brew = str(prefix/'bin/brew')
    if command(brew,'--prefix') != str(prefix):
        raise ValueError('isolated Homebrew resolved a different prefix')
    if command(brew,'--cellar') != str(prefix/'Cellar') or command(brew,'--repository') != str(prefix):
        raise ValueError('isolated Homebrew selected a non-fixture repository/Cellar')
    tap = prefix/'Library/Taps/gitstore-fixture/homebrew-cli'
    (tap/'Formula').mkdir(parents=True)
    command('git','-C',str(tap),'init','-b','main')
    formula_name = 'gitstore-fixture/cli/gitstore'
    receipt = dict(channel='homebrew', fixture=str(root), toolingRevision=args.brew_revision,
        host=platform.platform(), qualification='controlled package lifecycle only; not release qualification',artifacts=[])
    print(json.dumps(dict(fixture=str(root), phase='selected')),flush=True)
    for index,(artifact,digest,version,formula,formula_digest) in enumerate(selections):
        payload = checked_formula(formula,formula_digest,version,digest)
        (tap/'Formula/gitstore.rb').write_bytes(payload)
        command('git','-C',str(tap),'add','Formula/gitstore.rb')
        command('git','-C',str(tap),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid',
                'commit','-m','Controlled package version fixture')
        tooling_version = command(brew,'--version')
        major = re.search(r'Homebrew ([0-9]+)\.', tooling_version)
        if major and int(major[1]) >= 7:
            # Homebrew 7's formula trust stays in this fixture's XDG config.
            # Trust only the exact binary-only template checked above.
            command(brew,'trust','--formula',formula_name)
        cached = Path(command(brew,'--cache',formula_name))
        if not cached.is_relative_to(root/'cache'):
            raise ValueError('Homebrew selected a non-fixture cache path')
        cached.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(artifact,cached)
        command(brew,'install' if index == 0 else 'upgrade','--formula',formula_name)
        private = (prefix/'opt/gitstore/libexec').resolve(strict=True)
        value = checked_archive(artifact,digest,version)
        validate_tree(value,private)
        before = sorted(str(p.relative_to(root)) for name in ('config','state','runtime') for p in (root/name).rglob('*'))
        for name in ('gs','gitstored'):
            public = prefix/'bin'/name
            if not public.is_symlink() or public.resolve(strict=True) != private/'bin'/name:
                raise ValueError('Homebrew link does not select the private sibling pair')
            for flag in ('--help','--version'):
                result = subprocess.run([str(public),flag],env={**env,'PATH':'/nonexistent'},
                    cwd=root,capture_output=True,check=True,timeout=15)
                if result.stderr or not result.stdout or flag == '--version' and version.encode() not in result.stdout:
                    raise ValueError('installed Homebrew command identity/help failed')
        if sys.platform == 'darwin':
            unavailable = subprocess.run([str(prefix/'bin/gs'),'deployment','list','-o','json'],
                env={**env,'PATH':'/nonexistent'},cwd=root,capture_output=True,timeout=15)
            if unavailable.returncode != 6 or unavailable.stdout:
                raise ValueError('macOS package must expose the explicit unavailable-runtime boundary')
        after = sorted(str(p.relative_to(root)) for name in ('config','state','runtime') for p in (root/name).rglob('*'))
        if before != after:
            raise ValueError('help/version initialized GitStore runtime state')
        receipt['artifacts'].append(dict(sha256=digest,formulaSha256=formula_digest,applicationVersion=version))
    command(brew,'uninstall','--force',formula_name)
    if any((prefix/'bin'/name).exists() or (prefix/'bin'/name).is_symlink() for name in ('gs','gitstored')) or (prefix/'Cellar/gitstore').exists():
        raise ValueError('Homebrew removal left selected command code/links')
    receipt['removed'] = True
    (root/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt))
