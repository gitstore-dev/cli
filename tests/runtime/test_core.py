"""Real public CLI acceptance; opt in with GITSTORE_RUNTIME_FIXTURE (see README)."""
import signal
import os
import select
import subprocess
import time
import unittest
from support import Fixture, DropStartAck


class Core(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture(unsupported=self._testMethodName == "test_missing_execution_capability_fails_before_effects")
        self.addCleanup(self.fixture.close)

    def test_authorized_cold_start_requires_current_generation_health(self):
        f = self.fixture
        self.assertEqual(f.assets(), {})
        operation = f.start("cold")
        deployment = f.ready("cold", operation)
        self.assertTrue(f.endpoints(deployment))
        self.assertTrue(f.assets())
        # PATH contains no Git, crun, curl, gh, Docker or language toolchain.
        self.assertEqual(f.environment["PATH"], "/nonexistent")

    def test_deployments_have_separate_volumes_names_and_loopback_ports(self):
        f = self.fixture
        first = f.start("first")
        second = f.start("second")
        a, b = f.ready("first", first), f.ready("second", second)
        self.assertTrue(f.endpoints(a).isdisjoint(f.endpoints(b)))
        a_ids = {r["resourceId"] for r in a["resources"]}
        b_ids = {r["resourceId"] for r in b["resources"]}
        self.assertTrue(a_ids and b_ids and a_ids.isdisjoint(b_ids))
        self.assertTrue(any(r["resourceType"] == "repository-volume" for r in a["resources"]))

    def test_interactive_cold_start_requires_displayed_asset_approval(self):
        f = self.fixture
        f.deployments.add("interactive")
        master, slave = os.openpty()
        process = subprocess.Popen(f.command("serve", "--deployment", "interactive", "--detach",
                                    "--idempotency-key", "interactive-start"), env=f.environment,
                                   stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave)
        self.addCleanup(os.close, master)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        buffer = bytearray()
        approved = False
        deadline = time.monotonic() + 90
        while process.poll() is None:
            self.assertLess(time.monotonic(), deadline, "Interactive provisioning did not complete")
            if select.select([master], [], [], 0.1)[0]:
                try:
                    buffer.extend(os.read(master, 65536))
                except OSError:
                    break
            self.assertLess(len(buffer), 1048576, "Unbounded terminal output")
            if not approved and b"Provision these assets? [y/N]" in buffer:
                self.assertTrue(b"ghcr.io/gitstore-dev/" in buffer and b"sha256:" in buffer,
                                "Approval omitted immutable asset source/identity")
                os.write(master, b"y\n")
                approved = True
        process.wait(timeout=10)
        self.assertTrue(approved, "No explicit provisioning approval was requested")
        self.assertEqual(process.returncode, 0)
        deployment = f.call("deployment", "inspect", "interactive")
        self.assertTrue(deployment.get("lastOperationId"))
        f.ready("interactive", {"operationId": deployment["lastOperationId"]})

    def test_dropped_durable_start_ack_replays_the_original_operation(self):
        f = self.fixture
        proxy = DropStartAck(f)
        self.addCleanup(proxy.close)
        f.deployments.add("acknowledgment")
        subprocess.run(f.command("--timeout", "10s", "--non-interactive", "serve", "--deployment",
                       "acknowledgment", "--allow-provision", "--detach", "--idempotency-key", "drop-start-ack",
                       socket=proxy.path), env=f.environment, stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        self.assertTrue(proxy.finished.wait(16), "Proxy never completed its owned exchange")
        self.assertIsNone(proxy.error, "Proxy failed before observing the durable ACK")
        self.assertTrue(proxy.operation, "The manager did not return a durable operation identity")
        replay = f.start("acknowledgment", key="drop-start-ack")
        self.assertEqual(proxy.operation, replay["operationId"])
        f.ready("acknowledgment", replay)

    def test_detached_exit_and_same_key_reconnect_preserve_operation(self):
        f = self.fixture
        accepted = f.start("detached", key="lost-client-ack")
        # Original CLI has exited. Reconnect with the identical semantic intent.
        replay = f.start("detached", key="lost-client-ack")
        self.assertEqual(accepted["operationId"], replay["operationId"])
        deployment = f.ready("detached", replay)
        self.assertEqual(deployment["desiredState"], "running")
        f.stop("detached")
        stopped = f.call("deployment", "inspect", "detached")
        self.assertEqual(stopped["desiredState"], "stopped")
        self.assertTrue(any(r["resourceType"] == "repository-volume" for r in stopped["resources"]))

    def test_foreground_interrupt_requests_owned_cleanup(self):
        f = self.fixture
        f.deployments.add("foreground")
        process = subprocess.Popen(f.command("--non-interactive", "serve", "--deployment", "foreground",
                                    "--allow-provision", "--idempotency-key", "foreground-start"),
                                   env=f.environment, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        deadline = time.monotonic() + 75
        while True:
            self.assertIsNone(process.poll(), "Foreground CLI exited before an interrupt")
            try:
                deployment = f.call("deployment", "inspect", "foreground", timeout=5)
                if deployment["readiness"] == "ready":
                    break
            except AssertionError:
                pass
            self.assertLess(time.monotonic(), deadline, "Foreground deployment never became ready")
            time.sleep(0.1)
        process.send_signal(signal.SIGINT)
        process.wait(timeout=40)
        stopped = f.call("deployment", "inspect", "foreground")
        self.assertEqual(stopped["desiredState"], "stopped")
        self.assertNotEqual(stopped["writerOwnership"], "uncertain")

    def test_unavailable_optional_mapping_has_no_asset_side_effect(self):
        f = self.fixture
        before = f.assets()
        f.call("--non-interactive", "serve", "--deployment", "unsupported", "--add", "storefront",
               "--allow-provision", "--detach", success=False)
        self.assertEqual(before, f.assets())

    def test_missing_execution_capability_fails_before_effects(self):
        f = self.fixture
        result = f.call("--non-interactive", "serve", "--deployment", "missing-host", "--allow-provision",
                        "--detach", success=False)
        self.assertIn(b"UNSUPPORTED_CAPABILITY", result.stderr)
        self.assertEqual(f.assets(), {})
        self.assertEqual(list((f.root / "credentials").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
