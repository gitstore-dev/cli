"""Public persistent lifecycle and registered standalone migration on Linux."""
import json
import os
import signal
import time
import fcntl
import sqlite3
import unittest
from support import Fixture
from test_recovery import process_identity


class PersistentScylla(unittest.TestCase):
    def test_persistent_observed_exit_restart(self):
        from test_recovery import Recovery
        Recovery.observed_exit_restart(self, Fixture(offline="persistent"), datastore="scylladb")

    def test_private_backup_restore_and_unsafe_cancellation(self):
        # Shared native verification, explicitly separate from a public restore
        # capability. The runner pins both selected probes before dispatch.
        from test_scylla import ScyllaNative
        ScyllaNative.test_pinned_schema_lock_and_observed_retained_restart(self)

    def test_unverified_scylla_image_blocks_api_start(self):
        f = Fixture(offline="persistent")
        self.addCleanup(f.close)
        # Explicitly mismatched digest input, not a registry outage or host fill.
        # Restart only the fixture manager to load this owned offline selection.
        f.process.terminate()
        f.process.wait(timeout=10)
        f.settings["offlineLayouts"]["scylladb"] = f.settings["offlineLayouts"]["api"]
        f.write_settings()
        f.start_manager()
        f.deployments.add("bad-scylla")
        accepted = f.call("--non-interactive", "serve", "--deployment", "bad-scylla",
                          "--datastore", "scylladb", "--detach",
                          "--idempotency-key", "unverified-scylla")
        deadline = time.monotonic() + 60
        while True:
            operation = f.call("deployment", "inspect", "bad-scylla",
                               "--operation", accepted["operationId"])["operation"]
            if operation["state"] in ("failed", "cancelled", "succeeded"):
                break
            self.assertLess(time.monotonic(), deadline, "Unverified input did not produce a bounded outcome")
            time.sleep(0.1)
        self.assertEqual(operation["state"], "failed")
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
            rows = db.execute("SELECT backend_identity FROM resource_ownership WHERE resource_type='workload'").fetchall()
            self.assertTrue(rows)
            for (identity,) in rows:
                self.assertNotIn("native", json.loads(identity) if identity else {})
        self.assertNotIn("scylladb", f.active_components())
        f.stop("bad-scylla", key="cleanup-unverified-scylla")

    def test_public_start_stop_migrate_and_same_key_replay(self):
        f = Fixture(offline="persistent")
        self.addCleanup(f.close)
        f.ready_timeout = 180
        f.deployments.add("persistent")
        accepted = f.call("--non-interactive", "serve", "--deployment", "persistent",
                          "--datastore", "scylladb", "--detach",
                          "--idempotency-key", "public-persistent-start")
        f.ready("persistent", accepted)
        # Pause only the exact recorded Scylla init, with a held pidfd. This
        # creates a bounded stop timeout without filling the host or killing data.
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
            (raw,) = db.execute("SELECT backend_identity FROM resource_ownership WHERE resource_id=?",
                ("run-scylladb-" + accepted["operationId"],)).fetchone()
            (volume,) = db.execute("SELECT volume_id FROM datastore_volumes WHERE allocated=1").fetchone()
        native = json.loads(raw)["native"]
        pidfd = os.pidfd_open(native["pid"])
        self.addCleanup(os.close, pidfd)
        self.assertEqual(process_identity(native["pid"]), native["startTime"])
        paused = [True]
        def resume():
            if paused[0]:
                try:
                    signal.pidfd_send_signal(pidfd, signal.SIGCONT)
                except ProcessLookupError:
                    pass
                paused[0] = False
        self.addCleanup(resume)
        signal.pidfd_send_signal(pidfd, signal.SIGSTOP)
        stop = f.call("deployment", "stop", "persistent", "--detach",
                      "--idempotency-key", "persistent-timeout-stop")
        deadline = time.monotonic() + 45
        while True:
            failed = f.call("deployment", "inspect", "persistent", "--operation", stop["operationId"])["operation"]
            if failed["state"] == "failed":
                break
            self.assertLess(time.monotonic(), deadline, "Persistent stop did not record its deadline")
            time.sleep(0.1)
        self.assertEqual(failed["error"]["code"], "OWNERSHIP_UNCERTAIN")
        writer = os.open(f.root / "state/volumes" / f"writer-{volume}.lock", os.O_RDWR | os.O_NOFOLLOW)
        self.addCleanup(os.close, writer)
        with self.assertRaises(BlockingIOError):
            fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
        resume()
        f.stop("persistent", key="before-public-migrate")
        self.assertEqual(f.call("deployment", "inspect", "persistent", "--operation", stop["operationId"])["operation"], failed)
        fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(writer, fcntl.LOCK_UN)
        def snapshot():
            with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
                volume = db.execute("SELECT volume_id,producer_operation_id,volume_device,volume_inode,data_device,data_inode,writer_device,writer_inode FROM datastore_volumes WHERE allocated=1").fetchone()
                return volume
        before = snapshot()
        arguments = ("--timeout", "180s", "migrate", "--deployment", "persistent",
                     "--target", "gitstore-06152d-schema-012", "--backup-acknowledged",
                     "--idempotency-key", "public-persistent-migration")
        migrated = f.call(*arguments, timeout=190)
        self.assertEqual(migrated["state"], "succeeded")
        self.assertTrue(migrated["result"]["migrationCompleted"])
        repeated = f.call(*arguments, timeout=20)
        self.assertEqual(repeated["operationId"], migrated["operationId"])
        self.assertEqual(snapshot(), before)
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM operations WHERE method='migration.start'").fetchone()[0], 1)
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0], 0)
            methods = db.execute("SELECT resource_id,backend_identity FROM resource_ownership WHERE operation_id=? AND resource_type='workload'", (migrated["operationId"],)).fetchall()
        self.assertEqual(len(methods), 3)
        self.assertEqual({resource for resource, _ in methods},
                         {prefix + migrated["operationId"] for prefix in ("run-scylladb-", "run-cql-", "run-schema-")})
        for resource, identity in methods:
            value = json.loads(identity)
            self.assertIn(value["native"]["component"], ("scylladb", "api"))
            if not resource.startswith("run-scylladb-"):
                self.assertEqual(value["helperControl"]["stage"], "deleted")
                self.assertEqual(value["helperControl"]["result"], 0)
                self.assertIn(value["helperControl"]["purpose"], (3, 5))
        f.stop("persistent", key="after-public-migrate")


if __name__ == "__main__":
    unittest.main()
