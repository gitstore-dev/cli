"""Privileged fixture boundaries; no package manager mutation in these tests."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import system_packages as system


class SystemPackages(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.root.chmod(0o700)
        self.path = self.root / 'fixture.json'
        self.value = dict(schemaVersion=1, disposableUid=os.geteuid(), disposableGid=os.getegid(),
            dedicatedTestAccount=True, disposableHost=True, hostname='owned-test-host',
            osId='ubuntu', osVersion='24.04', arch='arm64', fixtureParent=str(self.root))
        self.path.touch(mode=0o600)

    def check(self):
        self.path.write_text(json.dumps(self.value))
        with patch.object(system.sys, 'platform', 'linux'), patch.object(system.os, 'geteuid', return_value=0), patch.object(
                system.socket, 'gethostname', return_value='owned-test-host'), patch.object(
                system.platform, 'freedesktop_os_release', return_value=dict(ID='ubuntu', VERSION_ID='24.04')), patch.object(
                system.platform, 'machine', return_value='aarch64'):
            return system.fixture(self.path, self.root, 'deb')

    @unittest.skipIf(os.geteuid() == 0, 'portable boundary fixture models a distinct non-root owner')
    def test_explicit_host_account_distribution_architecture_and_private_parent(self):
        self.assertEqual(self.check()[-1], 'arm64')
        for field, bad in [('hostname','another-host'), ('disposableUid',0), ('disposableGid',0),
                ('dedicatedTestAccount',False), ('disposableHost',False), ('osVersion','22.04'),
                ('arch','x64'), ('fixtureParent','/tmp')]:
            original = self.value[field]
            self.value[field] = bad
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.check()
            self.value[field] = original
        self.root.chmod(0o755)
        with self.assertRaises(ValueError):
            self.check()

    def test_altered_package_and_maintainer_script_fail_before_installation(self):
        artifact = self.root / 'input.deb'
        artifact.write_bytes(b'synthetic package bytes')
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        with patch.object(system, 'command') as command:
            with self.assertRaises(ValueError):
                system.inspect(artifact, 'a' * 64, '0.1.0-alpha.1', 'deb', 'arm64', os.geteuid())
            command.assert_not_called()
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode='w') as archive:
            for name in ('control','postinst'):
                payload = b'controlled fixture'
                entry = tarfile.TarInfo(name)
                entry.size = len(payload)
                archive.addfile(entry, io.BytesIO(payload))
        responses = [subprocess.CompletedProcess([],0,b'gitstore\n0.1.0~alpha.1-1\narm64',b''),
                     subprocess.CompletedProcess([],0,buffer.getvalue(),b'')]
        with patch.object(system, 'command', side_effect=responses) as command:
            with self.assertRaisesRegex(ValueError, 'package scripts'):
                system.inspect(artifact, digest, '0.1.0-alpha.1', 'deb', 'arm64', os.geteuid())
            self.assertEqual(command.call_count, 2)

    def test_rpm_implicit_build_id_links_are_not_part_of_the_reviewed_layout(self):
        artifact = self.root / 'input.rpm'
        artifact.write_bytes(b'synthetic RPM bytes')
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        responses = [subprocess.CompletedProcess([],0,b'gitstore\n0.1.0\n0.alpha.1.1\naarch64',b''),
                     subprocess.CompletedProcess([],0,b'',b''), subprocess.CompletedProcess([],0,b'',b''),
                     subprocess.CompletedProcess([],0,b'/usr/lib/.build-id/aa/id\t../../outside\t120777\n',b'')]
        with patch.object(system, 'command', side_effect=responses):
            with self.assertRaisesRegex(ValueError, 'RPM payload'):
                system.inspect(artifact, digest, '0.1.0-alpha.1', 'rpm', 'arm64', os.geteuid())

    def test_shared_rpm_policy_rejects_scripts_unsafe_links_and_privileged_modes(self):
        fields=['gitstore','0.1.0','0.alpha.1.1','aarch64']
        listing='/usr/bin/gs\t../libexec/gitstore/bin/gs\t120777\n/usr/libexec/gitstore/bin/gs\t\t100755\n'
        system.validate_rpm_metadata(fields,b'',b'',listing,'0.1.0-alpha.1','arm64')
        for scripts,triggers,entries in ((b'root script',b'',listing),(b'',b'root trigger',listing),
                (b'',b'',listing.replace('../libexec/gitstore/bin/gs','../../etc/passwd')),
                (b'',b'',listing.replace('100755','104755')),
                (b'',b'',listing.replace('/usr/libexec/gitstore/bin/gs','/etc/owned-by-another-package'))):
            with self.subTest(scripts=bool(scripts),triggers=bool(triggers),entries=entries),self.assertRaises(ValueError):
                system.validate_rpm_metadata(fields,scripts,triggers,entries,'0.1.0-alpha.1','arm64')


if __name__ == '__main__':
    unittest.main()
