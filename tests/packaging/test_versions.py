import importlib.util
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('versions', ROOT / 'runtime/packaging/versions.py')
versions = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(versions)


class VersionTests(unittest.TestCase):
    def test_release_proposal_rejects_accidental_stable_major_or_stuck_override(self):
        self.assertEqual(versions.validate_alpha_proposal('0.1.0-alpha.0', '0.1.0-alpha.1')['tag'], 'v0.1.0-alpha.1')
        versions.validate_alpha_proposal('0.1.0-alpha.1', '0.1.0-alpha.2')
        for bad in ['0.1.0', '1.0.0-alpha.1', '0.2.0-alpha.1', '0.1.0-alpha.1', '0.1.0-alpha.3']:
            with self.assertRaises(ValueError):
                versions.validate_alpha_proposal('0.1.0-alpha.1', bad)
    def test_alpha_one(self):
        self.assertEqual(versions.encode('0.1.0-alpha.1'), {
            'application': '0.1.0-alpha.1', 'npm': '0.1.0-alpha.1',
            'tag': 'v0.1.0-alpha.1', 'debian': '0.1.0~alpha.1-1',
            'rpmVersion': '0.1.0', 'rpmRelease': '0.alpha.1.1', 'cmake': '0.1.0'})

    def test_package_rebuild_is_not_application_version(self):
        result = versions.encode('0.1.0-alpha.2', rebuild=3)
        self.assertEqual(result['application'], '0.1.0-alpha.2')
        self.assertEqual(result['debian'], '0.1.0~alpha.2-3')
        self.assertEqual(result['rpmRelease'], '0.alpha.2.3')

    def test_stable_encoding(self):
        result = versions.encode('0.1.0')
        self.assertEqual(result['debian'], '0.1.0-1')
        self.assertEqual(result['rpmRelease'], '1')

    def test_invalid_versions(self):
        for bad in ['v0.1.0', '00.1.0', '0.1.0-alpha.01', '0.1.0+secret',
                    '0.1.0;touch x', '0.1.0-alpha.1\n', '../0.1.0']:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                versions.encode(bad)
        for bad in [0, -1, True, '1']:
            with self.assertRaises(ValueError):
                versions.encode('0.1.0', rebuild=bad)

    def test_prerelease_order(self):
        self.assertLess(versions.order_key('0.1.0-alpha.1'), versions.order_key('0.1.0-alpha.2'))
        self.assertLess(versions.order_key('0.1.0-alpha.2'), versions.order_key('0.1.0'))
        self.assertLess(versions.order_key('0.1.0-alpha.2'), versions.order_key('0.1.0-alpha.10'))

    @unittest.skipUnless(shutil.which('dpkg'), 'native Debian comparator unavailable on this host')
    def test_native_debian_order(self):
        values = [versions.encode(v)['debian'] for v in ['0.1.0-alpha.1', '0.1.0-alpha.2', '0.1.0']]
        for left, right in zip(values, values[1:]):
            subprocess.run(['dpkg', '--compare-versions', left, 'lt', right], check=True)


if __name__ == '__main__':
    unittest.main()
