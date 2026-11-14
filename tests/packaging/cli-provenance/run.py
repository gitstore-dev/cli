#!/usr/bin/env python3
"""Real offline gh verification controls; never publishes or executes inputs."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

INPUTS = {
    "sigstore-js-2.1.0.tgz": "f0c641fce0310cb43208f4272dc1423153630e44ba8cdc0b5ce8466f724e0eed",
    "sigstore-js-2.1.0-bundle.json": "cfadddd579b9fb45947530cd0e860986ec610a28f7fa6de3ef31cc051f07f761",
    "trusted_root.json": "455b3fe53e2678889f093d66ebbfda5f12ebb9d682b146c3535fa04e97689787",
}
REVISION = "4f2cfe92e3499f8581bf39db77e0a1431fa7b55c"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", required=True, type=Path)
    parser.add_argument("--cli-artifact", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    gh = shutil.which("gh")
    if not gh:
        parser.error("Select an installed maintainer gh; no automatic download")
    if not args.output.is_absolute() or args.output.exists():
        parser.error("Select a new absolute evidence output")
    for name, digest in INPUTS.items():
        path = args.inputs / name
        if path.is_symlink() or path.stat().st_size > 65536:
            parser.error("Select bounded ordinary pinned fixture inputs")
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            parser.error("Fixture digest differs from pinned upstream input")
    record = {"schemaVersion": 1, "scope": "maintainer offline verification experiment; no GitStore release verified",
              "upstream": "cli/cli", "revision": REVISION, "inputs": INPUTS,
              "ghVersion": subprocess.check_output([gh, "--version"], text=True).splitlines()[0],
              "cases": {}}
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="gitstore-provenance-controls-") as directory:
        root = Path(directory)
        for name in INPUTS:
            shutil.copyfile(args.inputs / name, root / name)
        (root / "tampered.tgz").write_bytes((root / "sigstore-js-2.1.0.tgz").read_bytes() + b"tampered")
        bundle = json.loads((root / "sigstore-js-2.1.0-bundle.json").read_bytes())
        signature = bundle["dsseEnvelope"]["signatures"][0]["sig"]
        bundle["dsseEnvelope"]["signatures"][0]["sig"] = ("A" if signature[0] != "A" else "B") + signature[1:]
        (root / "tampered-bundle.json").write_text(json.dumps(bundle))
        environment = {"PATH": "/usr/bin:/bin", "HOME": str(root), "GH_CONFIG_DIR": str(root / "gh"),
                       "GH_TOKEN": "offline-verification-placeholder", "GH_PROMPT_DISABLED": "1",
                       "GH_NO_UPDATE_NOTIFIER": "1", "GH_NO_EXTENSION_UPDATE_NOTIFIER": "1",
                       "HTTP_PROXY": "http://127.0.0.1:9", "HTTPS_PROXY": "http://127.0.0.1:9",
                       "LANG": "C", "NO_COLOR": "1"}
        base = {"artifact": "sigstore-js-2.1.0.tgz", "repo": "sigstore/sigstore-js",
                "workflow": "sigstore/sigstore-js/.github/workflows/release.yml",
                "ref": "refs/heads/main", "source": "26d16513386ffaa790b1c32f927544f1322e4194",
                "bundle": "sigstore-js-2.1.0-bundle.json", "issuer": "https://token.actions.githubusercontent.com"}
        cases = {"signed-positive-control": {}, "wrong-producer": {"repo": "gitstore-dev/cli"},
                 "wrong-workflow": {"workflow": "sigstore/sigstore-js/.github/workflows/other.yml"},
                 "wrong-source-ref": {"ref": "refs/tags/v0.0.0"}, "wrong-source-revision": {"source": "a" * 40},
                 "wrong-issuer": {"issuer": "https://example.invalid"},
                 "tampered-artifact": {"artifact": "tampered.tgz"},
                 "tampered-signature": {"bundle": "tampered-bundle.json"},
                 "missing-bundle": {"bundle": "missing.json"}}
        for name, changes in cases.items():
            case = {**base, **changes}
            command = [gh, "attestation", "verify", str(root / case["artifact"]),
                       "--repo", case["repo"], "--digest-alg", "sha512",
                       "--signer-workflow", case["workflow"], "--source-ref", case["ref"],
                       "--source-digest", case["source"], "--cert-oidc-issuer", case["issuer"],
                       "--deny-self-hosted-runners", "--bundle", str(root / case["bundle"]),
                       "--custom-trusted-root", str(root / "trusted_root.json"), "--format", "json"]
            result = subprocess.run(command, env=environment, stdin=subprocess.DEVNULL,
                                    capture_output=True, timeout=60, check=False)
            expected = name == "signed-positive-control"
            passed = (result.returncode == 0) == expected
            record["cases"][name] = {"accepted": result.returncode == 0, "passed": passed}
            if expected and result.returncode == 0:
                certificate = json.loads(result.stdout)[0]["verificationResult"]["signature"]["certificate"]
                record["positiveRepositoryId"] = certificate["sourceRepositoryIdentifier"]
        path = Path(__file__).resolve().parents[3] / "runtime/packaging/release_verification.py"
        spec = importlib.util.spec_from_file_location("release_verification", path)
        policy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(policy)
        artifact_hash = hashlib.sha256(args.cli_artifact.read_bytes()).hexdigest()
        try:
            policy.verify(args.cli_artifact, root / base["bundle"], root / "trusted_root.json",
                          "refs/tags/v0.1.0", "a" * 40, artifact_hash)
            rejected = False
        except (OSError, ValueError, subprocess.SubprocessError):
            rejected = True
        record["cases"]["unsigned-own-cli"] = {"accepted": not rejected, "passed": rejected}
        record["unsignedCliSha256"] = artifact_hash
    record["passed"] = all(case["passed"] for case in record["cases"].values())
    record["elapsedSeconds"] = round(time.monotonic() - started, 3)
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w") as stream:
        json.dump(record, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    print(json.dumps({"passed": record["passed"], "record": str(args.output), "cases": len(record["cases"])}))
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
