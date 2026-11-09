"""APT/DNF lifecycle checks, restricted to an explicitly selected disposable host.

Contributor fixture tooling only. Privileged installation owns code; all product
invocations run as the dedicated non-root account with isolated private state.
"""
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import socket
import stat
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
from inventory import validate_tree
from versions import encode, order_key


def command(*args, **kwargs):
    return subprocess.run(args, capture_output=True, check=True, timeout=180,
                          env={'PATH':'/usr/sbin:/usr/bin:/sbin:/bin', 'LANG':'C',
                               'DEBIAN_FRONTEND':'noninteractive'}, **kwargs)


def fixture(path, parent, channel):
    if sys.platform != 'linux' or os.geteuid() != 0:
        raise ValueError('system package lifecycle requires root inside the selected disposable Linux fixture')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_mode & 0o077 or not 0 < info.st_size <= 65536:
            raise ValueError('select a private ordinary fixture manifest')
        value = json.loads(os.read(fd, 65537))
    finally:
        os.close(fd)
    uid, gid = value['disposableUid'], value['disposableGid']
    if (type(uid) is not int or type(gid) is not int or uid <= 0 or gid <= 0 or
            info.st_uid not in (0, uid) or value['schemaVersion'] != 1 or
            value['dedicatedTestAccount'] is not True or value['disposableHost'] is not True or
            value['hostname'] != socket.gethostname()):
        raise ValueError('fixture account/disposable-host identity mismatch')
    release = platform.freedesktop_os_release()
    if (release['ID'], release['VERSION_ID']) != (value['osId'], value['osVersion']):
        raise ValueError('fixture OS identity mismatch')
    if (channel == 'deb' and (release['ID'], release['VERSION_ID']) != ('ubuntu','24.04') or
            channel == 'rpm' and (release['ID'], release['VERSION_ID']) != ('fedora','44')):
        raise ValueError('system package lifecycle requires the exact selected distribution version')
    arch = {'aarch64':'arm64', 'x86_64':'x64'}.get(platform.machine())
    if arch is None or value['arch'] != arch or str(parent) != value['fixtureParent']:
        raise ValueError('fixture architecture/private parent mismatch')
    current = Path(parent.anchor)
    for part in parent.parts[1:]:
        current /= part
        if current.is_symlink():
            raise ValueError('fixture parent must not follow links')
    info = parent.stat()
    if not parent.is_absolute() or not parent.is_dir() or info.st_uid != uid or info.st_mode & 0o077:
        raise ValueError('fixture parent must belong to the dedicated user with mode 0700')
    return uid, gid, arch


def allowed_path(name):
    return name in ('usr', 'usr/bin', 'usr/bin/gs', 'usr/bin/gitstored', 'usr/lib',
                    'usr/lib/systemd', 'usr/lib/systemd/user', 'usr/lib/systemd/user/gitstored.service',
                    'usr/libexec', 'usr/libexec/gitstore') or name.startswith('usr/libexec/gitstore/')


def validate_rpm_metadata(fields, scripts, triggers, listing, version, arch):
    """Shared pre-install policy for local and explicitly isolated Docker fixtures."""
    identity = encode(version)
    if fields != ['gitstore', identity['rpmVersion'], identity['rpmRelease'], 'x86_64' if arch == 'x64' else 'aarch64']:
        raise ValueError('RPM package version/platform/name mismatch')
    if scripts or triggers:
        raise ValueError('system installation must not run package scripts/triggers')
    for line in listing.splitlines():
        name, link, mode = line.split('\t')
        mode = int(mode, 8)
        if (not allowed_path(name.removeprefix('/')) or '..' in name.split('/') or
                not stat.S_ISLNK(mode) and mode & 0o7022 or
                not (stat.S_ISREG(mode) or stat.S_ISDIR(mode) or stat.S_ISLNK(mode))):
            raise ValueError('unexpected privileged RPM payload')
        if link and (name not in ('/usr/bin/gs', '/usr/bin/gitstored') or
                link != '../libexec/gitstore/bin/' + name.rsplit('/',1)[1]):
            raise ValueError('unexpected privileged RPM link')


def inspect(path, digest, version, channel, arch, uid):
    if not path.is_absolute() or path.is_symlink():
        raise ValueError('select an absolute ordinary package')
    info = path.stat()
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid not in (0, uid)
            or info.st_mode & 0o022 or not 0 < info.st_size <= 512 * 1024 * 1024
            or hashlib.sha256(path.read_bytes()).hexdigest() != digest):
        raise ValueError('system package identity mismatch')
    identity = encode(version)
    if channel == 'deb':
        fields = command('dpkg-deb', '--show', '--showformat=${Package}\n${Version}\n${Architecture}', str(path)).stdout.decode().splitlines()
        if fields != ['gitstore', identity['debian'], 'amd64' if arch == 'x64' else 'arm64']:
            raise ValueError('Debian package version/platform/name mismatch')
        controls = command('dpkg-deb', '--ctrl-tarfile', str(path)).stdout
        with tarfile.open(fileobj=io.BytesIO(controls)) as archive:
            if any(entry.isfile() and entry.name.removeprefix('./') not in ('control', 'md5sums') for entry in archive):
                raise ValueError('system installation must not run package scripts')
        payload = command('dpkg-deb', '--fsys-tarfile', str(path)).stdout
        with tarfile.open(fileobj=io.BytesIO(payload)) as archive:
            for entry in archive:
                name = entry.name.removeprefix('./').rstrip('/')
                if not name or name == '.':
                    continue
                if not allowed_path(name) or '..' in name.split('/') or entry.islnk() or not entry.issym() and entry.mode & 0o7022:
                    raise ValueError('unexpected privileged package payload')
                if entry.issym() and (name not in ('usr/bin/gs', 'usr/bin/gitstored') or
                        entry.linkname != '../libexec/gitstore/bin/' + name.rsplit('/',1)[1]):
                    raise ValueError('unexpected privileged package link')
                if not (entry.isfile() or entry.isdir() or entry.issym()):
                    raise ValueError('unexpected special package file')
    else:
        fields = command('rpm', '-qp', '--qf', '%{NAME}\n%{VERSION}\n%{RELEASE}\n%{ARCH}', str(path)).stdout.decode().splitlines()
        validate_rpm_metadata(fields,
            command('rpm', '-qp', '--scripts', str(path)).stdout,
            command('rpm', '-qp', '--triggers', str(path)).stdout,
            command('rpm', '-qp', '--qf', '[%{FILENAMES}\t%{FILELINKTOS}\t%{FILEMODES:octal}\n]', str(path)).stdout.decode(),
            version, arch)


def run(args):
    if args.system_fixture is None:
        raise ValueError('select an explicit disposable system fixture manifest')
    uid, gid, arch = fixture(args.system_fixture, args.fixture_parent, args.channel)
    selections = [(args.artifact, args.sha256, args.version)]
    if args.replacement:
        selections.append((args.replacement, args.replacement_sha256, args.replacement_version))
        if order_key(args.version) >= order_key(args.replacement_version):
            raise ValueError('replacement fixture must advance the prerelease')
    pinned = tempfile.TemporaryDirectory(prefix='gitstore-system-package-inputs-')
    pinned_selections = []
    for index, (source, digest, version) in enumerate(selections):
        if not source.is_absolute():
            raise ValueError('select an absolute package path')
        fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        with os.fdopen(fd, 'rb') as stream:
            before = os.fstat(stream.fileno())
            if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                    before.st_uid not in (0, uid) or before.st_mode & 0o022 or
                    not 0 < before.st_size <= 512 * 1024 * 1024):
                raise ValueError('unsafe selected package file')
            payload = stream.read(512 * 1024 * 1024 + 1)
            after = os.fstat(stream.fileno())
        if ((before.st_size, before.st_mtime_ns, before.st_ctime_ns) !=
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns) or
                len(payload) != before.st_size or hashlib.sha256(payload).hexdigest() != digest):
            raise ValueError('package changed while pinning installation input')
        destination = Path(pinned.name) / (str(index) + '.' + args.channel)
        destination.write_bytes(payload)
        pinned_selections.append((destination, digest, version))
    selections = pinned_selections
    for selection in selections:
        inspect(*selection, args.channel, arch, uid)
    paths = [Path('/usr/bin/gs'), Path('/usr/bin/gitstored'), Path('/usr/libexec/gitstore')]
    if any(path.exists() or path.is_symlink() for path in paths):
        raise ValueError('fixture already contains GitStore code; refusing to replace unrelated installation')
    root = Path(tempfile.mkdtemp(prefix='system-package-', dir=args.fixture_parent))
    os.chown(root, uid, gid)
    env = {'PATH':'/nonexistent', 'LANG':'C'}
    for name, variable in [('home','HOME'),('config','XDG_CONFIG_HOME'),('state','XDG_STATE_HOME'),('cache','XDG_CACHE_HOME'),('runtime','XDG_RUNTIME_DIR')]:
        path = root / name
        path.mkdir(mode=0o700)
        os.chown(path, uid, gid)
        env[variable] = str(path)
    config = root / 'config/config.toml'
    config.touch(mode=0o600)
    os.chown(config, uid, gid)
    env['GITSTORE_CONFIG'] = str(config)
    receipt = dict(channel=args.channel, fixture=str(root), host=platform.platform(), artifacts=[],
        qualification='controlled system package lifecycle only; not release qualification')
    installed = False
    try:
        for artifact, digest, version in selections:
            if args.channel == 'deb':
                command('apt-get', '-y', '--no-install-recommends', 'install', str(artifact))
            else:
                command('dnf', '-y', '--setopt=install_weak_deps=False', 'install', str(artifact))
            installed = True
            prefix = Path('/usr/libexec/gitstore')
            inventory = json.loads((prefix / 'share/gitstore/install.json').read_text())
            validate_tree(inventory, prefix)
            if inventory['applicationVersion'] != version:
                raise ValueError('installed application inventory version mismatch')
            before = sorted(str(p.relative_to(root)) for p in root.rglob('*'))
            for name in ('gs', 'gitstored'):
                link = Path('/usr/bin') / name
                if not link.is_symlink() or link.resolve(strict=True) != prefix / 'bin' / name:
                    raise ValueError('installed command link does not select the private sibling pair')
                for flag in ('--help','--version'):
                    result = subprocess.run([str(link), flag], user=uid, group=gid, extra_groups=[],
                        env=env, cwd=root, capture_output=True, check=True, timeout=15)
                    if result.stderr or not result.stdout or flag == '--version' and version.encode() not in result.stdout:
                        raise ValueError('installed non-root command identity/help mismatch')
            if before != sorted(str(p.relative_to(root)) for p in root.rglob('*')):
                raise ValueError('system installation/help/version initialized user state')
            receipt['artifacts'].append(dict(sha256=digest, applicationVersion=version))
    finally:
        if installed:
            if args.channel == 'deb':
                command('apt-get', '-y', 'remove', 'gitstore')
            else:
                command('dnf', '-y', 'remove', 'gitstore')
    if any(path.exists() or path.is_symlink() for path in paths):
        raise ValueError('system removal left command code/links')
    receipt['removed'] = True
    destination = root / 'receipt.json'
    destination.write_text(json.dumps(receipt, indent=2) + '\n')
    destination.chmod(0o600)
    os.chown(destination, uid, gid)
    print(json.dumps(receipt))
