"""Target/dev filtering and source-bound notice extraction are release gates."""
import hashlib
import io
from pathlib import Path
import tempfile
import tarfile
import unittest
import sys
import gzip
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'runtime/packaging'))

from formats import tar_bytes
from source_materials import expand, notice_members, production_packages, selected_texts, select_text, license_expression


class SourceMaterials(unittest.TestCase):
    def test_only_reviewed_legacy_cargo_license_aliases_are_normalized(self):
        self.assertEqual(license_expression('MIT/Apache-2.0'),'MIT OR Apache-2.0')
        self.assertEqual(license_expression('Apache-2.0 / MIT'),'Apache-2.0 OR MIT')
        self.assertEqual(license_expression('LGPL-2.1-or-later AND BSD-3-Clause'),'LGPL-2.1-or-later AND BSD-3-Clause')
        with self.assertRaises(ValueError):
            license_expression('Unknown/Other')

    def test_sqlite_zip_header_is_selected_explicitly_with_exact_bytes(self):
        stream=io.BytesIO()
        source=b'/* Complete synthetic public-domain notice. */\nextern int example;\n'
        with zipfile.ZipFile(stream,'w') as archive:
            archive.writestr('sqlite/header.h',source)
        data=stream.getvalue()
        text=select_text(selected_texts(data,['sqlite/header.h'])['sqlite/header.h'],'first-comment')
        self.assertEqual(text,b'/* Complete synthetic public-domain notice. */\n')
        entry=dict(path='source.zip',sha256=hashlib.sha256(data).hexdigest(),files=[dict(
            member='sqlite/header.h',selector='first-comment',path='licenses/SQLite',sha256=hashlib.sha256(text).hexdigest())])
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory).resolve()
            expand(root,entry,data)
            self.assertEqual((root/'licenses/SQLite').read_bytes(),text)

    def test_debian_legacy_source_requires_a_complete_added_notice(self):
        patch=b'--- pcre.orig/debian/copyright\tdate\n+++ pcre/debian/copyright\tdate\n@@ -0,0 +1,2 @@\n+Synthetic full copyright\n+Synthetic full terms\n'
        data=gzip.compress(patch)
        self.assertEqual(selected_texts(data,['debian/copyright'],'debian-diff')['debian/copyright'],
            b'Synthetic full copyright\nSynthetic full terms\n')
        with self.assertRaises(ValueError):
            selected_texts(gzip.compress(patch.replace(b'-0,0',b'-1,1')),['debian/copyright'],'debian-diff')
        with self.assertRaises(ValueError):
            selected_texts(gzip.compress(patch.replace(b'+1,2',b'+1,3')),['debian/copyright'],'debian-diff')

    def test_dev_only_edges_are_excluded_and_shared_build_dependencies_remain(self):
        metadata = dict(workspace_members=['app'], packages=[dict(id=x) for x in ['app','runtime','build','test-only']],
            resolve=dict(nodes=[dict(id='app',deps=[dict(pkg='runtime',dep_kinds=[dict(kind=None)]),
                dict(pkg='build',dep_kinds=[dict(kind='build')]),dict(pkg='test-only',dep_kinds=[dict(kind='dev')])]),
                *[dict(id=x,deps=[]) for x in ['runtime','build','test-only']]]))
        self.assertEqual({p['id'] for p in production_packages(metadata)},{'app','runtime','build'})
        with self.assertRaises(ValueError):
            production_packages(dict(packages=[]))

    def test_native_embedded_notices_are_kept_and_unlisted_source_is_not_extracted(self):
        payload = tar_bytes({'sample-1/LICENSE':(b'Synthetic package license',0o644),
            'sample-1/vendor/native/COPYING':(b'Synthetic bundled native license',0o644),
            'sample-1/source.rs':(b'Public source fixture',0o644)})
        texts = notice_members(payload,'sample-1')
        self.assertEqual(set(texts),{'sample-1/LICENSE','sample-1/vendor/native/COPYING'})
        entry = dict(path='sources/sample.crate',sha256=hashlib.sha256(payload).hexdigest(),files=[
            dict(member=name,path='licenses/'+name,sha256=hashlib.sha256(data).hexdigest()) for name,data in texts.items()])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            expand(root,entry,payload)
            self.assertEqual((root/'licenses/sample-1/vendor/native/COPYING').read_bytes(),b'Synthetic bundled native license')
            self.assertFalse((root/'sample-1/source.rs').exists())
            with self.assertRaises(FileExistsError):
                expand(root,entry,payload)

    def test_tampered_linked_and_traversing_notices_fail_before_writes(self):
        for change in ('digest','link','traversal'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                stream = io.BytesIO()
                with tarfile.open(fileobj=stream,mode='w:gz') as archive:
                    member = tarfile.TarInfo('../outside' if change=='traversal' else 'sample/LICENSE')
                    member.size = 1
                    if change=='link':
                        member.type,member.linkname,member.size = tarfile.SYMTYPE,'/etc/passwd',0
                    archive.addfile(member,io.BytesIO(b'x') if member.isfile() else None)
                payload = stream.getvalue()
                entry = dict(path='source.tar.gz',sha256=hashlib.sha256(payload).hexdigest(),files=[
                    dict(member='sample/LICENSE',path='licenses/LICENSE',sha256='0'*64 if change=='digest' else hashlib.sha256(b'x').hexdigest())])
                with self.assertRaises(ValueError):
                    expand(root,entry,payload)
                self.assertEqual(list(root.iterdir()),[])
