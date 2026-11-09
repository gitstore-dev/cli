"""Privileged package staging never follows links or overwrites retained inputs."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('review_channels',ROOT/'tests/packaging/check-review-channels.py')
review=importlib.util.module_from_spec(spec);spec.loader.exec_module(review)


class ReviewChannels(unittest.TestCase):
    def test_preserves_bytes_and_uses_private_read_only_staging(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve();source=root/'original.deb';source.write_bytes(b'synthetic package')
            target=root/'staged.deb';review.stage_package(source,target,os.geteuid())
            self.assertEqual(target.read_bytes(),source.read_bytes())
            self.assertEqual(target.stat().st_mode&0o777,0o400)

    def test_retained_destination_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve();source=root/'original.rpm';source.write_bytes(b'new')
            target=root/'retained';target.write_bytes(b'retained')
            with self.assertRaises(FileExistsError):review.stage_package(source,target,os.geteuid())
            self.assertEqual(target.read_bytes(),b'retained')

    def test_linked_or_writable_source_never_creates_staged_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve();source=root/'source';source.write_bytes(b'synthetic package')
            link=root/'link';link.symlink_to(source);target=root/'must-not-exist'
            with self.assertRaises(OSError):review.stage_package(link,target,os.geteuid())
            source.chmod(0o666)
            with self.assertRaises(ValueError):review.stage_package(source,target,os.geteuid())
            self.assertFalse(target.exists())


if __name__=='__main__':unittest.main()
