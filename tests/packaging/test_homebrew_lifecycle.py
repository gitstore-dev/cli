"""Formula selection cannot execute arbitrary Ruby or select different native bytes."""
import hashlib
from pathlib import Path
import tempfile
import unittest

import homebrew_lifecycle as lifecycle


class HomebrewSelection(unittest.TestCase):
    def test_exact_reviewed_template_and_current_archive_identity(self):
        version = '0.1.0-alpha.1'
        rows = [dict(os=os_name, arch=arch,
            url=f'https://github.com/gitstore-dev/cli/releases/download/v{version}/gitstore-{version}-{os_name}-{arch}.tar.gz',
            sha256='a' * 64) for os_name in ('linux','darwin') for arch in ('x64','arm64')]
        payload = lifecycle.render(version, rows).encode()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve() / 'gitstore.rb'
            path.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            self.assertEqual(lifecycle.checked_formula(path,digest,version,'a' * 64),payload)
            with self.assertRaises(ValueError):
                lifecycle.checked_formula(path,digest,version,'b' * 64)
            injected = payload + b'\nsystem "arbitrary fixture code"\n'
            path.write_bytes(injected)
            with self.assertRaisesRegex(ValueError, 'reviewed binary-only template'):
                lifecycle.checked_formula(path,hashlib.sha256(injected).hexdigest(),version,'a' * 64)


if __name__ == '__main__':
    unittest.main()
