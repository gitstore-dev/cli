"""Native core retained-volume checks; never operate on personal repositories."""
import fcntl
import os
import sqlite3
import shutil
import tarfile
import unittest
from support import Fixture


class Volumes(unittest.TestCase):
    def test_observed_stop_restart_preserves_data_identity_hash_and_writer_lease(self):
        f = Fixture(offline="verified")
        self.addCleanup(f.close)
        first = f.start("retained", provision=False)
        f.ready("retained", first)
        with sqlite3.connect((f.root / "state/state.db").as_uri() + "?mode=ro", uri=True) as db:
            volume, device, inode = db.execute(
                "SELECT v.volume_id,v.data_device,v.data_inode FROM repository_volumes v "
                "JOIN resource_ownership r ON r.resource_id=v.volume_id "
                "WHERE r.deployment_id='retained' AND v.allocated=1").fetchone()
        self.assertRegex(volume, r"\Arepo-[0-9a-f]{56}\Z")
        root = f.root / "state/volumes"
        lock = os.open(root / f"writer-{volume}.lock", os.O_RDWR | os.O_NOFOLLOW)
        self.addCleanup(os.close, lock)
        with self.assertRaises(BlockingIOError):
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        f.stop("retained")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        data = root / f"volume-{volume}/data"
        self.assertFalse(data.is_symlink())
        self.assertEqual((data.stat().st_dev, data.stat().st_ino), (int(device), int(inode)))
        # Seed synthetic retained bytes only after observed stop, while holding
        # the actual writer lease. No live repository contents are rewritten.
        marker = data / ".retention-fixture"
        with marker.open("xb") as stream:
            os.fchmod(stream.fileno(), 0o600)
            stream.write(b"synthetic-retained-content\n")
            stream.flush()
            os.fsync(stream.fileno())
        digest = f.digest(marker)
        # Experiment-only backup/restore of this stopped synthetic volume. This
        # establishes filesystem/lease behavior, not a product backup command or
        # datastore migration guarantee. Preserve the original allocation inode.
        tree = data / ".retention-tree"
        tree.mkdir(mode=0o700)
        for index in range(8):
            path = tree / f"item-{index}"
            with path.open("xb") as stream:
                os.fchmod(stream.fileno(), 0o600)
                stream.write(bytes([index]) * (index + 1) * 1024)
                stream.flush()
                os.fsync(stream.fileno())
        def hashes(root):
            paths = list(root.rglob("*"))
            self.assertLessEqual(len(paths), 10000)
            self.assertFalse(any(p.is_symlink() for p in paths))
            self.assertLessEqual(sum(p.stat().st_size for p in paths if p.is_file()), 134217728)
            return {str(p.relative_to(root)): f.digest(p) for p in paths if p.is_file()}
        retained_hashes = hashes(data)
        def preserved():
            current = hashes(data)
            # A real restart adds RPC replay records. Every original retained
            # file must remain present with its original content hash.
            self.assertTrue(all(current.get(path) == digest
                                for path, digest in retained_hashes.items()),
                            "A pre-existing retained file changed or disappeared")
        backup = f.root / "repository-backup.tar"
        with backup.open("xb") as stream:
            os.fchmod(stream.fileno(), 0o600)
            def ordinary(member):
                self.assertTrue(member.isfile() or member.isdir())
                return member
            with tarfile.open(fileobj=stream, mode="w") as archive:
                archive.add(data, arcname="repositories", filter=ordinary)
            stream.flush()
            os.fsync(stream.fileno())
        restore = f.root / "restored-backup"
        restore.mkdir(mode=0o700)
        with tarfile.open(backup, "r") as archive:
            archive.extractall(restore, filter="data")
        self.assertEqual(hashes(restore / "repositories"), retained_hashes)
        # Only replace our newly seeded synthetic files, under the held lease.
        marker.unlink()
        for path in tree.iterdir():
            path.unlink()
        shutil.copyfile(restore / "repositories/.retention-fixture", marker)
        marker.chmod(0o600)
        for path in (restore / "repositories/.retention-tree").iterdir():
            shutil.copyfile(path, tree / path.name)
            (tree / path.name).chmod(0o600)
        for path in [marker, *tree.iterdir(), tree, data]:
            synced = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
            try:
                os.fsync(synced)
            finally:
                os.close(synced)
        self.assertEqual(hashes(data), retained_hashes)
        fcntl.flock(lock, fcntl.LOCK_UN)
        second = f.start("retained", provision=False, key="retained-restart")
        f.ready("retained", second)
        self.assertEqual((data.stat().st_dev, data.stat().st_ino), (int(device), int(inode)))
        self.assertEqual(f.digest(marker), digest)
        preserved()
        with self.assertRaises(BlockingIOError):
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        f.stop("retained", key="retained-final-stop")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertEqual(f.digest(marker), digest)
        preserved()
        fcntl.flock(lock, fcntl.LOCK_UN)


if __name__ == "__main__":
    unittest.main()
