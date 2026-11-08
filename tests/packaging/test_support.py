import os
from pathlib import Path
import tempfile
import unittest

from support import isolated_fixture, owned_file


class HarnessTests(unittest.TestCase):
    def test_private_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            with isolated_fixture(Path(tmp)) as (root, env, credential):
                self.assertEqual(env['PATH'], '/nonexistent')
                self.assertEqual(set(env), {'PATH', 'LANG', 'GIT_CONFIG_NOSYSTEM', 'HOME',
                    'XDG_CONFIG_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR',
                    'NPM_CONFIG_PREFIX', 'GITSTORE_CONFIG'})
                self.assertTrue(credential.is_relative_to(root))
                self.assertEqual(credential.stat().st_mode & 0o777, 0o600)
                self.assertEqual(root.stat().st_mode & 0o777, 0o700)
                # macOS TemporaryDirectory may live under /var -> /private/var.
                owned_file(credential.resolve())
            self.assertFalse(root.exists())

    def test_reject_shared_parent(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp).chmod(0o755)
            with self.assertRaises(ValueError), isolated_fixture(Path(tmp)):
                pass

    def test_reject_links_and_inherited_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            file = root / 'token'
            file.write_text('synthetic')
            file.chmod(0o600)
            owned_file(file)
            alias = root / 'alias'
            alias.symlink_to(file)
            with self.assertRaises(ValueError):
                owned_file(alias)
            os.link(file, root / 'hardlink')
            with self.assertRaises(ValueError):
                owned_file(file)
