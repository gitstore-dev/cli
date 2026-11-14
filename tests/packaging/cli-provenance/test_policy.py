"""Local policy/input tests only; these do not establish a valid signature."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

path = Path(__file__).resolve().parents[3] / "runtime/packaging/release_verification.py"
spec = importlib.util.spec_from_file_location("release_verification", path)
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class ReleasePolicy(unittest.TestCase):
    def test_certificate_repository_id_is_required(self):
        matching = [{"verificationResult": {"signature": {"certificate": {
            "sourceRepositoryIdentifier": policy.REPOSITORY_ID}}}}]
        self.assertTrue(policy.repository_identity(matching))
        for value in ([], {}, [None], [{"statement": {"repositoryId": policy.REPOSITORY_ID}}],
                      [{"verificationResult": {"signature": {"certificate": {
                          "sourceRepositoryIdentifier": "another-repository"}}}}]):
            self.assertFalse(policy.repository_identity(value))

    def test_identity_flags_and_snapshot_are_mandatory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact, bundle, trust = (root / name for name in ("artifact", "bundle", "trust"))
            artifact.write_bytes(b"unpublished synthetic artifact")
            bundle.write_text("{}")
            trust.write_text("{}")
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            result = [{"verificationResult": {"signature": {"certificate": {
                "sourceRepositoryIdentifier": policy.REPOSITORY_ID}}}}]

            def verifier(command, **kwargs):
                self.assertNotEqual(Path(command[3]), artifact)
                self.assertEqual(Path(command[3]).read_bytes(), artifact.read_bytes())
                for flag, value in (("--repo", policy.REPOSITORY),
                                    ("--cert-identity", "https://github.com/" + policy.WORKFLOW + "@refs/tags/v0.1.0"),
                                    ("--source-ref", "refs/tags/v0.1.0"),
                                    ("--source-digest", "a" * 40)):
                    self.assertEqual(command[command.index(flag) + 1], value)
                self.assertIn("--deny-self-hosted-runners", command)
                self.assertIn("--custom-trusted-root", command)
                self.assertNotEqual(kwargs["env"]["HOME"], str(Path.home()))
                return subprocess.CompletedProcess(command, 0, json.dumps(result).encode(), b"")

            with patch.object(policy.shutil, "which", return_value="/fixture/gh"), \
                    patch.object(policy.subprocess, "run", side_effect=verifier) as run:
                self.assertTrue(policy.verify(artifact, bundle, trust, "refs/tags/v0.1.0",
                                              "a" * 40, digest)["verified"])
                self.assertEqual(run.call_count, 1)
                artifact.write_bytes(b"tampered")
                with self.assertRaises(ValueError):
                    policy.verify(artifact, bundle, trust, "refs/tags/v0.1.0", "a" * 40, digest)
                self.assertEqual(run.call_count, 1)

    def test_missing_evidence_bad_revision_and_crypto_failure_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact, bundle, trust = (root / name for name in ("artifact", "bundle", "trust"))
            for file in (artifact, bundle, trust):
                file.write_bytes(b"synthetic")
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            with patch.object(policy.shutil, "which", return_value="/fixture/gh"), \
                    patch.object(policy.subprocess, "run", return_value=
                                 subprocess.CompletedProcess([], 1, b"", b"rejected")):
                with self.assertRaises(ValueError):
                    policy.verify(artifact, bundle, trust, "refs/tags/v0.1.0", "a" * 40, digest)
                with self.assertRaises(ValueError):
                    policy.verify(artifact, bundle, trust, "refs/heads/main", "a" * 40, digest)
                bundle.unlink()
                with self.assertRaises(OSError):
                    policy.verify(artifact, bundle, trust, "refs/tags/v0.1.0", "a" * 40, digest)


if __name__ == "__main__":
    unittest.main()
