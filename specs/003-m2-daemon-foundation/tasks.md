# Tasks: M2 Runtime — Unblocked Implementation

**Input**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md),
[data-model.md](data-model.md), [contracts](contracts/cli.md),
[release evidence](release-evidence.md), [validation guide](quickstart.md).
**Status**: Closed — all 85 implementation tasks are complete; the requirements
checklist is 16/16. Acceptance evidence is recorded in
[local runtime validation](../../docs/contributing/validation/local-runtime.md).
See [current release qualification](../../docs/contributing/validation/release-qualification.md)
for published support; closure does not expand the named dependency/platform gates.
**Tests**: Explicitly required by FR-021, SC-001–013 and the constitution. For each
story, write meaningful behavior tests before its implementation and observe the
expected failure; complete the listed checks before marking work done.

## Format: `[ID] [P?] [Story] Description`

Every task uses an unchecked box, sequential ID, optional `[P]`, story label only
in story phases, and exact repository-relative output paths. Paths may designate
planned files. `[P]` means file-separated work can overlap after its stated common
prerequisites; it never overrides a dependency or authorizes agent delegation.

## Path Conventions and Delivery Boundaries

- Rust: existing `crates/gs/`, pinned Rust 1.96.0. C17 daemon: planned `daemon/`.
  Runtime metadata/experiments: `runtime/`; real fixtures: `tests/runtime/`.
- Libraries mean code dependencies declared in Cargo/native build manifests.
  Static/dynamic linking is measured, not prescribed. Do not introduce a plugin
  loader or host-library dependency solely because a library is preferred.
- No default bundle of Skopeo, umoci, crane, pasta, slirp4netns, CNI or VM tools.
  Evaluate maintained libraries first; justify any executable fallback, reuse a
  compatible host component or provision a missing external dependency with consent.
- Attestations gate artifacts published by `gitstore-dev/cli`. Upstream GitStore
  images require trusted-source/platform/digest checks, not attestations. No gh bundle.
- The original <=5 MiB stripped-core target is a reporting reference. The user
  accepted 5,590,568-byte gs and 5,723,600-byte gitstored for this increment;
  published-binary optimization is deferred to their follow-up. Continue reporting
  library/package/download/cache/memory costs separately.
- Controlled tests do not establish real backend support. No publication or cloud
  workspace push is authorized by this checklist. No upstream checkout is modified.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Reproducible builds, isolated fixtures and source identity before behavior.

- [X] T001 Create C17 core library/executable and CTest targets in `daemon/CMakeLists.txt`, `daemon/src/main.c` and `daemon/include/gitstored/`; require CMake >=3.28, standard C without extensions, project warnings as errors, explicit testing/sanitizer/fuzz options and a production build excluding test backends; preserve the Cargo workspace and add only relevant build output to `.gitignore`.
- [X] T002 Lock Jansson 2.15.1 and SQLite >=3.51.3 candidate source revisions/archive digests, build flags, platform/toolchain, licenses and transitive inventory in `runtime/dependencies.lock.json` and wire reproducible native dependency builds in `daemon/cmake/Dependencies.cmake`; ordinary code dependencies require no host discovery or runtime download by default, and candidate updates require recorded rationale.
- [X] T003 [P] Create fixture isolation/cleanup support in `daemon/tests/support.c`, `crates/gs/tests/support/daemon.rs` and `scripts/test-daemon.sh`; use mode-0700 temporary config/state/runtime/cache roots, synthetic identities, owned volumes, explicit test binary paths and no real user config, credential store or existing deployment access.
- [X] T004 [P] Add C build/CTest and Clang 18 ASan/UBSan lanes in `.github/workflows/daemon.yml` alongside the executable targets; retain existing Rust 1.96.0 locked checks, avoid document-only jobs and leave `.github/workflows/pr-title-lint.yml` unchanged.
- [X] T005 [P] Record pinned upstream handler/config/entrypoint/readiness/image evidence for revision `06152d193e854bc643fa884daa8b9f30b0d75d0f` in `runtime/profiles/upstream-evidence.json` and `specs/003-m2-daemon-foundation/release-evidence.md`; inspect actual gitctl handlers rather than help text, resolve platform image digests, separate available mappings from missing optional mappings, and preserve historical M0/M1 evidence.

**Checkpoint**: T001–T002 precede T003–T005. Native builds/checks exist, fixtures
are isolated and upstream evidence is committed-source based. No runtime selected yet.

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Shared bounded protocol, storage and lifecycle abstractions. All stories
consume these interfaces; no image/network/container operation is enabled here.

- [X] T006 Create canonical boundary/example vectors in `tests/fixtures/daemon/` and typed C/Rust definitions in `daemon/include/gitstored/protocol.h` and `crates/gs/src/daemon_protocol.rs`; preserve identifiers “1–64 ASCII characters”, “Optional fields are omitted, not null”, UTC RFC3339 timestamps, exact enum spellings, per-method required/unknown-field rules and the accepted protocol 1.0 error/limit mappings.
- [X] T007 Implement strict bounded framing/JSON in `daemon/src/protocol.c` and `daemon/tests/test_protocol.c`; use duplicate rejection, full-input UTF-8 decoding, exact integers in ±9,007,199,254,740,991, no real-number tokens, 1–1,048,576-byte payloads, 32-level depth before recursive allocation, 262,144-byte strings and 4096 collection items; audit parser depth accounting and enforce allocator budgets before materialization.
- [X] T008 Implement the Rust duplicate-rejecting decoder/framer in `crates/gs/src/daemon_protocol.rs` and boundary tests in `crates/gs/tests/daemon_wire.rs`; prove C/Rust agreement on T006 vectors, fragmented/coalesced frames, malformed and trailing data, bounded allocations, exact integers, unknown optional envelope fields versus unknown params and separate response/event correlation.
- [X] T009 Implement owner-only Unix socket/state locations, safe stale-path handling, peer authorization before parsing, lifetime lock and version/capability negotiation in `daemon/src/server.c`, `daemon/src/paths.c` and `daemon/tests/test_server.c`; reject symlink/foreign-owner/second-manager cases, require 0700 directory and 0600 socket, and use supported OS peer-credential mechanisms without a TCP listener.
- [X] T010 Implement initial SQLite schema, transactional schema-version checks and owned storage initialization in `daemon/src/state.c` and `daemon/tests/test_state.c`; enforce “One lifetime lock/control owner; version mismatch/corruption blocks mutations; no unsafe automatic schema downgrade”, foreign keys, DELETE journal/EXTRA synchronization and bounded DB/journal disk accounting without transactions across runtime I/O.
- [X] T011 Implement shared Deployment/Operation/DedupeRecord/Component/ResourceOwnership records in `daemon/src/records.c` and schema definitions in `daemon/src/schema.sql`; preserve “readiness applies only to current generation”, “terminal result/error only; no raw credentials”, “runtime identity is not just PID” and “Reserved before side effects; recorded owner required for cleanup; uncertainty cannot be released from PID absence”; validate all fields against T006 typed rules.
- [X] T012 Implement atomic accepted-intent/dedupe/generation/resource-reservation transactions in `daemon/src/operations.c` and `daemon/tests/test_acceptance.c`; enforce “Unique composite identity; active and deployment-start identities not evicted; terminal retention at least seven days”, canonical SHA-256 typed fingerprints/defaults with significant array order, same-key replay/changed-intent conflict and commit-before-dispatch/acknowledgment without persisting secrets.
- [X] T013 Implement bounded scheduler/control interfaces in `daemon/src/scheduler.c`, `daemon/src/backend.h` and `daemon/tests/test_scheduler.c`; reserve capacity durably before acceptance, cap eight runtime workers/64 pending jobs/one deployment mutation, keep observation/cancellation independent of saturated execution and make controlled backend registration a test-build-only facility, without selecting a libcrun process topology yet.
- [X] T014 Implement deployment/canonical-volume ownership and recovery interfaces in `daemon/src/ownership.c` and `daemon/tests/test_ownership.c`; combine persistent reservations, filesystem locks and actual backend identity/termination evidence so distinct keys/deployments cannot bypass one writer; represent “normal -> reconciling -> normal | uncertain” and never release on daemon/PID absence alone.
- [X] T015 Implement the bounded blocking socket client in `crates/gs/src/daemon_client.rs` and `crates/gs/tests/daemon_client.rs`; negotiate first, enforce 1..3,600,000 ms local timeouts, unique active request IDs, out-of-order response/event dispatch, same-key reconnect after lost acknowledgment and actionable version/capability/transport errors without adding an async runtime.
- [X] T016 Split local command routing from remote credential/context resolution in `crates/gs/src/lib.rs`, `crates/gs/src/cli.rs` and `crates/gs/tests/daemon_routing.rs`; reserve the planned local command shapes, retain DEP-004 only for remote correlated waits, validate formats/stdin conflicts before effects, and prove remote help/version/catalogue behavior with daemon absent and no runtime downloads.
- [X] T017 Run foundational CTest/Rust process checks via `scripts/test-daemon.sh` and record actual local/controlled results in `docs/contributing/validation/local-runtime.md`; verify M0 negotiation/framing examples and pre-acceptance failures, retain absent runtime capabilities and distinguish portable control checks from Linux execution or hosted CI evidence.

**Checkpoint**: Foundation passes before story implementation. T006→T007–T008;
T009–T014 build on those contracts and state primitives; T015→T016 integrates the
client; T017 validates the combined boundary. All runtime side effects stay gated.

## Phase 3: User Story 1 — Check local readiness safely (Priority: P1, MVP)

**Goal**: Useful non-provisioning profile/configuration preflight.
**Independent Test**: An isolated manager reports versions, limits, effective plan
and exact unavailable checks for invalid/missing prerequisites without downloads,
credentials generated, workload starts or remote-context changes.

### Tests

- [X] T018 [P] [US1] Add validation contract cases to `daemon/tests/test_validate.c` for unknown profiles/components/datastores, unmet host/assets, ignored allowProvision side effects, selected-version config layering, removed keys and distinct controller preparation/enrollment/launch prerequisites; assert no asset, workload or credential writes.
- [X] T019 [P] [US1] Add real-process CLI preflight cases in `crates/gs/tests/daemon_validate.rs` for version/capability mismatch, output formats, no-input behavior, separate local selection precedence, preserved remote context, missing external dependency diagnostics and no secret output.

### Implementation

- [X] T020 [US1] Implement trusted Profile parsing/registration in `daemon/src/profiles.c` and `runtime/profiles/core.json` from T005 evidence, enforcing “Trusted installed metadata; no caller paths, arbitrary mounts/entrypoints or mutable asset tags; exact immutable profile content”; validate profileId/profileVersion, enum/unique component arrays and map wire `scylladb` to upstream `scylla` without inventing missing optional profiles.
- [X] T021 [US1] Implement versioned configuration/native-validation planning in `daemon/src/config_validate.c` and `daemon/src/bootstrap_plan.c`; preserve API/controller additive overlays versus Git single-file semantics, config source reporting and rejected removed keys, and report missing native validators rather than claiming complete validation; do not generate/enroll/rotate credentials or expose optional profiling.
- [X] T022 [US1] Implement read-only host/asset/external-dependency assessment and `deployment.validate` in `daemon/src/validate.c`; report namespace/mapping/cgroup/mount/port restrictions, verified cache/required downloads and supported runtime dependency compatibility without pulling, loading unverified components or elevating privileges; library code dependencies do not imply host shared-library discovery.
- [X] T023 [US1] Wire component config validation/local selection and safe table/JSON/YAML plans in `crates/gs/src/deployment.rs` and `crates/gs/src/cli.rs`; implement flag/environment/local-config/default precedence, reject ambiguity/Markdown and unknown choices before effects, and preserve the proposed CLI contract without adding caller paths to the wire protocol.
- [X] T024 [US1] Run T018–T019 and US1 quickstart checks, then update `docs/contributing/validation/local-runtime.md` and `docs/local-runtime.md` with actual preflight support and unperformed checks; confirm SC-001/006/008 and that no successful runnable-profile claim depends on unavailable validation.

**Checkpoint**: US1 can be demonstrated without an execution backend. This is the
first MVP checkpoint, not completion of M2 or permission to omit real startup.

## Phase 4: User Story 2 — Inspect managed local state (Priority: P1)

**Goal**: Truthful, bounded deployment/operation observation.
**Independent Test**: Empty and test-seeded stores return exact paged records,
operation identity, recovery and generation readiness consistently after restart.

### Tests

- [X] T025 [P] [US2] Add deployment/operation query tests in `daemon/tests/test_inspect.c` for empty state, page bounds, foreign/stale/expired cursors, exact IDs, retained terminal failures, redacted diagnostics and stale-generation/failed/cancelled ready waits.
- [X] T026 [P] [US2] Add CLI list/inspect/wait process cases in `crates/gs/tests/daemon_inspect.rs`, including unavailable manager, ambiguous target, operation/deployment mismatch, table/JSON/YAML outputs, unsupported formats and timeout preserving known identity without cancellation.

### Implementation

- [X] T027 [US2] Implement `deployment.list/inspect` and `operation.inspect` queries in `daemon/src/inspect.c`; cap pages at 100, reject missing/expired IDs distinctly, implement opaque store/generation/position cursors with no invented snapshot guarantee, and return “desired running | stopped; readiness pending | ready | failed” with observed times and ownership.
- [X] T028 [US2] Implement monotonic observation deadlines and `operation.wait` in `daemon/src/wait.c`; use accepted/terminal/ready conditions, default 30-second reads/acceptance and 60-second waits, require observedGeneration == generation, reject ready for non-start operations and never convert timeout/disconnect into cancellation or old readiness into current success.
- [X] T029 [US2] Wire deployment list/inspect/operation-selector/wait commands in `crates/gs/src/deployment.rs`, `crates/gs/src/cli.rs` and shared output/error mapping; verify selected operation belongs to the deployment and keep local target selection independent of remote context/account defaults.
- [X] T030 [US2] Execute T025–T026 including restart over seeded records and record SC-002/006 evidence in `docs/contributing/validation/local-runtime.md`; publish exact observation formats, error categories and retention limits in `docs/local-runtime.md`, without presenting seeded records as real containers.

## Phase 5: User Story 5 — Run GitStore locally from its distribution (Priority: P1)

**Goal**: Real Linux core service execution with library-based capabilities and
verified selective provisioning, not a simulated-only final deliverable.
**Independent Test**: An isolated clean supported Linux host runs the candidate
core profile as a normal user; public catalogue/Git operations, readiness, stop,
restart and retained repository checks pass without manually installed tool stacks.

**Dependency rule**: US5 uses the foundational lifecycle invariants. Its safety
cases below must pass before enabling its real backend. P2 stress/diagnostic work
in US3/US4 remains mandatory before release support, rather than blocking all P1
implementation on story ordering. No unsupported backend is advertised along the way.

### Tests and selection checkpoints

- [X] T031 [P] [US5] Add real-profile lifecycle/first-use test scenarios in `tests/runtime/test_core.py` and `tests/runtime/test_assets.py` covering the six US5 acceptance cases, controlled lost acknowledgments, foreground versus detach, cache/consent/offline/wrong-platform/tamper failures and no extra tool prerequisite; initially select a test fixture only, never real user state.
- [X] T032 [P] [US5] Evaluate maintained image-transfer/unpack code dependencies and actual language/ABI/size costs in `runtime/experiments/image-library.md` and its reproducible `runtime/experiments/image-library/` probes; compare supported Rust/C integration with container-libs/go-containerregistry/umoci APIs, reject custom registry/extraction code and default executable bundling, document one implementable choice plus protected auth/extraction tests, then lock chosen dependencies before implementation.
- [X] T033 [P] [US5] Execute EXP-003A probes in `tests/runtime/libcrun/` and record results in `runtime/experiments/libcrun.md`; compare static/shared libcrun 1.30.1 code integration, allocation/free, callback lifetime, fork/signals/reaping, cancellation and native crashes; justify any worker-process boundary in `docs/ADRs/0002-runtime-ownership.md` and `docs/ADRs/0003-runtime-assets-packaging.md` before adopting it, without crun CLI or mandated dlopen.
- [X] T034 [P] [US5] Evaluate maintained rootless network library integration first in `runtime/build-inputs/libslirp/` and `runtime/experiments/network-library.md`; compare libslirp-based integration with justified pasta/slirp4netns/CNI host/on-demand fallbacks, pin inputs and test DNS, ports, helper cleanup and footprint under EXP-004A; select only a passing topology without rebuilding a network stack.

### Implementation

- [X] T035 [US5] Implement selected library capability adapters/build bindings in `daemon/src/image_backend.h`, `daemon/src/image_backend.c` and the selected Cargo/native manifests after T032; keep a bounded internal API for acquisition/unpack and delegate registry/layer semantics to maintained dependencies, recording any unavoidable executable fallback justification and delivery path in `runtime/experiments/image-library.md`.
- [X] T036 [US5] Implement immutable asset-manifest/DependencyResolution validation in `daemon/src/dependencies.c`, `daemon/src/assets.c` and `runtime/assets/manifest.schema.json`; track capability/origin/identity/version/platform/compatibility and preserve “Prefer declared code dependencies; static/dynamic linking follows measured tradeoffs; remaining incompatible external runtime dependencies require authorized provisioning, never silent fallback after side effects”; compile ordinary code dependencies normally, reuse only compatible trusted external host components and require authorized provisioning for missing/incompatible ones without silent post-effect fallback.
- [X] T037 [US5] Implement isolated registry/bootstrap credential brokerage in `crates/gs/src/runtime_credentials.rs` and `daemon/src/credentials.c`; extend purpose/registry/repository scope on the OS-store default with explicit protected-file fallback, use bounded private descriptors rather than argv/URLs/logs, disable ambient auth/helper discovery and keep API issuer, controller, Git, registry and remote account identities separate.
- [X] T038 [US5] Implement selective acquisition, safe unpack, verified atomic cache activation and cleanup in `daemon/src/provisioning.c` using T035–T037; preserve “staging -> verified -> active” and “active only after trusted-source/platform/digest verification”, credential scope/redirect rules, cancellation/disk bounds and “refcount prevents live asset eviction”; upstream images do not require attestations.
- [X] T039 [US5] Implement offline import, validator/digest-safe resume, consent-plan binding and bounded unreferenced eviction in `daemon/src/asset_cache.c`; invalid/cancelled partial bytes never activate, changed immutable plan/source requires renewed consent, active roots/retained volumes are never evicted, and no-input missing permission fails before downloading rather than invoking package managers or elevation.
- [X] T040 [US5] Implement real libcrun lifecycle/signal/exit/backoff adapter in `daemon/src/backend_libcrun.c` and `daemon/src/execution.c` after T033; use its C library API and proven process model, close unintended inherited descriptors, preserve parent-only durable state and cancellation responsiveness, enforce registered UID/mount/seccomp/resource policy and report unsupported host combinations explicitly.
- [X] T041 [US5] Implement named retained volumes and network resources in `daemon/src/volumes.c` and `daemon/src/networking.c` after T034/T040; enforce “Named profile volume, no user-selected host mount; one repository writer per canonical volume identity” and “Per-deployment isolation; owned reverse cleanup; internal endpoints not publicly bound”, using high loopback host ports and owned dependency/namespace/attachment identities before allocation.
- [X] T042 [US5] Implement the core memdb bootstrap/readiness graph in `daemon/src/bootstrap.c` and `runtime/profiles/core.json` using T005/T020 evidence: Git/API staged mutual callouts, private API-only issuer material, separate existing-key controller signing preparation/enrollment/assigned UID/launch, actual generation health/endpoints, memdb ephemeral-state explanation and no credential regeneration on restart or unsafe secret-in-argv helper copying.
- [X] T043 [US5] Connect `deployment.start/stop` to real resources in `daemon/src/deployments.c` and `daemon/src/cleanup.c`; enforce foundational dedupe/locks, commit resource stages before effects, await observed termination, retain volumes on stop/failed start, restrict reverse cleanup to recorded owners and keep timeout/uncertainty instead of pretending rollback or releasing a live writer.
- [X] T044 [US5] Implement matching installed per-user daemon startup/readiness handshake and user-service packaging in `crates/gs/src/daemon_start.rs` and `runtime/packaging/`; use the lifetime lock, sanitized environment and authenticated installed binary, disclose session/cgroup prerequisites, document manager shutdown preserving ownership, and do not require sudo, source checkout, Docker, Compose or language toolchains.
- [X] T045 [US5] Implement serve/component-serve UX and consent in `crates/gs/src/deployment.rs` and `crates/gs/src/cli.rs`; validate first, submit the immutable approved plan with explicit/generated stable key, separate accepted detached results from current readiness, display dependency downloads/source/size and reject unverified optional mappings before effects without disabling verified core profiles.
- [X] T046 [US5] Implement foreground ready observation, bounded provisional progress and explicit interrupt cancellation in `crates/gs/src/deployment.rs`; remain attached until stopped/interrupted, keep logs/progress on stderr, unsubscribe observers without killing detached work and report known operation/key plus cleanup uncertainty on transport/deadline failure; full replayable standalone streams arrive in US4.
- [X] T047 [US5] Add `scripts/test-runtime.sh` and `tests/runtime/support.py` with explicit experiment/scenario dispatch, disposable host/registry/deployment selection, safe owned cleanup and result records; route EXP-003B and core-memdb scenarios to real selected image/runtime/network adapters, and separate missing fixture/host evidence from failed assertions or controlled substitutes.
- [X] T048 [US5] Run EXP-003B and the local-volume EXP-004B cases through `scripts/test-runtime.sh`, store evidence in `runtime/experiments/images.md` and `runtime/experiments/local-volumes.md`, and fix actual adapters; cover malicious/truncated layers, auth expiry/denial/scope/redirects, host dependency compatibility, offline/cache/disk failures, retained hashes, backup/restore and no overlapping writers without requiring upstream attestations.
- [X] T049 [US5] Prepare candidate minimal packaging and run core/clean-install acceptance using `runtime/packaging/`, `tests/runtime/test_clean_install.py` and `scripts/test-runtime.sh`; start from only gs/gitstored plus essential metadata, prove consented provisioning or compatible host reuse, no unused downloads, actual public GraphQL/Git/readiness and stop/restart, and record stripped core/library/package/download/cache/memory costs against <=5 MiB per-core targets without claiming publication.
- [X] T050 [US5] Verify optional OIDC/Admin/Storefront image/config/enrollment/readiness mappings in `runtime/profiles/optional-evidence.json`, implement each verified graph in `runtime/profiles/` and add `tests/runtime/test_optional.py`; for an unavailable mapping record the exact external prerequisite in `docs/contributing/validation/local-runtime.md` and preserve explicit component-specific failure instead of invented endpoints, bundled tools or blanket Linux disablement.

  Completed in [follow-up #9](https://github.com/gitstore-dev/cli/issues/9); see [lifecycle acceptance](../../runtime/experiments/oidc-client/lifecycle-evidence.json). Admin/Storefront retain their explicitly documented external mapping prerequisites.


**Checkpoint**: Real core execution is implemented and its own lifecycle/integrity
checks pass on recorded hosts. US3/US4/US6 and cross-cutting gates still apply before
release claims; a candidate package is not a published release. Host unavailable
means that lane is recorded unverified, not replaced by a simulated startup claim.

## Phase 6: User Story 3 — Verify durable lifecycle safety (Priority: P2)

**Goal**: Controlled fault reproduction proves durable/dedupe/ownership invariants.
**Independent Test**: Isolated real daemon/socket/store with controlled workloads
passes crash boundaries, concurrent keys/volumes, saturated cancellation, retained
hash and corrupted-state cases without requiring a real container backend.

### Tests

- [X] T051 [P] [US3] Add process-kill/replay/conflict/failure-boundary cases in `daemon/tests/test_crash_replay.c` for pre/post transaction commit, dispatch, allocation and acknowledgment, including interrupted accepted detached work, unchanged terminal replay and wrong-intent key reuse.
- [X] T052 [P] [US3] Add canonical-volume concurrent ownership, PID-reuse, disk-full/corruption/quota/retention and saturated-cancellation cases in `daemon/tests/test_recovery_faults.c`, asserting preserved retained hashes and no second writer while prior termination is uncertain.

### Implementation

- [X] T053 [US3] Implement deterministic controlled workloads and crash failpoints in `daemon/tests/backend_controlled.c` and `daemon/tests/failpoints.c`; exclude both from production compilation/registration/capabilities, support slow/failed/uncertain/unsafe-migration stages and do not accept arbitrary host commands to drive the fixture.
- [X] T054 [US3] Complete replay-safe lifecycle stages, retry-safe owner-only compensation and reconciliation in `daemon/src/recovery.c` and `daemon/src/operations.c` against T051–T053; preserve “accepted -> running -> succeeded | failed | cancelled” and “Terminal outcome is immutable”, with accepted-to-cancelled allowed before work and no manufactured terminal success during uncertain recovery.
- [X] T055 [US3] Implement safe metadata retention/eviction and full-store rejection in `daemon/src/retention.c`; enforce “256 MiB/100,000 operation records, >=7-day terminal retention”, active/deployment-start/ownership protection and DB/journal reserve accounting, returning RESOURCE_LIMIT before side effects when no safe space exists rather than silently discarding accepted history.
- [X] T056 [US3] Complete durable independent cancellation/no-op terminal-target handling and safe stop deadlines in `daemon/src/cancellation.c` and `daemon/src/scheduler.c`; cancellation has its own operation/key, unsafe stages reject explicitly, all eight busy workers cannot starve the control path, and 30-second stop/shutdown expiry preserves held/uncertain ownership.
- [X] T057 [US3] Exercise actual `gs` same-key reconnect/lost-ack/foreground-interrupt behavior in `crates/gs/tests/daemon_lifecycle.rs`, repair `crates/gs/src/daemon_client.rs` and `crates/gs/src/deployment.rs` as needed, and prove timeout/client exit does not cancel detached work or mutate local Git history/configuration.
- [X] T058 [US3] Run controlled crash/disk/retention/ownership/cancellation tests and repeat affected real core checks via `scripts/test-daemon.sh` and `scripts/test-runtime.sh`; record SC-003/004/005 outcomes and controlled-versus-native limits in `docs/contributing/validation/local-runtime.md`, without closing all DEP-012 from fake workloads.

## Phase 7: User Story 4 — Diagnose failures within safe limits (Priority: P2)

**Goal**: Bounded redacted logs/events and safe malformed/slow-client failure.
**Independent Test**: Synthetic secret-bearing streams, slow subscribers and
malformed/over-limit clients cannot leak secrets, erase accepted history or kill
the manager; correct observer identity/expiry/gap errors remain inspectable.

### Tests

- [X] T059 [P] [US4] Add stream/queue/cursor/redaction tests in `daemon/tests/test_streams.c`, covering credential splits across chunks, disconnect/unsubscribe, foreign subscriptions, overflow isolation and expired replay; assert unrelated operations/observers continue within published bounds.
- [X] T060 [P] [US4] Add protocol/framer fuzz harnesses and deterministic malformed/resource corpus in `daemon/fuzz/` and `scripts/test-daemon-fuzz.sh`; expose the planned 60-second smoke command with exact corpus/compiler settings, parser allocation/depth faults, unauthorized peer cases and ASan/UBSan results without claiming exhaustive coverage.

### Implementation

- [x] T061 [US4] Implement redacted segmented logs/event cursors in `daemon/src/logs.c`; preserve “Bounded retention; redacted on ingestion; cursor expiry explicit; metadata retention is independent”, 64 MiB/7-day per-deployment and 1 GiB global retained log bounds, ordered subscription sequences and no credential persistence before redaction.
- [x] T062 [US4] Implement subscribe/unsubscribe, replay and slow-consumer shedding in `daemon/src/streams.c`; enforce eight subscriptions per connection, 256 events/4 MiB per queue and 64 MiB global queues, explicit CURSOR_EXPIRED/STREAM_OVERFLOW and separate request/subscription identities, with disconnect affecting observation only.
- [x] T063 [US4] Wire `gs deployment logs`, JSON event/text output and full foreground stream observation in `crates/gs/src/deployment.rs`, `crates/gs/src/daemon_client.rs` and `crates/gs/tests/daemon_logs.rs`; stdout contains requested standalone logs, foreground logs use stderr, invalid YAML/table log requests fail before subscribe and Ctrl-C unsubscribes without implicit workload cancellation.
- [X] T064 [US4] Finish global/per-client parser/request/connection/job/time-bound enforcement and fuzz integration in `daemon/src/server.c`, `daemon/src/protocol.c` and `.github/workflows/daemon.yml`; verify 32 connections/16 outstanding requests, 10-second partial frame/5-minute idle bounds, and preserve independent cancellation/control capacity under hostile load.
- [x] T065 [US4] Run stream/peer/over-limit/fuzz/sanitizer and real log capture checks through `scripts/test-daemon.sh`, `scripts/test-daemon-fuzz.sh` and `scripts/test-runtime.sh`; document SC-005/006 and exact untested cases in `docs/contributing/validation/local-runtime.md`, and confirm production builds cannot load test backends.

## Phase 8: User Story 6 — Maintain persistent deployments safely (Priority: P2)

**Goal**: Real persistent Scylla schema/lifecycle and crash-safe retained ownership.
**Independent Test**: Disposable Scylla/repository volumes pass compatible exclusive
migration, conflict/failure/unsafe-cancel cases and actual daemon/workload restart
without data loss, implicit downgrade or overlapping writers.

### Tests

- [X] T066 [P] [US6] Add migration/profile contract cases to `daemon/tests/test_migrations.c` for registered targets, missing backup/keyspace/compatibility, unsupported datastore, concurrent daemon/datastore lock and unsafe cancellation; assert failed/incomplete migrations block dependent API startup and accepted identity survives observation failure.
- [x] T067 [P] [US6] Add persistent Scylla and real backend recovery scenarios in `tests/runtime/test_scylla.py` and `tests/runtime/test_recovery.py`, including retained repository/datastore hashes, live container after manager kill, uncertain stop timeout, disk failure, concurrent replacement and explicit backup/restore.

### Implementation

- [X] T068 [US6] Define pinned Scylla dependencies, keyspace preparation and targetVersion-to-image/helper/schema compatibility mappings in `runtime/profiles/scylladb.json` and `runtime/profiles/migration-targets.json`; verify T005 helper flags/protected input and image versions, distinguish `scylladb`/`scylla`, forbid arbitrary scripts/downgrades and name any actual missing upstream contract before enabling it.
- [x] T069 [US6] Implement migration preconditions/execution tracking in `daemon/src/migration.c` and persistent bootstrap graph in `daemon/src/bootstrap.c`; enforce “Exclusive against workload mutations and datastore migration mechanism; failure blocks dependent startup”, backup acknowledgment, existing-keyspace requirements and standalone image-native migration without starting API, followed by API migration-check mode rather than racing auto-migration.
- [x] T070 [US6] Implement persistent datastore named mounts/retention/compatibility and restore validation in `daemon/src/volumes.c` and `daemon/src/backup.c`; use only owned profile paths, preserve repository/datastore state through stop/replacement and disk failures, and prevent a copied DB/volume snapshot from authorizing takeover of a live independent deployment.
- [x] T071 [US6] Complete actual backend/workload/network/volume discovery and restart reconciliation in `daemon/src/recovery.c` and `daemon/src/backend_libcrun.c`; observe canonical identity and prior termination before replacement/release, restore inspect/log observation, persist uncertain ownership on inaccessible state and preserve lock identity through daemon shutdown and PID reuse.
- [x] T072 [US6] Wire `gs migrate` and real persistent stop/recovery status in `crates/gs/src/deployment.rs`, `crates/gs/src/cli.rs` and `crates/gs/tests/daemon_migrate.rs`; require target/backup selection, stable idempotency keys and truthful terminal/deadline/cancel results, keep secret input protected and never fall back to another datastore/write path.
- [x] T073 [US6] Run persistent Scylla migration, failed-start, backup/restore and controlled/native cancellation cases via `scripts/test-runtime.sh --scenario persistent-scylla`; fix actual failures and record exact image/helper/schema/host identities and SC-011 results in `runtime/experiments/scylladb.md` and `docs/contributing/validation/local-runtime.md`.
- [X] T074 [US6] Run actual manager/workload crash and concurrent Git-writer replacement cases via `scripts/test-runtime.sh --scenario recovery`; verify retained contents and absent overlap through real backend observations, record unresolved host/filesystem behavior explicitly in `runtime/experiments/recovery.md`, and do not substitute mock evidence.
- [x] T075 [US6] Update persistent operation, migration backup/recovery prerequisites and memdb-versus-retained-volume explanations in `docs/local-runtime.md` and `docs/contributing/validation/local-runtime.md`; verify T066–T074 results, report failed/unsafe/uncertain states truthfully and map FR-009–013/028 to actual evidence before declaring this story complete.

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Feasibility, footprint, release policy and evidence reconciliation.
All support claims remain scoped to actual results; no binary is published here.

- [x] T076 [P] Implement/run EXP-005A Apple container/direct Containerization feasibility in `runtime/experiments/apple/` and `runtime/experiments/apple.md` on explicitly selected compatible disposable Mac fixtures; compare maintained library integration, pin inputs and test installation privilege, private images, loopback discovery, logs, health, retained volumes and stop/crash recovery, or record unavailable hardware without blocking Linux.
- [x] T077 [P] Implement/run EXP-005B managed Linux guest feasibility in `runtime/experiments/macos-vm/` and `runtime/experiments/macos-vm.md`; prefer maintained integration, avoid default VM-tool bundling, verify authorized missing-component/guest assets, authenticated host/guest control and all core lifecycle/retention failures, with unavailable/failed combinations explicitly unsupported and no privileged automatic fallback.
- [x] T078 Implement/run EXP-003C CLI publication verification fixtures in `tests/packaging/cli-provenance/` and document the trusted producer/workflow/source/digest policy in `runtime/packaging/release-policy.md`; validate CLI file/offline release evidence and wrong/missing/tampered cases, add OCI only if this repository publishes OCI, scope credentials to trusted release build jobs when targets exist and keep upstream service startup independent of attestations with no bundled gh.
- [x] T079 Measure and record stripped core release artifacts through `scripts/measure-runtime-size.sh` and `docs/contributing/binary-sizes.md`; use the user-accepted baseline (gs 5,590,568 bytes; gitstored 5,723,600 bytes), preserve exact identities and separately report required libraries/package/download/cache/memory/cold-warm readiness. Published-binary optimization is deferred to the user's follow-up and is not a completion gate for this increment.
- [x] T080 Add real Linux reference-host amd64/arm64 validation/package check lanes in `.github/workflows/runtime.yml` and `runtime/packaging/` after executable runners exist; include library-first selections, host-present/incompatible/missing/denied/offline dependency cases, sanitizers and clean installation without manually installed tools, and never present unavailable runner/host or untrusted PR outputs as release evidence.
- [x] T081 Update `README.md`, `CONTRIBUTING.md` and `docs/local-runtime.md` for actual install/daemon/user-service lifecycle, formats, prerequisites, footprint and supported subsets; preserve remote-only operation and M1 gates, document no default helper bundle and distinguish ordinary code dependencies from remaining host/runtime requirements.
- [x] T082 Run the complete applicable `specs/003-m2-daemon-foundation/quickstart.md` validation through `scripts/test-daemon.sh` and `scripts/test-runtime.sh`, plus pinned `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets --locked -- -D warnings`, `cargo test --workspace --locked` and `cargo build --workspace --locked`; record actual local/controlled/native/hosted/unavailable results and unresolved Postman environment failures in `docs/contributing/validation/local-runtime.md` without claiming planned commands ran.
- [x] T083 Reconcile proven per-command/profile gates in `docs/contracts/command-matrix.md`, `docs/contracts/dependencies.md`, `docs/ADRs/0002-runtime-ownership.md`, `docs/ADRs/0003-runtime-assets-packaging.md`, `docs/ADRs/0004-rootless-network-volumes.md` and `docs/ADRs/0005-platform-backends.md`; preserve stable IDs/historical evidence, close only proven scope and retain named missing optional mappings/host checks without disabling verified subsets.
- [x] T084 Audit FR-001–031, SC-001–013, all six stories and task completion evidence in `specs/003-m2-daemon-foundation/tasks.md`, `specs/003-m2-daemon-foundation/checklists/requirements.md` and `docs/contributing/validation/local-runtime.md`; check local links/whitespace and that no credentials, bundled-tool/dynamic-loader assumptions, upstream-attestation gates or unsupported release claims remain, marking only actually completed tasks checked.

## Dependencies & Execution Order

### Phase and story dependencies

```text
Setup T001–T005 -> Foundation T006–T017
  -> US1 T018–T024 -> US2 T025–T030
  -> US5 T031–T050 (P1 real execution; builds on US1/US2/foundation)
  -> US3 T051–T058 (controlled stress/recovery coverage)
  -> US4 T059–T065 (full streams/resource/fuzz coverage)
  -> US6 T066–T075 (persistent datastore and real recovery)
  -> Final T076–T084
```

Story IDs retain spec identity; P1 US5 precedes P2 US3/US4/US6 in the task file.
Tests are independently runnable with each story's own fixtures once its shared
code dependencies exist; lifecycle stories are not falsely described as independent
of required ownership/protocol/asset implementations. US1 alone is the first MVP.

Within US5: T031–T034 can start after US1/US2; T035 requires T032; T036 can proceed
after the contracts/locks are stable; T037 precedes private auth in T038;
T038→T039, T033→T040, T034/T040→T041, T038–T041→T042→T043;
T044→T045, T043/T045→T046→T047→T048→T049→T050. Test runner scaffolding may be
prepared earlier but no real backend is declared proven until its checks pass.
T033/T034 may need minimal owned image/rootfs fixtures and foundational resource
reservations; these probes use isolated experiment inputs and do not depend on
completed core-profile provisioning.

US3/US4 test drafting can overlap US5 after shared interfaces stabilize; US3 must
finish before US6 real recovery validation. T059–T060 can overlap within US4;
T061→T062→T063/T064→T065. US6 requires US5 and the completed US3/US4 invariants;
T066–T067 precede T068→T069, T070/T071→T072→T073→T074→T075.

Mac experiments T076–T077 may begin after reusable US5 boundaries exist, independently
of Linux completion, provided isolated host fixtures are available. T078 is a
publication-policy check, not a prerequisite for normal upstream image startup.
T079→T080→T081→T082→T083→T084 consolidates results; unavailable platform/registry
fixtures are recorded explicitly and must not be relabeled completed validation.

### Shared-file serialization

Changes to `cli.rs`, `lib.rs`, `deployment.rs`, core C headers, SQLite schema,
`runtime/dependencies.lock.json`, profile manifests, ADRs and `docs/contributing/validation/local-runtime.md`
serialize. `[P]` does not permit concurrent edits to these shared files. A dependency
lock update is integrated after candidate results, not raced across experiments.
The sketch above orders integrated completion, not a claim all tests can run before
the modules they exercise are built.

## Parallel Execution Examples

| Story | Eligible separate work after common prerequisites |
| --- | --- |
| US1 | T018 daemon validation tests and T019 CLI preflight tests; integrate before T020–T024 |
| US2 | T025 daemon query tests and T026 CLI inspection tests; shared record interfaces are already stable |
| US5 | T031 fixture tests, T032 image-library probes, T033 execution-library probes and T034 network probes touch separate files; serialize lock/ADR integration |
| US3 | T051 crash replay cases and T052 ownership/quota/cancellation cases before controlled implementation/fixes |
| US4 | T059 stream scenarios and T060 fuzz harness/corpus; build/CI integration follows their implementation |
| US6 | T066 migration unit/contract cases and T067 real fixture scenarios; profile/volume/recovery implementation integrates sequentially |

T003–T005 are setup opportunities after T001–T002. T076/T077 are separate host
experiments, not authorization to create agents or install privileged software.
Optional host absence must not stop unrelated implementation.

## Requirement Coverage

| Requirement | Tasks |
| --- | --- |
| FR-001 | T003, T009, T017, T044, T064 |
| FR-002 | T006–T009, T015, T017, T050 |
| FR-003 | T018–T024 |
| FR-004 | T016, T019, T023, T026, T029, T045 |
| FR-005 | T025–T030 |
| FR-006 | T010–T015, T027–T030, T051–T058, T071 |
| FR-007 | T012, T015, T043, T051, T054, T057 |
| FR-008 | T010–T012, T052, T055 |
| FR-009 | T013–T014, T041, T043, T052, T068–T074 |
| FR-010 | T014, T054, T071, T074 |
| FR-011 | T013, T015, T028, T046, T056–T058, T072 |
| FR-012 | T041, T043, T048, T052, T054, T070–T075 |
| FR-013 | T053, T056, T066, T068–T075 |
| FR-014 | T059–T065 |
| FR-015 | T006–T009, T013, T052, T055–T056, T059–T065 |
| FR-016 | T009, T018, T020–T022, T035–T043, T068 |
| FR-017 | T016, T019, T023, T026, T029, T045–T046, T063, T072 |
| FR-018 | T003, T011–T012, T019, T037–T039, T059, T061 |
| FR-019 | T001, T013, T053, T065 |
| FR-020 | T017, T022, T033–T050, T068–T075, T083 |
| FR-021 | T003–T004, T006–T019, T025–T026, T031–T034, T047–T052, T057–T060, T064–T067, T073–T080, T082 |
| FR-022 | T016–T019, T024, T081–T084 |
| FR-023 | T005, T018–T024, T042, T050, T068 |
| FR-024 | T033, T040–T050, T071, T074, T080 |
| FR-025 | T031–T032, T036, T038–T039, T045, T048–T049 |
| FR-026 | T032, T037–T038, T048 |
| FR-027 | T022, T034, T041, T048–T049 |
| FR-028 | T041, T043, T048, T067, T070, T073–T075 |
| FR-029 | T005, T020–T024, T042–T050, T068–T069 |
| FR-030 | T002, T005, T033–T034, T048–T050, T073–T080, T083 |
| FR-031 | T002, T022, T032–T036, T039, T049, T076–T081 |

| Outcome | Acceptance evidence tasks |
| --- | --- |
| SC-001 | T018–T024 |
| SC-002 | T025–T030, T054, T057 |
| SC-003 | T051–T058, T071, T074 |
| SC-004 | T048, T052, T054, T058, T070–T075 |
| SC-005 | T007–T009, T052, T056, T059–T065 |
| SC-006 | T019, T024, T026, T030, T049, T063, T082 |
| SC-007 | T017, T024, T030, T048–T050, T058, T065, T073–T084 |
| SC-008 | T018, T020–T024 |
| SC-009 | T031–T050, T080, T082 |
| SC-010 | T031–T039, T048–T049 |
| SC-011 | T066–T075 |
| SC-012 | T032–T034, T048–T050, T073–T080 |
| SC-013 | T022, T031–T036, T039, T049, T079–T082 |

## Implementation Strategy

1. Complete setup/foundation and US1; validate the read-only MVP independently.
2. Add US2 observation, then execute the real P1 Linux story US5. Library selection
   and packaging/network experiments are implementation tasks, not excuses to
   terminate with only fake workloads. Continue unblocked work when one optional
   mapping or host fixture is missing.
3. Complete controlled stress US3, bounded diagnostics US4 and persistent real
   maintenance US6. Repair affected earlier code and rerun relevant behavior checks.
4. Complete feasible Mac experiments and final size/distribution/evidence checks.
   CLI publication provenance stays separate from upstream image integrity.
5. Mark tasks only after code and required checks pass. A report of unavailable
   hardware is an honest limit, not completion of the missing platform validation;
   keep that task/subset open and report the concrete blocker. Never equate a
   supported-contract row, passing simulation or small executable with a complete
   M2 release. No task here authorizes publishing, merging or credential exposure.

## Generation Validation

84 unchecked tasks: setup 5, foundation 12, US1 7, US2 6, US5 20, US3 8,
US4 7, US6 10, final 9. Every task has a sequential ID, applicable story label
and exact output path. All six stories have independent fixture criteria and
parallel examples; every FR-001–031 and SC-001–013 has mapped work. No extension
hook file is configured, so no before/after tasks hooks require dispatch.

T070 validation: typed-volume/backup/schema-upgrade checks pass on the host and
Linux ASan/UBSan; the bounded tmpfs ENOSPC case preserves actual Scylla data and
writer identities. Saved native independent backup/restore and retained actor
restart cover payload compatibility and continuity. The live copied-state case
`runtime-run-scylla-native-7qmdiq8f.json` rejects copied SQLite/volume/cache-owner
metadata before dispatch while preserving all four original workload identities
and both writer locks, then observes recovery/stop (79.814 s). See
[runtime evidence](../../runtime/experiments/scylladb.md#persistent-actor-recovery-checks).
Public persistent lifecycle and standalone migration remain separately gated.

Standalone migration execution checkpoint: T069 passes its private Linux arm64
image-native path under ASan/UBSan in `runtime-run-scylla-native-o2jjyeup.json`
(42.019 s). Production schema/stop actors bind the registered retained policy,
existing datastore and journaled namespace; inspect the existing keyspace and
compatible schema history; commit the single schema-helper dispatch; validate
all 12 records afterward; and record owned source deletion, writer reacquisition
and network termination before successful finalization. No API workload runs.
The separately controlled schema-v4 upgrade, attachment gates, failed migration
startup block and uncertain helper ownership remain covered. T072 manager/public
wiring and T073 wider failure/cancellation/platform validation remain open.

Recovery checkpoint: T071 is implemented with native identity/population reopening,
read-only inspection/log receipts, exact kernel writer/network observation and
separate STOP reconciliation of interrupted migration. Native manager-dispatch
proof `runtime-run-scylla-native-rx1sztj2.json` passes; the subsequent Linux
ASan/UBSan CTest selection passes 59 tests and skips the unavailable cross-UID
fixture (60 selected, 75.46 s). The two connection-deadline tests were excluded
from this focused regression and retain their separately recorded results.
Inaccessible or replaced identities remain uncertain. T072 public persistent
startup/migration integration remains in progress; no release claim is made.

Public persistent integration checkpoint: T072 passes the real source-built
`gs`/daemon Unix socket path in `runtime-run-persistent-scylla-d72b4527.json`
(103.439 s), including Scylla startup/readiness, owned stop, standalone registered
migration, safe target/completion projection, same-key replay and unchanged
canonical datastore identity. The previous `yxfq8068` run remains failed: native
migration succeeded but its public result omitted migration completion fields.
The bounded result projection is now covered by positive and forged-digest
inspection tests. Queued migration cancellation passes metadata/sanitizer tests;
running cancellation is explicitly refused. Seven focused Rust lifecycle/migration
cases and two daemon-backed validation cases pass. The wider failure/restore
matrix remains T067/T073; publication/platform gates remain open.

Persistent scenario checkpoint: T067's test scenarios are now implemented,
including the actual Scylla paused-init stop timeout, held datastore writer and
separate cleanup retry (`runtime-run-persistent-scylla-u2bm8u0n.json`, 148.345 s),
manager-loss/concurrent replacement, retained hashes, independent backup/restore
and the bounded 1 MiB private-tmpfs ENOSPC probe for both volume roles. Public
Scylla digest rejection before any native service passes in
`runtime-run-persistent-scylla-yqmqo5y0.json` (0.594 s). Final current native
backup/restore/cancellation validation remains under T073.

Publication/CI checkpoint: T078 adds the fixed own-producer offline verification
gate and three policy tests. Ten real GitHub CLI 2.103.2 cryptographic controls
pass using pinned official signed inputs; the own unsigned CLI is rejected and
no GitStore release is claimed. T080 adds amd64/arm64 normal/sanitizer library,
production-package and explicitly dispatched native reference lanes with read-only
permissions. Guarded matrix/YAML/pin checks pass; hosted jobs and unavailable
reference hosts remain not-run, not substituted by local results. T081 updates
user-facing setup, retained Scylla/migration/recovery/footprint and optional service
configuration, separating source prerequisites from product dependencies.

## Phase 10: Completion audit remediation

- [x] T085 Integrate production bounded whole-deployment restart supervision per FR-024 and the T084 audit (partial): react only to observed native exit from a previously ready, current owned generation; create separately inspectable idempotent stop/start operations, wait for observed cleanup and released writers before any replacement, enforce the existing bounded backoff/attempt budget without blocking the control loop, and stop retrying on uncertainty, operator intervention or manager loss. Add real workload-exit/no-overlap and control/budget checks; preserve immutable prior operation outcomes and remote-only behavior. The existing `gstd_execution_backoff` unit test alone does not satisfy this requirement.

### Feasibility task outcome scope

T076/T077 are complete as bounded feasibility assessments, not passing Mac product
validation. The ordinary Apple SDK probes and exact final artifact receipt are in
`runtime/experiments/apple/evidence.json`; `apple.md` and `macos-vm.md` record the
actual guest passes, direct seccomp rejection and unavailable clean disposable Mac,
amd64/Lima, host endpoint/provisioning/control/interruption combinations. No public
Mac backend is selected; DEP-015 and all those support gates remain open. The tasks'
unavailable-fixture paths are used explicitly rather than relabeling missing host
validation as a pass.

### Final applicable checks and audit scope

T082/T084 completed the applicable source-preview verification and requirement
audit on 2026-11-05: pinned Rust fmt/clippy/test/build, full remote controlled
runner, actual Rust/C daemon runner, complete earlier timer runs plus all affected
post-supervision C/sanitizer/native checks. The evidence archive explicitly names
Postman default initialization failure, ignored/skipped cases, unrun hosted lanes,
unsupported hosts and optional provider gates. All 256 changed/new local Markdown
file targets exist; changed JSON parses, the three-job runtime workflow parses
with read-only default permissions and pinned actions, `git diff --check` passes,
and the read-only quality checklist remains 16/16. Production symbols include the
supervisor and no `gstd_test_` entry points. No generated binary or large runtime
payload is included in the untracked source/evidence files.

That earlier audit left T050 open for the optional OIDC lifecycle. The follow-up
below completes its source implementation and acceptance. Admin/Storefront
published mappings remain unavailable; DEP-013/015 release and clean-host/platform
gates remain open independently of source task completion.

### T050 completion (2026-11-06)

The follow-up integrates immutable core/2 selection and consent, separate provider
material and SQLite volumes, two journaled image-native migrations, five services,
one-shot enrollment, API issuer/client/audience configuration, bounded role health
and discovery/JWKS, V6 metadata, endpoint/log projection, reverse stop and read-only
recovery. Every new generation checks all unresolved owners and recovery state,
including standalone selection without a repository volume. A final read-only
provider sweep follows all selected service startup within a 30-second window.

Public five-provider and combined eight-service memdb acceptance passed. The
non-test production build passed the nine-service retained-Scylla graph, stalled
bridge readiness, foreground interruption, retained startup, actual manager loss,
refused replacement and owned stop; original successful operations stayed immutable.
Standalone sanitizer acceptance covers fresh/retained startup, dropped-ACK replay,
redacted aggregate/bridge logs, retained ports/material/volumes/cache and manager
loss. The final sanitizer build also passed a separately accepted retained
standalone generation after a known pre-native preparation failure.

Normal and ASan/UBSan C regression selections each have 71 passes, zero failures
and one cross-UID skip. The unchanged long connection timer passed in an earlier
complete run. Pinned Rust fmt/Clippy/test/build, Python syntax, JSON/local links and
whitespace checks accompany the change. The requirement checklist remains 16/16.
Exact artifact identities, failed disk-pressure attempts and acceptance receipts
are in [lifecycle evidence](../../runtime/experiments/oidc-client/lifecycle-evidence.json).
The user approved cleanup of seven stopped failed fixtures; only verified generated
execution/cache copies were removed. Data, material, logs, journals and inputs remain.

All 85 implementation tasks are checked. This does not qualify native amd64,
macOS, clean-host/provider support or own-artifact publication. Admin UI still
requires its published image/authentication/startup/readiness mapping; Storefront
still requires its reference runtime, publication reads and trusted target binding.
Those named failures remain explicit before effects. CLI login and local alpha.9
schema upgrades are outside this follow-up.
