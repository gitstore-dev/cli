"""Reject extra, altered and privileged archive members before any installation."""
import hashlib
import io
from pathlib import Path
import tarfile
import unittest
from unittest.mock import patch

import homebrew_lifecycle as lifecycle
import test_layout


class ArchiveSelection(unittest.TestCase):
    def test_exact_inventory_and_tampering(self):
        fixture = test_layout.LayoutTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        source = fixture.root/'original.tar.gz'
        test_layout.stage.archive_layout(fixture.prefix, fixture.metadata(), source)
        with tarfile.open(source) as archive:
            entries = [(m, archive.extractfile(m).read()) for m in archive.getmembers()]
        with patch.object(lifecycle.sys, 'platform', 'linux'), patch.object(lifecycle.platform, 'machine', return_value='aarch64'):
            lifecycle.checked_archive(source, hashlib.sha256(source.read_bytes()).hexdigest(), '0.1.0-alpha.1')
            for change in ('extra', 'setuid', 'bytes', 'link'):
                target = fixture.root/(change+'.tar.gz')
                with tarfile.open(target, 'w:gz') as output:
                    for member, payload in entries:
                        import copy
                        member = copy.copy(member)
                        if member.name == 'bin/gs':
                            if change == 'setuid':
                                member.mode = 0o4755
                            if change == 'bytes':
                                payload = b'changed' + payload[7:]
                            if change == 'link':
                                member.type, member.linkname, member.size = tarfile.SYMTYPE, '/bin/sh', 0
                        output.addfile(member, io.BytesIO(payload) if member.isfile() else None)
                    if change == 'extra':
                        member = tarfile.TarInfo('lib/uninventoried')
                        member.size, member.mode = 1, 0o644
                        output.addfile(member, io.BytesIO(b'x'))
                with self.subTest(change=change), self.assertRaises(ValueError):
                    lifecycle.checked_archive(target, hashlib.sha256(target.read_bytes()).hexdigest(), '0.1.0-alpha.1')
