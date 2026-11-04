"""Real public manager-loss recovery; retained roots and synthetic credentials only."""
import json
import os
from pathlib import Path
import sqlite3
import signal
import time
import unittest
from support import Fixture


def process_identity(pid):
    path = Path("/proc") / str(pid)
    if path.stat().st_uid != os.geteuid():
        raise AssertionError("Recorded workload is not owned by the fixture account")
    with (path / "stat").open("rb") as stream:
        data = stream.read(4097)
    if len(data) > 4096:
        raise AssertionError("Unexpected process identity size")
    return int(data[data.rindex(b")") + 2:].split()[19])


class Recovery(unittest.TestCase):
    def test_killed_manager_reopens_owned_resources_for_public_stop(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        accepted = f.start("recovery", key="before-manager-loss")
        ready = f.ready("recovery", accepted)
        terminal = f.call("deployment", "inspect", "recovery", "--operation", accepted["operationId"])["operation"]
        material = f.credential_path("recovery")
        material_hash = f.digest(material)
        assets = f.assets()
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
            native = [json.loads(row[0])["native"] for row in database.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' AND ownership_state='held'")]
            volume, device, inode = database.execute(
                "SELECT volume_id,data_device,data_inode FROM repository_volumes WHERE allocated=1").fetchone()
        self.assertEqual(len(native), 3)
        self.assertRegex(volume, r"\Arepo-[0-9a-f]{56}\Z")
        data = f.root / f"state/volumes/volume-{volume}/data"
        self.assertEqual((data.stat().st_dev, data.stat().st_ino), (int(device), int(inode)))
        for record in native:
            self.assertEqual(process_identity(record["pid"]), record["startTime"])
        api = next(record for record in native if record["component"] == "api")
        api_fd = os.pidfd_open(api["pid"])
        self.addCleanup(os.close, api_fd)
        self.assertEqual(process_identity(api["pid"]), api["startTime"])
        paused = [True]
        def resume():
            if paused[0]:
                try:
                    signal.pidfd_send_signal(api_fd, signal.SIGCONT)
                except ProcessLookupError:
                    pass
                paused[0] = False
        self.addCleanup(resume)
        signal.pidfd_send_signal(api_fd, signal.SIGSTOP)
        deadline = time.monotonic() + 15
        while True:
            with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
                observing, = database.execute("SELECT state='running' FROM jobs WHERE operation_id=?",
                                               (accepted["operationId"],)).fetchone()
            if observing:
                break
            self.assertLess(time.monotonic(), deadline, "No bounded read-only health lease was observed")
            time.sleep(0.01)
        f.process.kill()
        f.process.wait(timeout=5)
        resume()
        live = []
        for record in native:
            try:
                live.append(process_identity(record["pid"]) == record["startTime"])
            except FileNotFoundError:
                live.append(False)
        # Always bring back the owner before an assertion can enter teardown.
        f.start_manager()
        self.assertTrue(all(live), "Core workload identity did not survive manager loss")
        uncertain = f.call("deployment", "inspect", "recovery")
        self.assertNotEqual(uncertain["readiness"], "ready")
        self.assertEqual(uncertain["recovery"], "uncertain")
        self.assertEqual(f.call("deployment", "inspect", "recovery", "--operation", accepted["operationId"])["operation"], terminal)
        replay = f.start("recovery", key="before-manager-loss")
        self.assertEqual(replay["operationId"], accepted["operationId"])
        f.stop("recovery", key="stop-after-manager-loss")
        stopped = f.call("deployment", "inspect", "recovery")
        self.assertEqual(stopped["desiredState"], "stopped")
        self.assertEqual(stopped["writerOwnership"], "released")
        self.assertEqual(stopped["recovery"], "normal")
        self.assertEqual((data.stat().st_dev, data.stat().st_ino), (int(device), int(inode)))
        self.assertEqual(f.digest(material), material_hash)
        self.assertEqual(f.assets(), assets)
        again = f.start("recovery", provision=False, key="restart-after-observed-stop")
        restarted = f.ready("recovery", again)
        self.assertGreater(restarted["generation"], ready["generation"])
        self.assertEqual((data.stat().st_dev, data.stat().st_ino), (int(device), int(inode)))
        self.assertEqual(f.digest(material), material_hash)
        self.assertEqual(f.assets(), assets)
        f.stop("recovery", key="final-observed-stop")

    @staticmethod
    def api_identity(f):
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
            records = [json.loads(row[0])["native"] for row in database.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' AND ownership_state='held'")]
        return next(record for record in records if record["component"] == "api")

    def test_workload_exit_invalidates_readiness_and_preserves_start_history(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        accepted = f.start("exit")
        f.ready("exit", accepted)
        terminal = f.call("deployment", "inspect", "exit", "--operation", accepted["operationId"])["operation"]
        record = self.api_identity(f)
        fd = os.pidfd_open(record["pid"])
        self.addCleanup(os.close, fd)
        self.assertEqual(process_identity(record["pid"]), record["startTime"])
        signal.pidfd_send_signal(fd, signal.SIGKILL)
        deadline = time.monotonic() + 10
        while f.call("deployment", "inspect", "exit")["readiness"] == "ready":
            self.assertLess(time.monotonic(), deadline, "Dead workload left current readiness ready")
            time.sleep(0.02)
        self.assertEqual(f.call("deployment", "inspect", "exit", "--operation", accepted["operationId"])["operation"], terminal)
        f.call("--timeout", "2s", "--wait", "ready", "deployment", "inspect", "exit",
               "--operation", accepted["operationId"], success=False, timeout=5)
        f.stop("exit", key="observed-stop-after-api-exit")
        self.assertEqual(f.call("deployment", "inspect", "exit")["writerOwnership"], "released")

    def test_paused_live_service_loses_and_regains_readiness_without_restart(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        accepted = f.start("health")
        initial = f.ready("health", accepted)
        terminal = f.call("deployment", "inspect", "health", "--operation", accepted["operationId"])["operation"]
        material = f.credential_path("health")
        digest = f.digest(material)
        record = self.api_identity(f)
        fd = os.pidfd_open(record["pid"])
        self.addCleanup(os.close, fd)
        self.assertEqual(process_identity(record["pid"]), record["startTime"])
        paused = [True]
        def resume():
            if paused[0]:
                try:
                    signal.pidfd_send_signal(fd, signal.SIGCONT)
                except ProcessLookupError:
                    pass
                paused[0] = False
        self.addCleanup(resume)
        signal.pidfd_send_signal(fd, signal.SIGSTOP)
        deadline = time.monotonic() + 25
        while f.call("deployment", "inspect", "health")["readiness"] == "ready":
            self.assertLess(time.monotonic(), deadline, "Live but stalled API left readiness ready")
            time.sleep(0.02)
        self.assertEqual(process_identity(record["pid"]), record["startTime"])
        resume()
        f.ready_timeout = 35
        recovered = f.ready("health", accepted)
        self.assertEqual(recovered["generation"], initial["generation"])
        self.assertEqual(process_identity(record["pid"]), record["startTime"])
        self.assertEqual(f.digest(material), digest)
        self.assertEqual(f.call("deployment", "inspect", "health", "--operation", accepted["operationId"])["operation"], terminal)
        f.stop("health", key="observed-stop-after-health-recovery")


if __name__ == "__main__":
    unittest.main()
