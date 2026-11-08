from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
import homebrew


class HomebrewTests(unittest.TestCase):
    def test_complete_set_versioned_urls_and_private_pair(self):
        rows = [{'os': os_name, 'arch': arch, 'sha256': 'a' * 64,
                 'url': f'https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.1/gitstore-0.1.0-alpha.1-{os_name}-{arch}.tar.gz'}
                for os_name, arch in sorted(homebrew.TUPLES)]
        text = homebrew.render('0.1.0-alpha.1', rows)
        self.assertIn('bin.install_symlink libexec/"bin/gs", libexec/"bin/gitstored"', text)
        self.assertNotIn('system "', text)
        self.assertNotIn('services start', text)
        with self.assertRaises(ValueError):
            homebrew.render('0.1.0-alpha.1', rows[:-1])
        rows[0]['url'] = 'https://github.com/gitstore-dev/cli/releases/latest/download/archive.tgz'
        with self.assertRaises(ValueError):
            homebrew.render('0.1.0-alpha.1', rows)
