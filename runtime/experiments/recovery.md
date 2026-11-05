# Native recovery verification

These are actual Linux arm64 libcrun/libslirp/image-library checks using the
public CLI and disposable UID 11000. They are development-host evidence, not
published release or cross-platform support. The persistent Scylla daemon graph
is still gated; the cases below exercise the implemented core memdb graph with
retained repository volumes. A retained Git volume does not restore memdb
catalogue identity.

Run saved cases with `scripts/test-runtime.sh --scenario recovery --fixture
/absolute/fixture.json`, optionally selecting `--case test_method_name`. The
explicit fixture selects installed own binaries and verified offline images;
credentials/configuration are synthetic and isolated. All roots remain available
for inspection. A timeout or lost owner never authorizes deleting retained data.

| Boundary | Actual checks |
| --- | --- |
| Manager SIGTERM or SIGKILL | Workload PID/birth/boot identities survive; a replacement manager reopens exact recorded native/network/volume identities and performs observed stop. Credentials and asset hashes remain unchanged. |
| Lost native CREATE reply | Manager dies after canonical native creation but before its reply; recovery validates the partial graph before cleanup or replacement. |
| Stop timeout | Paused live work prevents proof of exit; stop fails with uncertain ownership. A later observed stop releases the original writer without rewriting failed history. |
| Partial stop and manager loss | Committed termination receipts permit already-deleted workloads to remain absent; remaining live work is handled through its own identity. |
| Workload exit/health loss | Current readiness is invalidated without rewriting the successful start outcome; paused live work can regain readiness without a restart. |
| Public logs | Replay/resume, cursor expiry and observer interruption preserve workload identity, writer ownership and immutable start history. |

The full eight-case recovery checkpoint passed in
`runtime-run-recovery-covwa9md.json` (426.8 seconds). The later standalone public
log check passed in `runtime-run-recovery-dk4ib1st.json` (39.187 seconds).
These records refer to the exact selected historical binaries; subsequent
focused cases record their own executable snapshots.

The new concurrent-writer case seeds only synthetic retained bytes after an
observed stop while holding the real writer lease. It then starts the actual
Git service, kills the manager, checks the original native identities remain
live and the same lock inode is held, and submits three simultaneous new start
keys. Each must fail before replacement. Only an observed stop may free that
same lock inode; the retained hash must survive the later authorized start.
Failed preliminary runs reached correct `OWNERSHIP_UNCERTAIN` rejection but had
an assertion that attempted to parse stderr progress as JSON. The corrected
assertion reads the final machine error record. They are failed test records,
not claimed completed verification.

Remaining persistent-profile gates include the registered Scylla graph, datastore
writer leases and restore/compatibility proofs. Native Scylla helper/schema and
retained-ledger experiments are documented separately in
[Scylla verification](scylladb.md); they do not close those daemon integration
gates or authorize takeover from a copied metadata/volume snapshot.

The corrected concurrent-writer case passed in
`runtime-run-recovery-kjv0_xve.json` (112.252 seconds; retained root
`gs-public-runtime-fh3c19b2`). Three concurrent requests each returned
`OWNERSHIP_UNCERTAIN` before accepting a replacement. The original three native
PID/birth identities remained live, the exact writer-lock inode stayed held,
and the seeded retained hash remained unchanged across observed stop and a
subsequent successful start. This completes T074's core native recovery scope;
it does not enable the pending persistent datastore graph.

After observed stop, generated execution rootfs copies were reclaimed from the
four stopped fixture roots `gs-public-runtime-jm0h4gb_`,
`gs-public-runtime-s_74kvyu`, `gs-public-runtime-fh3c19b2` and
`gs-public-runtime-6jg18v22`. Cleanup required both lifetime locks, released
resource records, absence of the recorded live native identities and exact
owned execution inode receipts. Their metadata, logs, cache, credentials and
retained data volumes remain; `execution-cleanup.json` records the removed
generated copies. Historical evidence therefore does not imply those execution
directories still exist.

## Observed-exit restart supervision

[Exact source/native receipts](restart-evidence.json) extend manager-loss recovery
with bounded whole-deployment replacement after held native exit observations.
The production manager records independent idempotent stop/start operations and
uses the existing observed-cleanup/writer gates. Core and retained Scylla tests
check all original service pidfds before waiting for replacement readiness,
unchanged volume identity and immutable historical success. A live paused API
regains readiness without replacement. Manager loss ends automatic supervision;
recovered owners still require explicit observed stop/start.

Four attempts use 250/500/1000/2000 ms delays without blocking control. Failed or
uncertain stop, failed replacement, operator intervention or exhausted budget
ends retries. Replacement uses existing verified assets with provisioning off.
This qualifies the named Linux arm64 source fixture only; other host/release
claims and complete optional-provider graphs remain separate gates.
