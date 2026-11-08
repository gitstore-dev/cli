"""Meaningful package controls: inventory tampering, byte identity and scripts."""
import io
import json
from pathlib import Path
import sys
import tarfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
import formats
import test_layout as layout


class FormatTests(unittest.TestCase):
    setUp = layout.LayoutTests.setUp
    metadata = layout.LayoutTests.metadata
    def test_npm_platform_real_pair_and_no_download_scripts(self):
        output = self.root / 'platform.tgz'
        value = self.metadata()
        formats.npm(self.prefix, value, output)
        with tarfile.open(output) as archive:
            metadata = json.load(archive.extractfile('package/package.json'))
            self.assertEqual(metadata['os'], ['linux'])
            self.assertEqual(metadata['cpu'], ['arm64'])
            self.assertEqual(metadata['version'], '0.1.0-alpha.1')
            self.assertNotIn('scripts', metadata)
            self.assertNotIn('dependencies', metadata)
            for name in ['gs', 'gitstored']:
                entry = archive.getmember('package/bin/' + name)
                self.assertTrue(entry.isfile())
                self.assertEqual(entry.mode, 0o755)
                self.assertEqual(archive.extractfile(entry).read(), (self.prefix / 'bin' / name).read_bytes())
        with self.assertRaises(FileExistsError):
            formats.npm(self.prefix, value, output)

    def test_tampered_payload_never_packaged(self):
        value = self.metadata()
        (self.prefix / 'bin/gs').write_bytes(b'changed')
        with self.assertRaises(ValueError):
            formats.npm(self.prefix, value, self.root / 'tampered.tgz')
        self.assertFalse((self.root / 'tampered.tgz').exists())

    def test_system_code_ownership_only_and_private_pair(self):
        payload, links = formats.system_layout(self.prefix, self.metadata())
        for name in ['gs', 'gitstored']:
            self.assertIn('usr/libexec/gitstore/bin/' + name, payload)
            self.assertEqual(links['usr/bin/' + name], '../libexec/gitstore/bin/' + name)
        self.assertFalse(any(name.startswith(('home/', 'var/', 'etc/')) for name in payload))
        unit = payload['usr/lib/systemd/user/gitstored.service'][0]
        self.assertIn(b'--runtime-dir %t/gitstore', unit)
        self.assertNotIn(b'User=root', unit)
