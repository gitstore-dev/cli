"""Selected real native loader/version failures; never alter installed libraries."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import uuid


class NativeFailures(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'linux' and os.environ.get('GITSTORE_PACKAGE_PAIR') and shutil.which('patchelf'),
        'select an actual Linux production pair and fixture patchelf tool')
    def test_missing_native_library_fails_before_runtime_state(self):
        pair = Path(os.environ['GITSTORE_PACKAGE_PAIR'])
        fixture = os.environ.get('GITSTORE_RUNTIME_FIXTURE')
        parent = Path(json.loads(Path(fixture).read_text())['fixtureParent']) if fixture else None
        # Fedora's system temporary directory may be mounted noexec. Keep the
        # copied negative-control binary in the explicitly selected fixture.
        with tempfile.TemporaryDirectory(prefix='ld-', dir=parent) as directory:
            root = Path(directory).resolve()
            daemon = root/'gitstored'
            shutil.copyfile(pair/'gitstored', daemon)
            daemon.chmod(0o700)
            missing = 'libgitstore_missing_'+uuid.uuid4().hex+'.so.0'
            subprocess.run([shutil.which('patchelf'),'--add-needed',missing,str(daemon)],check=True)
            reply = subprocess.run([str(daemon),'--version'],env={'PATH':'/nonexistent','HOME':str(root)},
                capture_output=True,timeout=10)
            self.assertNotEqual(reply.returncode, 0)
            self.assertFalse(reply.stdout)
            self.assertIn(missing.encode(), reply.stderr)
            self.assertEqual({p.name for p in root.iterdir()}, {'gitstored'})

    @unittest.skipUnless(sys.platform == 'linux' and os.environ.get('GITSTORE_PACKAGE_PAIR')
        and os.environ.get('GITSTORE_RUNTIME_FIXTURE') and os.environ.get('GITSTORE_MISMATCH_DAEMON'),
        'select a dedicated native fixture, actual pair and explicit different-version production daemon')
    def test_running_different_version_daemon_is_refused_and_not_stopped(self):
        fixture = json.loads(Path(os.environ['GITSTORE_RUNTIME_FIXTURE']).read_text())
        self.assertNotEqual(os.geteuid(), 0)
        self.assertIs(fixture['dedicatedTestAccount'], True)
        self.assertEqual(fixture['disposableUid'], os.geteuid())
        parent = Path(fixture['fixtureParent'])
        # Qualification nests this fixture beneath its cold-start workspace.
        # Keep the client socket pathname within Linux's 108-byte sun_path.
        root = Path(tempfile.mkdtemp(prefix='v-',dir=parent))
        root.chmod(0o700)
        env = {'PATH':'/nonexistent','LANG':'C'}
        for name,variable in [('home','HOME'),('runtime','XDG_RUNTIME_DIR'),('state','XDG_STATE_HOME'),
                ('config','XDG_CONFIG_HOME'),('cache','XDG_CACHE_HOME')]:
            (root/name).mkdir(mode=0o700)
            env[variable] = str(root/name)
        (root/'runtime/gitstore').mkdir(mode=0o700)
        (root/'state/gitstore').mkdir(mode=0o700)
        config = root/'config/config.toml'
        config.touch(mode=0o600)
        env['GITSTORE_CONFIG'] = str(config)
        gs = Path(os.environ['GITSTORE_PACKAGE_PAIR'])/'gs'
        daemon = Path(os.environ['GITSTORE_MISMATCH_DAEMON'])
        self.assertNotEqual(subprocess.check_output([str(gs),'--version'],env=env).split()[1],
            subprocess.check_output([str(daemon),'--version'],env=env).split()[1])
        process = subprocess.Popen([str(daemon),'--runtime-dir',str(root/'runtime/gitstore'),
            '--state-dir',str(root/'state/gitstore')],env=env,cwd=root,
            stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic()+10
            while not (root/'runtime/gitstore/gitstored.sock').exists() and time.monotonic()<deadline:
                self.assertIsNone(process.poll(), 'selected manager exited before negotiation')
                time.sleep(.02)
            self.assertTrue((root/'runtime/gitstore/gitstored.sock').is_socket())
            reply = subprocess.run([str(gs),'deployment','list','-o','json'],env=env,cwd=root,
                capture_output=True,timeout=10)
            self.assertEqual(reply.returncode, 6, reply.stderr.decode(errors='replace'))
            self.assertFalse(reply.stdout)
            self.assertIn(b'INCOMPATIBLE_VERSION', reply.stderr)
            self.assertIsNone(process.poll(), 'client must not stop/replace the existing manager')
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=35)
            print('Retained actual version-refusal fixture:',root)
