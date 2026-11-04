"""Opt-in real host precondition denial; host mutation belongs to fixture tooling."""
import os
import time
import unittest
from support import Fixture


@unittest.skipUnless(os.environ.get("GITSTORE_EXPECT_MISSING_TUN") == "1",
                     "Select a disposable fixture where invoking UID cannot open TUN")
class Host(unittest.TestCase):
    def test_tun_denial_is_exact_and_precedes_provisioning(self):
        f = Fixture()
        self.addCleanup(f.close)
        accepted = f.start("missing-tun")
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            selected = f.call("deployment", "inspect", "missing-tun", "--operation", accepted["operationId"])
            if selected["operation"]["state"] in ("succeeded", "failed", "cancelled"):
                break
            time.sleep(0.05)
        else:
            self.fail("Host preflight did not finish within its worker bound")
        self.assertEqual(selected["operation"]["state"], "failed")
        self.assertEqual(selected["operation"]["error"]["code"], "UNSUPPORTED_HOST")
        self.assertEqual(selected["operation"]["error"]["requirement"], "tun")
        self.assertEqual(f.assets(), {})
        self.assertEqual(list((f.root / "credentials").iterdir()), [])
        self.assertFalse(any(r["resourceType"] == "workload" for r in selected["deployment"]["resources"]))


if __name__ == "__main__":
    unittest.main()
