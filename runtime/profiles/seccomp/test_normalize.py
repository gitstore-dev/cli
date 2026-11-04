import copy
import unittest
from normalize import locked_source, normalize


class PolicyTests(unittest.TestCase):
    def test_native_zero_capability_policies_deny_privileged_namespace_operations(self):
        for arch in ("amd64", "arm64"):
            policy = normalize(locked_source(), arch)
            allowed = {name for rule in policy["syscalls"] if rule["action"] == "SCMP_ACT_ALLOW"
                       for name in rule["names"]}
            self.assertFalse(allowed & {"bpf", "mount", "setns", "unshare", "keyctl", "reboot"})
            self.assertTrue({"read", "write", "execve", "clone"} <= allowed)
            self.assertFalse("clone3" in allowed)
            clone3 = [rule for rule in policy["syscalls"] if "clone3" in rule["names"]]
            self.assertEqual([(x["action"], x["errnoRet"]) for x in clone3], [("SCMP_ACT_ERRNO", 38)])
            self.assertTrue(all(not ({"includes", "excludes", "comment"} & set(rule))
                                for rule in policy["syscalls"]))

    def test_architecture_predicates_are_not_silently_ignored(self):
        def allowed(arch):
            return {name for rule in normalize(locked_source(), arch)["syscalls"]
                    if rule["action"] == "SCMP_ACT_ALLOW" for name in rule["names"]}
        self.assertIn("arch_prctl", allowed("amd64"))
        self.assertNotIn("arch_prctl", allowed("arm64"))

    def test_unknown_predicate_or_architecture_fails(self):
        source = copy.deepcopy(locked_source())
        source["syscalls"][0]["includes"] = {"futurePredicate": True}
        with self.assertRaises(ValueError):
            normalize(source, "arm64")
        with self.assertRaises(ValueError):
            normalize(locked_source(), "unknown")


if __name__ == "__main__":
    unittest.main()
