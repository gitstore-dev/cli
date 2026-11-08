import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
import notices


class NoticeTests(unittest.TestCase):
    def test_coverage_full_text_and_lgpl_material(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / 'license').write_text('Synthetic full notice')
            file = {'path': 'license', 'sha256': hashlib.sha256((root / 'license').read_bytes()).hexdigest()}
            item = {'id': 'example@1.0.0', 'license': 'MIT', 'source': 'https://example.org/pinned', 'licenses': [file]}
            manifest = {'components': [item]}
            metadata = {'packages': [{'name': 'example', 'version': '1.0.0', 'source': 'registry'}]}
            notices.generate(root, manifest, metadata, [], root / 'positive')
            self.assertIn('no upgrade compatibility', (root / 'positive/PRERELEASE.txt').read_text())
            with self.assertRaises(ValueError):
                notices.generate(root, {'components': []}, metadata, [], root / 'missing')
            item['license'] = 'LGPL-2.1-or-later'
            with self.assertRaises(KeyError):
                notices.generate(root, manifest, metadata, [], root / 'missing-kit')
            item['correspondingSource'] = {**file, 'releaseAsset': 'source.tar.gz'}
            item['relinkKit'] = {**file, 'releaseAsset': 'relink.tar.gz'}
            notices.generate(root, manifest, metadata, [], root / 'lgpl')
            (root / 'license').write_text('altered')
            with self.assertRaises(ValueError):
                notices.generate(root, manifest, metadata, [], root / 'changed')
