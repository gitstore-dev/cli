#!/usr/bin/env python3
"""Maintainer-only offline release gate. Never imported by gs/gitstored."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile

REPOSITORY = "gitstore-dev/cli"
REPOSITORY_ID = "1431005655"
WORKFLOW = REPOSITORY + "/.github/workflows/release.yml"
PREDICATE = "https://slsa.dev/provenance/v1"


def snapshot(source, destination, maximum):
    """Pin ordinary input bytes; verifier never executes the supplied artifact."""
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= maximum:
            raise ValueError("input is not a bounded ordinary file")
        with os.fdopen(os.dup(fd), "rb") as stream:
            payload = stream.read(maximum + 1)
        after = os.fstat(fd)
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
        if identity(before) != identity(after) or len(payload) != before.st_size:
            raise ValueError("input changed during verification")
        destination.write_bytes(payload)
        destination.chmod(0o400)
        return hashlib.sha256(payload).hexdigest()
    finally:
        os.close(fd)


def repository_identity(results):
    # These are the cryptographically verified certificate claims returned by gh,
    # not workflow-controlled statement.predicate repository strings.
    if not isinstance(results, list) or not 0 < len(results) <= 128:
        return False
    for result in results:
        try:
            certificate = result["verificationResult"]["signature"]["certificate"]
            if certificate["sourceRepositoryIdentifier"] != REPOSITORY_ID:
                return False
        except (KeyError, TypeError):
            return False
    return True


def verify(artifact, bundle, trusted_root, source_ref, source_digest, expected_sha256):
    if not re.fullmatch(r"refs/tags/v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", source_ref):
        raise ValueError("select an exact version tag ref")
    if not re.fullmatch(r"[0-9a-f]{40}", source_digest):
        raise ValueError("select the reviewed full source revision")
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise ValueError("select the expected artifact SHA-256")
    gh = shutil.which("gh")
    if not gh:
        raise ValueError("maintainer GitHub CLI is unavailable")
    with tempfile.TemporaryDirectory(prefix="gitstore-release-verification-") as directory:
        root = Path(directory)
        digest = snapshot(artifact, root / "artifact", 128 * 1024 * 1024)
        if digest != expected_sha256:
            raise ValueError("artifact digest mismatch")
        snapshot(bundle, root / "bundle.jsonl", 4 * 1024 * 1024)
        snapshot(trusted_root, root / "trusted_root.jsonl", 4 * 1024 * 1024)
        command = [gh, "attestation", "verify", str(root / "artifact"),
                   "--repo", REPOSITORY, "--signer-workflow", WORKFLOW,
                   "--cert-identity", f"https://github.com/{WORKFLOW}@{source_ref}",
                   "--cert-oidc-issuer", "https://token.actions.githubusercontent.com",
                   "--source-ref", source_ref, "--source-digest", source_digest,
                   "--deny-self-hosted-runners", "--predicate-type", PREDICATE,
                   "--bundle", str(root / "bundle.jsonl"),
                   "--custom-trusted-root", str(root / "trusted_root.jsonl"), "--format", "json"]
        # Offline verification uses no personal credentials or ambient gh config.
        # This placeholder only bypasses gh's login preflight; it grants no access.
        environment = {"PATH": "/usr/bin:/bin", "HOME": str(root),
                       "GH_CONFIG_DIR": str(root / "gh"), "GH_HOST": "github.com",
                       "GH_TOKEN": "offline-verification-placeholder", "GH_PROMPT_DISABLED": "1",
                       "LANG": "C", "NO_COLOR": "1"}
        result = subprocess.run(command, env=environment, stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=60, check=False)
        if result.returncode or len(result.stdout) > 16 * 1024 * 1024:
            raise ValueError("cryptographic provenance or producer policy verification failed")
        if not repository_identity(json.loads(result.stdout)):
            raise ValueError("verified repository ID mismatch or missing certificate identity")
        return {"verified": True, "repository": REPOSITORY, "repositoryId": REPOSITORY_ID,
                "workflow": WORKFLOW, "sourceRef": source_ref,
                "sourceDigest": source_digest, "sha256": digest}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("artifact", "bundle", "trusted-root"):
        parser.add_argument("--" + name, required=True, type=Path)
    for name in ("source-ref", "source-digest", "expected-sha256"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    try:
        result = verify(args.artifact, args.bundle, args.trusted_root,
                        args.source_ref, args.source_digest, args.expected_sha256)
    except (OSError, ValueError, subprocess.SubprocessError):
        print(json.dumps({"verified": False, "code": "RELEASE_PROVENANCE_REJECTED"}))
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
