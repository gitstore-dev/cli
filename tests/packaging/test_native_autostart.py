"""Actual installed first-use startup on an explicitly dedicated Linux account."""
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import tempfile
import time
import unittest


@unittest.skipUnless(os.environ.get('GITSTORE_RUNTIME_FIXTURE'),
                     'select the dedicated non-root Linux runtime manifest')
class NativeAutostart(unittest.TestCase):
    def test_public_links_and_concurrent_first_launch(self):
        uid = os.geteuid()
        value = json.loads(Path(os.environ['GITSTORE_RUNTIME_FIXTURE']).read_text())
        self.assertNotEqual(uid, 0)
        self.assertEqual(value['disposableUid'], uid)
        self.assertIs(value['dedicatedTestAccount'], True)
        runtime = Path(f'/run/user/{uid}/gitstore')
        self.assertFalse((runtime / 'gitstored.sock').exists(),
                         'dedicated user already has a manager; do not replace it')
        root = Path(tempfile.mkdtemp(prefix='package-autostart-', dir=value['fixtureParent']))
        root.chmod(0o700)
        for name in ['bin', 'public', 'home', 'config', 'state', 'cache']:
            (root / name).mkdir(mode=0o700)
        for key, name in [('gs', 'gs'), ('daemon', 'gitstored')]:
            shutil.copyfile(value[key], root / 'bin' / name)
            (root / 'bin' / name).chmod(0o755)
            (root / 'public' / name).symlink_to(root / 'bin' / name)
        env = {'PATH': '/nonexistent', 'HOME': str(root / 'home'),
            'XDG_CONFIG_HOME': str(root / 'config'), 'XDG_STATE_HOME': str(root / 'state'),
            'XDG_CACHE_HOME': str(root / 'cache'), 'XDG_RUNTIME_DIR': f'/run/user/{uid}',
            'DBUS_SESSION_BUS_ADDRESS': f'unix:path=/run/user/{uid}/bus', 'LANG': 'C'}
        children = []
        handles = []

        def managers():
            found = []
            for path in Path('/proc').iterdir():
                if not path.name.isdecimal():
                    continue
                try:
                    if path.stat().st_uid != uid:
                        continue
                    argv = (path / 'cmdline').read_bytes().split(b'\0')
                    if b'--state-dir' not in argv:
                        continue
                    state = argv[argv.index(b'--state-dir') + 1].decode()
                    if state != str(root / 'state/gitstore'):
                        continue
                    birth = (path / 'stat').read_text().rsplit(')', 1)[1].split()[19]
                    fd = os.pidfd_open(int(path.name))
                    if (path / 'stat').read_text().rsplit(')', 1)[1].split()[19] != birth:
                        os.close(fd)
                        continue
                    found.append(fd)
                except (OSError, ValueError, IndexError):
                    continue
            return found

        try:
            for _ in range(2):
                children.append(subprocess.Popen([str(root / 'public/gs'), 'deployment',
                    'list', '-o', 'json'], env=env, cwd=root, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE))
            for child in children:
                out, err = child.communicate(timeout=20)
                self.assertEqual(child.returncode, 0, err.decode(errors='replace'))
                self.assertTrue(json.loads(out) is not None)
            handles = managers()
            self.assertEqual(len(handles), 1, 'concurrent first use must converge to one owned manager')
            self.assertEqual((runtime / 'gitstored.sock').stat().st_mode & 0o777, 0o600)
            self.assertEqual(runtime.stat().st_uid, uid)
            self.assertEqual(runtime.stat().st_mode & 0o777, 0o700)
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=5)
            if not handles:
                handles = managers()
            for fd in handles:
                try:
                    signal.pidfd_send_signal(fd, signal.SIGTERM)
                    poll = select.poll()
                    poll.register(fd, select.POLLIN)
                    self.assertTrue(poll.poll(35000), 'owned manager did not terminate; retain evidence')
                finally:
                    os.close(fd)
            # Keep owned state/evidence even on failure; never infer data recovery.
            print(f'Retained package autostart fixture: {root}')


if __name__ == '__main__':
    unittest.main()
