"""Real CLI provisioning/cache failures; no synthetic startup success substitutes."""
import unittest
from support import Fixture


class Assets(unittest.TestCase):
    def setUp(self):
        offline = {"test_offline_layouts_authorize_only_the_selected_verified_bytes": "verified",
                   "test_warm_restart_reuses_assets_and_existing_private_material": "verified",
                   "test_tampered_warm_blob_cannot_be_reused_for_execution": "verified",
                   "test_wrong_platform_offline_input_never_activates": "wrong-platform",
                   "test_interrupted_offline_blob_never_activates": "truncated"}.get(self._testMethodName)
        self.fixture = Fixture(offline=offline)
        self.addCleanup(self.fixture.close)

    def test_no_input_without_permission_downloads_nothing(self):
        f = self.fixture
        self.assertEqual(f.assets(), {})
        result=f.call("--non-interactive", "serve", "--deployment", "denied", "--detach", success=False)
        self.assertIn(b"PROVISIONING_REQUIRED",result.stderr)
        self.assertEqual(f.assets(), {})
        self.assertEqual(list((f.root / "credentials").iterdir()), [])

    def test_warm_restart_reuses_assets_and_existing_private_material(self):
        f = self.fixture
        first = f.start("warm")
        old = f.ready("warm", first)
        assets = f.assets()
        material = f.digest(f.credential_path("warm"))
        f.stop("warm")
        second = f.start("warm", provision=False, key="warm-restart")
        new = f.ready("warm", second)
        self.assertGreater(new["generation"], old["generation"])
        self.assertEqual(assets, f.assets())
        self.assertEqual(material, f.digest(f.credential_path("warm")))

    def test_offline_layouts_authorize_only_the_selected_verified_bytes(self):
        f = self.fixture
        self.assertIsNotNone(f.offline_layouts, "Select three verified offline core OCI layouts")
        accepted = f.start("offline", provision=False)
        self.assertEqual(f.ready("offline", accepted)["readiness"], "ready")

    def test_wrong_platform_offline_input_never_activates(self):
        f = self.fixture
        self.assertIsNotNone(f.wrong_platform_layouts, "Select a verified other-platform OCI fixture")
        f.failed_start("wrong-platform")
        self.assertEqual(f.active_components(), set())

    def test_interrupted_offline_blob_never_activates(self):
        f = self.fixture
        self.assertIsNotNone(f.offline_layouts, "Select verified core OCI inputs before making a private truncated copy")
        f.failed_start("interrupted")
        self.assertNotIn("api", f.active_components())
        self.assertEqual(list((f.root / "credentials").iterdir()), [])

    def test_tampered_warm_blob_cannot_be_reused_for_execution(self):
        f = self.fixture
        accepted = f.start("tamper")
        f.ready("tamper", accepted)
        f.stop("tamper")
        candidates = [p for p in (f.root / "state/assets").rglob("*")
                      if p.is_file() and not p.is_symlink() and "blobs" in p.parts and p.stat().st_size]
        self.assertTrue(candidates, "Expected the verified compressed cache blobs")
        blob = candidates[0]
        self.assertEqual(blob.stat().st_nlink, 1)
        mode = blob.stat().st_mode & 0o7777
        blob.chmod(0o600)
        with blob.open("r+b") as stream:
            value = stream.read(1)
            stream.seek(0)
            stream.write(bytes([value[0] ^ 1]))
        blob.chmod(mode)
        f.failed_start("tamper", key="tampered-restart")
        deployment = f.call("deployment", "inspect", "tamper")
        self.assertNotEqual(deployment["readiness"], "ready")


if __name__ == "__main__":
    unittest.main()
