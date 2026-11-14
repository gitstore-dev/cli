#!/usr/bin/env python3
"""Run the actual image library's opt-in test against an owned synthetic registry."""
import argparse
import json
from pathlib import Path
import selectors
import subprocess
import tempfile


TEST = "acquire::tests::fixture_streams_verify_platform_quota_and_unpack"
POLICY_TEST = "transport::tests::fixture_redirects_strip_credentials_and_reject_unapproved_origins"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry-fixture", required=True, type=Path)
    parser.add_argument("--sdk-test", required=True, type=Path)
    args = parser.parse_args()
    registry = args.registry_fixture.resolve(strict=True)
    test = args.sdk_test.resolve(strict=True)
    for mode in ("basic", "expired", "scope-denied", "policy"):
        run_fixture(registry, test, mode)
    with tempfile.TemporaryDirectory(prefix="gitstore-image-security-") as tmp:
        print("Image SDK unit/security suite", flush=True)
        subprocess.run([str(test)], check=True, timeout=30,
                       env={"PATH": "/nonexistent", "HOME": tmp, "TMPDIR": tmp})


def run_fixture(registry, test, mode):
    with tempfile.TemporaryDirectory(prefix="gitstore-image-sdk-") as tmp:
        env = {"PATH": "/nonexistent", "HOME": tmp, "TMPDIR": tmp}
        listed = subprocess.run([str(test), "--list", "--ignored"], env=env,
                                check=True, capture_output=True, timeout=5)
        selected = POLICY_TEST if mode == "policy" else TEST
        if f"{selected}: test" not in listed.stdout.decode():
            raise RuntimeError("Select the actual SDK test executable")
        command = ([str(registry), "serve-policy-fixture"] if mode == "policy" else
                   [str(registry), "serve-fixture"] if mode == "basic" else
                   [str(registry), "serve-auth-fixture", mode])
        fixture = subprocess.Popen(command, env=env,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL)
        try:
            with selectors.DefaultSelector() as ready:
                ready.register(fixture.stdout, selectors.EVENT_READ)
                if not ready.select(timeout=5):
                    raise RuntimeError("Synthetic registry startup deadline")
                line = fixture.stdout.readline(16385)
            if len(line) > 16384:
                raise RuntimeError("Fixture metadata bound")
            info = json.loads(line)
            if mode != "policy" and (not isinstance(info, dict) or not all(
                    isinstance(info.get(key), str) for key in ("reference", "configDigest"))):
                raise RuntimeError("Registry fixture metadata is incompatible; rebuild the selected developer fixture")
            if mode == "policy":
                env["GITSTORE_TEST_IMAGE_REDIRECT_FIXTURE"] = json.dumps(info)
            else:
                env.update({"GITSTORE_TEST_IMAGE_REFERENCE": info["reference"],
                            "GITSTORE_TEST_IMAGE_CONFIG_DIGEST": info["configDigest"]})
                if mode != "basic":
                    env["GITSTORE_TEST_IMAGE_AUTH_FAILURE"] = "1"
            print(f"Synthetic image registry case: {mode}", flush=True)
            subprocess.run([str(test), "--exact", selected, "--ignored"], env=env,
                           check=True, timeout=15)
        finally:
            fixture.stdin.close()
            try:
                status = fixture.wait(timeout=3)
            except subprocess.TimeoutExpired:
                fixture.kill()
                fixture.wait(timeout=3)
                raise RuntimeError("Synthetic registry shutdown deadline") from None
            if status:
                raise RuntimeError("Synthetic registry did not observe the intended authorization flow")


if __name__ == "__main__":
    main()
