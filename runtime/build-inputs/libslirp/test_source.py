"""Regression controls for immutable source identity under Gitiles timestamp changes."""
import gzip
import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

spec = importlib.util.spec_from_file_location('fetch_source', Path(__file__).with_name('fetch-source.py'))
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)


def archive(timestamp=1, content=b'known source', mode=0o644, link='source.c'):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w', format=tarfile.PAX_FORMAT) as target:
        item = tarfile.TarInfo('source.c')
        item.mtime, item.mode, item.size = timestamp, mode, len(content)
        target.addfile(item, io.BytesIO(content))
        item = tarfile.TarInfo('alias')
        item.type, item.linkname = tarfile.SYMTYPE, link
        target.addfile(item)
    return gzip.compress(output.getvalue(), mtime=int(timestamp))


class SourceIdentity(unittest.TestCase):
    def test_transient_fetch_recovers_and_persistent_or_permanent_errors_stop(self):
        url='https://example.invalid/pinned-source'
        response=io.BytesIO(archive()); response.url=url
        def failure(code):
            return HTTPError(url,code,'synthetic fixture',{},io.BytesIO())
        for outcomes,calls,success in [([failure(503),response],2,True),
                                       ([failure(503) for _ in range(3)],3,False),
                                       ([failure(404)],1,False)]:
            with self.subTest(calls=calls), patch.object(source.urllib.request,'urlopen',side_effect=outcomes) as request, patch.object(source.time,'sleep') as sleep:
                if success:
                    self.assertTrue(source.download(url))
                else:
                    with self.assertRaises(HTTPError):
                        source.download(url)
                self.assertEqual(request.call_count,calls)
                self.assertEqual(sleep.call_count,calls-1)

    def test_timestamp_changes_preserve_source_identity(self):
        self.assertEqual(source.normalized_archive(archive(1.25)),
                         source.normalized_archive(archive(200.75)))

    def test_content_modes_and_links_remain_bound(self):
        expected = hashlib.sha256(source.normalized_archive(archive())).digest()
        for changed in [archive(content=b'changed'), archive(mode=0o755), archive(link='other.c')]:
            self.assertNotEqual(expected, hashlib.sha256(source.normalized_archive(changed)).digest())

    def test_escaping_links_and_oversized_archives_are_rejected(self):
        for changed in [archive(link='../outside'), archive(link='/outside'),
                        archive(content=b'x' * (source.MAX_SOURCE + 1))]:
            with self.assertRaises(ValueError):
                source.normalized_archive(changed)


if __name__ == '__main__':
    unittest.main()
