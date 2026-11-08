"""Portable production executable checks; native workload lanes are separate."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from support import isolated_fixture, owned_file
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
import stage


class InstalledCommandTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('GITSTORE_PACKAGE_PAIR'),
                         'select exact production package bin directory for installed checks')
    def test_help_version_without_runtime_side_effects(self):
        pair = Path(os.environ['GITSTORE_PACKAGE_PAIR'])
        for name in ['gs', 'gitstored']:
            owned_file(pair / name, executable=True)
        with tempfile.TemporaryDirectory() as temporary:
            with isolated_fixture(Path(temporary).resolve()) as (root, env, _):
                before = {str(path.relative_to(root)) for path in root.rglob('*')}
                for name in ['gs', 'gitstored']:
                    for flag in ['--help', '--version']:
                        reply = subprocess.run([str(pair / name), flag], env=env, cwd=root,
                            capture_output=True, timeout=10, check=True)
                        self.assertTrue(reply.stdout)
                        self.assertFalse(reply.stderr)
                self.assertEqual(before, {str(path.relative_to(root)) for path in root.rglob('*')})

    @unittest.skipUnless(os.environ.get('GITSTORE_RUNTIME_FIXTURE') and
                         os.environ.get('GITSTORE_PACKAGE_PAIR'),
                         'select dedicated native fixture and exact production pair')
    def test_native_installed_lifecycle(self):
        import json
        fixture = owned_file(Path(os.environ['GITSTORE_RUNTIME_FIXTURE']))
        value = json.loads(fixture.read_text())
        if (not value.get('dedicatedTestAccount') or
                value.get('disposableUid') != os.geteuid() or os.geteuid() == 0):
            self.fail('native package tests require an explicitly dedicated non-root account')
        pair = Path(os.environ['GITSTORE_PACKAGE_PAIR'])
        for key, name in [('gs', 'gs'), ('daemon', 'gitstored')]:
            self.assertEqual(Path(value[key]).resolve(), (pair / name).resolve(),
                             'fixture must select the exact packaged executable pair')
        repo = Path(__file__).resolve().parents[2]
        # Existing clean-install cases cover concurrent isolation, cold/warm assets,
        # denial, foreground/detached lifetime and unavailable mappings. The harness
        # retains failure evidence and performs only observed owned teardown.
        subprocess.run([os.sys.executable, str(repo / 'tests/runtime/run.py'),
            '--scenario', 'clean-install', '--fixture', str(fixture)],
            check=True, cwd=repo, timeout=1800)

    @unittest.skipUnless(os.environ.get('GITSTORE_PACKAGE_PAIR'),
                         'select exact production pair for staging smoke')
    def test_real_production_pair_staging(self):
        import platform
        import shutil
        import tarfile
        pair = Path(os.environ['GITSTORE_PACKAGE_PAIR'])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            prefix = root / 'prefix with spaces'
            (prefix / 'bin').mkdir(parents=True)
            for name in ['gs', 'gitstored']:
                owned_file(pair / name, executable=True)
                shutil.copyfile(pair / name, prefix / 'bin' / name)
                (prefix / 'bin' / name).chmod(0o755)
            for name in ['notices/synthetic.txt', 'reference/synthetic.md', 'completions/synthetic.bash']:
                path = prefix / 'share' / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('Synthetic staging smoke documentation; not release material.\n')
                path.chmod(0o644)
            version = stage.reported_version(prefix / 'bin/gs')
            value = stage.inspect_layout(prefix, version, 'a' * 40,
                'darwin' if sys.platform == 'darwin' else 'linux',
                'arm64' if platform.machine() in ['arm64', 'aarch64'] else 'x64',
                'smoke test only, not release ABI evidence')
            archive = root / 'unqualified-smoke.tar.gz'
            stage.archive_layout(prefix, value, archive)
            with tarfile.open(archive) as handle:
                self.assertIn('share/gitstore/install.json', handle.getnames())
