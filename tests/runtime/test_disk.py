"""Bounded actual ENOSPC, with only test-owned user/mount namespaces."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class Disk(unittest.TestCase):
    def test_capacity_preflight_and_full_volume_preserve_existing_bytes(self):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_text())
        root = Path(tempfile.mkdtemp(prefix="runtime-disk-", dir=settings["fixtureParent"]))
        root.chmod(0o700)
        print(f"Retained disk-probe root: {root}", flush=True)
        # Probe mounts only a 1 MiB private tmpfs. It never fills the host disk;
        # mount and synthetic data cease to exist with its private namespace.
        result = subprocess.run([settings["volumeDiskProbe"], str(root)], check=True,
                                capture_output=True, timeout=30, env={"PATH": "/nonexistent"})
        evidence = json.loads(result.stdout)
        self.assertEqual(evidence["volumeFull"], "allocation-rejected")
        self.assertEqual(evidence["retainedBytes"], "preserved")
        self.assertEqual(evidence["datastoreFull"], "allocation-rejected")
        self.assertEqual(evidence["datastoreRetainedBytes"], "preserved")


if __name__ == "__main__":
    unittest.main()
