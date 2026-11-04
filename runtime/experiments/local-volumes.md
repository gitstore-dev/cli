# EXP-004B-local: retained volume evidence

Recorded 2026-11-04 on disposable Linux arm64 fixture
`gitstore-m2-linux-a1a13c`, dedicated UID 11000. No personal repository,
configuration or credential store is used.

`scripts/test-runtime.sh --scenario exp-004b-local --fixture
/home/gitstorefixture/experiments/public-runtime-fixture.json` passed the public
CLI native-core scenario in 78.407 seconds. Its result record is
`/home/gitstorefixture/experiments/runtime-run-exp-004b-local-4rb1q9jr.json`;
its retained scenario root is
`/home/gitstorefixture/experiments/gs-public-runtime-fx_pvj_h`.
The runner snapshots and hashes the selected `gs` and `gitstored` binaries before
any case; later developer builds cannot change the selected bytes mid-batch.

The actual Git service held the canonical volume writer lock while ready.
A competing nonblocking exclusive lock failed. An observed public stop made that
same lock acquirable. Only then, while holding the actual lease, the harness
created a private synthetic marker in the retained volume. A second real start
reused the recorded data device/inode and preserved its SHA-256 hash; its live
writer again excluded the competing lock. A final observed stop released the
lease and preserved the marker hash. No live repository data was rewritten.

The Linux C `retained_named_volumes` test additionally forks a child inheriting
the real writer descriptor: closing the parent's copy still rejects a competing
acquisition until the child closes its descriptor and its exit is collected.
The same native primitive tests reject substituted volume/data/lock inodes,
hardlinked locks, symlink aliases, changed store identity and incomplete roots.
`durable_named_volume_records` verifies parent-only transactional receipts,
immutable allocated identity, foreign-owner rejection and database reopening.
All 37 Linux native behavior tests passed after this addition.

These initial checks do not establish full-disk behavior of a live core,
manager-crash reconciliation, PID-reuse handling or safe migration. Later
experiments below add bounded disk and harness backup/restore evidence. Those cases remain open;
retained bytes are never deleted merely to recover a failed test. Recovery and migration tasks retain their separate gates.

Actual full-disk adapter checks also passed through `exp-004b-disk`, record
`/home/gitstorefixture/experiments/runtime-run-exp-004b-disk-w7u1nxb5.json`.
The test-only probe runs as UID 11000 inside its own user/mount namespaces and
mounts a 1 MiB tmpfs. The actual image supervisor returns `QUOTA` before transfer
when the reservation exceeds capacity. After filling only that tmpfs to ENOSPC,
new volume allocation fails with no returned authority; the pre-existing volume
still reopens with the same recorded inode and unchanged retained bytes. The
private mount and its synthetic contents disappear with the probe namespace;
the fixture parent/result remain. No host filesystem is filled or remounted.
This is a native primitive test, separate from full-disk behavior of a live core.

The expanded stopped-volume archive/restore experiment passed in 77.966 seconds,
record `/home/gitstorefixture/experiments/runtime-run-exp-004b-local-mwwnhce6.json`,
retained root `/home/gitstorefixture/experiments/gs-public-runtime-ww2eluno`.
Under the actual exclusive lease, the harness seeds bounded synthetic files,
archives the repository data with the Python tar library into a private file,
extracts with its data filter into a separate private root, and compares every
file hash. Only newly seeded synthetic files are removed/restored under the lease;
restored files/directories are synced before restart. The original volume inode
and every pre-existing file hash survive actual native restart and final stop.
This is an experiment harness workflow, not a product backup command, full-volume
replacement or datastore migration proof. Git legitimately adds new RPC replay
records on restart. The initial overbroad full-directory-equality assertion failed
(`runtime-run-exp-004b-local-irur_89c.json`); the corrected check permits new files
but still rejects any changed/missing pre-existing file. Both results are retained.
