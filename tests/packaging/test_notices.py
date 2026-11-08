import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
import notices


class NoticeTests(unittest.TestCase):
    def test_same_package_version_cannot_hide_a_changed_git_commit_or_registry_checksum(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve()
            (root/'source').write_bytes(b'synthetic full source and notice')
            entry=dict(path='source',sha256=hashlib.sha256((root/'source').read_bytes()).hexdigest())
            source='git+https://github.com/example/library?rev='+('a'*40)+'#'+('a'*40)
            component=dict(id='example@1.0.0',license='MIT',source='https://codeload.github.com/example/library/tar.gz/'+('a'*40),
                licenses=[entry],sourceArchive=entry,cargoSource=source)
            package=dict(name='example',version='1.0.0',source=source)
            notices.generate(root,dict(components=[component]),dict(packages=[package]),[],root/'git-positive')
            package['source']=source.replace('a'*40,'b'*40)
            with self.assertRaises(ValueError):
                notices.generate(root,dict(components=[component]),dict(packages=[package]),[],root/'stale-git')
            component['cargoSource']=package['source']
            with self.assertRaises(ValueError):
                notices.generate(root,dict(components=[component]),dict(packages=[package]),[],root/'stale-archive')
            package.update(source='registry+https://github.com/rust-lang/crates.io-index',sourceChecksum=entry['sha256'])
            component.update(cargoSource=package['source'],source='https://static.crates.io/crates/example/example-1.0.0.crate')
            notices.generate(root,dict(components=[component]),dict(packages=[package]),[],root/'registry-positive')
            package['sourceChecksum']='0'*64
            with self.assertRaises(ValueError):
                notices.generate(root,dict(components=[component]),dict(packages=[package]),[],root/'stale-registry')

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
