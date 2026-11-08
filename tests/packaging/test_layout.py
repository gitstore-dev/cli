"""Production staging contract, exercised with synthetic native-file fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'runtime/packaging'))
import inventory
import stage


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.prefix = self.root / 'stage with spaces'
        for name in ['bin/gs', 'bin/gitstored', 'share/notices/LICENSE',
                     'share/reference/gs.md', 'share/completions/gs.bash']:
            path = self.prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            header = bytearray(64)
            header[:6] = b'\x7fELF\x02\x01'
            header[18:20] = (183).to_bytes(2, 'little')
            path.write_bytes(header if name.startswith('bin/') else b'test fixture')
            path.chmod(0o755 if name.startswith('bin/') else 0o644)

    def metadata(self):
        return stage.inspect_layout(self.prefix, '0.1.0-alpha.1', 'a' * 40,
            'linux', 'arm64', 'glibc 2.39', version_reader=lambda path: '0.1.0-alpha.1')

    def test_complete_layout(self):
        value = self.metadata()
        inventory.validate_tree(value, self.prefix)
        self.assertEqual(set(value['executables']), {'gs', 'gitstored'})

    def test_reject_unshipped_docs(self):
        (self.prefix / 'share/completions/gs.bash').unlink()
        with self.assertRaises(ValueError):
            self.metadata()

    def test_generated_secret_service_license_is_not_a_credential_file(self):
        path = self.prefix / 'share/notices/dbus-secret-service@4.1.0/LICENSE-0.txt'
        path.parent.mkdir(parents=True)
        path.write_text('Synthetic full-license fixture')
        path.chmod(0o644)
        self.assertIn(path.relative_to(self.prefix).as_posix(), self.metadata()['notices'])

    def test_reject_helpers_images_and_credentials(self):
        for name in ['bin/probe', 'images/rootfs', 'share/notices/secret.key', '.env']:
            path = self.prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('synthetic')
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.metadata()
            path.unlink()

    def test_reject_linked_sibling(self):
        (self.prefix / 'bin/gitstored').unlink()
        os.link(self.prefix / 'bin/gs', self.prefix / 'bin/gitstored')
        with self.assertRaises(ValueError):
            self.metadata()

    def test_reject_wrong_reported_version(self):
        with self.assertRaises(ValueError):
            stage.inspect_layout(self.prefix, '0.1.0-alpha.1', 'a' * 40,
                'linux', 'arm64', 'glibc 2.39', version_reader=lambda path: '0.1.0')

    def test_reject_wrong_architecture(self):
        with self.assertRaises(ValueError):
            stage.inspect_layout(self.prefix, '0.1.0-alpha.1', 'a' * 40,
                'linux', 'x64', 'glibc 2.39', version_reader=lambda path: '0.1.0-alpha.1')

    def test_archive_is_inventory_only_and_no_overwrite(self):
        value = self.metadata()
        target = self.root / 'candidate.tar.gz'
        stage.archive_layout(self.prefix, value, target)
        import tarfile
        with tarfile.open(target) as archive:
            self.assertEqual(set(archive.getnames()), {f['path'] for f in value['files']} |
                             {'share/gitstore/install.json'})
            self.assertTrue(all(item.isfile() for item in archive))
        with self.assertRaises(FileExistsError):
            stage.archive_layout(self.prefix, value, target)
