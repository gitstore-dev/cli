"""Replacement review refuses incomplete selections and unsafe fixture paths."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'runtime/packaging'))
from review_replacements import arguments


class ReviewReplacements(unittest.TestCase):
    def test_no_selection_preserves_install_remove_checks(self):
        self.assertEqual(arguments(Path('/absent'),{},'linux','x64','rpm'),[])

    def test_partial_producer_selection_fails_before_network_or_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output=Path(temporary).resolve()/'absent'
            reply=subprocess.run([sys.executable,str(ROOT/'scripts/prepare-package-review.py'),
                '--cli-run','1','--linux-run','2','--replacement-cli-run','3','--output',str(output)],
                capture_output=True,text=True,timeout=15)
            self.assertNotEqual(reply.returncode,0)
            self.assertIn('both replacement producer runs or neither',reply.stderr)
            self.assertFalse(output.exists())

    def test_version_traversal_is_rejected_before_reading(self):
        with self.assertRaises(ValueError):
            arguments(Path('/absent'),dict(version='0.1.0-alpha.1',replacementVersion='../other'), 'linux','x64','rpm')

    def test_selected_exact_file_digest_and_link_refusal(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary).resolve()
            path=root/'replacements/linux-x64/packages/UNQUALIFIED-ONLY-0.1.0-alpha.2-linux-x64.rpm'
            path.parent.mkdir(parents=True);path.write_bytes(b'synthetic exact package bytes')
            record=dict(version='0.1.0-alpha.1',replacementVersion='0.1.0-alpha.2')
            result=arguments(root,record,'linux','x64','rpm')
            self.assertEqual(result[1],str(path))
            self.assertEqual(result[3],hashlib.sha256(path.read_bytes()).hexdigest())
            selected=path.with_suffix('.original');path.rename(selected);path.symlink_to(selected)
            with self.assertRaises(ValueError):arguments(root,record,'linux','x64','rpm')


if __name__=='__main__':
    unittest.main()
