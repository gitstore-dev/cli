"""Pinned real Scylla native checks; public persistent graph remains gated."""
import json
import os
from pathlib import Path
import subprocess
import sys
import sqlite3
import tempfile
import time
import shutil
import hashlib
import fcntl
import unittest
import selectors
import uuid
import stat
import signal
from test_recovery import process_identity


class ScyllaNative(unittest.TestCase):
    def test_private_persistent_actor_retained_restart(self):
        self._persistent_actor("restart-actor-scylla", False)

    def test_private_persistent_observer_restoration(self):
        self._persistent_actor("restore-actor-scylla", True)

    def test_private_persistent_manager_loss(self):
        self._persistent_actor("recover-actor-scylla", True, manager_loss=True)

    def _persistent_actor(self, mode, restored, manager_loss=False):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_bytes())
        self.assertEqual(settings["disposableUid"], os.geteuid())
        self.assertIs(settings["dedicatedTestAccount"], True)
        root = Path(tempfile.mkdtemp(prefix="scylla-retained-actor-", dir=settings["fixtureParent"]))
        credentials = root / "credentials"
        credentials.mkdir(mode=0o700)
        broker = root / "gs"
        shutil.copyfile(settings["gs"], broker)
        broker.chmod(0o500)
        self.assertEqual(broker.stat().st_nlink, 1)
        with broker.open("rb") as copy, Path(settings["gs"]).open("rb") as source:
            self.assertEqual(hashlib.file_digest(copy, "sha256").digest(),
                             hashlib.file_digest(source, "sha256").digest())
        layouts = {name: settings[key] for name, key in
                   (("api", "offlineApiLayout"), ("git", "offlineGitLayout"),
                    ("controller", "offlineControllerLayout"), ("scylladb", "offlineScyllaLayout"))}
        owner = root / "settings.json"
        owner.write_text(json.dumps({"schemaVersion": 1, "offlineLayouts": layouts,
                         "bootstrapCredentialSource": {"kind": "protected-directory", "path": str(credentials)}}))
        owner.chmod(0o600)
        started = time.monotonic()
        loss_evidence = None
        if manager_loss:
            loss_evidence = self._kill_persistent_owner(settings, root, broker, layouts)
        result = subprocess.run([settings["datastoreProbe"], str(root), str(broker),
                                 layouts["api"], layouts["git"], layouts["controller"],
                                 mode],
                                env={"PATH": "/nonexistent", "LANG": "C"},
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=610)
        evidence = {"root": str(root), "seconds": time.monotonic() - started,
                    "exit": result.returncode, "publicCapability": False, "passed": False,
                    "stdout": result.stdout.decode(errors="replace")[-8192:],
                    "stderr": (result.stderr[:4096] + result.stderr[-4096:]).decode(errors="replace")}
        if loss_evidence is not None:
            evidence["managerLoss"] = loss_evidence
        record = root / "retained-actor-evidence.json"
        record.write_text(json.dumps(evidence, indent=2)); record.chmod(0o600)
        self.assertEqual(result.returncode, 0, str(record))
        if restored:
            self.assertEqual(result.stderr.count(b"observer restoration outcome=0"), 1 if manager_loss else 2)
            evidence["observerRestorations"] = 1 if manager_loss else 2
        if manager_loss:
            evidence["managerLoss"] = loss_evidence
            self.assertTrue(loss_evidence["workloadsSurvived"])
            self.assertEqual(loss_evidence["heldWriterLocks"], 2)
            self.assertTrue(loss_evidence["copiedState"]["rejected"])
            self.assertTrue(loss_evidence["populationMismatch"]["rejected"])
        completions = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(completions), 1 if manager_loss else 2)
        for completion in completions:
            self.assertEqual(completion["deploymentActor"], "ready")
            self.assertEqual(completion["stop"], "observed")
            self.assertEqual(completion["volumes"], "retained")
        with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM operations WHERE state='succeeded'").fetchone()[0], 2 if manager_loss else 4)
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0], 0)
            configs = [json.loads(row[0]) for row in db.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='configuration' ORDER BY rowid")]
            self.assertEqual(len(configs), 1 if manager_loss else 2)
            for config in configs:
                self.assertEqual(config["graph"]["stage"], 9)
                self.assertEqual(config["graph"]["outcome"], 0)
            self.assertIs(configs[0]["graph"]["freshDatastore"], True)
            if not manager_loss:
                self.assertIs(configs[1]["graph"]["freshDatastore"], False)
                self.assertEqual(configs[0]["graph"]["controllerUid"], configs[1]["graph"]["controllerUid"])
                self.assertEqual(configs[1]["controllerIdentitySource"]["operationId"], configs[0]["operationId"])
                second = configs[1]["operationId"]
                purposes = [row[0] for row in db.execute(
                    "SELECT json_extract(backend_identity,'$.helperControl.purpose') FROM resource_ownership "
                    "WHERE operation_id=? AND json_type(backend_identity,'$.helperControl') IS NOT NULL", (second,))]
                self.assertEqual(purposes, [1])  # retained startup checks only; no schema/keyspace mutation
            volumes = db.execute("SELECT volume_id,writer_device,writer_inode FROM repository_volumes UNION ALL "
                                 "SELECT volume_id,writer_device,writer_inode FROM datastore_volumes").fetchall()
            self.assertEqual(len(volumes), 2)
            for volume, device, inode in volumes:
                fd = os.open(root / "volumes" / f"writer-{volume}.lock", os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
                try:
                    info = os.fstat(fd)
                    self.assertEqual((str(info.st_dev), str(info.st_ino)), (device, inode))
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                finally:
                    os.close(fd)
            evidence.update(configurations=configs, volumes=volumes)
        evidence["passed"] = True
        record.write_text(json.dumps(evidence, indent=2))
        print(json.dumps({"retainedActorEvidence": str(record), "passed": True}))

    def _kill_persistent_owner(self, settings, root, broker, layouts):
        diagnostic = root / "lost-manager.stderr"
        descriptor = os.open(diagnostic, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            owner = subprocess.Popen([settings["datastoreProbe"], str(root), str(broker),
                                      layouts["api"], layouts["git"], layouts["controller"],
                                      "leave-actor-scylla"],
                                     env={"PATH": "/nonexistent", "LANG": "C"},
                                     stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=stream)
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(owner.stdout, selectors.EVENT_READ)
                    if not selector.select(400):
                        raise AssertionError(f"Private owner did not become ready; inspect {diagnostic}")
                line = owner.stdout.readline(4097)
                if not line or len(line) > 4096 or not json.loads(line).get("managerReady"):
                    raise AssertionError(f"Private owner failed before readiness; inspect {diagnostic}")
                with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
                    identities = [json.loads(row[0])["native"] for row in db.execute(
                        "SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' "
                        "AND lifecycle_stage='running' AND ownership_state='held'")]
                    volumes = db.execute("SELECT volume_id FROM repository_volumes UNION ALL "
                                         "SELECT volume_id FROM datastore_volumes").fetchall()
                try:
                    copied = self._reject_persistent_copy(settings, root, broker, layouts)
                except (OSError, subprocess.SubprocessError) as error:
                    copied = {"rejected": False, "failureType": type(error).__name__}
                # Kill only this explicitly selected disposable owner. Original workload
                # identities and kernel locks must survive; recovery then observes stop.
                owner.kill()
                owner.wait(timeout=5)
                try:
                    population = self._reject_population_mismatch(settings, root, broker, layouts)
                except (OSError, subprocess.SubprocessError) as error:
                    population = {"rejected": False, "failureType": type(error).__name__}
                try:
                    live = all(process_identity(item["pid"]) == item["startTime"] for item in identities)
                except (OSError, ValueError):
                    live = False
                held = 0
                for volume, in volumes:
                    fd = os.open(root / "volumes" / f"writer-{volume}.lock", os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
                    try:
                        try:
                            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        except BlockingIOError:
                            held += 1
                    finally:
                        os.close(fd)
                return {"workloadsSurvived": live and len(identities) == 4,
                        "nativeIdentities": identities, "heldWriterLocks": held,
                        "managerExit": owner.returncode, "copiedState": copied, "populationMismatch": population}
            finally:
                if owner.poll() is None:
                    owner.kill()
                    owner.wait(timeout=5)
                owner.stdout.close()

    def _reject_population_mismatch(self, settings, root, broker, layouts):
        with sqlite3.connect(root / "state.db") as db:
            resource, original = db.execute("SELECT resource_id,backend_identity FROM resource_ownership WHERE resource_type='workload' AND lifecycle_stage='running' AND json_type(backend_identity,'$.population')='object' ORDER BY resource_id LIMIT 1").fetchone()
            changed = json.loads(original)
            changed["population"]["cgroupInode"] = str(int(changed["population"]["cgroupInode"]) + 1)
            db.execute("UPDATE resource_ownership SET backend_identity=? WHERE resource_id=?", (json.dumps(changed, separators=(",", ":")), resource))
        try:
            result = subprocess.run([settings["datastoreProbe"], str(root), str(broker),
                                     layouts["api"], layouts["git"], layouts["controller"],
                                     "reject-population-scylla"],
                                    env={"PATH": "/nonexistent", "LANG": "C"},
                                    stdin=subprocess.DEVNULL, capture_output=True, timeout=40)
        finally:
            with sqlite3.connect(root / "state.db") as db:
                db.execute("UPDATE resource_ownership SET backend_identity=? WHERE resource_id=?", (original, resource))
        return {"rejected": result.returncode == 0 and result.stdout.strip() == b'{"populationMismatchRejected":true,"nativeDispatched":false}',
                "exit": result.returncode, "stdout": result.stdout.decode(errors="replace")[-1024:],
                "stderr": (result.stderr[:1024] + result.stderr[-1024:]).decode(errors="replace")}

    def _reject_persistent_copy(self, settings, root, broker, layouts):
        copied = Path(tempfile.mkdtemp(prefix="scylla-untrusted-copy-", dir=settings["fixtureParent"]))
        # This is deliberately an untrusted live copy, not a consistent backup or
        # supported restore. Payload bytes cannot transfer native/lease authority.
        with sqlite3.connect(root / "state.db") as source, sqlite3.connect(copied / "state.db") as destination:
            source.backup(destination)
        (copied / "state.db").chmod(0o600)
        shutil.copytree(root / "volumes", copied / "volumes", symlinks=True)
        (copied / "assets").mkdir(mode=0o700)
        for name in (".gitstored-owner", ".gitstored-lock"):
            shutil.copyfile(root / "assets" / name, copied / "assets" / name)
            (copied / "assets" / name).chmod(0o600)
        result = subprocess.run([settings["datastoreProbe"], str(copied), str(broker),
                                 layouts["api"], layouts["git"], layouts["controller"],
                                 "reject-copy-scylla"],
                                env={"PATH": "/nonexistent", "LANG": "C"},
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=20)
        evidence = {"root": str(copied), "exit": result.returncode,
                    "stdout": result.stdout.decode(errors="replace")[-4096:],
                    "stderr": result.stderr.decode(errors="replace")[-4096:]}
        record = copied / "rejected-copy-evidence.json"
        # Defer assertions until the independent live owner has been recovered/stopped.
        evidence["rejected"] = (result.returncode == 0 and
            result.stdout.strip() == b'{"copiedStateRejected":true,"nativeDispatched":false}' and
            not list(copied.glob("native-*")) and not list(copied.glob("resources-*")))
        record.write_text(json.dumps(evidence, indent=2)); record.chmod(0o600)
        return evidence

    def test_private_standalone_migration_preparation(self):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_bytes())
        root = Path(settings["persistentPreparationRoot"])
        info = root.lstat()
        self.assertTrue(root.is_absolute() and stat.S_ISDIR(info.st_mode))
        self.assertEqual((info.st_uid, stat.S_IMODE(info.st_mode)), (os.geteuid(), 0o700))
        def payload_hash(directory):
            digest = hashlib.sha256()
            total = 0
            for item in sorted(directory.rglob("*")):
                details = item.lstat()
                if stat.S_ISDIR(details.st_mode):
                    continue
                self.assertTrue(stat.S_ISREG(details.st_mode))
                total += details.st_size
                self.assertLessEqual(total, 8 * 1024 ** 3)
                digest.update(str(item.relative_to(directory)).encode() + b"\0")
                digest.update(str(details.st_size).encode() + b"\0")
                with item.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(65536), b""):
                        digest.update(chunk)
            return digest.hexdigest()
        with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
            volumes = db.execute("SELECT volume_id FROM repository_volumes UNION ALL SELECT volume_id FROM datastore_volumes").fetchall()
            self.assertEqual(len(volumes), 2)
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0], 0)
        before = {volume: payload_hash(root / "volumes" / f"volume-{volume}" / "data") for volume, in volumes}
        key = "prepare-migration-" + uuid.uuid4().hex
        started = time.monotonic()
        result = subprocess.run([settings["migrationProbe"], str(root), key],
                                env={"PATH": "/nonexistent", "LANG": "C"},
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=310)
        evidence = {"root": str(root), "seconds": time.monotonic() - started,
                    "exit": result.returncode, "stdout": result.stdout.decode(errors="replace")[-8192:],
                    "stderr": (result.stderr[:4096] + result.stderr[-4096:]).decode(errors="replace"),
                    "publicCapability": False, "passed": False}
        record = root / (key + "-evidence.json")
        descriptor = os.open(record, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            json.dump(evidence, stream, indent=2)
        self.assertEqual(result.returncode, 0, str(record))
        completion = json.loads(result.stdout)
        self.assertIs(completion["migrationPreparation"], True)
        self.assertEqual(completion["materializations"], 3)
        for field in ("nativeDispatched", "migrationCompleted", "publicCapability"):
            self.assertIs(completion[field], False)
        with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
            operation, state, code = db.execute("SELECT operation_id,state,json_extract(error,'$.code') FROM operations WHERE method='migration.start' ORDER BY rowid DESC LIMIT 1").fetchone()
            self.assertEqual((state, code), ("failed", "MIGRATION_PRECONDITION"))
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE operation_id=? AND resource_type='workload'", (operation,)).fetchone()[0], 3)
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE operation_id=? AND (json_type(backend_identity,'$.native') IS NOT NULL OR json_type(backend_identity,'$.helperControl') IS NOT NULL)", (operation,)).fetchone()[0], 0)
        after = {volume: payload_hash(root / "volumes" / f"volume-{volume}" / "data") for volume, in volumes}
        self.assertEqual(before, after)
        evidence.update(passed=True, retainedPayloads=after, operationId=operation)
        record.write_text(json.dumps(evidence, indent=2))
        print(json.dumps({"migrationPreparationEvidence": str(record), "passed": True}))

    def test_private_migration_preparation_recovery(self):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_bytes())
        self.assertEqual(settings["disposableUid"], os.geteuid())
        self.assertIs(settings["dedicatedTestAccount"], True)
        root = Path(settings["persistentRecoveryRoot"])
        info = root.lstat()
        self.assertTrue(root.is_absolute() and stat.S_ISDIR(info.st_mode))
        self.assertEqual((info.st_uid, stat.S_IMODE(info.st_mode)), (os.geteuid(), 0o700))
        def payloads():
            result = {}
            for directory in sorted((root / "volumes").glob("volume-*/data")):
                digest = hashlib.sha256()
                size = 0
                for item in sorted(directory.rglob("*")):
                    details = item.lstat()
                    if stat.S_ISDIR(details.st_mode):
                        continue
                    self.assertTrue(stat.S_ISREG(details.st_mode))
                    size += details.st_size
                    self.assertLessEqual(size, 8 * 1024 ** 3)
                    digest.update(str(item.relative_to(directory)).encode() + b"\0")
                    digest.update(str(details.st_size).encode() + b"\0")
                    with item.open("rb") as stream:
                        for chunk in iter(lambda: stream.read(65536), b""):
                            digest.update(chunk)
                result[directory.parent.name] = digest.hexdigest()
            self.assertEqual(len(result), 2)
            return result
        with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
            migration = db.execute("SELECT o.operation_id FROM operations o JOIN resource_ownership r ON r.resource_id='migration-'||o.operation_id WHERE o.method='migration.start' AND r.ownership_state IN ('held','uncertain') ORDER BY o.rowid DESC LIMIT 1").fetchone()[0]
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE operation_id=? AND resource_type='workload'", (migration,)).fetchone()[0], 3)
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE operation_id=? AND (json_type(backend_identity,'$.native') IS NOT NULL OR json_type(backend_identity,'$.helperControl') IS NOT NULL)", (migration,)).fetchone()[0], 0)
            original = db.execute("SELECT producer_operation_id,data_device,data_inode,writer_device,writer_inode FROM datastore_volumes").fetchall()
        before = payloads()
        key = "recover-preparation-" + uuid.uuid4().hex
        started = time.monotonic()
        result = subprocess.run([settings["migrationProbe"], str(root), key, "--recover", settings.get("persistentDeployment", "one")],
                                env={"PATH": "/nonexistent", "LANG": "C"}, stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=310)
        evidence = {"root": str(root), "seconds": time.monotonic() - started, "exit": result.returncode,
                    "stdout": result.stdout.decode(errors="replace")[-8192:],
                    "stderr": (result.stderr[:4096] + result.stderr[-4096:]).decode(errors="replace"),
                    "publicCapability": False, "passed": False}
        record = root / (key + "-evidence.json")
        descriptor = os.open(record, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            json.dump(evidence, stream, indent=2)
        self.assertEqual(result.returncode, 0, str(record))
        completion = json.loads(result.stdout)
        self.assertIs(completion["migrationRecovery"], True)
        self.assertIs(completion["originalOutcomePreserved"], True)
        self.assertIs(completion["createOrStartDispatched"], False)
        self.assertIs(completion["publicCapability"], False)
        self.assertEqual(completion["volumes"], "retained")
        with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT state,recovery,json_extract(error,'$.code') FROM operations WHERE operation_id=?", (migration,)).fetchone(), ("failed", "normal", "OWNERSHIP_UNCERTAIN"))
            self.assertEqual(db.execute("SELECT state,json_extract(result,'$.migrationOutcomePreserved') FROM operations WHERE method='deployment.stop' ORDER BY rowid DESC LIMIT 1").fetchone(), ("succeeded", 1))
            self.assertEqual(db.execute("SELECT producer_operation_id,data_device,data_inode,writer_device,writer_inode FROM datastore_volumes").fetchall(), original)
        self.assertEqual(payloads(), before)
        evidence.update(passed=True, migration= migration, retainedPayloads=before)
        record.write_text(json.dumps(evidence, indent=2))
        print(json.dumps({"migrationPreparationRecoveryEvidence": str(record), "passed": True}))

    def test_private_standalone_migration_execution(self):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_bytes())
        self.assertEqual(settings["disposableUid"], os.geteuid())
        self.assertIs(settings["dedicatedTestAccount"], True)
        root = Path(settings["persistentPreparationRoot"])
        info = root.lstat()
        self.assertTrue(root.is_absolute() and stat.S_ISDIR(info.st_mode))
        self.assertEqual((info.st_uid, stat.S_IMODE(info.st_mode)), (os.geteuid(), 0o700))
        with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0], 0)
            original = db.execute("SELECT producer_operation_id,data_device,data_inode,writer_device,writer_inode FROM datastore_volumes").fetchall()
            self.assertEqual(len(original), 1)
        key = "execute-migration-" + uuid.uuid4().hex
        started = time.monotonic()
        result = subprocess.run([settings["migrationProbe"], str(root), key, "--runtime"],
                                env={"PATH": "/nonexistent", "LANG": "C"},
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=310)
        evidence = {"root": str(root), "seconds": time.monotonic() - started,
                    "exit": result.returncode, "stdout": result.stdout.decode(errors="replace")[-8192:],
                    "stderr": (result.stderr[:4096] + result.stderr[-4096:]).decode(errors="replace"),
                    "publicCapability": False, "passed": False}
        record = root / (key + "-evidence.json")
        descriptor = os.open(record, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            json.dump(evidence, stream, indent=2)
        self.assertEqual(result.returncode, 0, str(record))
        completion = json.loads(result.stdout)
        for field in ("privateStandaloneMigration", "sourceStopped", "migrationCompleted"):
            self.assertIs(completion[field], True)
        self.assertIs(completion["publicCapability"], False)
        self.assertEqual(completion["historyRows"], 12)
        with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
            operation, state, completed = db.execute("SELECT operation_id,state,json_extract(result,'$.migrationCompleted') FROM operations WHERE method='migration.start' ORDER BY rowid DESC LIMIT 1").fetchone()
            self.assertEqual((state, completed), ("succeeded", 1))
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT producer_operation_id,data_device,data_inode,writer_device,writer_inode FROM datastore_volumes").fetchall(), original)
            workloads = db.execute("SELECT json_extract(backend_identity,'$.native.component'),json_extract(backend_identity,'$.helperControl.purpose'),json_extract(backend_identity,'$.helperControl.stage'),json_extract(backend_identity,'$.helperControl.result'),json_extract(backend_identity,'$.termination.kind') FROM resource_ownership WHERE operation_id=? AND resource_type='workload' ORDER BY resource_id", (operation,)).fetchall()
            self.assertEqual(len(workloads), 3)
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE operation_id=? AND resource_type='workload' AND json_extract(backend_identity,'$.population.schemaVersion')=1 AND json_type(backend_identity,'$.population.scopePath')='text'", (operation,)).fetchone()[0], 3)
            self.assertEqual(sorted(row[1] for row in workloads if row[1] is not None), [3, 5])
            for component, purpose, stage, outcome, termination in workloads:
                if purpose is None:
                    self.assertEqual((component, termination), ("scylladb", "migration-service-delete"))
                else:
                    self.assertEqual((stage, outcome), ("deleted", 0))
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE operation_id=? AND json_type(backend_identity,'$.graph') IS NOT NULL", (operation,)).fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT count(*) FROM network_attachments WHERE operation_id=?", (operation,)).fetchone()[0], 3)
        evidence.update(passed=True, operationId=operation, retainedDatastoreIdentity=original)
        record.write_text(json.dumps(evidence, indent=2))
        print(json.dumps({"standaloneMigrationEvidence": str(record), "passed": True}))

    def test_private_surviving_migration_helper_recovery(self):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_bytes())
        self.assertEqual(settings["disposableUid"], os.geteuid())
        self.assertIs(settings["dedicatedTestAccount"], True)
        root = Path(settings["persistentPreparationRoot"])
        info = root.lstat()
        self.assertTrue(root.is_absolute() and stat.S_ISDIR(info.st_mode))
        self.assertEqual((info.st_uid, stat.S_IMODE(info.st_mode)), (os.geteuid(), 0o700))
        with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0], 0)
            original = db.execute("SELECT volume_id,producer_operation_id,data_device,data_inode,writer_device,writer_inode FROM datastore_volumes").fetchone()
            self.assertIsNotNone(original)
        key = "surviving-mutator-" + uuid.uuid4().hex
        owner_log = root / (key + "-owner.log")
        descriptor = os.open(owner_log, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        started = time.monotonic()
        evidence = {"root": str(root), "ownerLog": str(owner_log), "publicCapability": False, "passed": False}
        helper_watch = -1
        recovery = None
        owner = None
        checkpoint = False
        with os.fdopen(descriptor, "wb") as errors:
            try:
                owner = subprocess.Popen([settings["migrationProbe"], str(root), key, "--leave-mutator", settings.get("persistentDeployment", "one")],
                                         env={"PATH": "/nonexistent", "LANG": "C"}, stdin=subprocess.DEVNULL,
                                         stdout=subprocess.PIPE, stderr=errors)
                with selectors.DefaultSelector() as events:
                    events.register(owner.stdout, selectors.EVENT_READ)
                    waiting = time.monotonic() + 240
                    ready = b""
                    while time.monotonic() < waiting and b"\n" not in ready:
                        if events.select(0.25):
                            chunk = os.read(owner.stdout.fileno(), 4096)
                            if not chunk:
                                break
                            ready += chunk
                        if owner.poll() is not None:
                            break
                self.assertEqual(json.loads(ready), {"migrationHelperPaused": True, "publicCapability": False})
                with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
                    migration = db.execute("SELECT operation_id FROM operations WHERE method='migration.start' ORDER BY rowid DESC LIMIT 1").fetchone()[0]
                    helper = json.loads(db.execute("SELECT backend_identity FROM resource_ownership WHERE resource_id='run-schema-'||?", (migration,)).fetchone()[0])
                    source = json.loads(db.execute("SELECT backend_identity FROM resource_ownership WHERE resource_id='run-scylladb-'||?", (migration,)).fetchone()[0])["native"]
                    self.assertEqual(helper["helperControl"]["stage"], "created")
                    self.assertIs(helper["helperControl"]["unsafe"], True)
                native = helper["native"]
                self.assertEqual(native["bootId"], Path("/proc/sys/kernel/random/boot_id").read_text().strip())
                self.assertEqual(process_identity(native["pid"]), native["startTime"])
                helper_watch = os.pidfd_open(native["pid"])
                self.assertEqual(process_identity(native["pid"]), native["startTime"])
                owner.kill()
                owner.wait(timeout=5)
                self.assertEqual(process_identity(source["pid"]), source["startTime"])
                recovery = subprocess.Popen([settings["migrationProbe"], str(root), "recover-" + uuid.uuid4().hex, "--recover", settings.get("persistentDeployment", "one")],
                                            env={"PATH": "/nonexistent", "LANG": "C"}, stdin=subprocess.DEVNULL,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                captured = bytearray()
                with selectors.DefaultSelector() as events:
                    events.register(recovery.stderr, selectors.EVENT_READ)
                    waiting = time.monotonic() + 45
                    while time.monotonic() < waiting:
                        if events.select(0.25):
                            chunk = os.read(recovery.stderr.fileno(), 4096)
                            if not chunk:
                                break
                            captured.extend(chunk)
                            if b"recovery observed schema helper\n" in captured:
                                checkpoint = True
                                break
                        if recovery.poll() is not None:
                            break
                self.assertTrue(checkpoint, bytes(captured[-2048:]).decode(errors="replace"))
                self.assertEqual(process_identity(source["pid"]), source["startTime"])
                writer = os.open(root / "volumes" / ("writer-" + original[0] + ".lock"), os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
                try:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
                finally:
                    os.close(writer)
                # Resume only the original test-paused helper after the new
                # observer holds its exact pidfd/cgroup. Recovery sends no signal
                # to a running mutator and never restarts the helper.
                signal.pidfd_send_signal(helper_watch, signal.SIGCONT)
                stdout, stderr = recovery.communicate(timeout=260)
                captured.extend(stderr)
                evidence.update(migration=migration, source=source, helper=native, managerExit=owner.returncode,
                                recoveryExit=recovery.returncode, helperObservedBeforeResume=checkpoint,
                                stdout=stdout.decode(errors="replace")[-4096:],
                                stderr=(captured[:4096] + captured[-4096:]).decode(errors="replace"))
                self.assertEqual(recovery.returncode, 0)
                self.assertIs(json.loads(stdout)["originalOutcomePreserved"], True)
                with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as db:
                    self.assertEqual(db.execute("SELECT state,recovery,json_extract(error,'$.code') FROM operations WHERE operation_id=?", (migration,)).fetchone(), ("failed", "normal", "OWNERSHIP_UNCERTAIN"))
                    reconciled = json.loads(db.execute("SELECT backend_identity FROM resource_ownership WHERE resource_id='run-schema-'||?", (migration,)).fetchone()[0])
                    self.assertEqual(reconciled["helperControl"]["stage"], "created")
                    self.assertIsNone(reconciled["helperControl"]["result"])
                    self.assertIs(reconciled["reconciliation"]["exitStatusKnown"], False)
                    self.assertEqual(reconciled["reconciliation"]["native"], native)
                    self.assertEqual(db.execute("SELECT count(*) FROM resource_ownership WHERE ownership_state!='released'").fetchone()[0], 0)
                    self.assertEqual(db.execute("SELECT volume_id,producer_operation_id,data_device,data_inode,writer_device,writer_inode FROM datastore_volumes").fetchone(), original)
                evidence["passed"] = True
            finally:
                if owner is not None:
                    if owner.poll() is None:
                        owner.kill()
                        owner.wait(timeout=5)
                    owner.stdout.close()
                if recovery is not None and recovery.poll() is None:
                    recovery.kill()
                    recovery.wait(timeout=5)
                if helper_watch >= 0:
                    # A failed pre-observation test retains the paused disposable
                    # helper for exact-identity repair; it must not lose its
                    # cgroup before a replacement observer can capture proof.
                    os.close(helper_watch)
                evidence["seconds"] = time.monotonic() - started
                record = root / (key + "-evidence.json")
                descriptor = os.open(record, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                with os.fdopen(descriptor, "w") as stream:
                    json.dump(evidence, stream, indent=2)
                print(json.dumps({"survivingHelperRecoveryEvidence": str(record), "passed": evidence["passed"]}))

    def test_private_journaled_persistent_bootstrap(self):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_bytes())
        self.assertEqual(settings["disposableUid"], os.geteuid())
        self.assertIs(settings["dedicatedTestAccount"], True)
        root = Path(tempfile.mkdtemp(prefix="scylla-journaled-", dir=settings["fixtureParent"]))
        layouts = {name: settings[key] for name, key in
                   (("api", "offlineApiLayout"), ("git", "offlineGitLayout"),
                    ("controller", "offlineControllerLayout"), ("scylladb", "offlineScyllaLayout"))}
        selected = root / "settings.json"
        selected.write_text(json.dumps({"schemaVersion": 1, "offlineLayouts": layouts}))
        selected.chmod(0o600)
        command = [settings["datastoreProbe"], str(root), settings["gs"], layouts["api"],
                   layouts["git"], layouts["controller"], "start-scylla"]
        started = time.monotonic()
        result = subprocess.run(command, env={"PATH": "/nonexistent", "LANG": "C"},
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=610)
        # Only the probe's fixed diagnostics enter evidence; raw workload output
        # and generated synthetic bootstrap credentials remain private files.
        evidence = {"root": str(root), "exit": result.returncode,
                    "seconds": time.monotonic() - started,
                    "stdout": result.stdout.decode(errors="replace")[-8192:],
                    "stderr": (result.stderr[:4096] +
                               (b"\n[diagnostic middle omitted]\n" if len(result.stderr) > 8192 else b"") +
                               (result.stderr[-4096:] if len(result.stderr) > 4096 else b"")).decode(errors="replace"),
                    "publicCapability": False, "passed": False}
        record = root / "managed-bootstrap-evidence.json"
        record.write_text(json.dumps(evidence, indent=2)); record.chmod(0o600)
        self.assertEqual(result.returncode, 0, str(record))
        outputs = [json.loads(line) for line in result.stdout.splitlines()]
        completion = next(item for item in outputs if "journaledMigration" in item)
        for key in ("journaledReadiness", "journaledKeyspace", "journaledMigration", "datastoreStopped"):
            self.assertIs(completion[key], True)
        self.assertIs(completion["publicCapability"], False)
        with sqlite3.connect(f"file:{root}/state.db?mode=ro", uri=True) as database:
            rows = [json.loads(row[0]) for row in database.execute(
                "SELECT backend_identity FROM resource_ownership WHERE "
                "json_type(backend_identity,'$.helperControl') IS NOT NULL")]
            self.assertEqual(len(rows), 2)
            self.assertEqual({row["helperControl"]["purpose"] for row in rows}, {2, 3})
            for row in rows:
                control = row["helperControl"]
                self.assertEqual(control["stage"], "deleted")
                self.assertEqual(control["result"], 0)
                self.assertIs(control["unsafe"], False)
                self.assertEqual(control["termination"]["kind"], "helper-native-delete")
                self.assertEqual(control["termination"]["native"], row["native"])
                self.assertIn("migrationTargetDigest", row["executionManifest"])
                self.assertEqual(row["native"]["id"].startswith(
                    "run-schema-" if control["purpose"] == 3 else "run-cql-"), True)
            configurations = [json.loads(row[0]) for row in database.execute(
                "SELECT backend_identity FROM resource_ownership WHERE resource_type='configuration'")]
            self.assertEqual(len(configurations), 1)
            graph = configurations[0]["graph"]
            self.assertEqual(graph["stage"], 1)  # application launch boundary
            self.assertEqual(graph["outcome"], 0)
            self.assertIs(graph["persistent"], True)
            self.assertIs(graph["freshDatastore"], True)
            for row in rows:
                self.assertEqual(row["executionManifest"], configurations[0]["executionManifest"])
            self.assertEqual(database.execute("SELECT count(*) FROM network_attachments").fetchone()[0], 3)
            evidence.update(helpers=rows, graph=graph,
                            executionManifest=configurations[0]["executionManifest"])
        native = list(root.glob("native-*"))
        self.assertEqual(len(native), 1)
        # libcrun retains its seccomp cache and empty mount-source directory.
        # Neither is a native instance or a termination receipt.
        entries = list(native[0].iterdir())
        self.assertLessEqual({entry.name for entry in entries}, {".cache", ".empty-directory"})
        for entry in entries:
            self.assertFalse(entry.is_symlink())
            self.assertTrue(entry.is_dir())
            self.assertEqual(entry.stat().st_uid, os.geteuid())
            self.assertEqual(entry.stat().st_mode & 0o022, 0)
            if entry.name == ".empty-directory":
                self.assertEqual(list(entry.iterdir()), [])
        evidence["passed"] = True
        record.write_text(json.dumps(evidence, indent=2))
        print(json.dumps({"managedBootstrapEvidence": str(record), "passed": True}))

    def test_pinned_schema_lock_and_observed_retained_restart(self):
        settings = json.loads(Path(os.environ["GITSTORE_RUNTIME_FIXTURE"]).read_bytes())
        self.assertEqual(settings["disposableUid"], os.geteuid())
        self.assertIs(settings["dedicatedTestAccount"], True)
        source = Path(__file__).resolve().parents[2]
        command = [sys.executable, str(source / "runtime/experiments/scylladb/server.py"),
                   "--image-root", settings["scyllaImage"],
                   "--api-image-root", settings["migrationApiImage"],
                   "--git-image-root", settings["gitImage"],
                   "--native-probe", settings["scyllaNativeProbe"],
                   "--backup-probe", settings["scyllaBackupProbe"],
                   "--fixture-parent", settings["fixtureParent"], "--restart"]
        result = subprocess.run(command, env={"PATH": "/nonexistent", "LANG": "C"},
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=600)
        # Output is fixed credential-free evidence from this saved probe. No
        # arbitrary helper commands, daemon configuration or personal inputs.
        self.assertEqual(result.returncode, 0, result.stdout.decode(errors="replace")[-8192:])
        evidence = json.loads(result.stdout)
        self.assertIs(evidence["passed"], True)
        self.assertIs(evidence["registeredServicePlanVerified"], True)
        self.assertIs(evidence["registeredWorkerDispatchVerified"], True)
        self.assertIs(evidence["nativeWriterLeaseVerified"], True)
        self.assertIs(evidence["productionHelperActorVerified"], True)
        self.assertIs(evidence["unsafeHelperCancellationVerified"], True)
        self.assertIs(evidence["keyspaceCreated"], True)
        self.assertIs(evidence["migrationCompleted"], True)
        self.assertIs(evidence["apiCheckModeVerified"], True)
        self.assertEqual(evidence["apiGitControllerExit"], 0)
        self.assertIs(evidence["idempotentMigrationCompleted"], True)
        self.assertEqual(evidence["verifiedLedgerEntries"], 12)
        self.assertIs(evidence["datastoreLockConflictVerified"], True)
        self.assertIs(evidence["retainedLedgerRestartVerified"], True)
        self.assertIs(evidence["retainedRepositoryRestartVerified"], True)
        self.assertIs(evidence["backupRestoreVerified"], True)
        self.assertEqual(evidence["restartData"], "independent-restored-copy")
        self.assertEqual(evidence["controllerExit"], 0)
        self.assertEqual(evidence["restartControllerExit"], 0)
        self.assertNotEqual(evidence["pid"], evidence["restartPid"])


if __name__ == "__main__":
    unittest.main()
