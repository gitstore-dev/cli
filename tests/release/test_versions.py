import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bootstrap', ROOT / 'scripts/remove-release-bootstrap.py')
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class ReleaseVersions(unittest.TestCase):
    def test_initial_identity_and_bounded_history(self):
        config = json.loads((ROOT / 'release-please-config.json').read_text())
        self.assertEqual(config['bootstrap-sha'], 'b5c393e37b4787de0407ac3ccc1b058c067d69d8')
        self.assertLessEqual(config['commit-search-depth'], 500)
        self.assertLessEqual(config['release-search-depth'], 50)
        version = (ROOT / 'version.txt').read_text().strip()
        cargo = tomllib.loads((ROOT / 'Cargo.toml').read_text())
        lock = tomllib.loads((ROOT / 'Cargo.lock').read_text())
        self.assertEqual(cargo['workspace']['package']['version'], version)
        self.assertEqual(next(p['version'] for p in lock['package'] if p['name'] == 'gs'), version)
        self.assertEqual(version, '0.1.0-alpha.1')

    def test_bootstrap_removed_only_in_synchronized_first_release_pr(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # Model bootstrap state explicitly: the checked-out release proposal
            # has already removed release-as and advanced its manifest.
            (root / 'version.txt').write_text('0.1.0-alpha.1\n')
            (root / 'Cargo.toml').write_text('[workspace.package]\nversion = "0.1.0-alpha.1"\n')
            (root / 'release-please-config.json').write_text(json.dumps({
                'packages': {'.': {'release-as': '0.1.0-alpha.1'}}}))
            (root / '.release-please-manifest.json').write_text('{".":"0.1.0-alpha.0"}')
            with self.assertRaises(ValueError):
                bootstrap.remove(root)
            (root / '.release-please-manifest.json').write_text('{".":"0.1.0-alpha.1"}')
            self.assertTrue(bootstrap.remove(root))
            self.assertFalse(bootstrap.remove(root))
            self.assertNotIn('release-as', json.loads((root / 'release-please-config.json').read_text())['packages']['.'])

    @unittest.skipUnless(os.environ.get('GITSTORE_RELEASE_PLEASE_MODULE'), 'select pinned Release Please 17.3.0 module for offline history fixtures')
    def test_actual_engine_alpha1_then_alpha2_and_synchronized_updaters(self):
        reply = subprocess.run(['node', str(ROOT / 'tests/release/release-please-fixture.cjs'), str(ROOT)],
            check=True, capture_output=True, text=True, timeout=30)
        self.assertEqual(json.loads(reply.stdout)['second'], '0.1.0-alpha.2')
