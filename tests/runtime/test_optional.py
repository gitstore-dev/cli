"""Component-specific public preflight failures; no substitute OIDC readiness."""
import unittest
from support import Fixture


class Optional(unittest.TestCase):
    def test_unavailable_components_fail_before_runtime_effects(self):
        f = Fixture()
        self.addCleanup(f.close)
        before = f.call("deployment", "list")
        for component, prerequisite in (("oidc", b"SMTP"), ("admin", b"published image"),
                                        ("storefront", b"published image")):
            for prefix in ((component, "serve"), ("serve", "--add", component)):
                with self.subTest(component=component, prefix=prefix):
                    result = f.call("--non-interactive", *prefix, "--deployment", "optional",
                                    "--allow-provision", "--detach", success=False)
                    self.assertEqual(result.returncode, 6)
                    self.assertIn(prerequisite, result.stderr)
                    self.assertIn(b"UNSUPPORTED", result.stderr)
                    self.assertEqual(f.assets(), {})
                    self.assertEqual(list((f.root / "credentials").iterdir()), [])
                    self.assertEqual(f.call("deployment", "list"), before)

    def test_duplicate_selection_is_usage_error_before_capability_failure(self):
        f = Fixture()
        self.addCleanup(f.close)
        result = f.call("serve", "--add", "admin", "--add", "admin", success=False)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(f.assets(), {})


if __name__ == "__main__":
    unittest.main()
