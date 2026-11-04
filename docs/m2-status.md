# M2 Implementation and Evidence

**Recorded**: 2026-11-04. **State**: In progress. T001–T049 have development
implementations and scoped checks. The real `gs`/`gitstored` core memdb path now
passes all eight public CLI lifecycle scenarios on the selected Linux arm64
fixture, plus installed startup, exact host denial and offline scenario dispatch. All six
offline asset/cache scenarios also pass; registry timing failures remain open
in the [image evidence](../runtime/experiments/images.md).
T051–T053 controlled-test facilities also pass. T050 and the remaining recovery,
streaming, persistent-profile and delivery work remain in progress in the
[task record](../specs/003-m2-daemon-foundation/tasks.md).

No third-party executable bundle is used: Git, image acquisition/unpack, libcrun
and libslirp use code libraries. The current stripped Linux arm64 candidates are 5,525,032 bytes (`gs`) and
5,658,000 bytes (`gitstored`); both exceed the 5 MiB target. The unpublished
[package inventory](../runtime/packaging/linux-arm64-candidate.json) separates
core, compressed-package and reused host-library costs. Broader live image/volume recovery, logs,
migrations, clean-install costs, cross-host support and publication gates are open.
This is neither complete M2 nor release support. Older checkpoints below describe
what was verified at that time; the current status takes precedence.

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

The internal [image code library](../daemon/image-backend/README.md) now performs
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

The internal [credential broker](../runtime/experiments/credential-broker/README.md)
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

See [cache pipeline evidence](../runtime/experiments/provisioning/evidence.json)
for actual image bytes and the cache-only accepted/pending laboratory intent.
Logical tree bytes count hard-linked names separately and are not a physical
installed-size result. Owner settings/cache binding, offline import/resume/eviction
and public service dispatch are subsequent tasks.

## Dependency/source details

[Dependency lock](../runtime/dependencies.lock.json) records exact sources/build
choices. The private Jansson patch allows NUL-containing JSON keys using its
existing length-aware APIs/duplicate checks, preserving M0's UTF-8 string-key
contract. A preparse structural bound counts at most 32 containers; the private
parser cap is 33 because it also counts the scalar value at the leaf. JSON
allocations are bounded at 16 MiB cumulative per parse and 64 MiB live globally
on the single control owner. None of this is a general JSON parser rewrite.

[Upstream evidence](../runtime/profiles/upstream-evidence.json) records pinned
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
[provisioning record](../runtime/experiments/provisioning/evidence.json).
No service is started by these checks; abandoned-job compensation, public owner
settings/consent UX and runtime dispatch remain later implementation tasks.


The T040 preparation probe additionally passes real delegated systemd cgroups
(cpu/memory/pids), a small explicit seccomp filter, no-new-privileges and zero
capabilities for the tiny static payload. Normal/static and direct-ELF Clang 18
ASan/UBSan runs pass both normal and lost-start-ack scenarios. Guest-only fixture
session preparation is recorded in the
[delegated libcrun evidence](../runtime/experiments/libcrun/delegated-evidence.json).
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
control suite each pass 26 CTests. See the [adapter evidence](../runtime/experiments/libcrun/adapter.md).
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
[network adapter evidence](../runtime/experiments/network-library/adapter-evidence.json).
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
[bootstrap.c](../daemon/src/bootstrap.c), with
[recorded real-image evidence](../runtime/experiments/provisioning/bootstrap-evidence.json)
and a [reproducible runner](../runtime/experiments/provisioning/README.md).
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

Public CLI acceptance scaffolding is in [tests/runtime](../tests/runtime/README.md).
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

[The optional evidence record](../runtime/profiles/optional-evidence.json) pins the
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

[Recovery prerequisite evidence](../runtime/experiments/recovery-evidence.json)
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
