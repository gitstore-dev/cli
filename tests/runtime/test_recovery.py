"""Real public manager-loss recovery; retained roots and synthetic credentials only."""
import json
import fcntl
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import sqlite3
import signal
import selectors
import subprocess
import socket
import struct
import time
import unittest
from support import Fixture, DropStartAck


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
    def test_live_git_writer_blocks_concurrent_replacement_after_manager_loss(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        first = f.start("writer", provision=False, key="allocate-writer")
        f.ready("writer", first)
        f.stop("writer", key="stop-before-marker")
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
            volume, = db.execute("SELECT volume_id FROM repository_volumes WHERE allocated=1").fetchone()
        self.assertRegex(volume, r"\Arepo-[0-9a-f]{56}\Z")
        lock = os.open(f.root / f"state/volumes/writer-{volume}.lock", os.O_RDWR | os.O_NOFOLLOW)
        self.addCleanup(os.close, lock)
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        data = f.root / f"state/volumes/volume-{volume}/data"
        marker = data / ".recovery-writer-fixture"
        with marker.open("xb") as stream:
            os.fchmod(stream.fileno(), 0o600)
            stream.write(b"retained writer crash fixture\n")
            stream.flush()
            os.fsync(stream.fileno())
        marker_hash = f.digest(marker)
        lock_identity = os.fstat(lock)
        fcntl.flock(lock, fcntl.LOCK_UN)
        second = f.start("writer", provision=False, key="live-before-manager-loss")
        f.ready("writer", second)
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
            identities = [json.loads(row[0])["native"] for row in db.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' AND ownership_state='held'")]
        self.assertEqual(len(identities), 3)
        with self.assertRaises(BlockingIOError):
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        f.process.kill()
        f.process.wait(timeout=5)
        live = all(process_identity(item["pid"]) == item["startTime"] for item in identities)
        f.start_manager()  # restore the owner before assertions can enter teardown
        self.assertTrue(live)
        with self.assertRaises(BlockingIOError):
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        def replace(index):
            return f.call("--non-interactive", "serve", "--deployment", "writer", "--detach",
                          "--idempotency-key", f"concurrent-replacement-{index}", success=False, timeout=20)
        with ThreadPoolExecutor(max_workers=3) as peers:
            rejected = list(peers.map(replace, range(3)))
        for index, result in enumerate(rejected):
            diagnostic = f.root / f"replacement-diagnostic-{index}.json"
            with diagnostic.open("x") as stream:
                os.fchmod(stream.fileno(), 0o600)
                json.dump({"exit": result.returncode, "stderr": result.stderr.decode(errors="replace")[:8192],
                           "stdout": result.stdout.decode(errors="replace")[:8192]}, stream)
            try:
                response = json.loads(result.stderr.splitlines()[-1])
            except ValueError:
                self.fail(f"Replacement diagnostic is not a machine error; inspect {diagnostic}")
            self.assertIn(response["code"], {"OWNERSHIP_UNCERTAIN", "DEPLOYMENT_CONFLICT"})
            self.assertFalse(result.stdout)
        for item in identities:
            self.assertEqual(process_identity(item["pid"]), item["startTime"])
        self.assertEqual(f.digest(marker), marker_hash)
        f.stop("writer", key="observed-stop-after-conflicts")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        current_lock = os.stat(f.root / f"state/volumes/writer-{volume}.lock", follow_symlinks=False)
        self.assertEqual((current_lock.st_dev, current_lock.st_ino), (lock_identity.st_dev, lock_identity.st_ino))
        self.assertEqual(f.digest(marker), marker_hash)
        fcntl.flock(lock, fcntl.LOCK_UN)
        third = f.start("writer", provision=False, key="replacement-after-observed-stop")
        f.ready("writer", third)
        self.assertEqual(f.digest(marker), marker_hash)
        f.stop("writer", key="final-writer-stop")

    def test_public_log_replay_interrupt_preserves_workloads_and_history(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        accepted = f.start("logs")
        initial = f.ready("logs", accepted)
        terminal = f.call("deployment", "inspect", "logs", "--operation", accepted["operationId"])["operation"]
        material = f.credential_path("logs")
        material_hash = f.digest(material)
        native = self.api_identity(f)
        def observe(cursor=None):
            arguments = ["deployment", "logs", "logs", "--component", "api"]
            if cursor is not None:
                arguments += ["--cursor", cursor]
            process = subprocess.Popen(f.command(*arguments), env=f.environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    self.assertTrue(selector.select(10), "Public log replay did not produce a record")
                line = process.stdout.readline(1048577)
                self.assertTrue(0 < len(line) <= 1048576, "Public log event exceeded its frame bound")
                record = json.loads(line)
                self.assertEqual(record["type"], "log")
                self.assertEqual(record["deploymentId"], "logs")
                self.assertEqual(record["payload"]["component"], "api")
                self.assertIsInstance(record["payload"]["text"], str)
                process.send_signal(signal.SIGINT)
                remaining, diagnostic = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, "Standalone log interrupt failed")
                self.assertFalse(diagnostic, "Successful standalone observation wrote diagnostics")
                # Values remain private; failure output never prints a credential.
                credential = json.loads(material.read_bytes())
                for name in ("password", "passwordHash", "jwt", "hmac"):
                    value = credential.get(name)
                    if isinstance(value, str) and value:
                        self.assertFalse(value.encode() in line + remaining, "Credential appeared in public logs")
                return record["payload"]["cursor"]
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
        cursor = observe()
        observe(cursor)
        self.assertEqual(process_identity(native["pid"]), native["startTime"])
        current = f.call("deployment", "inspect", "logs")
        self.assertEqual(current["generation"], initial["generation"])
        self.assertEqual(current["desiredState"], "running")
        self.assertEqual(current["writerOwnership"], "held")
        self.assertEqual(f.call("deployment", "inspect", "logs", "--operation", accepted["operationId"])["operation"], terminal)
        self.assertEqual(f.digest(material), material_hash)
        expired = f.call("deployment", "logs", "logs", "--cursor", "log:expired:1", success=False, timeout=10)
        self.assertEqual(expired.returncode, 8)
        self.assertEqual(json.loads(expired.stderr)["code"], "CURSOR_EXPIRED")
        self.assertFalse(expired.stdout)
        f.stop("logs", key="explicit-stop-after-log-observation")

    def test_manager_loss_after_native_create_before_reply_recovers_partial_graph(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        material = f.credential_path("lost-create")
        accepted = f.start("lost-create", key="before-native-reply")
        frozen = [False]
        def resume_manager():
            if frozen[0] and f.process.poll() is None:
                f.process.send_signal(signal.SIGCONT)
                frozen[0] = False
        self.addCleanup(resume_manager)
        deadline = time.monotonic() + 60
        while True:
            with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
                # Pin committed state before stopping the writer. A new reader
                # after SIGSTOP can block behind the stopped writer's lock.
                database.execute("BEGIN")
                store, = database.execute("SELECT store_id FROM query_meta").fetchone()
                row = database.execute("SELECT resource_id,backend_identity FROM resource_ownership WHERE operation_id=? AND resource_type='workload' AND json_extract(backend_identity,'$.native.component')='api'", (accepted["operationId"],)).fetchone()
                start_state, = database.execute("SELECT state FROM operations WHERE operation_id=?", (accepted["operationId"],)).fetchone()
                self.assertNotIn(start_state, {"failed", "cancelled"}, "Startup failed before native CREATE; inspect the retained operation")
                native = json.loads(row[1]).get("native", {}) if row else {}
                if row and native.get("pid") == 0:
                    status_path = f.root / "state" / f"native-{store}" / row[0] / "status"
                    try:
                        status = json.loads(status_path.read_bytes())
                    except (FileNotFoundError, ValueError):
                        status = {}
                    if status.get("pid", 0) > 0:
                        f.process.send_signal(signal.SIGSTOP)
                        frozen[0] = True
                        stop_deadline = time.monotonic() + 3
                        while True:
                            raw = Path(f"/proc/{f.process.pid}/stat").read_bytes()
                            if raw[raw.rindex(b")") + 2:].split()[0] in {b"T", b"t"}:
                                break
                            self.assertLess(time.monotonic(), stop_deadline, "Owned manager did not enter the stopped state")
                            time.sleep(0.001)
                        record, = database.execute("SELECT backend_identity FROM resource_ownership WHERE resource_id=?", (row[0],)).fetchone()
                        source_state, = database.execute("SELECT state FROM operations WHERE operation_id=?", (accepted["operationId"],)).fetchone()
                        controller, = database.execute("SELECT backend_identity FROM resource_ownership WHERE operation_id=? AND resource_id LIKE 'run-controller-%'", (accepted["operationId"],)).fetchone()
                        break
            self.assertLess(time.monotonic(), deadline, "Did not observe native CREATE before its manager reply")
            time.sleep(0.001)
        self.assertEqual(source_state, "running")
        self.assertEqual(json.loads(record)["native"]["pid"], 0, "Manager already committed the native reply")
        self.assertNotIn("native", json.loads(controller))
        self.assertEqual(process_identity(status["pid"]), status["process-start-time"])
        material_hash = f.digest(material)
        assets = f.assets()
        f.process.kill()
        f.process.wait(timeout=5)
        frozen[0] = False
        self.assertEqual(process_identity(status["pid"]), status["process-start-time"])
        f.start_manager()
        failed = f.call("deployment", "inspect", "lost-create", "--operation", accepted["operationId"])["operation"]
        self.assertEqual(failed["state"], "failed")
        self.assertEqual(failed["error"]["code"], "OWNERSHIP_UNCERTAIN")
        self.assertEqual(f.start("lost-create", key="before-native-reply")["operationId"], accepted["operationId"])
        f.stop("lost-create", key="stop-discovered-partial-graph")
        stopped = f.call("deployment", "inspect", "lost-create")
        self.assertEqual(stopped["writerOwnership"], "released")
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
            recovered, = database.execute("SELECT backend_identity FROM resource_ownership WHERE resource_id=?", (row[0],)).fetchone()
        self.assertEqual(json.loads(recovered)["native"]["pid"], status["pid"])
        self.assertEqual(json.loads(recovered)["native"]["startTime"], status["process-start-time"])
        self.assertEqual(f.call("deployment", "inspect", "lost-create", "--operation", accepted["operationId"])["operation"], failed)
        self.assertEqual(f.digest(material), material_hash)
        self.assertEqual(f.assets(), assets)

    def test_active_cancel_deadline_retains_ownership_and_terminal_replay(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        accepted = f.start("cancel-start", key="active-start")
        deadline = time.monotonic() + 60
        while True:
            with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
                rows = database.execute("SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' AND backend_identity IS NOT NULL").fetchall()
            records = [json.loads(row[0]).get("native", {}) for row in rows]
            api = next((record for record in records if record.get("component") == "api" and record.get("pid", 0) > 0), None)
            if api:
                break
            self.assertLess(time.monotonic(), deadline, "No recorded API init appeared")
            time.sleep(0.005)
        fd = os.pidfd_open(api["pid"])
        self.addCleanup(os.close, fd)
        self.assertEqual(process_identity(api["pid"]), api["startTime"])
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
        current = f.call("deployment", "inspect", "cancel-start", "--operation", accepted["operationId"])
        self.assertEqual(current["operation"]["state"], "running")
        # Exercise the documented public IPC directly: the CLI exposes this
        # request through foreground SIGINT rather than a standalone command.
        def cancel(key):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
                peer.settimeout(10)
                peer.connect(str(f.socket))
                def exchange(request):
                    data = json.dumps(request, separators=(",", ":")).encode()
                    peer.sendall(struct.pack(">I", len(data)) + data)
                    _, reply = DropStartAck.frame(peer)
                    self.assertEqual(reply.get("requestId"), request["requestId"])
                    error = reply.get("error")
                    self.assertIsNone(error.get("code") if isinstance(error, dict) else error)
                    return reply["result"]
                exchange({"version": {"major": 1, "minor": 0}, "requestId": "negotiate", "method": "system.negotiate", "params": {"supported": [{"major": 1, "minor": 0}], "requiredCapabilities": ["operation.cancel"]}})
                return exchange({"version": {"major": 1, "minor": 0}, "requestId": "cancel", "method": "operation.cancel", "idempotencyKey": key, "params": {"operationId": accepted["operationId"]}})
        control = cancel("independent-cancel")
        self.assertNotEqual(control["operationId"], accepted["operationId"])
        self.assertEqual(cancel("independent-cancel")["operationId"], control["operationId"])
        deadline = time.monotonic() + 45
        while True:
            outcome = f.call("deployment", "inspect", "cancel-start", "--operation", control["operationId"])["operation"]
            if outcome["state"] in {"succeeded", "failed"}:
                break
            self.assertLess(time.monotonic(), deadline, "Cancellation stop deadline did not finish")
            time.sleep(0.1)
        self.assertEqual(outcome["state"], "failed")
        self.assertEqual(outcome["error"]["code"], "OWNERSHIP_UNCERTAIN")
        source = f.call("deployment", "inspect", "cancel-start", "--operation", accepted["operationId"])["operation"]
        self.assertEqual(source["state"], "cancelled")
        retained = f.call("deployment", "inspect", "cancel-start")
        self.assertNotEqual(retained["writerOwnership"], "released")
        resume()
        f.stop("cancel-start", key="retry-after-cancel-deadline")
        self.assertEqual(f.call("deployment", "inspect", "cancel-start")["writerOwnership"], "released")
        self.assertEqual(f.call("deployment", "inspect", "cancel-start", "--operation", control["operationId"])["operation"], outcome)
        self.assertEqual(f.call("deployment", "inspect", "cancel-start", "--operation", accepted["operationId"])["operation"], source)
        noop = cancel("cancel-terminal-source")
        deadline = time.monotonic() + 5
        while True:
            terminal = f.call("deployment", "inspect", "cancel-start", "--operation", noop["operationId"])["operation"]
            if terminal["state"] == "succeeded":
                break
            self.assertLess(time.monotonic(), deadline, "Terminal-target cancel did not become a no-op")
            time.sleep(0.01)
        self.assertEqual(terminal["result"]["targetState"], "cancelled")
        self.assertTrue(terminal["result"]["noOp"])
        self.assertFalse(terminal["result"]["cancelled"])

    def test_graceful_manager_exit_preserves_workloads_and_owned_recovery(self):
        self._manager_exit_recovery(graceful=True)

    def test_killed_manager_reopens_owned_resources_for_public_stop(self):
        self._manager_exit_recovery(graceful=False)

    def _manager_exit_recovery(self, graceful):
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
        exiting = time.monotonic()
        if graceful:
            f.process.terminate()
        else:
            f.process.kill()
        f.process.wait(timeout=30 if graceful else 5)
        if graceful:
            self.assertEqual(f.process.returncode, 0)
            self.assertLess(time.monotonic() - exiting, 30, "Manager shutdown exceeded its bound")
            with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
                rows = database.execute("SELECT ownership_state FROM resource_ownership WHERE operation_id=?", (accepted["operationId"],)).fetchall()
            self.assertTrue(rows and all(row[0] in {"held", "uncertain"} for row in rows))
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

    def test_stop_timeout_can_be_retried_without_rewriting_failed_history(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        accepted = f.start("retry-stop", key="initial-start")
        initial = f.ready("retry-stop", accepted)
        material = f.credential_path("retry-stop")
        material_hash = f.digest(material)
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
            records = [json.loads(row[0])["native"] for row in database.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' AND ownership_state='held'")]
        git = next(record for record in records if record["component"] == "git")
        fd = os.pidfd_open(git["pid"])
        self.addCleanup(os.close, fd)
        self.assertEqual(process_identity(git["pid"]), git["startTime"])
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
        stop = f.call("deployment", "stop", "retry-stop", "--detach", "--idempotency-key", "timed-out-stop")
        deadline = time.monotonic() + 40
        while True:
            failed = f.call("deployment", "inspect", "retry-stop", "--operation", stop["operationId"])["operation"]
            if failed["state"] == "failed":
                break
            self.assertLess(time.monotonic(), deadline, "Stop deadline did not produce an inspectable outcome")
            time.sleep(0.1)
        self.assertEqual(failed["error"]["code"], "OWNERSHIP_UNCERTAIN")
        uncertain = f.call("deployment", "inspect", "retry-stop")
        self.assertNotEqual(uncertain["writerOwnership"], "released")
        self.assertGreater(uncertain["generation"], initial["generation"])
        replay = f.call("deployment", "stop", "retry-stop", "--detach", "--idempotency-key", "timed-out-stop")
        self.assertEqual(replay["operationId"], stop["operationId"])
        resume()
        f.stop("retry-stop", key="retry-owned-stop")
        stopped = f.call("deployment", "inspect", "retry-stop")
        self.assertEqual(stopped["writerOwnership"], "released")
        self.assertEqual(stopped["recovery"], "normal")
        self.assertGreater(stopped["generation"], uncertain["generation"])
        self.assertEqual(f.call("deployment", "inspect", "retry-stop", "--operation", stop["operationId"])["operation"], failed)
        self.assertEqual(f.digest(material), material_hash)

    def test_manager_loss_after_partial_stop_uses_committed_termination_receipts(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        accepted = f.start("partial-stop", key="partial-initial-start")
        f.ready("partial-stop", accepted)
        material = f.credential_path("partial-stop")
        digest = f.digest(material)
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
            records = [json.loads(row[0])["native"] for row in database.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' AND ownership_state='held'")]
        git = next(record for record in records if record["component"] == "git")
        fd = os.pidfd_open(git["pid"])
        self.addCleanup(os.close, fd)
        self.assertEqual(process_identity(git["pid"]), git["startTime"])
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
        stop = f.call("deployment", "stop", "partial-stop", "--detach", "--idempotency-key", "interrupted-partial-stop")
        deadline = time.monotonic() + 15
        while True:
            with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
                journal = database.execute("SELECT lifecycle_stage,backend_identity FROM resource_ownership WHERE resource_type='workload'").fetchall()
            completed = [json.loads(identity) for stage, identity in journal if stage == "retained"]
            if len(completed) == 2:
                break
            self.assertLess(time.monotonic(), deadline, "No durable partial-stop receipts were committed")
            time.sleep(0.01)
        for identity in completed:
            proof = identity["termination"]
            self.assertEqual(proof["operationId"], stop["operationId"])
            self.assertEqual(proof["kind"], "libcrun-delete")
            self.assertEqual(proof["native"], identity["native"])
        f.process.kill()
        f.process.wait(timeout=5)
        # Keep Git paused so the new manager first reattaches the remaining exact
        # live identity. Resumption happens only after its stop advances generation.
        f.start_manager()
        interrupted = f.call("deployment", "inspect", "partial-stop", "--operation", stop["operationId"])["operation"]
        self.assertEqual(interrupted["state"], "failed")
        retried = f.call("deployment", "stop", "partial-stop", "--detach", "--idempotency-key", "recovered-partial-stop")
        deadline = time.monotonic() + 15
        while True:
            observed = f.call("deployment", "inspect", "partial-stop")
            if observed["lastOperationId"] == retried["operationId"]:
                break
            self.assertLess(time.monotonic(), deadline, "Partial-stop reattachment did not prove the recorded resources")
            time.sleep(0.02)
        resume()
        f.call("--timeout", "40s", "--wait", "terminal", "deployment", "inspect", "partial-stop", "--operation", retried["operationId"])
        stopped = f.call("deployment", "inspect", "partial-stop")
        self.assertEqual(stopped["writerOwnership"], "released")
        self.assertEqual(stopped["recovery"], "normal")
        self.assertEqual(f.call("deployment", "inspect", "partial-stop", "--operation", stop["operationId"])["operation"], interrupted)
        self.assertEqual(f.digest(material), digest)

    @staticmethod
    def api_identity(f):
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as database:
            records = [json.loads(row[0])["native"] for row in database.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' AND ownership_state='held' "
                "AND resource_id LIKE 'run-api-%'")]
        return next(record for record in records if record["component"] == "api")

    def test_workload_exit_invalidates_readiness_and_preserves_start_history(self):
        self.observed_exit_restart(Fixture(offline="verified"))

    def observed_exit_restart(self, f, datastore="memdb"):
        self.addCleanup(f.close)
        f.ready_timeout = 120 if datastore == "scylladb" else 60
        accepted = f.start("exit", datastore=datastore)
        f.ready("exit", accepted)
        terminal = f.call("deployment", "inspect", "exit", "--operation", accepted["operationId"])["operation"]
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
            volume = db.execute("SELECT * FROM repository_volumes").fetchall()
            originals = [value["native"] for row in db.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' AND ownership_state='held'")
                if "helperControl" not in (value := json.loads(row[0]))]
        self.assertEqual(len(originals), 4 if datastore == "scylladb" else 3)
        original_fds = []
        for native in originals:
            held = os.pidfd_open(native["pid"])
            self.addCleanup(os.close, held)
            self.assertEqual(process_identity(native["pid"]), native["startTime"])
            original_fds.append(held)
        record = Recovery.api_identity(f)
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
        # Observed death requests a separately inspectable whole-graph stop and
        # replacement. A readiness-only health failure uses a separate test.
        deadline = time.monotonic() + 90
        replacement = None
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
            while replacement is None:
                row = db.execute("SELECT operation_id FROM operations WHERE deployment_id='exit' "
                                 "AND method='deployment.start' AND operation_id!=?",
                                 (accepted["operationId"],)).fetchone()
                if row:
                    replacement = row[0]
                    break
                self.assertLess(time.monotonic(), deadline, "Observed exit did not complete owned restart")
                time.sleep(0.05)
            stop = db.execute("SELECT operation_id,state FROM operations WHERE deployment_id='exit' "
                              "AND method='deployment.stop'").fetchall()
            self.assertEqual(len(stop), 1)
            self.assertEqual(stop[0][1], "succeeded")
            self.assertFalse(db.execute("SELECT 1 FROM resource_ownership WHERE operation_id=? "
                                        "AND resource_type='workload' AND ownership_state!='released'",
                                        (accepted["operationId"],)).fetchone())
            restarted_intent, = db.execute("SELECT intent FROM operations WHERE operation_id=?",
                                          (replacement,)).fetchone()
            self.assertFalse(json.loads(restarted_intent)["allowProvision"])
        # Check the old held identities as soon as the replacement is accepted,
        # before waiting for replacement readiness; metadata release alone is
        # not the no-overlap proof.
        for held in original_fds:
            with self.assertRaises(ProcessLookupError):
                signal.pidfd_send_signal(held, 0)
        f.ready("exit", {"operationId": replacement})
        self.assertEqual(f.call("deployment", "inspect", "exit", "--operation", accepted["operationId"])["operation"], terminal)
        current = Recovery.api_identity(f)
        self.assertNotEqual((current["pid"], current["startTime"]), (record["pid"], record["startTime"]))
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
            self.assertEqual(db.execute("SELECT * FROM repository_volumes").fetchall(), volume)
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
