"""Disposable producer fixtures; these do not constitute release qualification."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'runtime/packaging'))
import release_candidate as candidate
import publication


class Draft:
    def __init__(self):
        self.tag = 'v0.1.0-alpha.1'
        self.revision = 'a' * 40
        self.draft = True
        self.prerelease = True
        self.assets = {}
        self.fail_after = None
        self.promotions = 0
        self.tap = None
        self.fail_tap = False
        self.body = 'Synthetic release history, preserved by publication.'

    def inspect(self):
        return dict(tag=self.tag, revision=self.revision, draft=self.draft,
                    prerelease=self.prerelease, assets=dict(self.assets), body=self.body)

    def ensure_notice(self, notice):
        if not self.draft:
            raise AssertionError('must not silently edit published notes')
        self.body = notice + '\n\n' + self.body

    def upload(self, name, payload):
        if self.fail_after == len(self.assets):
            raise RuntimeError('fixture interrupted upload')
        if name in self.assets:
            raise AssertionError('must never clobber an existing asset')
        self.assets[name] = payload

    def promote(self):
        self.draft = False
        self.promotions += 1

    def update_tap(self, tag, payload):
        if self.draft:
            raise AssertionError('tap update before publication')
        if self.fail_tap:
            raise RuntimeError('fixture interrupted tap update')
        self.tap = (tag, payload)


class Promotion(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.value = candidate.propose('0.1.0-alpha.1', 'a' * 40)
        for slot in candidate.slots():
            name = candidate.slot_filename(self.value['version'], *slot)
            payload = ('synthetic fixture: ' + name).encode()
            (self.root / name).write_bytes(payload)
            candidate.register(self.value, self.root, name, *slot,
                               install_inventory=candidate.slot_filename(self.value['version'], 'manifest', slot[1], slot[2])
                               if slot[1] != 'any' else 'release-inventory.json', unpacked_bytes=len(payload))
        by_name = {a['filename']: a for a in self.value['artifacts']}
        for row in candidate.platforms():
            os_name, arch = row['os'], row['arch']
            executables, files = {}, []
            for binary in ('gs', 'gitstored'):
                artifact = by_name[candidate.slot_filename(self.value['version'], binary, os_name, arch)]
                executables[binary] = dict(version=self.value['version'], sha256=artifact['sha256'])
                files.append(dict(path='bin/' + binary, sha256=artifact['sha256'], bytes=artifact['bytes'], mode=0o755))
            for name in ('notices', 'reference', 'completions'):
                files.append(dict(path='share/' + name + '/fixture.txt', sha256='b' * 64, bytes=1, mode=0o644))
            manifest = dict(schemaVersion=1, applicationVersion=self.value['version'], sourceRevision='a' * 40,
                os=os_name, arch=arch, abiFloor=row['abiFloor'], runtime=row['runtime'], executables=executables,
                files=files, libraryClosure=[], **{name:['share/' + name + '/fixture.txt'] for name in ('notices','reference','completions')})
            name = candidate.slot_filename(self.value['version'], 'manifest', os_name, arch)
            payload = json.dumps(manifest).encode()
            (self.root / name).write_bytes(payload)
            by_name[name].update(sha256=hashlib.sha256(payload).hexdigest(), bytes=len(payload), unpackedBytes=len(payload))

    def packaged(self):
        value = candidate.advance(self.value, 'version-reviewed', self.root,
                                  reviewed_tag=self.value['tag'], reviewed_revision='a' * 40)
        value = candidate.advance(value, 'built', self.root)
        return candidate.advance(value, 'packaged', self.root)

    def test_complete_matrix_and_immutable_checksums(self):
        value = self.packaged()
        candidate.validate_bytes(value, self.root)
        self.assertEqual(set(value['requiredArtifacts']), {a['filename'] for a in value['artifacts']})
        lines = (self.root / 'SHA256SUMS').read_text().splitlines()
        self.assertEqual(len(lines), len(value['artifacts']) - 1)
        self.assertFalse(any(line.endswith('  SHA256SUMS') for line in lines))
        with self.assertRaises(ValueError):
            candidate.register(value, self.root, 'other', 'archive', 'linux', 'x64',
                               install_inventory='fixture', unpacked_bytes=1)
        with self.assertRaises(FileExistsError):
            candidate.advance(candidate.advance(self.value, 'version-reviewed', self.root,
                reviewed_tag=value['tag'], reviewed_revision='a' * 40), 'built', self.root)
            candidate.freeze(self.value, self.root)

    def test_incomplete_matrix_wrong_tag_or_skipped_state_rejected(self):
        for change in ['missing', 'wrong-arch', 'wrong-source']:
            with self.subTest(change=change):
                value = copy.deepcopy(self.value)
                if change == 'missing':
                    value['artifacts'].pop()
                elif change == 'wrong-arch':
                    value['artifacts'][0]['arch'] = 'any'
                else:
                    value['artifacts'][0]['sourceRevision'] = 'b' * 40
                with self.assertRaises(ValueError):
                    reviewed = candidate.advance(value, 'version-reviewed', self.root,
                        reviewed_tag=value['tag'], reviewed_revision='a' * 40)
                    candidate.advance(reviewed, 'built', self.root)
        with self.assertRaises(ValueError):
            candidate.advance(self.value, 'packaged', self.root)
        with self.assertRaises(ValueError):
            candidate.advance(self.value, 'version-reviewed', self.root,
                reviewed_tag='v0.1.0', reviewed_revision='a' * 40)

    def test_version_can_be_reviewed_before_subjects_exist(self):
        value = candidate.propose('0.1.0-alpha.1', 'a' * 40)
        value = candidate.advance(value, 'version-reviewed', self.root,
            reviewed_tag=value['tag'], reviewed_revision='a' * 40)
        with self.assertRaises(ValueError):
            candidate.advance(value, 'built', self.root)

    def test_loose_executable_must_match_its_platform_install_inventory(self):
        value = copy.deepcopy(self.value)
        artifact = next(a for a in value['artifacts'] if a['format'] == 'binary')
        payload = b'another executable'
        (self.root / artifact['filename']).write_bytes(payload)
        artifact.update(sha256=hashlib.sha256(payload).hexdigest(), bytes=len(payload))
        reviewed = candidate.advance(value, 'version-reviewed', self.root,
            reviewed_tag=value['tag'], reviewed_revision='a' * 40)
        with self.assertRaises(ValueError):
            candidate.advance(reviewed, 'built', self.root)

    def test_validation_requires_every_selected_host_and_channel_receipt(self):
        value = self.packaged()
        with self.assertRaises(ValueError):
            candidate.advance(value, 'validated', self.root)
        receipts = []
        for index, row in enumerate(candidate.qualification_rows()):
            name = f'qualification-{index}.json'
            receipt = dict(schemaVersion=1, result='pass', tag=value['tag'],
                           sourceRevision=value['sourceRevision'], selection=list(row),
                           subjects={a['filename']: a['sha256'] for a in value['artifacts']})
            commands={key:dict(samples=100,p95Milliseconds=1) for key in
                ('gs --help','gs --version','gitstored --help','gitstored --version')}
            performance=(dict(status='passed',managerChildStopped=True,commandLatency=dict(commands=commands),
                daemonInspect=dict(samples=100,p95Milliseconds=1)) if row[0]=='linux' else dict(commands=commands))
            performance['sourceInputs']={binary:{key:next(a for a in value['artifacts'] if a['filename']==
                candidate.slot_filename(value['version'],binary,row[0],row[1]))[key] for key in ('sha256','bytes')}
                for binary in ('gs','gitstored')}
            receipt['performance']=performance
            (self.root / name).write_text(json.dumps(receipt))
            receipts.append(name)
        value['receipts'] = receipts
        first=self.root/receipts[0]
        original=first.read_text()
        for change in ('missing-performance','wrong-pair'):
            altered=json.loads(original)
            if change=='missing-performance':del altered['performance']
            else:altered['performance']['sourceInputs']['gs']['sha256']='0'*64
            first.write_text(json.dumps(altered))
            with self.subTest(change=change),self.assertRaises(ValueError):
                candidate.advance(value,'validated',self.root)
        first.write_text(original)
        value = candidate.advance(value, 'validated', self.root)
        with self.assertRaises((ValueError, OSError)):
            candidate.advance(value, 'attested', self.root)
        (self.root / value['artifacts'][0]['filename']).write_bytes(b'changed')
        with self.assertRaises(ValueError):
            candidate.validate_bytes(value, self.root)

    def verified_fixture(self):
        value = self.packaged()
        # Exercise publication separately from the cryptographic verifier. Never
        # pass this forged fixture to real GitHub tooling.
        value['state'] = 'producer-verified'
        for artifact in value['artifacts']:
            artifact['receipt'] = 'controlled-fixture-only'
            artifact['bundle'] = artifact['filename'] + '.sigstore.jsonl'
            (self.root / artifact['bundle']).write_text('controlled fixture bundle')
        value['receipts'] = ['controlled-fixture-only']
        return value

    def test_failure_leaves_draft_then_retry_reuses_matching_bytes(self):
        value = self.verified_fixture()
        backend = Draft()
        backend.fail_after = 2
        with self.assertRaises(RuntimeError):
            publication.publish(value, self.root, backend)
        self.assertTrue(backend.draft)
        self.assertEqual(backend.promotions, 0)
        backend.fail_after = None
        publication.publish(value, self.root, backend)
        self.assertFalse(backend.draft)
        self.assertEqual(backend.promotions, 1)
        publication.publish(value, self.root, backend)
        self.assertEqual(backend.promotions, 1)
        self.assertTrue(backend.body.startswith(candidate.DISCLAIMER + '\n\n'))
        self.assertIn('Synthetic release history', backend.body)

    def test_conflicts_wrong_source_and_unexpected_assets_never_promote(self):
        value = self.verified_fixture()
        for failure in ['bytes', 'revision', 'extra', 'stable']:
            backend = Draft()
            if failure == 'bytes':
                backend.assets[value['artifacts'][0]['filename']] = b'conflicting'
            elif failure == 'revision':
                backend.revision = 'b' * 40
            elif failure == 'extra':
                backend.assets['unexpected'] = b'x'
            else:
                backend.prerelease = False
            with self.subTest(failure=failure), self.assertRaises(ValueError):
                publication.publish(value, self.root, backend)
            self.assertTrue(backend.draft)
            self.assertEqual(backend.promotions, 0)

    def test_pending_formula_pr_is_reported_separately_from_publication(self):
        class Pending(Draft):
            def update_tap(self, tag, payload):
                super().update_tap(tag,payload)
                return dict(status='pending',pullRequest='https://github.com/gitstore-dev/cli/pull/123')
        backend=Pending()
        result=publication.publish(self.verified_fixture(),self.root,backend,update_tap=True)
        self.assertTrue(result['published'])
        self.assertEqual(result['tap']['status'],'pending')
        self.assertFalse(backend.draft)
        self.assertEqual(backend.promotions,1)

    def test_tap_only_after_promotion_and_retry_after_tap_failure(self):
        value = self.verified_fixture()
        backend = Draft()
        backend.fail_after = 1
        with self.assertRaises(RuntimeError):
            publication.publish(value, self.root, backend, update_tap=True)
        self.assertIsNone(backend.tap)
        backend.fail_after = None
        backend.fail_tap = True
        with self.assertRaises(RuntimeError):
            publication.publish(value, self.root, backend, update_tap=True)
        self.assertFalse(backend.draft)
        self.assertIsNone(backend.tap)
        backend.fail_tap = False
        publication.publish(value, self.root, backend, update_tap=True)
        self.assertEqual(backend.tap, (value['tag'], (self.root / 'gitstore.rb').read_bytes()))
        self.assertEqual(backend.promotions, 1)


if __name__ == '__main__':
    unittest.main()
