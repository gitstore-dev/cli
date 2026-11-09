"""Producer verifier boundary controls. Mocked gh output is not a signed positive."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'runtime/packaging'))
import release_verification as policy


class Attestations(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.artifact = self.root / 'artifact'
        self.artifact.write_bytes(b'synthetic final bytes')
        self.bundle = self.root / 'bundle.jsonl'
        self.bundle.write_text('synthetic bundle, never a signed-positive receipt')
        self.trust = self.root / 'trusted-root.json'
        self.trust.write_text('{}')
        self.digest = hashlib.sha256(self.artifact.read_bytes()).hexdigest()

    def verify(self, ref='refs/tags/v0.1.0-alpha.1', revision='a' * 40):
        return policy.verify(self.artifact, self.bundle, self.trust, ref, revision, self.digest)

    def test_verifier_receives_all_required_signed_identity_constraints(self):
        reply = [dict(verificationResult=dict(signature=dict(certificate=dict(
            sourceRepositoryIdentifier=policy.REPOSITORY_ID))))]
        with patch.object(policy.shutil, 'which', return_value='/fixture/gh'), patch.object(
                policy.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, json.dumps(reply).encode(), b'')) as run:
            self.assertTrue(self.verify()['verified'])
        command = run.call_args.args[0]
        for flag, expected in {
            '--repo': 'gitstore-dev/cli',
            '--cert-identity': 'https://github.com/' + policy.WORKFLOW + '@refs/tags/v0.1.0-alpha.1',
            '--cert-oidc-issuer': 'https://token.actions.githubusercontent.com',
            '--source-ref': 'refs/tags/v0.1.0-alpha.1', '--source-digest': 'a' * 40,
            '--predicate-type': 'https://slsa.dev/provenance/v1'
        }.items():
            self.assertEqual(command[command.index(flag) + 1], expected)
        self.assertIn('--deny-self-hosted-runners', command)
        # The maintained CLI rejects combinations of these identity aliases.
        self.assertEqual([flag for flag in ('--cert-identity','--cert-identity-regex',
            '--signer-repo','--signer-workflow') if flag in command],['--cert-identity'])
        self.assertEqual(run.call_args.kwargs['env']['GH_TOKEN'], 'offline-verification-placeholder')

    def test_altered_bytes_missing_bundle_and_unversioned_ref_fail_before_gh(self):
        for failure in ('bytes', 'bundle', 'ref', 'revision'):
            with self.subTest(failure=failure), patch.object(policy.shutil, 'which', return_value='/fixture/gh'), patch.object(policy.subprocess, 'run') as run:
                if failure == 'bytes':
                    self.artifact.write_bytes(b'altered')
                if failure == 'bundle':
                    self.bundle.unlink()
                with self.assertRaises((ValueError, OSError)):
                    self.verify(ref='refs/heads/main' if failure == 'ref' else 'refs/tags/v0.1.0-alpha.1',
                                revision='short' if failure == 'revision' else 'a' * 40)
                run.assert_not_called()
            self.artifact.write_bytes(b'synthetic final bytes')
            self.bundle.write_text('synthetic bundle')

    def test_wrong_repository_certificate_and_gh_policy_rejections_fail_closed(self):
        for claim in ([], [{}], [dict(verificationResult=dict(signature=dict(certificate=dict(
                sourceRepositoryIdentifier='untrusted-fork'))))]):
            with self.subTest(claim=claim), patch.object(policy.shutil, 'which', return_value='/fixture/gh'), patch.object(
                    policy.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, json.dumps(claim).encode(), b'')):
                with self.assertRaises(ValueError):
                    self.verify()
        # Cryptographic identity/workflow/ref/revision mismatches are rejected by
        # gh before certificate extraction, not by trusting predicate strings.
        for identity in ('producer', 'workflow', 'issuer', 'ref', 'revision', 'signature'):
            with self.subTest(identity=identity), patch.object(policy.shutil, 'which', return_value='/fixture/gh'), patch.object(
                    policy.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, b'', b'fixture policy mismatch')):
                with self.assertRaises(ValueError):
                    self.verify()


if __name__ == '__main__':
    unittest.main()
