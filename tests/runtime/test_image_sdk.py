"""Explicit image code-library tests, separate from public native CLI evidence."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest


class ImageSdk(unittest.TestCase):
    def test_actual_image_library_against_synthetic_private_registry(self):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_text())
        repository = Path(__file__).resolve().parents[2]
        subprocess.run([
            sys.executable,
            str(repository / "tests/runtime/image-library/test_sdk.py"),
            "--sdk-test", settings["imageSdkTest"],
            "--registry-fixture", settings["registryFixture"],
        ], check=True, timeout=30, env={"PATH": "/nonexistent"})


if __name__ == "__main__":
    unittest.main()
