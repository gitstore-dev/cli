"""Contract failures must not produce a qualified release inventory."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
import tempfile
import os

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('inventory', ROOT / 'runtime/packaging/inventory.py')
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.receipt = json.loads((ROOT / 'crates/gs/tests/fixtures/upstream/alpha9-capabilities.json').read_text())

    def test_receipt(self):
        module.validate_capabilities(self.receipt)

    def test_duplicate_commands(self):
        self.receipt['commands'][1]['id'] = 'CMD-001'
        with self.assertRaises(ValueError):
            module.validate_capabilities(self.receipt)

    def test_unknown_source(self):
        self.receipt['commands'][0]['sources'] = ['not-reviewed']
        with self.assertRaises(ValueError):
            module.validate_capabilities(self.receipt)

    def test_source_review_is_not_live_pass(self):
        self.receipt['commands'][0]['result'] = 'pass'
        with self.assertRaises(ValueError):
            module.validate_capabilities(self.receipt)

    def test_wait_overlay_is_not_submission(self):
        item = next(x for x in self.receipt['commands'] if x['id'] == 'CMD-045')
        self.assertEqual(item['classification'], 'implemented')
        self.assertEqual(item['overlays'][0]['classification'], 'external-contract')

    def test_schema_rejects_extra_properties(self):
        self.receipt['cloudPublished'] = True
        with self.assertRaises(ValueError):
            module.validate_capabilities(self.receipt)


class FileIdentityTests(unittest.TestCase):
    def test_hash_and_reject_links(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'bin').mkdir()
            payload = root / 'bin/gs'
            payload.write_bytes(b'synthetic executable')
            payload.chmod(0o755)
            result = module.snapshot_file(root, 'bin/gs')
            self.assertEqual(result['bytes'], 20)
            self.assertEqual(result['mode'], 0o755)
            (root / 'bin/gitstored').symlink_to(payload)
            with self.assertRaises(OSError):
                module.snapshot_file(root, 'bin/gitstored')
            os.link(payload, root / 'alias')
            with self.assertRaises(ValueError):
                module.snapshot_file(root, 'bin/gs')

    def test_parent_symlink_and_traversal(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'real').mkdir()
            (root / 'bin').symlink_to(root / 'real')
            with self.assertRaises(OSError):
                module.snapshot_file(root, 'bin/gs')
            for bad in ['/etc/passwd', '../gs', 'bin/../gs', 'bin//gs', 'bin/./gs', 'bin\\gs']:
                with self.subTest(path=bad), self.assertRaises(ValueError):
                    module.relative(bad)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        paths = ['bin/gs', 'bin/gitstored', 'share/notices.txt',
                 'share/reference.md', 'share/completions/gs.bash']
        files = []
        for name in paths:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic-fixture')
            path.chmod(0o755 if name.startswith('bin/') else 0o644)
            files.append(module.snapshot_file(self.root, name))
        self.value = {'schemaVersion': 1, 'applicationVersion': '0.1.0-alpha.1',
            'sourceRevision': 'a' * 40, 'os': 'linux', 'arch': 'arm64',
            'abiFloor': 'test only', 'runtime': 'native-linux',
            'executables': {name: {'version': '0.1.0-alpha.1', 'sha256': files[i]['sha256']}
                            for i, name in enumerate(['gs', 'gitstored'])},
            'files': files, 'libraryClosure': [], 'notices': [paths[2]],
            'reference': [paths[3]], 'completions': [paths[4]]}

    def test_valid_tree(self):
        module.validate_tree(self.value, self.root)

    def test_mismatched_version(self):
        self.value['executables']['gitstored']['version'] = '0.1.0'
        with self.assertRaises(ValueError):
            module.validate_install(self.value)

    def test_modified_bytes(self):
        (self.root / 'bin/gs').write_bytes(b'tampered')
        with self.assertRaises(ValueError):
            module.validate_tree(self.value, self.root)

    def test_missing_notice(self):
        self.value['notices'] = ['share/not-shipped']
        with self.assertRaises(ValueError):
            module.validate_install(self.value)

    def test_unqualified_macos_execution(self):
        self.value['os'] = 'darwin'
        with self.assertRaises(ValueError):
            module.validate_install(self.value)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.value = {'schemaVersion': 1, 'version': '0.1.0-alpha.1',
            'tag': 'v0.1.0-alpha.1', 'sourceRevision': 'a' * 40,
            'workflow': 'gitstore-dev/cli/.github/workflows/release.yml',
            'state': 'proposed', 'disclaimer': 'Early alpha; no compatibility or data guarantees.',
            'requiredArtifacts': ['candidate.tgz'], 'artifacts': [], 'receipts': []}

    def test_proposed_is_not_packaged(self):
        module.validate_release(self.value)
        self.value['state'] = 'packaged'
        with self.assertRaises(ValueError):
            module.validate_release(self.value)

    def test_tag_mismatch(self):
        self.value['tag'] = 'v0.1.0'
        with self.assertRaises(ValueError):
            module.validate_release(self.value)

    def test_reject_duplicate_required_names(self):
        self.value['requiredArtifacts'] *= 2
        with self.assertRaises(ValueError):
            module.validate_release(self.value)

    def test_require_bundles_after_qualification(self):
        artifact = {'filename': 'candidate.tgz', 'format': 'npm', 'os': 'linux',
            'arch': 'arm64', 'abiFloor': 'test', 'packageVersion': '0.1.0-alpha.1',
            'applicationVersion': '0.1.0-alpha.1', 'sourceRevision': 'a' * 40,
            'sha256': 'b' * 64, 'bytes': 123, 'unpackedBytes': 456,
            'installInventory': 'test-only', 'bundle': None, 'receipt': 'test-only'}
        self.value.update(state='validated', artifacts=[artifact], receipts=['test-only'])
        module.validate_release(self.value)
        self.value['state'] = 'attested'
        with self.assertRaises(ValueError):
            module.validate_release(self.value)


if __name__ == '__main__':
    unittest.main()
