#!/usr/bin/env python3
"""Run the native session-store positive on a fresh Linux bus and private HOME."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def inner(root):
    password = root / 'synthetic-password'
    password.write_text('synthetic-native-store-fixture-only\n')
    password.chmod(0o600)
    with password.open('rb') as source, (root / 'daemon.log').open('wb') as log:
        daemon = subprocess.Popen(['gnome-keyring-daemon', '--foreground', '--unlock', '--components=secrets',
            '--control-directory', str(root / 'runtime/gnome')], stdin=source, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 15
            while True:
                result = subprocess.run(['dbus-send','--session','--type=method_call','--print-reply',
                    '--dest=org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus.NameHasOwner',
                    'string:org.freedesktop.secrets'], capture_output=True, timeout=5)
                if b'boolean true' in result.stdout:
                    break
                if daemon.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('fresh Secret Service did not become available; inspect private daemon log')
                time.sleep(0.05)
            subprocess.run(['cargo','test','--locked','-p','gs','--test','native_credentials',
                'native_session_create_replace_read_and_remove','--','--ignored','--test-threads=1'],
                cwd=ROOT, check=True, timeout=180)
        finally:
            if daemon.poll() is None:
                daemon.terminate()
                try:
                    daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture-parent', required=True, type=Path)
    parser.add_argument('--inner', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    parent = args.fixture_parent
    if (sys.platform != 'linux' or os.geteuid() == 0 or not parent.is_absolute() or
            parent.is_symlink() or parent.stat().st_uid != os.geteuid() or parent.stat().st_mode & 0o077):
        parser.error('select an owned private fixture parent as a non-root Linux user')
    if args.inner:
        return inner(parent)
    root = Path(tempfile.mkdtemp(prefix='native-session-store-', dir=parent))
    root.chmod(0o700)
    env = {'PATH':os.environ['PATH'], 'LANG':'C', 'GIT_CONFIG_NOSYSTEM':'1'}
    for name, variable in [('home','HOME'), ('config','XDG_CONFIG_HOME'), ('state','XDG_STATE_HOME'),
                           ('cache','XDG_CACHE_HOME'), ('data','XDG_DATA_HOME'), ('runtime','XDG_RUNTIME_DIR')]:
        (root / name).mkdir(mode=0o700)
        env[variable] = str(root / name)
    # Build tooling only; no GitStore/user configuration or existing session bus
    # is inherited. The disposable account's compiler caches are not credentials.
    for name in ('CARGO_HOME','RUSTUP_HOME'):
        if os.environ.get(name):
            env[name] = os.environ[name]
    print('Owned native-store fixture:', root, flush=True)
    subprocess.run(['dbus-run-session','--',sys.executable,str(Path(__file__).resolve()),
        '--inner','--fixture-parent',str(root)], env=env, check=True, timeout=210)


if __name__ == '__main__':
    main()
