"""Failures expose bounded public diagnostics without leaking native workload logs."""
from contextlib import redirect_stderr
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('qualify_release', ROOT / 'scripts/qualify-release.py')
qualification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qualification)


class QualificationLogs(unittest.TestCase):
    def test_failed_public_check_retains_full_log_and_prints_only_bounded_tail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stderr = io.StringIO()
            command = [sys.executable, '-c',
                "import sys; print('early output' + 'x'*20000 + 'last error'); sys.exit(1)"]
            with redirect_stderr(stderr), self.assertRaises(subprocess.CalledProcessError):
                qualification.run_check('package-npm', command, root, {'LANG': 'C'})
            output = stderr.getvalue()
            self.assertNotIn('early output', output)
            self.assertIn('last error', output)
            self.assertLess(len(output), 16500)
            self.assertIn('early output', (root / 'package-npm.log').read_text())

    def test_native_control_failure_exposes_the_test_assertion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ['test_native_autostart.py', 'test_native_failures.py']:
                with self.subTest(name=name):
                    stderr = io.StringIO()
                    command = [sys.executable, '-c', "import sys; print('synthetic control assertion'); sys.exit(1)"]
                    with redirect_stderr(stderr), self.assertRaises(subprocess.CalledProcessError):
                        qualification.run_check(name, command, root, {'LANG': 'C'})
                    self.assertIn('synthetic control assertion', stderr.getvalue())

    def test_native_workload_failure_keeps_its_output_private(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stderr = io.StringIO()
            command = [sys.executable, '-c', "import sys; print('synthetic private workload'); sys.exit(1)"]
            with redirect_stderr(stderr), self.assertRaises(subprocess.CalledProcessError):
                qualification.run_check('native-cold', command, root, {'LANG': 'C'})
            self.assertEqual(stderr.getvalue(), '')
            self.assertIn('synthetic private workload', (root / 'native-cold.log').read_text())


if __name__ == '__main__':
    unittest.main()
