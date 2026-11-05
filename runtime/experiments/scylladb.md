# Persistent Scylla verification

The candidate [Scylla metadata](../profiles/scylladb.json) and
[migration targets](../profiles/migration-targets.json) remain
`executionEnabled: false`. The daemon does not advertise persistent deployment
execution or `migration.start`. The native evidence below is groundwork for that
integration, not public CLI acceptance or release support.

## Pinned inputs

| Input | Verified identity/interface |
| --- | --- |
| GitStore source | `06152d193e854bc643fa884daa8b9f30b0d75d0f`; committed handlers/config, not an upstream working tree. |
| Scylla image | `docker.io/scylladb/scylla:2026.1`; immutable index `sha256:e15660595e31d7391ad62930096f837010a68512bf238140b74b113b187191fb`. Exact child/config pins are in the candidate metadata. |
| Platforms | Linux arm64 manifest/config/layers/native CQL and migration verified; amd64 manifest/config only. |
| Image versions | Scylla `2026.1.15-0.20261012.e56847478575`, cqlsh `6.0.34`. |
| Migration helper | Pinned API image `/app/gitctl migrate`, with hosts/keyspace/username/TLS/shard-aware-port/timeout flags. Password uses `GITSTORE_API__DATASTORE__SCYLLA__PASSWORD`, never argv. Native authentication with nonempty datastore credentials remains unverified. |
| Schema | Target `gitstore-06152d-schema-012`: twelve embedded CQL files with SHA-256 source identities and exact completed ledger checksums/statement counts. Ledger MD5 is upstream compatibility bookkeeping, not a security signature. No arbitrary target/script or downgrade flag exists. |
| Keyspace | Image-native cqlsh executes the exact pinned upstream single-replica NetworkTopologyStrategy statement before the migration helper. |
| API check mode | Pinned configuration supports `api.datastore.scylla.auto_migrate=false`; private persistent actor startup and retained restart verify that mode; public activation remains gated. |

Anonymous acquisition through the production Rust OCI library verified
273,983,661 compressed bytes and 619,594,752 merged bytes into one retained
approximately 870 MiB stage. Allowed authentication/blob origins are pinned to
`https://auth.docker.io` and `https://production.cloudfront.docker.com`. Repeated
native checks reuse that stage. No host CQL/migration executable or generic engine
is installed by the product.

## Native execution constraints

The opt-in `scylla_image_probe`, `scylla_native_probe` and `scylla_backup_probe`
are developer targets excluded from production/installation. Fixed commands run
through libcrun under disposable Linux UID 11000, with read-only rootfs, empty
capabilities, private namespaces and an owned `/var/lib/scylla` bind. CQL has no
host-facing forward. Helpers borrow validated namespace handles and cannot tear
down the datastore namespace. Each has a distinct native process/cgroup identity.

The image user resolves to UID 998/GID 1000. A nonzero-only user mapping cannot
join through this libcrun build (`setresuid(0)` fails). The tested candidate uses
namespace UID/GID 0 mapped solely to the invoking non-root host account, retaining
empty capabilities. This is not host root or a subuid/helper fallback.

Scylla reserves 32 TiB of virtual address space even for `--version`; a smaller
address ceiling caused startup failure. The candidate retains a finite 64 TiB
virtual ceiling, separately limited to 2 GiB physical cgroup memory, one CPU and
1536 MiB Scylla memory. The controller retains its smaller soft address limit.
These are disposable source-preview settings, not production tuning. Direct native
invocation requires `--overprovisioned` and
`--options-file=/etc/scylla/scylla.yaml`; Docker entrypoint argument syntax and a
relative default configuration path do not work. See the vendor's
[container resource controls](https://docs.scylladb.com/manual/stable/operating-scylla/procedures/tips/best-practices-scylla-on-docker.html).

## Actual checks

| Check | Evidence |
| --- | --- |
| CQL readiness, keyspace, migration and idempotent rerun | `scylla-server-b65ytfcr`; image-native helpers, observed successful exits. |
| Actual datastore lock conflict | `scylla-server-wkzn_20r`; held LWT migration lock rejects a second helper, and all 12 ledger entries remain unchanged. The upstream lock TTL is 120 seconds; helper timeout is 60 seconds and never substitutes for observed termination. |
| Retained-data stop/restart | `scylla-server-mds9na2i` / `scylla-restart-zs1sv3ci`; a new native identity opens the same data only after prior exit, empty cgroup and non-forced deletion. Exact completed ledger survives. |
| Saved native scenario | `runtime-run-scylla-native-0phzbvg9.json`, 33.132 seconds; exact probe snapshot, migration/lock/retained-restart assertions. |
| Registered OCI service plan | `runtime-run-scylla-native-3eo7g4fa.json`, 29.519 seconds; actual startup/migration/lock/restore uses `gstd_execution_build` for fixed Scylla command, protected named data mount, private resolver and finite virtual/physical resource bounds. |
| Registered OCI helper plans | `runtime-run-scylla-native-tm8ezytu.json`, 31.060 seconds; repeats migration, lock conflict and independent restore with production plan purposes for service, readiness, keyspace preparation and migration. Helpers receive no datastore or configuration mounts. |
| Actual API check mode and repository continuity | `runtime-run-scylla-native-rov4kysp.json`, 55.400 seconds; `scylla-server-pqldbe6q`. Pinned Git/API use the production bootstrap/OCI builders and shared protected synthetic credentials. API reaches full registered readiness with `auto_migrate=false` before and after independent datastore restore; all 12 ledger entries remain unchanged. The same Git repository directory survives observed stop/restart with digest `ae5507b42bbb63a32b91877bc7ba4081f040ddad78b5f8514d10e0ceb0603488`. |
| Independent backup/restore | `runtime-run-scylla-native-xdjbwt0i.json`, 32.152 seconds; `scylla-server-fjiiu_22` / `scylla-restart-u7gpa0zw`. After observed full stop, independent backup/restored copies are checked by the C payload validator, then actual Scylla opens the restored copy with the same completed ledger. Payload is 1,226,046 bytes; tree digest `fabb14589d1d6a04660826f5cd6af79a83ad196acae820e48dee45665d9ec1b5`. |

Production native dispatch also passes in `runtime-run-scylla-native-kswdgcq4.json`
(57.862 seconds). It repeats the complete workload/restore case through
`gstd_native_launch`/`gstd_native_poll` for registered service/readiness/keyspace/
migration purposes. The backend admits Scylla helpers and requires an inherited
writer descriptor for the Scylla service. Durable acceptance/reconciliation is
separate from this developer-selected native fixture.

The production one-shot helper actor passes in
`runtime-run-scylla-native-oqaqf1xo.json` (71.122 seconds). Its paused CREATE
boundary requires an identity commit before START. Normal/failed helpers are
reusable only after observed exit, empty cgroup and non-forced DELETE. Actual
post-dispatch cancellation preserves a live migration helper and records
uncertainty. Separate natural-exit cleanup does not rewrite that decision, and
the ledger remains unchanged. `runtime-run-scylla-native-isr5k1dd.json`
(54.784 seconds) also verifies missing-writer rejection and a blocked kernel
writer lock after controller/worker copies close. Rootless `/proc/PID/fd` access
can be denied; that is not treated as a descriptor identity receipt.

The full helper/cancellation/restore scenario passes with instrumented control
code in `runtime-run-scylla-native-lygt74vl.json` (74.467 seconds). ASan/UBSan need
the larger control-process virtual-address allowance; normal production resource
limits are verified separately. The managed data, resource receipts and public
capability gates are not relaxed by instrumentation.

Actual datastore ENOSPC passes in `runtime-run-exp-004b-disk-whlsgghr.json`, using
a private 1 MiB user/mount-namespace tmpfs. New repository and Scylla allocation
fail without returned data/writer handles; existing datastore bytes and exact
data/writer inodes reopen unchanged. This checks filesystem primitives, not a
running database's disk-full recovery.

The backup validator describes only bounded owned ordinary payload files. It
rejects links/special files, altered bytes, incompatible targets and added native
identity fields. It grants no destination writer lease or process authority.
The independent-copy experiment is not an implemented product restore workflow.

Reproduce with:

```sh
scripts/test-runtime.sh --scenario scylla-native --fixture /absolute/fixture.json \
  --case test_pinned_schema_lock_and_observed_retained_restart
```

The dedicated-account manifest selects `scyllaNativeProbe`, `scyllaBackupProbe`,
`scyllaImage`, `migrationApiImage`, `gitImage` and `fixtureParent`. Runners retain evidence/data,
use fixed image commands and stop through held pidfds with observed scope cleanup.
They never kill guessed database PIDs or replace a live datastore directory.
API startup also requires Git's internal provisioning listener. Its fixed health
polling follows the daemon's 250 ms cadence; an earlier 1 ms experiment loop hit
the upstream HTTP 429 limit and failed correctly. The corrected scenario keeps
the full datastore/resource-watch readiness requirement. Those preliminary
failures are not migration or release-support evidence.

## Durable integration and remaining gates

Controlled migration tests cover backup/target/keyspace/compatibility/exclusivity,
same-key replay after reopening, failed dispatch writes, unsafe cancellation,
verified success/cleanup, uncertain failure blocking new START and immutable
terminal history. The private migration journal is not native execution authority.

Schema version 3 adds a separate immutable datastore-volume ledger and exact v1/v2
atomic upgrades. Typed filesystem markers and SQL constraints separate repository
and datastore receipts, preserve canonical roots/producers/inodes and inherited
writer locks, and reject cross-role inode reuse. Upgrade tests preserve existing
history and roll back denied writes. These primitives remain private until the
persistent actor is wired.

The private `datastore_start` actor commits canonical volume reservation and
bootstrap freshness before dispatching its filesystem worker, then records the
actual allocated inode receipt before exposing data/writer descriptors. Duplicate
startup is rejected before a second worker. Retained volumes skip keyspace and
migration mutations. Linux normal and ASan/UBSan/leak checks cover this prefix,
the retained original writer, and cache authorization. Application image cache
descriptors stay identical between memdb and Scylla; the implicit Scylla image
has its own compiled descriptor and requires a persistent accepted intent.
Schema/helper policy stays in the execution manifest rather than the per-image
cache descriptor, so changing it does not duplicate an unchanged Scylla image.
This prefix does not launch the datastore or its helpers; full native lifecycle
and reconciliation are still required before public activation.

The private preparation actor also reserves and materializes three independent
execution scopes: the datastore service, reusable CQL helper and schema helper.
The offline arm64 run in
`scylla-managed-t1zpci4f/state-sealed` completed all three with its canonical
datastore writer lease held and no native workload started; its log is
`source/build/daemon-native/datastore-sealed-check.log` in the disposable fixture.
The verified Scylla cache tree contains 940,509,781 logical bytes. Four owned
mode-000 account files require a read-only self-exec reader in an isolated user
namespace. Its same-ID mapping and sole DAC read/search capability preserve file
permissions, identities and modification/change timestamps. The reader accepts
libcrun's pinned self-cloned executable only with all immutable write/size seals
when its mode permits group/other writes. Normal and ASan/UBSan tests cover the
sealed executable, unchanged files, non-dumpable parent and hard-link rejection.
Earlier preparation attempts failed the integrity audit and remain failed
evidence; the successful run does not relabel them.

The private journaled-helper coordinator commits dispatch before CREATE, a
kernel-bound native identity before START and verified deletion before exposing
success or retry permission. It rechecks cancellation after the identity commit.
Controlled rejection of a forged identity retains the owner and withholds START.
The saved private journal scenario passes in
`runtime-run-scylla-native-c44wtayg.json` (33.065 s, Linux arm64 ASan/UBSan with leak
detection). It reserves/materializes the three scopes, starts the canonical
Scylla service, retries only observed-deleted read helpers, creates the fresh
keyspace once and runs the schema helper once. Each native identity is committed
before START and each helper deletion before graph advancement. The graph reaches
application launch; this scenario does not start API/Git/controller. Scylla is
then stopped with held pidfd/cgroup proof and non-forced native deletion. The
retained JSON evidence includes executable hashes, full execution policy and
terminal helper receipts. Earlier prototype receipts remain separate evidence.

Reproduce that scenario with `--case test_private_journaled_persistent_bootstrap`.
Its dedicated-account manifest selects `gs`, `datastoreProbe`, `fixtureParent`
and the four absolute `offlineApiLayout`, `offlineGitLayout`,
`offlineControllerLayout`, `offlineScyllaLayout` inputs. The probe is the development
`deployment_probe` target, with image SDK/runtime/network/bootstrap linkage.
The C control owner is instrumented; the Rust SDK and vendor libraries are
ordinary code dependencies. An explicit sanitizer-only SDK feature permits the
finite 64 TiB virtual-address allowance needed for ASan shadow mappings. Ordinary
SDK workers still require at most 1 GiB. Both builds reject unlimited address
space; these virtual limits are not physical-memory or package-size claims.
The initial sanitizer fixture lacked image linkage, then used the ordinary
SDK limit; those attempts remain failed evidence. A later test assertion rejected
libcrun's retained `.cache`/`.empty-directory` despite successful native deletion;
the corrected test still rejects leftover native instance directories.

Persistent scope receipts bind the complete execution manifest, including the
migration target and policy digest. Changed catalogue policy cannot reinterpret
a prior helper success as a new migration. Per-image cache projections remain
independent, so this does not duplicate unchanged image downloads.

## Persistent actor recovery checks

| Scenario | Linux arm64 ASan/UBSan evidence | Assertions |
| --- | --- | --- |
| Complete startup and retained restart | `runtime-run-scylla-native-9304brog.json`, 145.952 s | Scylla, Git, API and controller reach registered readiness and stop with observed native deletion. Both volume writer identities survive. |
| Observer restoration across both starts | `runtime-run-scylla-native-676lo9ec.json`, 214.412 s | Reopen exact configuration, image scopes, live native/cgroup identities and owned network; observe both volumes without acquiring writer locks; stop through the recovered observer. |
| Actual owner-process loss | `runtime-run-scylla-native-fop4057p.json`, 90.501 s | Four original workload PID/birth identities and both held writer locks survive SIGKILL of the private owner; a fresh process reopens the exact resources and completes observed stop. |
| Copied persistent state cannot transfer ownership | `runtime-run-scylla-native-7qmdiq8f.json`, 79.814 s | A live SQLite backup plus copied volume payload/markers and cache owner metadata is rejected before any native/resource scope dispatch. The original four workloads and both locks survive the copy attempt and owner kill, then recover and stop normally. |

Run these cases as `test_private_persistent_actor_retained_restart` and
`test_private_persistent_observer_restoration` with the same explicit fixture
manifest used above. The actual process-loss case is
`test_private_persistent_manager_loss`. Its initial harness rejected an already
bound cache before restoration; that failed run remains separate, and its live
fixture was recovered and stopped with the corrected harness. The scenarios use
private production actors, not an advertised
Scylla CLI capability. Restoration proves identity for observation/stop; it does
not rerun startup, access credentials or assert current readiness.

Retained startup uses the successful prior enrollment UID recorded with the same
complete execution policy and retained datastore. The upstream public schema has
no service-account lookup query; startup does not invent one or repeat a creation
whose authoritative name already exists. The new API process is bound to that
identity, and the controller must authenticate and pass readiness again. The
second start executes only the read-only CQL helper, without keyspace creation or
schema migration. Missing or changed enrollment evidence fails closed.

The native sanitizer regression selected 61 checks after these changes: 60 passed,
the non-root cross-UID test skipped, and none failed (64.21 s). The separately
verified five-minute idle-deadline check was excluded; the actual partial-frame
deadline check was included.

Recovery accepts a completed helper only with its committed deletion/exit/cgroup
receipt, matching generation, boot, purpose and execution policy, absent canonical
native instance and absent original PID birth identity. A reused PID is not the
original helper. An unresolved mutator remains uncertain and is never retried by
restoration. Linux sanitizer rejection tests cover live original processes,
remaining native state, unsafe flags, overflowed cgroup identity and malformed
policy/graph records.

To limit development disk use, superseded stopped fixtures have had only their
audited generated execution/cache `rootfs` and `blobs` removed. Each has an
`execution-cleanup.json` receipt. This includes the preparation/service/schema
roots under `scylla-managed-t1zpci4f`, the failed initial full actor, and the
superseded `persistent-installed-actor-dj0m0in9`,
`persistent-retained-identity-muv5vlcx`, `scylla-retained-actor-qla_rpmn` and
`scylla-retained-actor-xtlug8_u` runs. Volumes, credentials, configuration,
SQLite/resource receipts, logs and source layouts remain. These roots no longer
contain complete reusable execution scopes. The saved sanitizer actor/restoration
runs retain their scopes.

T066 and T068 are complete in their contract/definition scope. T070 is complete
for owned typed mounts, immutable compatibility/retention and bounded restore
validation: controlled/native disk failure, independent restored-payload validation,
retained restart and copied-live-state rejection are recorded above. This does
not provide a public restore execution workflow. T074's core native
recovery scope is documented in [recovery verification](recovery.md). At this earlier checkpoint, T067,
T071–T073 and T075 remained open: restart cleanup of interrupted migrations, product restore
execution, remaining disk/crash/cancellation cases, public persistent continuity
and amd64 native execution still need their own proof. Later public lifecycle and migration-recovery results below close the scoped task
gates; native amd64 and public restore remain separate limits. No upstream
GitStore service attestations are required.

Standalone migration preparation now has a private metadata resolver. It binds
an admitted running migration to the stopped source deployment's retained full
policy, registered target digest, generation and released resources. Its volume
reservation only reuses an already allocated canonical Scylla ledger; it cannot
allocate a missing datastore or reuse a repository-role volume. Controlled Linux
ASan/UBSan tests hold the original kernel writer while reserving migration
metadata, verify a second writer is refused, then verify the original producer
and inode receipts after deliberate writer release. This is preparation evidence,
not standalone helper dispatch or a public migration capability.

The full private standalone preparation path passes with actual verified image
cache reuse in `runtime-run-scylla-native-y_xu0_1t.json` (19.751 s, Linux arm64
ASan/UBSan). It reopens the canonical retained datastore and materializes service,
CQL and schema scopes under the migration operation. Both repository/datastore
payload hashes remain unchanged. No native service or helper is dispatched, no
credential is generated and no schema-completion claim is recorded. The developer
probe deliberately ends at preflight failure, releases its preparation references
and retains data/scopes. This is not a completed migration.

Reproduce with `--case test_private_standalone_migration_preparation`, selecting
`migrationProbe` and an explicit stopped `persistentPreparationRoot` in the
fixture manifest. The former is a test-only build target; the latter must have
an exact compatible retained policy and verified image cache. The source root is
reused deliberately, so its journal includes the new failed preflight operation.
An earlier probe completed preparation but failed its cleanup assertion by using
`retained` rather than `released` for image-reference lifecycle. Its failed record
and held preparation receipts under `scylla-retained-actor-v46ba7sy` are preserved;
no native dispatch occurred. That fixture requires observed preparation cleanup,
not an ownership reset or another migration attempt.

The standalone private execution scenario now passes in
`runtime-run-scylla-native-o2jjyeup.json` (42.019 s, same Linux arm64 fixture,
ASan/UBSan), selected with `--case test_private_standalone_migration_execution`
and `scylla-standalone-migration-fixture.json`. It uses the registered service,
CQL and schema scopes, validates existing-keyspace/history preconditions,
dispatches the image-native schema helper once, then validates all 12 completed
ledger entries. Its source receives an owned non-forced deletion receipt; the
original datastore producer/data/writer identities remain unchanged. All runtime
owners are released only after source, writer and network cleanup. There is no
API workload and no public migration capability claim. T069 is complete; T072
manager/public wiring and T073 broader failure/cancellation validation remain.

The preceding read-only run reused an actual surviving source after the probe
exited, checking its native PID/birth/boot, cgroup and network identities. Its
successful final diagnostic is `build/daemon-native-sanitized/migration-live-fixed.log`.
The initial helper failures were real: a helper-wide 64 KiB `RLIMIT_FSIZE` prevented
cqlsh's Python package extraction (`Errno 27`). That limit was removed. Migration
stdout now uses a pre-sized 64 KiB anonymous file sealed against growth/shrink;
the shared output offset records actual bytes and write sealing follows observed
helper deletion. This bounds output independently of helper scratch storage and
avoids SIGPIPE on owner loss. The failed records remain available.

Metadata schema version 4 upgrades exact versions 1–3 atomically. Its separate
migration attachment guard preserves the SQL expression-depth bound and only
admits canonical migration workloads with a stopped deployment and current held
preflight control. Six focused Linux sanitizer checks passed, including native
identity recovery, schema upgrades, migration control, execution plan, volume
records and store durability; the five applicable host checks passed as well.

The nonblocking production migration coordinator subsequently passes the same
scenario in `runtime-run-scylla-native-tmq55jin.json` (54.930 s). Repeating it with
pre-START population journals passes in `runtime-run-scylla-native-_pd11g5f.json`
(37.479 s): the real service and both helper scopes retain boot/path/cgroup/event
inode receipts. Public manager dispatch remains gated while recovery/control
integration is being verified. Both successful operations remain immutable.

The actual persistent manager-loss/copy test also passes with these new journals
in `runtime-run-scylla-native-57zenocb.json` (111.663 s). All four original native
identities and both kernel writers survive SIGKILL of the manager. A separately
restarted observer rejects a substituted API cgroup inode, after which the fixture
restores the original receipt and the next observer adopts/stops the exact owned
resources. No native replacement or signal is dispatched by the rejected observer.
The live-state copy is independently rejected as before. Its retained evidence is
under `scylla-retained-actor-y1bx2hvo`.

An observation-only failed-migration target now binds recovery to a separately
running current STOP, a newer control generation, the original released START
policy and the migration's exact configuration policy. It grants no preparation
or helper retry authority; controlled tests reject foreign/stale STOPs and prove
normal execution factories still refuse this failed operation. Integration of
interrupted migration helper observation and cleanup remains outstanding (T071).

### Interrupted migration ownership recovery

On the selected disposable Linux arm64 fixture, ASan/UBSan native result
`runtime-run-scylla-native-fywhqswa.json` (45.955 s) passes a surviving schema
helper case. The private fixture stops the exact helper with its held pidfd,
kills only its manager, and waits until the new recovery observer holds the
original helper pidfd/cgroup before resuming it. Recovery itself never signals a
running mutator. It observes natural helper death, non-force native deletion,
source stop, exclusive datastore writer reacquisition and network death. The
original failed migration state/result/error stay unchanged; the separate stop
succeeds. The reconciliation receipt records an unknown helper exit status, not
a fabricated successful schema result.

Prepared but undispatched migration recovery also passes in
`runtime-run-scylla-native-gsxwuq5i.json` (0.761 s, nine owners) and
`runtime-run-scylla-native-0ipb9o7u.json` (1.578 s, ten owners including an unbound
network reservation). Both preserve physical repository/datastore payload hashes.
These recover the earlier interrupted preparations in `scylla-retained-actor-v46ba7sy`
and `scylla-retained-actor-lsokn7w8`; their original failed evidence remains valid.
After all owners were released, inode-checked cleanup removed only generated
execution rootfs/blob copies, with receipts `execution-cleanup-1793874290.json`
and `execution-cleanup-1793874316.json`. Volumes, cache inputs, metadata and evidence
were retained. Missing/replaced cgroups or inaccessible ownership still retain
uncertainty. Public migration dispatch and the wider validation matrix remain open.

The surviving-helper scenario passes again through production manager stop
submission, durable acceptance, scheduler dispatch and recovery polling in
`runtime-run-scylla-native-rx1sztj2.json` (42.550 s). The private probe supplies
already reopened fixture roots; it no longer invokes the recovery actor directly.
Six focused Linux sanitizer checks and five focused host checks pass after this
dispatch change. This verifies manager integration, not installation setup or a
public migration capability.

### Public persistent lifecycle

`runtime-run-persistent-scylla-d72b4527.json` passes in 103.439 seconds on the
selected Linux arm64 ASan/UBSan fixture. `scripts/test-runtime.sh --scenario
persistent-scylla --fixture /absolute/owner-selected.json` now exercises actual
installed sibling `gs`/`gitstored` copies and their authenticated Unix socket.
The selected case starts a fresh Scylla-backed core graph, waits for current
readiness, stops it, invokes the registered standalone migration, replays its key,
and verifies canonical datastore producer/data/writer identity and released
ownership. It uses existing verified offline image layouts and synthetic protected
credentials. Matching targets/digests and `migrationCompleted` now survive the
public result projection. Unregistered or forged target digests stay private.

The earlier `runtime-run-persistent-scylla-yxfq8068.json` remains failed because
the original result projection omitted those completion fields; no native
migration failure is hidden by the later pass. Queued migration cancellation is
control-only and running cancellation returns `UNSAFE_CANCELLATION`. Migration
uses cached/offline registered images and grants no implicit download consent.
The public source preview is documented in `docs/local-runtime.md`; native amd64,
optional graphs, end-user backup/restore and publication are separate gates.

Disk reclamation after the failed projection case removed only 18 retired
execution rootfs/blob trees under `gs-public-runtime-qnb8pk7d/state` and 12 under
`scylla-retained-actor-lsokn7w8`, after exclusive state/cache/volume/writer locks,
released metadata owners, canonical native absence, original PID checks, inode
receipts and read-only tree audits. Receipts are `execution-cleanup-1793876479.json`
and `execution-cleanup-1793876486.json`; trimming restored Mac free space from
12 GiB to 29 GiB. No data volume, credential, cache input or evidence was removed.

The expanded public test passes in
`runtime-run-persistent-scylla-u2bm8u0n.json` (148.345 s). It pauses only the exact
Scylla init through a held pidfd, observes a failed stop deadline, proves the
canonical datastore writer remains excluded, resumes that init, and completes
a separate stop while the failed stop's public history remains identical.
Standalone migration and same-key replay then pass. Its migration workload
assertions check exact source/CQL/schema resource IDs and deleted helper receipts.
`runtime-run-persistent-scylla-yqmqo5y0.json` (0.594 s) rejects an explicitly
mismatched Scylla offline image before any native workload is recorded, and owned
cleanup succeeds. Neither scenario fills the host disk.

After these public runs, the generated execution copies were retired under the
same exclusive-lock/native-absence/inode/tree-audit checks. Cleanup receipts
`execution-cleanup-1793877252.json` (`gs-public-runtime-6jdysg60/state`, 18 trees)
and `execution-cleanup-1793877259.json` (`gs-public-runtime-k86sq57m/state`, 18 trees)
retain the original payload/metadata/cache/evidence.

Current combined private backend acceptance passes in
`runtime-run-persistent-scylla-zh3v4dcx.json` (62.860 s test/62.911 s runner), via
the `persistent-scylla` scenario. It verifies schema/lock/exclusive writer, unsafe
helper cancellation, unchanged ledger, independent retained backup/restore,
repository retention and API migration-check mode. The selected native probe hash
is `d05e13a808af5b9a05d7a2187b80f5eb676b6a3e2fd4f5d9d1666aa8d75457c6`;
backup probe hash is
`683bab0ac38ed245ec60ef1a01ec65648d9d83cdfa9a9bb06f49da478af650fb`.
The runner's generic public adapter label in that saved record does not describe
this private restore case; its case assertions and private probe evidence do.
Future records label that scope explicitly. T073/T075 are checked against the
requirement mapping in the contributor validation guide, not a public restore or
release-support claim.

The current 30-second interrupted-migration stop bound passes with an actual
surviving mutator in `runtime-run-scylla-native-gz0xqe0j.json` (29.129 s overall
setup/recovery). It reuses the stopped public-profile data/cache, waits for the
new manager observer before resuming the exact helper, and preserves the original
failed operation while owned stop succeeds. The preceding `xkmyiwaq` attempt
failed in test setup before acceptance because the older probe assumed deployment
`one`; its failure/early-return test allocation log is retained. The test-only
probe now takes a validated explicit deployment selector and filters recovery
lookup by that identity; it does not rename data or rewrite ownership to fit.
