"""Owner-only, synthetic package fixtures. Never inherit a user's environment."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile


def macos_runtime_boundary(binary, environment, workspace):
    """Check ordinary macOS shells as well as explicitly configured fixtures."""
    for configured in (True, False):
        selected = dict(environment)
        if not configured:
            selected.pop('XDG_RUNTIME_DIR', None)
        reply = subprocess.run([str(binary), 'deployment', 'list', '-o', 'json'],
            cwd=workspace, env=selected, capture_output=True, timeout=15)
        if reply.returncode != 6 or reply.stdout:
            raise ValueError('macOS local runtime must report UNSUPPORTED_HOST with or without XDG_RUNTIME_DIR')
        error = json.loads(reply.stderr)
        if error.get('code') != 'UNSUPPORTED_HOST' or 'Linux' not in error.get('message', ''):
            raise ValueError('macOS local runtime diagnostic must explain the Linux requirement')


def owned_file(path, executable=False):
    path = Path(path)
    if not path.is_absolute():
        raise ValueError('fixture inputs must be absolute')
    current = Path(path.anchor)
    for part in path.parts[1:-1]:
        current /= part
        if current.is_symlink():
            raise ValueError('fixture path has a symlink parent')
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
            info.st_uid != os.geteuid() or info.st_mode & 0o022 or
            executable and not info.st_mode & 0o111):
        raise ValueError('unsafe fixture file identity')
    return path


@contextmanager
def isolated_fixture(parent):
    parent = Path(parent)
    if not parent.is_absolute() or parent.is_symlink():
        raise ValueError('select an absolute private fixture parent')
    info = parent.stat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError('fixture parent must be owned and mode 0700')
    with tempfile.TemporaryDirectory(prefix='gs-package-', dir=parent) as temporary:
        root = Path(temporary)
        env = {'PATH': '/nonexistent', 'GIT_CONFIG_NOSYSTEM': '1', 'LANG': 'C'}
        for name, variable in [('home', 'HOME'), ('config', 'XDG_CONFIG_HOME'),
                               ('state', 'XDG_STATE_HOME'), ('cache', 'XDG_CACHE_HOME'),
                               ('runtime', 'XDG_RUNTIME_DIR'), ('prefix', 'NPM_CONFIG_PREFIX')]:
            path = root / name
            path.mkdir(mode=0o700)
            env[variable] = str(path)
        config = root / 'config' / 'config.toml'
        config.touch(mode=0o600)
        env['GITSTORE_CONFIG'] = str(config)
        credential = root / 'synthetic-credential'
        credential.write_text('synthetic-package-fixture-token\n')
        credential.chmod(0o600)
        yield root, env, credential
