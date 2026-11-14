# Local runtime validation archive

Historical implementation checkpoints for contributors. For usage, read the
[user guide](../../local-runtime.md). For current build instructions and limitations,
see [contributor documentation](../README.md). Older measurements and claims below
apply to their recorded checkpoints, not automatically to the current build.

**Updated**: 2026-11-06. **State**: Source preview; release qualification remains
incomplete. Public core memdb/retained Scylla and optional OIDC startup, readiness,
logs, stop/restart and owned manager-loss reconciliation have passed on the
explicitly selected Linux arm64 fixture. The non-test production build passed the
full nine-service graph; standalone OIDC also has sanitizer acceptance. Current
ordinary and sanitized C suites select 72 tests each, with one cross-UID skip;
exact results are in the latest evidence below. Pinned Rust format/Clippy/test/build
checks accompany the implementation.

The [task record](../../../specs/003-m2-daemon-foundation/tasks.md) distinguishes
implementation completion from release qualification. Admin UI and Storefront
retain their component-specific external mapping prerequisites. Private native
backup/restore does not provide a public restore command. The
[Mac library experiment](../../../runtime/experiments/apple.md) does not qualify
public macOS execution. Native amd64, clean-host/provider support, hosted workflow
runs and own signed CLI publication remain separate gates. Local Linux Postman
contract runs passed; earlier macOS Postman initialization failed with
`SecItemCopyMatching failed -50` and does not substitute for Linux contract evidence.

No third-party executable bundle is used: Git, image transfer/unpack, libcrun and
libslirp use ordinary code libraries. See [current measurements](../binary-sizes.md)
for exact artifact identities and separate core/package/host-library costs. The
user accepted the size baseline and deferred optimization. Retired execution
copies remain an additional disk cost, outside the shared-cache limit; automatic
execution-copy reclamation is not implemented.

Older entries below are checkpoint history. They retain failed attempts and
source-specific measurements; this current scope and the linked task/evidence
records take precedence over earlier statements about open tasks.

## Current cancellation and persistence checks

T056 is implemented and verified. Cancellation resumes durable running control
intent after transient storage failures, skips actors already handling that
request, and drains terminal control bookkeeping without releasing live health
leases. Terminal targets (`succeeded`, `failed`, `cancelled`) return an independent
successful no-op, including when their completed job row is absent. Enrollment
and native creation without a recorded PID reject cancellation before signaling.
Eight occupied runtime slots do not consume the separate bounded control queue.

Start/stop terminal writes now remain pending and retry their metadata transaction
when storage is unwritable. Actors, reservations and the parent's writer lease
remain held. Retrying finalization does not repeat native or credential effects.
Readiness is withheld while start finalization is pending. Stop helper deadlines
are capped by the overall 30-second stop budget; an expired private worker is
reaped without treating that as proof that a workload stopped.

| Check | Result and scope |
| --- | --- |
| Host daemon suite | 41 passed. |
| Linux native suite | 45 passed; cross-UID test skipped under UID 11000. |
| Linux ASan/UBSan suite | 45 passed; same explicit UID skip. Rust image SDK is not instrumented. |
| Terminal persistence | Actual SQLite write denial; successful retry; immutable history; retained writer lease on expiry. Linux additionally pauses and reaps a real private filesystem worker. |
| Real active cancellation | `runtime-run-recovery-ta96j3au.json`, 38.613 s, retained root `gs-public-runtime-46titx21`: independent/replayed cancel, cancelled start, cleanup deadline with retained ownership, new safe stop and terminal no-op. |
| Real graceful manager exit | `runtime-run-recovery-2up_aqsh.json`, 83.145 s overall: paused service/health lease, manager exits within 30 s, workloads and ownership survive, replacement manager stops and restarts the deployment. |

The first active-cancellation attempt (`runtime-run-recovery-pspchvjz.json`)
failed because the test opened a pidfd from reserved metadata with PID zero.
The saved case now waits for a positive recorded native PID. Its two surviving
fixture processes were separately stopped using held pidfds after exact UID,
boot and birth-time verification; data and durable ownership were not deleted
or released. This manual fixture cleanup is not product recovery evidence.

T054/T058 now pass the US3 controlled recovery boundaries and affected real core
checks. `runtime-run-recovery-covwa9md.json` records all eight actual Linux recovery
cases passing in 426.8 seconds. The new lost-CREATE-reply case freezes and kills
its manager after libcrun creates the API init but before its PID is committed.
Read-only recovery validates the recorded bundle/rootfs, discovers and holds the
exact native process/cgroup, persists that identity, skips the never-dispatched
controller and performs observed owned cleanup. Created inits use identity-bound
KILL rather than starting an application merely to stop it; running services keep
graceful termination. Earlier failed fixture cleanup also succeeds through a new
public stop with the fix. Missing native state, dead identities without sufficient
termination proof and incomplete earlier allocation receipts remain uncertain.

Current checks additionally pass 44 host tests excluding the already exercised
slow timer, the saved `scripts/test-daemon.sh` Rust/C process runner, a 47-case
native regression run (one non-root UID skip), and a 49-case ASan/UBSan run (same
skip) before the separate retained-log target was added. The new log-store target
and updated queue/redaction tests pass separately under native and sanitizers.

T064 production connection bounds pass on macOS (311.42 s), Linux (310.67 s) and
Linux ASan/UBSan (310.65 s), using the real 10-second partial-frame and five-minute
idle timers. The tests saturate 32 connections, reject the excess peer, preserve
coalesced request ordering, reject oversized frames and keep an unrelated client
responsive. The implementation admits one request per connection at a time,
within the published 16-request ceiling. Independent durable control-queue tests
still pass with eight runtime slots occupied. Existing bounded fuzz smoke/CI
integration is preserved; SDK CI avoids repeating the identical slow timer and
job time allows its real duration.

SC-003 controlled crash/dedupe/capacity and SC-004 retained-data/owned-stop cases
pass within these scopes. SC-005 parser/peer/connection/control tests pass; complete
live stream/log capture remains US4 work. Broader persistent orphan recovery,
automatic bounded restart and Scylla recovery remain T067/T071/T074 work. These
checks do not close all DEP-012 or establish complete M2/release support.

## Stream/log primitives under integration

T059 core tests pass on the host, Linux and Linux ASan/UBSan. The queue tests
cover connection-owned identities, bounded closed-subscription handling, stale
connection reuse, filtering, ordered event sequence, independent fast/slow
observers, eight subscriptions, 256 events, 4 MiB per queue and 64 MiB global
accounting. Redaction holds incomplete lines across every tested byte split,
removes known credentials and sensitive-field/PEM lines, omits oversized/binary
or invalid UTF-8 input and fails closed after sink errors.

The separate segmented log journal tests cover per-deployment/global eviction,
seven-day policy with a tightened fixture clock/limit, cursor replay/reopen and
explicit foreign/evicted/old-index expiry. An actual SQLite byte scan confirms
that the synthetic credential was never written before redaction. Operation
metadata remains separate and unchanged. Production log ceilings are 64 MiB per
deployment, 1 GiB globally and seven days maximum age. The 512 MiB log DB page
bound leaves room for a full rollback journal within 1 GiB physical storage;
physical pressure can therefore expire logs before the payload ceilings.

Native capture now runs before the first libcrun CREATE, using separate stdout
and stderr pipes for Git/API/controller. Its private collector reads the existing
bootstrap credential, redacts before append and survives manager loss while the
actual workload writers live. Capture receipts belong to the configuration scope
wrapper; material data retains its strict bootstrap allowlist. The real
`runtime-run-recovery-h__af2fd.json` case passed startup, workload-exit readiness
invalidation and owned stop in 45.864 s, retaining 36 API, 29 controller and 12 Git
records. A private byte scan found none of the fixture password/hash/JWT/HMAC
values. `runtime-run-recovery-pchs8ueq.json` separately passed lost-CREATE-reply
recovery with capture enabled in 11.935 s.

Private replay opens the dedicated store-bound log directory read-only and never
creates or repairs an absent index. At most two five-second readers share the
eight lifecycle/health worker slots. Disconnect abandons observation; its slot
remains reserved until waitpid proves the direct child is reaped. Metadata schema
version 2 adds these immutable leases through an atomic exact-schema upgrade;
operation history, deduplication and resource ownership survive. Tests exercise
missing history, replay pages larger than pipe capacity, cursor expiry, capacity,
disconnect, collision guards and version-1 migration/refusal of damaged schema.
Targeted replay delivers only to the selected subscriber, preserving other
subscriptions' cursors and sequence namespaces.

Verification: host regular suite 46/46 excluding its socket test, with that socket
test separately passed outside the binding-restricted sandbox. Linux regular
suite: 53 passed and the explicit non-root peer-UID skip, excluding the already
verified slow timer. Six focused Linux native and ASan/UBSan/leak checks pass for
capture, replay, migration, queueing and read-only history. Rust image SDK code
is not sanitizer-instrumented. The Linux daemon now advertises `stream.read`. The public socket test requires
that capability, observes its acknowledgment before 300 ordered events, exercises
unsubscribe/already-closed identity and explicit cursor expiry. The coordinator
test additionally isolates a slow observer's overflow and abandons only the
selected reader on disconnect. Full regular native regression has 55 passes plus
the non-root peer-UID skip. The public socket, coordinator, replay, capture and
queue checks also pass ASan/UBSan/leak instrumentation.

T061–T063 are implemented. Actual `gs` processes verify text and NDJSON output,
partial frames across polling windows, Ctrl-C during pending subscribe and blocked
stdout, and foreground stderr routing with retained owned-cleanup semantics.
Pinned Rust format, Clippy, full workspace tests and build pass. The host regular
C suite is 47/47, and the saved Rust/C runner passes. Production mode rebuilds and
contains no `gstd_test_*`/controlled-backend symbols.

`runtime-run-recovery-dk4ib1st.json` passes the real native public-log case in
39.187 s (`gs-public-runtime-idnvhuy5`). It resumes API logs after a retained cursor,
interrupts standalone observation, rejects an expired cursor and proves unchanged
API birth identity, generation, writer ownership, credential digest and terminal
start result before a separate explicit stop. This is Linux arm64 source-build
evidence, not publication or additional-host support.

T065 verification also passes the final 60-second fuzz smoke: 61,046 protocol
and 603,730 framer inputs, no findings under ASan/UBSan/leak checks. The saved
Rust/C runner now includes `daemon_logs`. Full final native and sanitizer suites
each select 56 regular entries: 55 passed, one explicit non-root peer-UID skip;
the previously proven five-minute production timer is excluded. A subsequent
collector test verifies survival of an identity-bound SIGXFSZ quota signal under
both builds, preserving the output drainer instead of propagating closed pipes
into workloads. The production compiler's conservative snprintf overlap warning
is resolved with a separate nonce buffer; the production build and test-symbol
exclusion check pass.

SC-005 stream isolation, replay gaps, queue bounds, peer/parser limits and safe
observation failure pass within these scopes. SC-006 local log workflows run with
isolated configuration, synthetic credentials and no host Git/runtime downloads;
standalone stdout and foreground stderr routing are verified. No additional host,
credential-store provider or published-release compatibility is established.


## Implemented development subset

- C17/CMake daemon/core targets with production/test build separation; version,
  help and explicitly selected owner-only Unix socket serving.
- Pinned Jansson 2.15.1 and SQLite 3.51.3 source archive builds using recorded hashes;
  optional offline source-cache directory still enforces those archive hashes.
- Bounded C/Rust JSON/framing, strict request shapes, identifier/enum/parameter
  validation, duplicate keys, safe integers, UTF-8, string/collection/depth limits.
  All 40 separate messages from the 22 M0 examples are exercised as wire vectors;
  this does not execute all lifecycle methods.
- Peer UID authorization, owner-only runtime directory/socket/lock, safe path
  traversal, duplicate-manager exclusion, negotiation and system capability reads.
  `system.read` and `deployment.validate` are advertised; configuring an explicit
  state directory additionally enables deployment/operation reads and waits.
  Fully linked installed Linux builds additionally advertise start/stop and bounded
  cancellation. Unavailable code libraries leave observation usable and lifecycle
  unavailable. Host/profile prerequisites remain independently checked.
- Shared profile/deployment/operation/dedupe/component/resource schemas enforce
  typed identifiers, registered enums, foreign keys, current-generation readiness
  and terminal-only result/error fields. Missing required record tables are refused
  on reopen; record/state schema checks pass locally.
- Initial private SQLite owner/schema checks, foreign keys, DELETE/EXTRA
  synchronization and rollback. The 4096-byte page size and 32,000-page bound leave
  room for a full rollback journal below the 256 MiB metadata budget. Later tasks
  add retention and actual backend crash recovery; current storage checks alone
  do not prove those guarantees.

- Atomic acceptance commits normalized SHA-256 intent, replay/conflict identity,
  generation, resource reservation and scheduler job before dispatch. Eight runtime
  slots/64 queued jobs and an independent bounded cancellation queue are tested.
- Canonical writer lock/recovery interfaces keep uncertainty until explicit backend
  termination evidence. Actual init exit, held-cgroup emptiness and Git writer
  lease reacquisition pass on the selected fixture. Lost-parent discovery/recovery
  retains its separate T054 gate.
- Deadline-bounded Rust client authenticates/negotiates, correlates out-of-order
  responses/events and preserves the caller key through explicit reconnect/replay.
  Local command syntax routes before remote configuration/credentials and rejects
  invalid formats, identifiers, timeouts and credential input before connection.
  Remote waits retain DEP-004; local execution commands remain capability-gated.

- Immutable compiled `core` / `1` profile metadata, image/platform pins and versioned
  config/bootstrap planning. Read-only validation expands core dependencies and
  reports backend, asset, native validation, host and optional profile limitations.
  API/controller normalized overlays and Git single-file rules are tested; native
  service TOML/environment validation is unperformed. Private local selection
  configuration has separate flag/environment/config/default precedence.

- Bounded deployment list/inspect and operation inspection/waits are available
  when an explicit private state directory is configured. Store-bound persisted
  five-minute cursors reject stale/foreign positions; waits use monotonic deadlines
  without cancellation and refuse old-generation readiness. State locks exclude
  another manager even through a different socket directory. Seeded process checks
  prove paging, restart, format and timeout identity behavior, not real workloads.

The state API is not yet connected to workload execution. Local routing is separate
from the existing remote dispatch. Existing user/configuration/credential stores
were not used.

The internal [image code library](../../../daemon/image-backend/README.md) now performs
bounded manifest/config/layer acquisition, explicit credential/origin policy,
digest/platform checks, byte-bounded merged tar output and maintained-library unpack.
It borrows a pinned owned staging descriptor, requires bounded native worker
memory/files and disabled core dumps, and never activates assets or writes the
database. C/Rust build integration is Linux-only and remains an explicit development
option in the current control build. Interactive consent/owner settings, profile
credential-reference wiring, service dispatch and lifecycle readiness remain
unimplemented. The native provisioning layer now adds the internal cache pipeline
recorded below. No new protocol
capability is advertised from this code.

The internal [credential broker](../../../tests/runtime/credential-broker/README.md)
uses the installed gs executable over a private same-UID descriptor, with exact
installation/issuer/account/purpose/registry/repository keys. OS-store reads are
default and protected files are explicit; failure never selects another source.
The Linux worker executes a pinned registered broker FD, closes unrelated FDs,
sanitizes its environment, bounds/reaps the process and erases secret buffers.
No third-party broker executable or public registry credential command is added.
Read brokerage is implemented; bootstrap generation/persistence and owner-managed
credential-reference wiring remain later work. Linux OS-store access through this
launcher currently requires the canonical /run/user/UID/bus. No real user store was
accessed, and native store availability is not established by synthetic checks.

## Local checks and limits

Host: macOS arm64; AppleClang 21.0.0, temporary CMake 3.31.6, Rust 1.96.0.
CMake and clang-format are development tools under `/private/tmp`, not runtime
assets or additions to the distributed executables.

| Check | Actual result / limit |
| --- | --- |
| Native debug and production C builds | Pass locally; production version identifies production build |
| Native CTest | 22 checks pass on macOS and in the linked-SDK Linux arm64 build, including parent-only asset records, Linux atomic promotion/cleanup, bounded tree verification, immutable descriptors, verified manifest file identity, image C/Rust boundary, framing/JSON, private store/records, atomic acceptance, scheduler, ownership interfaces and socket/path security |
| Native ASan/UBSan | All 22 C checks pass on macOS with SDK disabled; all 22 Linux arm64 Clang 18 C checks pass (including native promotion/cleanup). Rust SDK/compression code is not ASan-instrumented; hosted jobs remain unverified |
| Rust/C process runner | Two isolated setup/native negotiation cases, five wire/shape cases, two native read-only preflight/config cases and a seeded inspection/restart case pass with explicitly supplied C test binary, including both decoders over examples/adversarial inputs |
| Pinned Rust format, Clippy with warnings denied, locked build | Pass locally |
| Default locked Rust behavior suite | Pass with loopback fixture socket binding permitted; image/native-store checks remain intentionally ignored |
| Postman skills/status | Existing host Keychain error `SecItemCopyMatching failed -50`; no Postman simulation pass claimed |
| Linux arm64 native control/cache | 22 C checks pass in the disposable Ubuntu 24.04 guest as UID 11000; no GitStore services executed |
| Linux daemon CI | Workflow added; hosted execution not run in this session |
| Different-UID peer isolation | Native mechanism implemented; distinct-UID host test remains unrun |
| Native libcrun C API experiment | Tiny namespace lifecycle/worker-crash/FD/exit checks pass with static/shared callers; instrumented shared libcrun/caller passes Clang 18 ASan/UBSan; cgroups/network excluded |
| Image-library experiments | Rust native-TLS selected after macOS/Linux arm64 C bridge and seven policy/extraction checks pass; adapter evidence is separate below. Activation and provider/core-image validation remain pending |
| Image staging adapter | Seven Rust unit tests pass on macOS, thirteen on Linux plus an explicitly run synthetic private-registry scenario. Pinned Clippy passes on both. Actual API image staging is recorded below; other core images and service lifecycle remain unverified |
| Network-library experiment | Select libslirp 4.9.7-gstd-queueview2 with exact private access-view patch. Normal GCC Werror and Clang ASan/UBSan static/shared/concurrent probes, deliberate unaligned queue operations and all five upstream tests pass. Pristine 4.9.7 remains rejected; runtime integration pending |
| Credential broker | Four Rust scope/file/private-process tests pass on macOS and Linux arm64; C malformed/timeout/secret-release checks pass, and the full Linux C launcher → real installed gs → private-file path passes. No native OS-store availability claim |
| Internal image/cache pipeline | Actual pinned arm64 API image passes managed acquisition/digest/platform/unpack, synchronized tree receipt, parent-only durable verification and isolated atomic promotion. Held reference blocks deletion. Separate one-millisecond acquisition deadline and controlled staging-cleanup tests pass. No service is executed |
| Product lifecycle/network | Eight real CLI core cases plus installed startup, owned reverse stop, same-deployment warm restart and exact TUN denial pass on the selected Linux arm64 fixture. Full restart reconciliation, logs and migrations remain open |
| Core size target <=5 MiB | Earlier gs baseline: 8,808,560 stripped bytes. Current development daemon: 7,116,616 stripped bytes. Both miss the engineering target; optimization and complete package/runtime costs remain open |

The stripped Linux C/SDK staging-boundary test executable is 4,004,416 bytes and
uses OpenSSL 3/libgcc/platform shared dependencies. This excludes the full control
owner and runtime backend; it is not a final daemon size result. The earlier image
comparison separately records actual shared-library bytes. Ordinary SDK code is
compiled from its own Cargo manifest so native-TLS features cannot alter gs's
separate Cargo resolution. No Go/image/network executable is added to distribution.

T036 validates immutable core image descriptors against the compiled catalogue,
including tampering followed by rehashing, actual code/build observations and
read-only plan projection. Already-present host/cache manifest files are checked
through borrowed read-only descriptors: registered digest, trusted owner/mode,
single-link regular file, bounded bytes and unchanged identity during the read.
Consumers recheck the descriptor before use. The real public arm64 API manifest
fixture supplies positive byte-level evidence. This verifies manifest content,
not image layers/configuration/rootfs readiness or arbitrary native providers.
All selected native capabilities are ordinary code dependencies; no external
executable provider or host plugin loader is selected. Internal provisioning/activation passes its scoped image-cache probe; owner
owner settings, provisioning-job dispatch and lifecycle recovery remain T043/T045/T071 integration. The exact GHCR blob CDN origin is now registered from anonymous routing
observations for all six pinned component/platform references; unknown token/blob
destinations remain denied. No signed URLs/tokens are recorded.

The sandbox denies socket binding: initial socket/loopback tests failed with
`Operation not permitted`. Rerunning only the isolated fixture tests with binding
permitted passes; that is a test-environment distinction, not relaxed product auth.

The native cache filesystem phase is Linux-only; macOS checks exercise its shape
validation but do not establish macOS promotion/cleanup support. Controlled metadata
checks cover cold consent, reservation limits, restart, immutable receipts, held
references and rejection of missing/altered protection triggers. The required
schema definitions are compared through SQLite against the compiled initial
schema. Older unreleased development layouts are refused without repair; no
published-store migration is claimed.

See [cache pipeline evidence](../../../runtime/experiments/provisioning/evidence.json)
for actual image bytes and the cache-only accepted/pending laboratory intent.
Logical tree bytes count hard-linked names separately and are not a physical
installed-size result. Owner settings/cache binding, offline import/resume/eviction
and public service dispatch are subsequent tasks.

## Dependency/source details

[Dependency lock](../../../runtime/dependencies.lock.json) records exact sources/build
choices. The private Jansson patch allows NUL-containing JSON keys using its
existing length-aware APIs/duplicate checks, preserving M0's UTF-8 string-key
contract. A preparse structural bound counts at most 32 containers; the private
parser cap is 33 because it also counts the scalar value at the leaf. JSON
allocations are bounded at 16 MiB cumulative per parse and 64 MiB live globally
on the single control owner. None of this is a general JSON parser rewrite.

[Upstream evidence](../../../runtime/profiles/upstream-evidence.json) records pinned
GitStore source and alpha.8 Linux image index/platform/config digests. All core
image revision labels match the recorded server revision; labels and registry
metadata are not execution or attestation evidence. Optional component mappings
remain unverified. No upstream code was edited or executed.

## Remaining work

Continue with T031–T050 real runtime library/provisioning/lifecycle work, then
diagnostics, persistence and delivery gates. The user authorized either local container service. A disposable Apple-backed
Ubuntu 24.04 arm64 guest is running with no host home mount or SSH-agent socket.
All 22 C control/cache checks pass as UID 11000 under GCC 13.3.0/CMake 3.28.3
and OpenSSL 3.0.13. This is Linux control evidence, not libcrun/network/workload
validation; hosted CI and amd64 remain unverified.
DEP-012–015 remain open. The shipped server features are available integration
inputs, not evidence that this daemon has implemented them. CLI publication needs
its own artifact attestations; upstream image attestations are not required.


T039 adds explicit no-network OCI import with the same trusted pins, private
cache/store directory identity and lifetime lock, immutable registry/offline
source authorization, verified whole-blob resume (partial blobs restart without
range validators), and one-at-a-time zero-reference eviction. Eviction keeps its
budget until the separate worker has removed/synchronized owned paths and the
parent verifies their absence in the bound cache. Controlled checks cover
modified/occupied/foreign roots, interrupted cleanup, refused reference attachment
and immutable source/profile/ownership metadata. The real API offline cache
pipeline passes with `allowProvision:false` and no registry access; the real API
resume worker also passes network-isolated. The synthetic private registry proves
only the truncated layer is refetched in full. Exact evidence is in the
[provisioning record](../../../runtime/experiments/provisioning/evidence.json).
No service is started by these checks; abandoned-job compensation, public owner
settings/consent UX and runtime dispatch remain later implementation tasks.


The T040 preparation probe additionally passes real delegated systemd cgroups
(cpu/memory/pids), a small explicit seccomp filter, no-new-privileges and zero
capabilities for the tiny static payload. Normal/static and direct-ELF Clang 18
ASan/UBSan runs pass both normal and lost-start-ack scenarios. Guest-only fixture
session preparation is recorded in the
[delegated libcrun evidence](../../../tests/runtime/libcrun/delegated-evidence.json).
This original preparation checkpoint did not exercise the daemon adapter or an
actual GitStore service. The subsequent adapter checkpoint is recorded below.


T040 preparation also adds a no-network private workload materializer. The real API
image reconstructs from verified cached blobs into independent files, with the
shared source unchanged. Import/resume/materialization reject overlapping directory
ancestry. This prevents native mount-point preparation from mutating a shared
verified cache root. Additional transient disk costs remain visible, and native
backend/lifecycle ownership wiring is still pending.


T040 now has a private libcrun adapter, fixed registered core launch factory,
nonblocking bounded worker supervision and parent-only PID/start-time exit
collection. Real library tests pass normal and lost-start-ack cases with the
registered Git command running a synthetic payload; stale identity, cancellation,
read-only mounts, private tmpfs and inherited descriptor checks pass. The exact
static native library/caller also passes Clang 18 ASan/UBSan. Instrumented builds
omit worker/workload address-space limits for ASan shadow reservations; ordinary
runs verify those limits separately. The Mac control suite and instrumented Linux
control suite each pass 26 CTests. See the [adapter evidence](../../../tests/runtime/libcrun/adapter.md).
Actual GitStore execution, canonical-volume/network lifetimes and public lifecycle
dispatch remain T041–T043; capability advertisement stays gated.


T041 now includes private named repository-volume primitives and a parent-only
immutable SQLite ledger: resource intent commits before filesystem allocation,
root/store/producer/data/lock identities remain bound, allocated missing data is
never recreated, and retained metadata cannot be deleted or rewritten. Reopen,
second-writer, hardlink, symlink, replaced lock/data and changed-receipt checks pass.
The native registered probe keeps its writer lease at intentional FD 3 after the
parent closes its handles, while manager/SQLite descriptors stay out of the payload.
It reopens the same retained data only after observed exit and native delete.

The selected libslirp source build and private bounded network engine pass owned
TAP, DNS, fixed API/Git loopback forwarding, independent control and cleanup,
including concurrent instances and Clang 18 ASan/UBSan. The owned namespace
constructor checks descriptor types, owner, UID/GID maps, namespace linkage and
keeper PID/start time. Registered native normal/lost-ack cases join those exact
namespaces and mount the fixed read-only resolver; stale-keeper CREATE is rejected.
Both combined paths pass sanitizer checks. The control suite has 28 passing checks
on Mac and instrumented Linux at this checkpoint. See
[network adapter evidence](../../../runtime/build-inputs/libslirp/adapter-evidence.json).
T041 now also has a confined persistent network worker. Accepted ownership is
recorded before a blocked fork; the parent commits PID/start/boot/scope identity
before permitting namespace/TAP/listener effects. Only the user/net namespace FDs
are exported. Closing parent handles or losing readiness acknowledgment leaves the
owned worker running. Read-only reopen compares the persisted ledger with actual
kernel identity, keeper namespaces, held lock and high loopback listener inodes.
The namespace attachment is immutable and committed before native workload CREATE.

The worker uses a separate mapped user/mount/IPC namespace, a one-MiB/32-inode
read-only tmpfs root and zero capabilities/NNP. It drops outside directory FDs and
uses libcrun's maintained seccomp compiler with a strict reduction of the compiled
Moby policy. Actual exec, Unix-socket creation, new namespaces, host signals, new
pidfds and filesystem writes are denied; synthetic DNS/forwarding still works.
Concurrent normal/lost-ack scenarios pass in ordinary and Clang 18 ASan/UBSan
builds. Both combined libcrun scenarios also pass, including durable attachment,
idempotency/tamper refusal, retained writer lease and ordered owned cleanup.
The EINVAL old-mount-handle and bounded-JSON allocator failures found during this
work were fixed; failed attempts are not counted as passes. Native workload checks
now bind boot identity and actual systemd cgroup membership as well.

These are private code primitives and synthetic workloads. Public dispatch,
actual GitStore startup and whole-manager restart/reconciliation remain T042–T050;
capability advertisement stays gated.


T042 now has the memdb bootstrap graph and private preparation/readiness workers in
[bootstrap.c](../../../daemon/src/bootstrap.c), with
[recorded real-image evidence](../../../runtime/experiments/provisioning/bootstrap-evidence.json)
and a [reproducible runner](../../../runtime/experiments/provisioning/README.md).
Published alpha.8 API, Git and controller Linux arm64 images passed actual unprivileged
libcrun/libslirp execution, normal and ASan/UBSan adapter paths, current-instance
health, public Login/createServiceAccount enrollment of the existing controller key,
authenticated Git smart HTTP, and reverse-order stop. The first probe's 20-second
controller wait expired before recovery completed at about 25 seconds; its recorded
resources were subsequently stopped through verified identities. The corrected
probe uses a monotonic 60-second health deadline. This failed first attempt is
retained in evidence rather than counted as a passing run.

The graph rejects stale-generation observations, requires private UID binding before
controller launch, retries only pending health reads, and preserves known partial
UIDs with an uncertain enrollment outcome. Controller overlay receipts bind the
stored material, profile, generation and current API boot/PID/start identity; changed,
missing or partial once-created inputs fail without replacement. Core secrets are
independently generated and persisted add-only through the installed `gs` broker.
The OS store remains the default; these tests explicitly used a disposable protected
file and do not establish positive native OS-store creation. API issuer, controller
signing record, Git HMAC and public enrollment material remain separate.

The Linux normal and ASan/UBSan behavior suites each pass 31 checks. Host Rust
workspace tests, locked Clippy/build and formatting pass. The 29-check macOS suite
had one sandbox socket denial; the affected isolated socket security test passed
outside that sandbox. OpenAPI validation passes; Postman lint reports five requests,
zero errors and zero warnings. A saved API readiness request separately passed one
assertion against a controlled local endpoint. The full Postman collection, simulation,
native config validator, real user-repository restart/backup and public lifecycle
commands remain unverified. The native graph is development evidence, not T043
public dispatch, T049 clean-install acceptance or M2 completion.


T043 preparation now includes a stable, read-only private owner-settings snapshot
at explicit-state manager startup. Unknown keys, unsafe modes/links/parents,
embedded-NUL selectors, missing offline inputs and unsupported credential sources
are refused before opening state.db or performing credential/asset effects. The
settings tests pass on macOS and Linux; the Linux normal and sanitizer suites now
pass 32 checks. A separate new bootstrap-job boundary test passes normally and
under ASan/UBSan: worker crash, deadline and cancellation preserve uncertainty,
leave the cancel byte unconsumed and do not create credential files. The real core
probe also passes through the reusable nonblocking bootstrap-job interface on
both paths. These are T043 prerequisites; public start/stop dispatch is still gated.

Public CLI acceptance scaffolding is in [tests/runtime](../../../tests/runtime/README.md).
Its Python syntax is checked; its actual CLI scenarios have not been executed or
claimed passing. Interactive approval, deliberately lost acknowledgments and
missing-host/interrupted-acquisition cases remain to be added before T031 closes.


T043's stop preparation now includes an independent held-cgroup2 observer and a
real Git writer-lease lifetime check. The pinned libcrun delete path may warn on
cgroup destroy failure and still delete native status; its successful return alone
is therefore insufficient writer-termination proof. The normal combined-library
run and ASan/UBSan native run both confirm that actual Git retains FD3 after the
parent closes its writer handle, another acquisition is refused while Git lives,
and the same retained data inode can be reacquired after observed init exits plus
held-cgroup subtree empty/deactivation evidence. Live membership is checked against
captured kernel inodes, not a guessed path. Forged ordinary/unlinked files and
stale process identities fail the sanitizer negative checks. Two earlier unknown
observer outcomes and their retained, identity-checked stop are recorded as failures.
Linux kernfs returns ENODEV for the held deactivated events node while cached link
counts can remain nonzero; this handling is checked against the kernel sources
linked in the evidence record. Public resource dispatch and crash recovery remain
pending, and no volume ownership is released merely from native-delete success.


The latest full behavior runs pass 31 checks on macOS, 34 on Linux with the
combined image/execution/network/bootstrap libraries, and 34 on the Linux native
ASan/UBSan build (Rust image SDK not instrumented). The actual `gs` missing-execution
capability scenario also passes against an explicitly selected own control binary
on the dedicated Linux account: no asset or bootstrap credential effects occur.
Positive public CLI runtime scenarios still await the T043/T045 dispatcher/UX.


T043 preparation also has a nonblocking typed filesystem worker with private
SCM_RIGHTS transfer for cache/volume lifetime locks and reserved resource
directories. Its normal Linux tests pass accepted-intent setup, close-on-exec
handle transfer, parent-only cache/volume receipts, competing writer denial,
retained data reuse, fresh-cache tamper rejection and cancellation preserving
uncertainty/unconsumed notification. The updated settings tests prove distinct
protected-file names for different installations/deployments, stable same-identity
reuse and rejection after configured directory replacement. The owner setting is
now an explicit `protected-directory` policy resolving to ordinary scoped broker
files, avoiding a single shared bootstrap file. Adding reserved `configuration`
resources changes the unreleased prototype schema; prior experimental databases
remain retained and are refused by strict schema checks, not migrated or adopted.
Public lifecycle dispatch is still pending.


T031's public runtime scenario implementation is now checked as added, covering
all six US5 cases plus dropped durable acknowledgments and partial/offline/tamper
inputs. That task checkbox records test code, not a passing runtime suite. Python
syntax and the actual missing-execution-capability case pass; positive public CLI
runtime cases await T043/T045/T046 implementation and T047 execution.

T043 now has parent-owned asynchronous deployment and cleanup actors in
`deployments.c`/`cleanup.c`, composed with durable generation/configuration
journals and the typed image acquisition actor. The real offline API acquisition
probe passed cold import, same-inode warm use without consent and fresh tamper
rejection (`asset-actor-_81idlmo`). The full private core actor passed acquisition,
private materialization, retained volume, isolated network, Git/API/controller
bootstrap, current readiness and observed stop (`deployment-actor-5g2c0gad`).
The first full actor run (`deployment-actor-0lr092_f`) reached readiness and stopped
its processes but failed the final metadata transaction: asset unpin requires
`released`, not `retained`, lifecycle state. The fix preserves retained data while
releasing only the observed asset reference. That failed journal remains intact;
its exact recorded workload and network PIDs were confirmed absent. Public server
routing, lifecycle UX and restart reconciliation remain unverified; T043 remains
unchecked. The independent parent-journal tests pass on macOS and normal/sanitized
Linux, including stale generation, rollback, one-shot credential reservation and
partial enrollment retention. Host behavior tests pass 32/32 including the socket
security case rerun outside the sandbox's Unix-bind restriction.

T043's public Unix dispatcher now passes real-image start, same-key replay,
current readiness, safe loopback endpoint projection, responsive inspection and
observed stop (`dispatch-ktnjlg_2`). An extended run (`dispatch-ctx6a3b6`) also
passes repeated stop, generation-3 warm restart and unchanged scoped bootstrap
material. Parent capability disposal restores the pre-deployment FD count
(`deployment-actor-d60z0msg`). Installed-broker lookup accounts for libcrun's
sealed-memory re-execution: the preserved absolute entry point must match the
running executable bytes before its sibling `gs` is pinned. Lost manager readiness
is invalidated before replacements; full recovered-resource reconciliation remains
T054. The task is checked for implemented dispatch, not completed M2 or broad host
support. Native image-validator execution, failed partial-cache reclamation, logs,
migrations, clean-install costs and CLI lifecycle UX retain their later gates.

At this checkpoint the host behavior suite passes 32/32 and the Rust locked
workspace tests, Clippy and build pass. Unix-socket tests were rerun outside the
sandbox's bind restriction; it was an environment denial, not a passing in-sandbox
run. Normal and sanitized Linux behavior suites passed 36/36 before the subsequent
public-manager/readiness additions, which require their updated regression runs.

T044 installed serve startup is implemented and verified on the dedicated fixture
(`installed-startup-j5fmzv5w`): matching private binary copies, fixed installed FD
execution, a separate session, inherited bounded FD3 readiness, authenticated
socket/version handshake, and no accepted operation, asset or volume effects.
A regular-file readiness FD was rejected with exit 2 before state/runtime writes.
The configured optional user-service unit passes `systemd-analyze --user verify`
against its private candidate binary without installation or activation. Directory
and installed-file security tests pass; the host Rust workspace and C behavior
suites also pass. CLI serve UX remains T045/T046, and manager restart/lifetime
resource reconciliation remains T054. The canonical dedicated-account runtime
directory and all experimental state were retained; only the identity-pinned own
manager was terminated after the startup probe.

T045/T046 real public CLI tests now pass all eight core cases: consented cold
registry acquisition/readiness (`gs-public-runtime-_3a5ikpg`), foreground SIGINT
owned stop (`j2dcp_71`), simultaneous isolated deployments (`pg64qgzb`), absent
execution capability (`q1vqpcak`), unavailable optional mapping (`zppmtehu`), PTY
asset approval (`hz86g1j4`), dropped durable acknowledgment replay (`pp7_50ua`) and
detached same-key reconnect (`z1oqj3jv`). Short suffixes share the
`/home/gitstorefixture/experiments/gs-public-runtime-` prefix. All fixtures remain.
The added host preflight passes the native core actor (`deployment-actor-y4ve3tar`)
and actual TUN permission denial reports `UNSUPPORTED_HOST`/`tun` before any image,
credential or workload allocation (`gs-public-runtime-btd_ztx_`); the dedicated
guest's exact device mode was restored to 0666. Independent controlled metadata
tests also pass queued cancellation with all eight runtime slots reserved, terminal
no-op, refusal of unprovable active cancellation and stale foreground stop binding.
These controlled tests are separate from native interruption/stop evidence.

The current development daemon stripped copy is 7,116,616 bytes
(`core-size-4vabnjo6`), above the 5 MiB engineering target. This closes no packaging,
clean-install or size gate. Public asset failure/recovery, native validator, full
logs/migrations and manager restart reconciliation still require their later work.

T047's explicit scenario dispatcher is implemented. `core-offline` routed to the
actual CLI/SDK/native/network adapters and passed (`runtime-run-core-offline-6xe5yved.json`,
retained `gs-public-runtime-jge5_14i`). The runner verifies selected own binary
identities, distinguishes unavailable fixture/host evidence (exit 77) from failed
assertions (exit 1), writes private result metadata and retains all scenario roots.
EXP-003B's broader failure/security and volume-maintenance results remain T048;
calling a scenario dispatcher is not evidence that its unrun cases passed.

### Optional component audit (T050 remains open)

[The optional evidence record](../../../runtime/profiles/optional-evidence.json) pins the
source/configuration evidence and public registry manifest/config digests. The
OIDC bridge alpha.8 is published for amd64 and arm64 at the selected upstream
revision. Hydra, Kratos, their reference UI and PostgreSQL also have native image
metadata for both architectures. The reference MailSlurper image resolves to an
amd64-only manifest; the arm64 provider graph requires an approved native SMTP
replacement or a verified external SMTP mapping. This is not an attestation gate.

A complete provider graph, migrations, private client enrollment and current
issuer readiness are not implemented/verified yet, including on amd64. Image
metadata alone does not establish lifecycle support. Native libcrun UID 10000 and
owned TAP/slirp/DNS probes passed on the selected arm64 fixture; nonzero namespace
UID support is not itself an external blocker. The registered core graph continues
to use its existing UID policy.

At the pinned revision, ADR 0012 remains Proposed and no published Admin or
Storefront image/configuration/enrollment/readiness mapping has been identified.
Those components retain explicit prerequisites rather than invented endpoints.
`gs COMPONENT serve` and `gs serve --add COMPONENT` now name the missing component
prerequisite before manager discovery or provisioning. Duplicate selections are
usage errors first. Two Rust process checks and two actual Linux public CLI
scenarios passed; Linux record `runtime-run-optional-8vhn_ooy.json` retains the
exact binary identities. No deployment, asset or credential was created. T050 is
not checked complete because the shipped OIDC graph still has implementation work.

After the namespace UID policy changes, all 33 host daemon tests, 37 native Linux
tests and 37 ASan/UBSan tests (including leak detection) passed. These do not close
the later recovery, streaming, Scylla, size or host-matrix gates.

### Controlled process-kill replay (T051)

`daemon/tests/test_crash_replay.c` kills the actual database-owning child with
SIGKILL at eight boundaries: before acceptance, immediately before the acceptance
COMMIT, after durable acceptance without acknowledgment, after dispatch, after a
controlled allocated ownership receipt, and after each of failed/cancelled/
succeeded terminal commits. Reopening the store proves all-or-nothing operation,
dedupe, job and ownership records; replay preserves the original operation ID,
running/terminal outcome and allocated held receipt; wrong-intent key reuse
conflicts without creating another operation. Terminal mutation is rejected.
The pre-COMMIT crash uses SQLite's trace callback in the test executable only;
production code exposes no environment-triggered kill point. These checks pass
on macOS, native Linux and the Linux ASan/UBSan build. They exercise durable
receipts, not native container allocation/reconciliation. The actual public CLI
lost-ACK case remains separately recorded in the core acceptance evidence.

The reference SMTP architecture gap is being evaluated for a local profile
replacement, rather than treated as requiring an upstream GitStore release.
Mailpit documents native ARM images and SMTP on 1025:
[image documentation](https://mailpit.axllent.org/docs/install/docker/) and
[SMTP configuration](https://mailpit.axllent.org/docs/configuration/smtp/).
Candidate v1.33.0 is a separate runtime asset, not part of the core binaries or
an upstream reference image. Its immutable metadata and Kratos courier behavior
must pass before selecting it. The reference-image mismatch alone does not justify
closing T050 or declaring OIDC impossible on ARM.

The initial T052 checks in `test_recovery_faults.c` pass on macOS and native Linux,
including Linux ASan/UBSan. A separate process holds the canonical writer lease
through uncertain observation and actual SQLITE_FULL from a bounded private
SQLite page quota. Durable acceptance replays after reopening; corrupt-store
input is refused unchanged and retained marker bytes remain intact. This does not
close T052: retention pressure, additional PID identity and saturation combinations
remain to be exercised with the later recovery implementation.

### Controlled workload facility (T053)

`daemon/tests/backend_controlled.c` provides eight fixed-behavior direct-child
fixtures: slow, failed, uncertain observation and unsafe migration cancellation.
Private pipe release, exact registered identities, deterministic refusal of
unknown identities/unsafe cancellation, exit collection and owner-only cleanup
are checked in `test_controlled.c`. Children execute built-in C branches only;
there is no executable, shell, path or argument input. Uncertainty is explicitly a
controlled observer fault, not native backend evidence. `tests/failpoints.c`
provides the reusable pre-COMMIT process-kill hook. Both sources are compiled only
into dedicated test executables inside `GITSTORED_TESTING`; neither is linked into
the production daemon or available through production profiles/capabilities.

The complete Linux suite now passes 40/40 native and 40/40 ASan/UBSan checks,
including leak detection. The developer Go image/registry probe tests pass with
isolated local listeners. T051/T053 are checked; T050 and T052 remain open with
their explicitly recorded remaining work. T054 production reconciliation has not
been replaced by these controlled fixtures.

The T052 suite now also seeds the 100,000-record budget with recent controlled
terminal metadata, proves new acceptance returns RESOURCE_LIMIT while the original
key still replays, and preserves retained bytes. Scheduler checks saturate all
eight runtime slots, 64 pending runtime jobs and 64 independent pending control
jobs; the next control reservation fails explicitly and existing control work can
still dispatch. These are metadata/control tests, not native workload concurrency.
They do not implement safe historical eviction or the later reserve-accounting
policy.

The read-only `gstd_native_watch_open/poll` prerequisite binds a held Linux pidfd
to same-UID PID, kernel start time and creator boot, checked before/after opening.
Its process test watches a grandchild the observer cannot reap, rejects wrong
start/boot and ordinary descriptors, and observes exact death after the actual
parent reaps it. Native and ASan/UBSan tests pass. This does not by itself prove an
empty cgroup, release a writer, attach a recovered actor, signal a process or close
T054 production reconciliation.

T052 is now checked for its controlled fault-test scope, combining the new store/
writer/pressure scenarios with exact PID/start/boot and canonical inode/alias cases
in the shared native identity/volume tests. Actual kernel PID reuse is not forced;
wrong recorded start times and creator boots are explicitly rejected. Production
retention/eviction and crash reconciliation remain T054–T055 implementation work.

### Recovery implementation checkpoint (T054 remains open)

Startup invalidation now lives in `daemon/src/recovery.c`. Its short metadata
transaction marks lost resource ownership uncertain, invalidates old readiness,
and records failed/uncertain running operations without changing completed
outcomes. Undispatched accepted work remains queued. Repeating the boundary is
safe; interrupted stop requests also propagate uncertainty even after desired
state became stopped. Focused native and ASan/UBSan checks pass. The real public
core offline lifecycle passes after the initial refactor (`runtime-run-core-offline-kcbk_wf7.json`,
41.449 s) and again with pre-CREATE execution receipts
(`runtime-run-core-offline-yg9nj63x.json`, 40.649 s); these use the current development
pair, not a rebuilt minimal package candidate.

Before native CREATE, exact materialized rootfs/bundle device/inode receipts are
now committed into the workload identity. Repeated identical receipts are allowed;
changed handles, wrong ancestry or substituted directories are rejected. The new
private filesystem-worker reopen action validates selected profile/store/operation/
generation, ledger identity, protected owner record and exact directory identities.
Missing or replaced roots are refused without creating or repairing them. Native
and ASan/UBSan worker tests pass for configuration/workload reopening, stale
identity and missing/replaced rootfs. This action is not a public adoption command.

Native actor reattachment, complete owner-only compensation, stage reconciliation,
ongoing readiness monitoring and real post-manager-crash stop/restart still remain
open. The new read-only process and scope primitives do not substitute for those
checks. Older allocations without the new execution receipt must fail closed;
no compatibility/adoption of incomplete receipts has been claimed.

[Recovery prerequisite evidence](../../../runtime/experiments/recovery-evidence.json)
records source/probe hashes and exact retained roots. Network recovery now runs in
an asynchronous private worker and returns four explicit capabilities; the parent
requires equality with its expected ledger identity before adoption. Normal,
lost-ACK and sanitizer native TAP/slirp/DNS/forwarding/reopen/owned-stop probes pass.
A deliberately changed parent start identity is refused while ownership of the
reply FDs is preserved. These probes do not exercise the full restarted manager.

The retained-volume observation worker verifies existing marker/device/inode
receipts while returning no writer lease and making no creation/fsync/repair call.
The original writer remains held through observation and close. A separate
recovery target reader preserves the original resource generation when a later
stop advanced desired generation; it refuses released/foreign bindings and grants
no provisioning or credential creation. Focused native and sanitizer checks pass.

### Public manager-loss stop/restart checkpoint

The real Linux arm64 public recovery scenario passes in
`runtime-run-recovery-wb5g0wge.json` (85.436 s; retained fixture
`gs-public-runtime-1zu8q4qj`). The runner pins both executable hashes before the
batch. All three core native PID/start identities survive manager SIGKILL. The
replacement manager invalidates old readiness, preserves the completed operation
record and same-key replay, then uses read-only root/scope/volume reopening and
exact live native/network receipts to stop recorded resources. Held pidfds and
cgroup population handles prove termination without requiring the new manager to
be their reaper. Keeper exit and writer reacquisition are observed before release.
A warm restart reaches current-generation readiness while preserving credential
hashes, asset hashes and the retained volume device/inode. No credential generation,
image provisioning or unverified readiness occurs during reattachment for stop.

Missing recovery roots fail without creation/repair. Metadata preparation alone
cannot authorize cleanup; all resource identities must pass before the actor is
eligible. Cleanup can use the original resource generation separately from a
later desired stop generation. The complete current Linux ASan/UBSan suite passes
41/41, including leak detection; native focused worker/record/control tests also
pass. These are development checks, not release or cross-host support.

The earlier private observer regression passes at `actor-restoration-p4ytf19d`;
it kept the original process alive and is distinct from the public SIGKILL test.
Its first two setup failures are retained: `actor-restoration-d5e69iyg` ran the old
probe after a missing header stopped the build; `actor-restoration-f2_83vjr` rejected
Cargo's hard-linked broker before native service creation and completed owned
cleanup. A protected single-link broker copy fixed the latter fixture prerequisite.

T054 remains open for partial-stage/lost-reply reconciliation, retry-safe
compensation and ongoing readiness/restart behavior. No absent backend ID alone
is accepted as termination proof. T055 metadata retention/eviction, US4 streams,
persistent Scylla, optional OIDC graphs and delivery/size/host gates also remain.


### Metadata retention and same-manager stop retry checkpoint

T055 now implements bounded maintenance in `daemon/src/retention.c`, inside the
same acceptance transaction. A sweep removes at most 256 unreferenced, normal,
terminal operations whose acceptance and last update are both older than seven
days, and only after every associated dedupe key has expired. Active operations,
every deployment start while its deployment exists, current desired identity,
operation dependencies, live observer jobs, resource/asset/volume producers and
credential-reservation tombstones are preserved. No runtime directory, volume or
credential is deleted. Database pages are reused; maintenance does not VACUUM or
require another full database copy.

The 100,000-record ceiling reserves its final 128 positions for stop/cancellation.
Capacity checks account for SQLite free/max pages, 512 KiB of generated stage/result
metadata per active operation and new acceptance, current intent size, control/health
headroom, and filesystem room for rollback-journal pages/headers. This measures
available space; it cannot reserve space against unrelated disk writers. The fixed
32,000 × 4096-byte database limit still leaves database plus worst-case rollback
journal below 256 MiB. A quota or storage failure refuses acceptance before runtime
effects. Same-key replay precedes capacity checks and remains available at capacity.

The retention regression checks bounded and rollback-safe collection, recent
terminal retention, active/current/start/resource/dependency/observer/dedupe
protection, reduced page quota, reserved stop capacity, and successful fresh
acceptance after eligible expiration. Native 43/43, Linux ASan/UBSan 43/43 with leak
detection, and host control 39/39 passed at this checkpoint. Later added fuzz/peer
checks have separate evidence below.

The real Linux arm64 `test_stop_timeout_can_be_retried_without_rewriting_failed_history`
scenario passes in `runtime-run-recovery-mc9t0btm.json` (68.751 s;
`gs-public-runtime-jts22mti`). Pausing the exact owned Git process causes the first
30-second stop to fail with retained ownership. After resumption, a separately
accepted stop reuses held pidfd/cgroup proofs and already completed stages,
advances the desired generation, and observes writer release. The prior failed
operation remains unchanged and credential hashes are preserved. Controlled
metadata checks also force repeated deadlines without filesystem/native dispatch.
This addresses retry within the same manager; partial-stop manager-loss discovery
and lost native replies remain open under T054.

T057 adds actual `gs` process regressions in `crates/gs/tests/daemon_lifecycle.rs`
for same-key rerun after lost ACK, changed-intent conflict, detached acceptance
timeout without cancellation, and foreground SIGINT before/after terminal start.
Every case verifies unchanged disposable Git/configuration bytes. The controlled
peer does not establish native or durable-store behavior; the existing C crash and
public runtime cases supply their separate evidence. All three tests pass, with
both foreground paths exercised. Required Rust format, lint, workspace test and
build checks pass. No user configuration or credentials are consumed.


### Protocol allocation-failure and fuzz checkpoint

T060 provides the saved `scripts/test-daemon-fuzz.sh --seconds 60` command,
Clang-only CMake targets, deterministic seed generation, explicit allocation/input/
RSS/deadline limits, and separate JSON/request and incremental-frame harnesses.
The sanitizer CI lane now invokes the bounded smoke; hosted CI is not yet verified.
The test-only allocation-denial hook and peer-check wrapper are absent from the
production configuration, verified by its symbol inventory.

The first allocation-denial run exposed a reproducible assertion in pinned
Jansson 2.15.1: ignored saved-token buffer growth failure reached
`lex_unget_unsave` with an unsaved byte. The private audited dependency patch now
makes token-growth failure a sticky lexer error and returns it immediately.
It preserves the maintained parser and its normal cleanup paths. The deterministic
regression tests denial at 32 allocation positions, requires any successful result
to equal parsing without denial, and checks the next request still parses.
The dependency patch is recorded in `runtime/dependencies.lock.json`.

Final Linux arm64 smoke, Ubuntu Clang 18.1.3, ASan/UBSan/leak detection, seed 1:
53,566 JSON/request executions and 652,664 frame executions; each harness reports
31 seconds for its requested 30-second limit (libFuzzer's completion granularity).
Generated mutation corpora are reused between runs. The initial deterministic
source set contains 58 JSON and 121 frame seeds. The original assertion finding
remains in the ignored fixture build directory; no new finding occurs after the
fix. This is smoke evidence, not exhaustive fuzzing or all hostile-client limits.

The normal native and sanitizer suites each pass 43 tests with the cross-UID test
explicitly skipped when run as fixture UID 11000. That isolated kernel credential
test additionally passes in both configurations when explicitly run in the
root-capable disposable guest: an owner peer is accepted and a socket created
*after* its child drops to UID/GID 11001 is rejected. No daemon runs as root and
no host accounts are modified. Host controls pass 39/39; saved Rust/C process
interoperability and the required Rust format/lint/workspace-test/build checks pass.
The C sanitizer configuration does not instrument the Rust image SDK.

Current task count is 55/84. T054 partial-stage recovery, T056 completion of the
remaining cancellation/deadline cases, T058 consolidation, US4 streaming/logs and
resource-pressure cases, persistent Scylla, optional graphs and delivery gates
remain open. No M2 completion or release-support claim is made.


### Partial-stop manager-loss and observation persistence checkpoint

T054 now records an atomic post-delete receipt for each successfully stopped
workload, after held pidfd exit, held cgroup emptiness and successful non-forced
libcrun deletion. The receipt binds the original native identity and resource
generation, owning stop/cancel operation, desired generation and held cgroup
identities. Recovery accepts a retained stage only with that receipt and retained
matching operation history; it still reopens exact owned filesystem scopes and
proves all remaining live workloads/network identities. A missing PID/path or
bare retained label is insufficient. Retention preserves stop/cancel history while
the deployment has unreleased resources, including these receipt dependencies.

The real Linux arm64
`test_manager_loss_after_partial_stop_uses_committed_termination_receipts` passes
in `runtime-run-recovery-y1e9890f.json` (39.596 s;
`gs-public-runtime-f4epywyt`). Git is held paused while API/controller complete
observed deletion. After manager SIGKILL, the replacement manager validates those
two durable receipts and the remaining live graph. A new stop advances desired
generation, then Git resumes and observed cleanup releases the writer. The
interrupted operation remains failed/unchanged, and credential hashes persist.
Controlled receipt tests reject changed native identity/generation, zero cgroup
identity, foreign resources and missing owning history; they grant no native
recording permission without held kernel evidence.

Current health supervision also records an in-memory persistence-failure gate.
A failed health-state transaction cannot leave an old durable `ready` row exposed
as current proof: inspection projects failed/uncertain without endpoints, ready
waits remain pending, and listing refuses an inconsistent cursor snapshot.
Immutable operation history remains readable. The gate clears only after a fresh
observation commits, never after an idle poll. The controlled Linux monitor test
uses actual SQLite `query_only` write denial while the old ready row remains,
then proves isolation to the selected deployment/operation and recovery after
persistence is available again.

Cancellation verification additionally reopens the store between durable
acceptance and dispatch, replays the original cancellation identity, and checks
terminal no-op `targetState` through the safe inspection projection. T054 still
has unbuilt partial-start/lost-native-reply reconciliation and automatic bounded
restart work; these checkpoints do not close it. That checkpoint left T056 open; the current checks above supersede it.


Final checks for these recovery/guard changes: host 40/40; Linux native and
ASan/UBSan each 44 passed plus the explicit non-root cross-UID skip; the separate
cross-UID normal/sanitized checks already passed in the disposable guest. The
saved Rust/C interoperability runner also passes. The added cancellation-result
projection assertion passes in its focused check. The real updated readiness
pause/resume scenario passes in `runtime-run-recovery-jsqm9aim.json` (62.387 s;
`gs-public-runtime-94jlcdjt`), preserving PID/generation/credentials/history.
Postman status still cannot initialize its default macOS keychain context
(`SecItemCopyMatching failed -50`); no new public HTTP collection pass is claimed.


### Persistent datastore groundwork (2026-11-04)

T066 contract tests and T068 pinned compatibility metadata are complete. This
closes their test/definition scope, not T067/T069/T070 persistent deployment
execution. `runtime/profiles/scylladb.json` and `migration-targets.json` remain
`executionEnabled: false`; neither `scylladb` nor public `migration.start` is
advertised. The checked-in native experiment and its limits are documented in
[Scylla verification](../../../tests/runtime/scylladb.md).

`durable_migration_control_contract` passes on macOS normally and with ASan/UBSan,
and Linux arm64 normally and with ASan/UBSan/leak detection. It covers atomic
registered-target reservation, missing backup/keyspace/compatibility, unsupported
memdb, per-deployment conflict, private datastore lock observations, failed writes
at dispatch, reopening/same-key replay, safe/unsafe cancellation policy, verified
success/cleanup, unchanged terminal history, uncertain cleanup blocking startup,
safe failed retry and corrupt target/ownership metadata. The generic START
admission path also rejects a prior incomplete migration before accepting a new
operation. Journal metadata alone never authorizes a native helper or filesystem
resource takeover.

The real `scylla-native` runner passes in
`runtime-run-scylla-native-0phzbvg9.json` (33.132 s), selecting dedicated UID 11000
and a snapshot of the exact native probe. It verifies pinned image acquisition
inputs, rootless datastore readiness, image-native keyspace preparation, two
successful `/app/gitctl migrate` invocations, all 12 actual ledger checksums and
statement counts/completion times, real datastore lock rejection with unchanged
ledger, observed process exit/empty scope/non-forced deletion, and retained schema
ledger after a new native identity starts the same data directory. Images and
datastore directories are retained. No engine, host cqlsh or downloaded helper
binary substitutes for the library/native image paths.

Host regression checks after the initial module build: 47 passed inside the
sandbox and the isolated `server_security` Unix-socket test passed when rerun with
local socket permissions. The separate slow connection-deadline test was excluded
from this repetition; its earlier native evidence remains recorded above.
Protected datastore authentication, actual API migration-check startup, daemon
persistent graph/volume ownership, backup/restore, persistent crash/disk cases and
amd64 native Scylla execution remain unverified. Postman still fails to initialize
its macOS keychain (`SecItemCopyMatching failed -50`); no HTTP collection pass is
claimed for these Unix IPC/CQL checks.


T074's real manager/workload crash and concurrent Git-writer verification is
recorded in [native recovery](../../../runtime/experiments/recovery.md). The new
three-request replacement case passes in `runtime-run-recovery-kjv0_xve.json`
(112.252 s), retaining native identities, the writer-lock inode and synthetic
content hashes through manager loss and later observed stop/restart. It builds
on the previously recorded full native recovery cases; it is scoped to the
implemented core memdb graph, not the still-disabled persistent Scylla graph.

The final migration preflight transaction and START admission changes pass all
48 non-slow C checks on the host with Unix-socket permissions. Focused Linux
migration/acceptance/control checks pass 3/3; the final migration contract also
passes host and Linux sanitizers (Linux leak detection enabled). The generic
stage-transition API cannot bypass the private preflight observations needed to
commit the prepared stage. Native execution remains separately gated.


The CLI migration dispatcher now validates explicit deployment/target/backup
selection before connecting, requires advertised migration support, persists the
same explicit/generated key through submission and reports the accepted operation
identity before observing terminal completion. Controlled authenticated-peer cases
in `crates/gs/tests/daemon_migrate.rs` pass 4/4: no connection on invalid inputs,
no mutation when capability is absent, lost-ACK replay of identical target/backup/key,
and terminal prerequisite/ownership/deadline failure without cancellation or retry.
This is CLI protocol evidence, not live persistent migration support; T072 remains
open for real daemon integration. The public daemon still rejects migration support.

Required Rust checks pass after this dispatcher change: formatting, all-target
Clippy with warnings denied, workspace tests and workspace build. The focused
migration cases were rerun after preserving named terminal prerequisite errors.
The saved interoperability runner now includes these cases.

The new `scripts/measure-runtime-size.sh` creates stripped disposable copies and
records source/stripped hashes, an unpublished package and the unique reused
shared-library closure. The measured checkpoint is recorded in
[binary sizes](../binary-sizes.md); later CLI changes are not part of that artifact
pair. Matched fresh runtime memory/cold-warm measurements remain open in T079.


Private persistent integration now has a distinct Scylla datastore-volume ledger
(schema version 3), typed filesystem compatibility markers and a bounded read-only
backup payload validator. Exact v1/v2 upgrades preserve terminal/dedupe/resource
rows and roll back on denied journal writes; damaged/unknown schemas fail closed.
Linux tests reject using a repository's actual allocated inode receipt for a
datastore. Host/Linux sanitizer checks cover these boundaries. The later Linux native ASan/UBSan regression selected 61 checks: 60 passed and
the non-root cross-UID case skipped explicitly (64.21 s). It includes actual
partial-frame deadlines and excludes the separately verified five-minute idle
deadline. The Rust/C interoperability runner also passed.

The actual independent backup/restore scenario passes in
`runtime-run-scylla-native-xdjbwt0i.json` (32.152 s), with payload hashing/compatibility
verified through the C module before native Scylla opens restored data. The later
`runtime-run-scylla-native-3eo7g4fa.json` (29.519 s) repeats the native migration,
real datastore lock conflict and restore checks using the registered production
OCI service plan builder. These remain developer-native checks, not public daemon
Scylla deployment or restore workflow support.

The private persistent manifest pins the implicit datastore image and helper/schema
identities separately from the unchanged core app profile; memdb descriptors remain
byte-identical. Fresh-only bootstrap ordering requires datastore readiness,
keyspace preparation and migration before app launch. Retained data skips those
mutations and uses private API configuration with `auto_migrate=false`. Controlled
normal/sanitized tests cover graph generation, failed/uncertain migration blocking,
immutable configuration files and policy-flag binding. Private production actors now verify full startup, retained restart and observer
restoration. Public persistent startup remains gated pending remaining migration
and crash-recovery integration. See the scenario matrix in
[Scylla verification](../../../tests/runtime/scylladb.md#persistent-actor-recovery-checks).

The bounded collector now supports Git/API/[controller]/Scylla streams, with
redaction before persistence and Scylla filtering in the existing replay path.
Normal/sanitized Linux checks cover three- and four-node pipe/label layouts.
`scylladb` is a log selector for an implicit datastore dependency; caller-selected
app `components` still rejects it. No public Scylla startup capability is advertised.

The private startup prefix commits canonical datastore reservation and graph
freshness before its allocation worker, then records actual inode receipts before
exposing the writer descriptor. Duplicate starts fail before another worker.
Six focused Linux ASan/UBSan/leak checks pass for this pipeline, retained writer
rules, registered plans and per-image cache authorization. A private 1 MiB tmpfs
ENOSPC run (`runtime-run-exp-004b-disk-whlsgghr.json`) preserves existing Scylla
bytes/inodes while new allocation fails; it never fills the host disk.

`runtime-run-scylla-native-rov4kysp.json` (55.400 s) verifies actual API readiness
with `auto_migrate=false` and unchanged migration ledger before/after independent
Scylla restore, while the original Git repository hash survives restart.
`runtime-run-scylla-native-kswdgcq4.json` (57.862 s) repeats the scenario through the
production native workers, including the Scylla service's inherited writer rule.
Git must run before API bootstrap, and health polling uses the actor's 250 ms
cadence; an earlier 1 ms experiment hit HTTP 429 and failed. Neither result enables
public Scylla deployment, standalone migration or product restore support.

### Standalone migration and cgroup identity increment

The private standalone schema scenario passes with production schema/stop actors
in `runtime-run-scylla-native-o2jjyeup.json` (42.019 s), and with the nonblocking
runtime coordinator in `runtime-run-scylla-native-tmq55jin.json` (54.930 s).
The coordinator with pre-START cgroup journals passes again in
`runtime-run-scylla-native-_pd11g5f.json` (37.479 s). All runs use the selected
Linux arm64 disposable fixture and ASan/UBSan, dispatch the registered image-native
schema helper once, verify all 12 records afterward, start no API workload, and
observe source/native deletion, writer reacquisition and network death before
successful finalization. Original datastore producer/data/writer identities stay
unchanged. These close T069, not public manager/CLI wiring or all failure cases.

The full persistent manager-loss/copy scenario with population receipts passes in
`runtime-run-scylla-native-57zenocb.json` (111.663 s). It additionally substitutes
one stored cgroup inode and observes rejection without signalling the original
workloads. After restoring the original receipt, fresh observer adoption and owned
stop succeed. Four init identities and both kernel writers survive manager loss;
physical payload copying does not transfer runtime authority.

Metadata schema v5 upgrades exact versions 1–4 atomically and preserves rows on
unknown/damaged schema or denied writes. The migration attachment gate uses bounded
innocuous parsing and separate guards to preserve the existing SQL expression-depth
limit. Helper stdout uses a pre-sized 64 KiB growth/shrink-sealed anonymous file,
with actual length from its shared offset and write sealing after observed helper
deletion. A helper-wide file-size limit was rejected by actual cqlsh package
extraction; its failure diagnostics remain saved. Five focused host and the relevant
Linux sanitizer checks pass. Interrupted migration helper reconciliation, public
dispatch and the wider T073/T082 matrix remain open; no release support is inferred.

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

### Persistent acceptance and requirement mapping

The current native image/schema/unsafe-cancel/independent-backup-restore case
passes through the `persistent-scylla` runner in
`runtime-run-persistent-scylla-zh3v4dcx.json` (62.860 s test, 62.911 s runner).
It verifies the registered worker policy, exclusive writer, 12 migration ledger
rows, unchanged ledger under unsafe helper cancellation, API check mode, actual
lock conflict, retained repository restart and an independently restored datastore.
This is a private backend restore proof, not a user-facing restore command. The
runner's original generic adapter label is broader than this selected case; the
case name/assertions and this scope govern. Future records use an explicit private
restore label. Exact selected probe SHA-256 values are:

- Scylla native: `d05e13a808af5b9a05d7a2187b80f5eb676b6a3e2fd4f5d9d1666aa8d75457c6`.
- Backup probe: `683bab0ac38ed245ec60ef1a01ec65648d9d83cdfa9a9bb06f49da478af650fb`.

Image/helper/schema identities remain pinned to GitStore
`06152d193e854bc643fa884daa8b9f30b0d75d0f`, Scylla 2026.1 (observed 2026.1.15),
and target `gitstore-06152d-schema-012` with the 12 registered ledger checksums.
The profile JSON and runner executable snapshots preserve the full immutable
platform/image/helper inputs; the selected host remains Linux arm64 UID 11000.

| Requirement | Actual evidence and limit |
| --- | --- |
| FR-009 | Migration contract serialization/target conflicts and the real concurrent Git-writer replacement scenario in [recovery evidence](../../../runtime/experiments/recovery.md); new keys never transfer a live writer. |
| FR-010 | Persistent manager loss/copy/cgroup substitution (`57zenocb`) and migration manager-dispatch reconciliation (`rx1sztj2`); inaccessible identities retain uncertainty. |
| FR-011 | Rust lifecycle/migration socket checks, queued-only cancellation, native surviving-helper and unsafe-cancel cases; observation loss never retries a mutator. |
| FR-012 | Public paused Scylla stop/retry (`u2bm8u0n`) and image-rejection cleanup (`yqmqo5y0`); failed history and held writers remain truthful. |
| FR-013 | Public start/stop/migrate/replay (`d72b4527`, `u2bm8u0n`), exact ledger/schema and unsafe-cancel (`zh3v4dcx`); only the registered target executes and backup acknowledgment creates no backup. |
| FR-028 | Canonical inode/writer retention, copied-state rejection, physical payload hashes (`gsxwuq5i`, `0ipb9o7u`), bounded role-specific ENOSPC and independent restore (`zh3v4dcx`); no public restore command is claimed. |
| SC-011 | The preceding native stop/restart/migration/manager-loss and restored-copy checks pass on the selected arm64 fixture; amd64/provider publication evidence remains separate. |

`docs/local-runtime.md` now explains memdb resets, explicit retained Scylla
selection, migration backup/cache prerequisites, timeout/replay/cancel behavior,
owned recovery and execution-copy disk costs as user-facing instructions.

The current 30-second interrupted-migration stop bound passes with an actual
surviving mutator in `runtime-run-scylla-native-gz0xqe0j.json` (29.129 s overall
setup/recovery). It reuses the stopped public-profile data/cache, waits for the
new manager observer before resuming the exact helper, and preserves the original
failed operation while owned stop succeeds. The preceding `xkmyiwaq` attempt
failed in test setup before acceptance because the older probe assumed deployment
`one`; its failure/early-return test allocation log is retained. The test-only
probe now takes a validated explicit deployment selector and filters recovery
lookup by that identity; it does not rename data or rewrite ownership to fit.

## Final source checks and Mac feasibility (2026-11-05)

The final full host daemon suite passed all 52 cases in 342.72 seconds, including
its real idle timer (`build/final-host-daemon-tests.log`). The full applicable
Linux native ASan/UBSan suite passed 61 of 62 cases in 376.81 seconds; the explicit
cross-UID case skipped under fixture UID 11000. Its log remains in
`build/daemon-native-sanitized/final-native-tests.log` in the owned guest. The
Rust image library is not made sanitizer-instrumented by the C host build.
Pinned workspace fmt, clippy with warnings denied, tests and build passed;
`scripts/test-daemon.sh` and `scripts/test-remote.sh` passed separately with
isolated synthetic fixture configuration. Ignored live/native-store cases are
not passes. Production MinSizeRel builds with `GITSTORED_TESTING=OFF` and no test
entry points; measurement identity is in the packaging receipt.

The [Apple evidence receipt](../../../runtime/experiments/apple/evidence.json)
pins the final actual host-library binary and the three successful guest scenarios.
The direct Containerization guard rejects unenforceable seccomp before boot.
The [managed-guest report](../../../runtime/experiments/macos-vm.md) explicitly
separates guest daemon recovery from untested host VM lifecycle. No public Mac
backend or clean installation is implied. The current Postman status attempt
still exits 139; `build/m2-postman-final-status.log` preserves the environment
failure, not a collection result.

Safe fixture cleanup retained volumes, cache inputs, credentials and evidence.
Receipts `execution-cleanup-1793880513.json`, `1793880515` and `1793880516` cover
retired execution copies under the stopped public Scylla and first Mac guest
runs. Guest filesystem trimming returned backing blocks; host free space was
about 17 GiB afterward. This authorized development cleanup is not product GC.

## Requirement audit: evidence boundaries

The following is a traceability index for the source preview, not a blanket release
qualification. C tests include actual SQLite and framed sockets plus explicitly
controlled workloads; native scenario receipts identify real Linux services.

| Requirement | Implementation / checks | Remaining boundary |
| --- | --- | --- |
| FR-001 | security/state/server and peer-UID tests; installed public fixture | Current full suite skips cross-UID under UID 11000; separate earlier kernel evidence is scoped. |
| FR-002 | protocol/capability tests and Rust/C process runner | Optional component support is not advertised. |
| FR-003 | validate tests and Rust daemon_validate | Runtime/image/helper checks marked unperformed during preflight. |
| FR-004 | Rust lifecycle selection/usage tests; component-specific native optional scenario | Unknown/unsupported input rejects before provisioning. |
| FR-005 | inspect/pagination/expiry tests and Rust socket inspection | Retention and cursor expiry remain explicit. |
| FR-006 | terminal immutable triggers, finalization and real manager-loss recovery | Inaccessible old owners remain uncertain. |
| FR-007 | acceptance/crash replay tests; dropped public start acknowledgment | No retry of unobserved mutators. |
| FR-008 | retention/ownership/control-capacity tests | Metadata collection does not reclaim retained execution trees or volumes. |
| FR-009 | ownership tests; actual concurrent replacement rejection | Single named local writer only; no distributed storage claim. |
| FR-010 | native population/restoration and migration recovery | No adoption of copied live state or missing kernel proof. |
| FR-011 | explicit cancellation/control saturation and native paused workers | Running migration cancellation remains unsafe and refused. |
| FR-012 | termination/finalization tests and real stop-timeout/new-stop case | Deadline retains writers until later observed cleanup. |
| FR-013 | migration tests, public persistent migration/replay and native schema/lock checks | Registered target only; acknowledgment is not a backup command. |
| FR-014 | stream queue/socket/replay tests and real log capture | Bounded logs can expire; no unlimited replay promise. |
| FR-015 | parser/framer/resource/connection tests, real timers and bounded fuzz smoke | Fuzz smoke is not exhaustive security verification. |
| FR-016 | request/execution-plan policy tests and source-only production capabilities | No arbitrary executable/mount/runtime hooks or privilege escalation. |
| FR-017 | Rust output/error/no-input tests and public CLI scenarios | Local Markdown output unsupported. |
| FR-018 | redaction/credential worker/registry controls with synthetic secrets | Host personal credential stores are not test inputs. |
| FR-019 | separate testing build and production symbol/capability checks | Controlled actors establish no native compatibility. |
| FR-020 | public core/persistent dispatcher and provisioning scenarios | OIDC graph is incomplete; Admin/Storefront mappings unavailable (T050). |
| FR-021 | Rust/C interoperability, C sanitizers, deterministic bounds and fuzz evidence | Hosted lanes authored but not executed; Rust SDK itself not C-sanitized. |
| FR-022 | full remote runner and workspace regression checks without runtime assets | Remote command's existing M1 gates remain. |
| FR-023 | pinned alpha.8 profile, bootstrap material/enrollment/check-mode tests | Optional provider configuration remains gated independently. |
| FR-024 | actual libcrun lifecycle, pidfds/cgroups, signals, manager recovery and bounded whole-graph replacement | Four-attempt chain ends on uncertainty/operator intervention/manager loss; only the named arm64 fixture has native proof. |
| FR-025 | SDK/provision/cache/consent/offline scenarios | Cache budget excludes owned execution copies and retained data. |
| FR-026 | synthetic private registry, token-realm/redirect/expiry isolation | Only registered trusted origin policies; no arbitrary registry policy claim. |
| FR-027 | actual libslirp DNS/loopback/private ports and TUN-denied scenario | No CNI adapter or other host qualification. |
| FR-028 | canonical repository/Scylla leases, disk failure and private independent restore | No public backup/restore workflow or CSI. |
| FR-029 | full core readiness and protected controller enrollment; API check mode | Optional OIDC/Admin/Storefront are not substitutes for verified readiness. |
| FR-030 | pinned Linux experiments, bounded Apple/managed-guest reports, offline provenance controls | Clean Mac delivery, amd64 native fixture and own signed CLI positive unavailable. |
| FR-031 | ordinary library dependencies and separate measured core/package/system-library inventory | Published binary optimization deferred; no default executable bundle. |

| Outcome | Evidence and qualification |
| --- | --- |
| SC-001 | Preflight tests report diagnostics without downloads or generated credentials. |
| SC-002 | Generation-aware inspect/wait and manager-loss invalidation distinguish historical success from current readiness. |
| SC-003 | Crash/dropped-ack/same-key/native migration recovery preserve original identities and terminal results. |
| SC-004 | Actual concurrent Git writers, copied live roots, held leases and stop/restart retained-volume checks. |
| SC-005 | Parser/peer/unsafe-cancel/stream bounds and synthetic credential redaction checks; not exhaustive security proof. |
| SC-006 | Rust process runner and controlled public remote runner complete without prompts or runtime downloads. |
| SC-007 | Command matrix EVD-024 plus exact scenario receipts; no controlled result is a release qualification. |
| SC-008 | Versioned validation and bootstrap evidence; unavailable optional checks do not report runnable success. |
| SC-009 | Installed isolated source package runs as UID 11000 with PATH unavailable on named arm64 guest; not a fresh OS or other host claim. |
| SC-010 | Image SDK/public provisioning controls cover private auth, trust, corruption, interruption, consent and offline verification. |
| SC-011 | Persistent acceptance table above maps actual migration, retained restart, manager loss and private restored-copy checks. |
| SC-012 | Each bounded experiment records actual pass/fail/unavailable; Mac product paths remain unselected. |
| SC-013 | Size receipts preserve exact baseline/checkpoint bytes and package/library costs; host denial/offline and remote-only checks remain separate. |

Stories 1–4 have scoped protocol/control/observation evidence; story 5 has actual
core execution with optional provider work still open; story 6 has public Scylla
migration and owned recovery evidence with private restore limits. Requirement
quality checklist marks remain read-only and do not represent implementation
completion. T085 closes the discovered production restart gap only after its
separate actual workload-exit and non-replacement health cases pass.

### Bounded restart supervision acceptance

T085 is implemented in the production manager. The explicit evidence receipt is
[restart-evidence.json](../../../runtime/experiments/restart-evidence.json).
Current actual source-preview cases pass:

- Core API exit/replacement: `runtime-run-recovery-sd29vd0y.json`, 80.382 seconds.
- Retained Scylla API exit/replacement: `runtime-run-persistent-scylla-ugvs9kdp.json`,
  171.130 seconds. The same repository producer/device/inode/writer identity remains.
- Paused live API loses/regains readiness without replacement:
  `runtime-run-recovery-amt36z2_.json`, 60.078 seconds, same generation and identity.
- Killed manager reopens original owners for explicit stop/restart:
  `runtime-run-recovery-wnt9jx18.json`, 82.231 seconds; no unobserved automatic takeover.

The two exit tests hold every original service pidfd and verify original death
immediately after replacement acceptance, before waiting for new readiness.
They also require a successful separate stop and released old workload receipts,
verify downloads are disabled for replacement, and preserve the original terminal
start result. This is actual libcrun/slirp public dispatcher behavior, not a
controlled backend pass. The earlier persistent attempt `kw1sya61` failed in test
setup by selecting a completed preparation helper; its failure remains recorded.
Service selection now excludes helper-control receipts and selects the registered
API resource explicitly.

The restart controller uses the existing 250/500/1000/2000 ms backoff policy with
four replacement attempts per manager-owned chain, independent normal operation
deadlines and no control-loop sleep. It never treats failed HTTP health as native
exit. Stop/replacement failure, uncertainty, superseding operator intent and
manager loss end retries. Normal operations/dedupe persist all accepted automatic
stop/start identities; automatic supervision is deliberately not resumed from
metadata alone after manager loss. Explicit operator start establishes a new chain.

After integration, all 52 applicable non-slow host C tests passed (37.90 seconds),
and 61 applicable non-slow Linux sanitized C tests passed with one explicit
cross-UID skip (86.31 seconds). The previously recorded slow idle test is separate
pre-supervision evidence, not a rerun. The Rust/C daemon contract runner passed
again. New controlled supervision tests exercise the attempt budget, delay,
held-owner refusal, failed stop/replacement, superseding intent, uncertain source
and immutable prior success.

The final source audit resolves 256 changed/new local Markdown file targets with
zero missing files; all changed JSON parses and the three-job runtime workflow
parses with read-only default permissions and full action pins. Whitespace passes.
The reviewer-owned specification-quality checklist is unchanged at 16/16. The
untracked source/evidence set is about 1 MiB total with no file over 1 MiB; generated
binaries, image data and logs stay ignored. The production symbol inventory contains
`gstd_restart_*` and no `gstd_test_` entry points. This is a source audit, not a
hosted CI run or security certification.

Final restart test cleanup receipts `execution-cleanup-1793882908.json`,
`1793882916` and `1793882917` retire 22, 12 and 12 generated trees under the
stopped final Scylla/core/manager-loss fixtures. The earlier schema preparation
copy without a native receipt was explicitly retained by the cleanup guard; no
unproven absence was treated as termination. Volumes, credentials, cache inputs,
metadata and evidence remain. Trimming restored host free space to about 12 GiB.

## Optional OIDC enrollment implementation increment

At this earlier increment, T050 remained open. Its fixed-reference private request policy and HTTP worker
are now implemented and tested without enabling any public OIDC capability.
[The controlled receipt](../../../runtime/experiments/oidc-client/evidence.json)
records 19 real C-worker HTTP cases against a contract-derived Postman mock,
11 single-attempt mutations, zero redirect/proxy trap requests and a saved
three-request/three-assertion-block collection pass (4.734 seconds). Only typed
provider-ready, client-created and public-match statuses cross the private probe
boundary. Matching public fields after a lost POST response has a distinct result
and never retries or acknowledges creation. Request/response credentials remain
private; no CLI arguments or operation result contains them.

The source contract is Hydra 2.3.0 revision
`ee8c339ddada3a42529c0416897abc32bad03bbb`, whose actual creation handler echoes the
client secret and registration token. The existing libcurl transport enforces
fixed owned-namespace endpoints, no proxy/redirect, bounded bodies and deadlines.
Controlled timeout/disconnect-after-create cases remain uncertain; mismatched,
malformed, oversized and wrong-content-type acknowledgments never become success.

The Mac OpenAPI commands still fail native credential initialization. Linux
Postman 1.86.0 was installed only as a maintainer tool in the disposable account,
using the inspected official archive (106,533,450 bytes; SHA-256
`ea082afd6604e4d69b1fdd7d6d07344c66628996078ae679b92e2e638107cddf`).
It is not bundled in either core executable. Linux syntax-only OpenAPI lint and
local collection lint/run pass; cloud governance requires sign-in and was not run.
Official OpenAPI structural/component-schema validation also passes. Earlier
stale-probe and mock asynchronous-state failures are retained as failed fixtures.
The policy and existing private bootstrap worker/material checks pass under C
ASan/UBSan. Separate native provider checks are recorded below; the controlled
mock run alone is not provider-lifecycle evidence.

Pinned Hydra/Kratos images passed fresh SQLite migrations and readiness through
registered OCI adapters with live writer FD 3 inheritance and observed helper/
service exit, empty cgroups and deletion. See the
[registered adapter receipt](../../../runtime/experiments/oidc-client/registered-adapter-evidence.json).
Separate real Hydra creation tests retain `created` for an acknowledged write and
`uncertain` for a deliberately lost private reply, with exactly one client stored
and no second POST. The [journal receipt](../../../runtime/experiments/oidc-client/enrollment-journal-evidence.json)
and pure policy checks do not substitute for production parent-ledger integration.
The [normal/sanitizer network receipt](../../../runtime/experiments/oidc-client/network-evidence.json)
verifies seven role-specific forwards, five equal host/guest OIDC ports, loopback
binding, private-port refusal, stable-port rebinding and cleanup.

The [combined graph receipt](../../../runtime/experiments/oidc-client/combined-graph-evidence.json)
verifies five ready services, fresh migrations, one client registration and reverse
observed deletion. The [courier receipt](../../../runtime/experiments/oidc-client/courier-evidence.json)
adds real Kratos verification delivery to a synthetic Mailpit recipient: eight
requests, five scripted assertions and runner exit 0 inside the owned namespace.
Mutations are single-attempt, admin ports are not forwarded to the host, cloud
reporting is disabled and response/mail tokens are not exported. All five services
were deleted afterward and both retained databases passed integrity checks.
Audited execution rootfs/blob copies were removed after these runs; protected
material, databases, cached inputs and evidence were retained.

Distinct Hydra/Kratos filesystem volume markers now reject cross-provider,
repository and Scylla adoption, preserve immutable producers and retain the same
writer/observation boundaries. Focused native volume/policy tests pass. The
[provider volume evidence](../../../runtime/experiments/oidc-client/provider-volume-evidence.json)
also verifies private parent-transaction reservation/readiness/recovery primitives:
canonical distinct provider IDs, retained producers/inodes, immutable receipts and
refusal of a new writer while an inherited lease remains live. Schema v5 upgrades
exact v1–v4 stores atomically without raising the SQL expression-depth limit.
The production optional-profile actor is not yet connected to these primitives.

[Protected preparation evidence](../../../runtime/experiments/oidc-client/preparation-evidence.json)
also covers the separate OIDC material purpose, one-shot creation reservation,
retained source selection and public file/port receipts without reading secret
bytes in the control owner. Changing only the unmounted client secret is rejected.
The asynchronous worker passes cancellation/crash/deadline checks and actual
controlled Postman mock requests under namespace UID 10000, normally and with
ASan/UBSan. A matching public GET after a deliberately lost CREATE reply remains
distinct from acknowledged creation, with no mutation retry. Raw mock console
output is discarded. Actual provider execution through this new asynchronous
wrapper and production parent enrollment integration remain unverified.

[Alpha.9 source evidence](../../../runtime/profiles/alpha9-evidence.json) supplements
the alpha.8 execution baseline. ADR 0012 is now Accepted and the API endpoint split
is implemented in one `gitstore-api` binary. This does not supply the optional
Admin UI runtime. The Storefront handler deliberately returns HTTP 503
`STOREFRONT_UNAVAILABLE` until publication reads and trusted target binding exist.
File's normalized selector is now shipped, while high-level client compatibility
still needs verification. Changed Scylla migration checksums require an explicit
data compatibility decision; the existing images, volume markers and migration
target remain pinned. The OIDC compose source is byte-identical across the two
releases, so its existing mappings remain relevant without claiming alpha.9
image execution or silently changing an existing deployment.

The remaining work is durable parent enrollment/volume/network ownership,
current issuer integration and full lifecycle/recovery. Source mapping, native
serving checks and building blocks do not enable the public OIDC profile. The SQLite alternative has actual published-
image execution evidence; it is no longer an unverified build-tag assumption.

An automatic approval review rejected sending a project-specific OIDC discovery
query to the third-party Orbit service. The request was not retried or routed
indirectly; immutable official upstream sources were used instead. This does not
block local implementation or require cloud sign-in.


## Issue-9 follow-up: fixed health and migration journals

At this earlier checkpoint, spec-003 T050 remained open. The private asynchronous health worker and
provider-specific journal path in the production helper owner are implemented.
See the [coordination evidence](../../../runtime/experiments/oidc-client/coordination-evidence.json)
and [reproduction instructions](../../../runtime/experiments/oidc-client/README.md#follow-up-health-and-migration-coordination).
Normal GCC Release and Clang ASan/UBSan each exercise the focused daemon checks;
native migrations are separate from controlled HTTP and metadata-only policy.
The normal Rust checks pass (85 tests, 13 fixture-dependent ignored).

A native sanitizer run exposed the OIDC plan's finite address limit preventing
ASan's pre-exec shadow mappings. The diagnostic plan now uses the existing core
handling; ordinary finite address limits and physical cgroup limits remain.
Earlier failed fixtures retain their journals/data/ownership and are not treated
as successful migration or cleanup evidence. No sanitizer warning was suppressed.

These checks do not enable public OIDC serving. Deployment asset/preparation
orchestration, API issuer/discovery/JWKS readiness, secret-aware service logs,
all five services' inspection/stop/restoration, helper ACK racing STOP and public
CLI acceptance remain required. Admin UI and Storefront still fail with their
component-specific external prerequisites. No release/platform gate is closed.

The fixed-role layout extension passes 15 focused normal and 15 sanitizer checks
(including restart/recovery policy). The actual public core GraphQL/Git,
owned-stop and retained-restart regression passes in 72.813 s with verified
offline inputs, recorded as `runtime-run-clean-install-tp2brb2w.json`. This is
existing core behavior; it does not substitute for OIDC lifecycle acceptance.


## 2026-11-06: optional OIDC lifecycle follow-up

The production actor now integrates both provider-volume migrations, five native
services, one-shot enrollment, API issuer/client/audience configuration, bounded
role health/discovery/JWKS, V6 component metadata, endpoint/log projection, reverse
stop and read-only recovery. Standalone admission checks all unresolved ownership
and recovery state independently of repository-volume selection.

Public standalone ASan/UBSan acceptance passed fresh and retained readiness,
lost-ACK replay, redacted aggregate/bridge logs, retained ports/material/volumes,
manager loss without adopted readiness, rejected replacement and owned stop.
Combined API/Git/Controller/OIDC memdb acceptance also passed public readiness and
foreground interruption cleanup. The exact input hashes and receipts are in
[lifecycle evidence](../../../runtime/experiments/oidc-client/lifecycle-evidence.json).

The ordinary production build with test facilities disabled passed the full
nine-service OIDC/Scylla/Controller graph, including paused-bridge readiness,
foreground interruption and owned stop. A subsequent retained generation passed
paused-provider readiness, actual manager loss, rejected concurrent replacement,
read-only reconciliation and owned stop while preserving its original successful
operation. The final provider sweep and readiness commit share a 30-second window.
A sanitizer standalone fresh generation and a separately accepted retained
generation passed the same current actor, including manager loss and owned stop;
all five original cache producer identities were preserved.

Host ENOSPC caused earlier preparation/build failures and an aborted guest journal.
Those failed attempts remain failures in the evidence. The user explicitly
approved cleanup of seven stopped failed fixtures: 81 exact-inode generated
execution/cache copies were removed only after release/reference/stop checks and
verified original-input checks. Canonical data, credentials, logs, journals and
inputs remain. A later sanitizer retained preparation hit confirmed block I/O
errors before native dispatch; a new accepted generation passed after reclaiming
additional unreferenced successful-run copies. No failed operation was rewritten.

The public OIDC capability is now enabled only when the installation's native
runtime is capable. Admin/Storefront, native amd64, clean-host, macOS and binary
publication remain separate gates. The native source-preview acceptance does not
provide CLI login/browser/device flows or qualify a local alpha.9 data upgrade.


The final portable metadata/control build also passes on macOS: 58 tests pass,
zero fail and two Linux-only tests skip (42.07 s). Native runtime libraries are
disabled in that build; this protects portable control behavior and does not
qualify macOS native execution.
