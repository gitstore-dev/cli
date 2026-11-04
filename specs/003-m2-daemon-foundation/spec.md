# Feature Specification: M2 Runtime — Unblocked Implementation

**Feature Branch**: Existing checkout (`main`); no branch hook configured or branch change required.

**Created**: 2026-11-03

**Status**: Draft; ready for planning, no M2 implementation or runtime support claimed

**Input**: User description: "implement unblocked M2 capabilities"

This feature implements M2 capabilities whose dependencies are available or can
be resolved through work in this repository: local control, Linux execution,
verified assets, rootless networking, retained volumes, core-service lifecycle,
migrations and recovery. It includes the experiments needed to select and validate
these integrations. A validation gate limits activation and support claims; it is
not a reason to omit the implementation that produces its evidence. Truly missing
external contracts and unavailable host evidence remain explicitly bounded.

The [shipped-release review](release-evidence.md) supplements the original M0
baseline with GitStore `v0.1.0-alpha.8`, resolved to committed revision
`06152d193e854bc643fa884daa8b9f30b0d75d0f`. Shipped server features are available
inputs to profile design; they are not missing merely because daemon integration
has not been built. Runtime selection experiments and implementation checks are
work toward M2, not external server-feature blockers. The user clarified that
this feature covers unblocked M2 implementation, not a foundation-only deliverable.

## User Scenarios & Testing

### User Story 1 — Check local readiness safely (Priority: P1)

An operator checks whether the local manager is compatible and what prevents a
selected GitStore profile from running, without downloading assets, starting
services or changing their remote catalogue selection.

**Why this priority**: A trustworthy preflight is useful before a runtime is
available and prevents setup attempts from creating unsafe partial deployments.

**Independent Test**: On an isolated user configuration, inspect manager
capabilities and validate registered profiles with missing assets, unavailable
backends and invalid configuration. Verify that each diagnostic identifies the
next action and that no provisioning or workload starts.

**Acceptance Scenarios**:

1. **Given** a compatible local manager, **When** a client connects, **Then** it
   learns the supported capabilities and limits before requesting any operation.
2. **Given** an incompatible manager or required missing capability, **When** a
   client connects, **Then** it receives an actionable compatibility failure and
   no mutation occurs.
3. **Given** a registered profile whose execution prerequisites are unmet,
   **When** validation runs, **Then** it reports the effective non-secret plan,
   missing requirements and unavailable checks; it does not report the profile
   as runnable or start downloads, even if provisioning permission was supplied.
4. **Given** an unknown profile, component, datastore or conflicting configuration,
   **When** validation runs, **Then** it fails before any workload or asset change
   and preserves the operator's prior valid configuration.
5. **Given** a remote-only command, **When** it executes with no daemon installed,
   **Then** its existing behavior remains available without runtime provisioning.
6. **Given** configuration for a selected shipped server version, **When** its
   profile is validated, **Then** version-specific configuration layering and
   removed keys are respected, and controller credential preparation, enrollment
   and launch are shown as distinct prerequisites without generating or rotating
   credentials during validation.

### User Story 2 — Inspect managed local state (Priority: P1)

An operator lists and inspects local deployments and their recorded operations,
including failures and recovery uncertainty, without mistaking acceptance for
service readiness.

**Why this priority**: Inspection must be available before lifecycle execution;
otherwise detached work and failed recovery cannot be diagnosed safely.

**Independent Test**: With an empty manager, verify bounded empty results and
not-found errors. With isolated test-created records, verify pagination, exact
selection, redaction and consistent operation/deployment status after restart.
No real service backend is required for these checks.

**Acceptance Scenarios**:

1. **Given** no managed deployments, **When** list runs, **Then** it returns an
   empty result rather than fabricating a default deployment.
2. **Given** several recorded deployments, **When** the operator lists a page or
   inspects an exact ID, **Then** only the requested records are returned with
   current observation time, readiness, generation and ownership state.
3. **Given** an accepted, failed or uncertain operation, **When** it is inspected,
   **Then** its durable identity, progress and safe error remain available;
   acceptance or an older ready generation never appears as current readiness.
4. **Given** a missing or expired identity or cursor, **When** inspection or paging
   runs, **Then** it returns the documented error without changing any state.
5. **Given** an unavailable daemon or ambiguous deployment selection, **When** a
   lifecycle command runs non-interactively, **Then** it fails with an actionable
   category without a prompt or silently selecting the remote context's target.

### User Story 3 — Verify durable lifecycle safety before enabling execution (Priority: P2)

A runtime maintainer exercises interrupted, repeated and conflicting operations
against isolated test workloads to verify that accepted work survives client loss
and recovery never permits overlapping repository writers or deletes retained data.

**Why this priority**: These guarantees are prerequisites for activating a real
runtime. Their development must not depend on a production container installation.

**Independent Test**: Use a test-only controlled workload source, disposable
persistent state and synthetic retained data. Interrupt acceptance, execution,
stop and recovery, reconnect and replay operations, and check identity, ownership
and retained content. This does not demonstrate real container behavior.

**Acceptance Scenarios**:

1. **Given** a mutation accepted durably, **When** its response or client connection
   is lost and the same request is replayed, **Then** the original operation is
   returned without duplicate work. Changed intent with the same key conflicts.
2. **Given** accepted detached work, **When** its client exits or an observation
   times out, **Then** work is not implicitly cancelled and its identity remains
   inspectable after manager restart.
3. **Given** concurrent requests for the same deployment or repository volume,
   **When** different keys attempt to acquire ownership, **Then** at most one
   writer is permitted and the conflict is explicit.
4. **Given** uncertain ownership after restart or damaged durable state,
   **When** replacement is requested, **Then** it is refused until termination
   and exclusive ownership can be proven; uncertainty is not converted to success.
5. **Given** owned transient resources and retained data, **When** startup fails
   or stop is repeated, **Then** cleanup touches only recorded owned resources,
   retained contents remain unchanged and locks are not released prematurely.
6. **Given** all execution workers are occupied, **When** cancellation is requested,
   **Then** the bounded control path can record it without waiting behind the
   target's exclusive mutation; unsafe cancellation is refused explicitly.

### User Story 4 — Diagnose failures within safe limits (Priority: P2)

An operator or automation client receives bounded logs, events and structured
failures while a slow or malformed client cannot exhaust the local manager.

**Why this priority**: Inspectable failures and bounded observation are necessary
before long-running production services can be entrusted to the manager.

**Independent Test**: Feed synthetic secret-bearing logs, slow subscriptions,
malformed messages and quota pressure through isolated client/manager processes;
verify safe errors, stream recovery guidance and continued inspection availability.

**Acceptance Scenarios**:

1. **Given** recorded test-workload logs and events, **When** a client subscribes,
   **Then** observations carry the correct deployment or operation identity,
   remain bounded and omit credentials.
2. **Given** a slow subscriber or an expired replay cursor, **When** limits are
   reached, **Then** the gap or expiry is explicit and unrelated requests and
   accepted work remain intact.
3. **Given** an unauthorized peer, malformed message or excess request,
   **When** it reaches the manager, **Then** it is rejected within the published
   bounds before side effects, without exposing secrets or crashing the manager.

### User Story 5 — Run GitStore locally from its distribution (Priority: P1)

A developer installs GitStore on a supported Linux host and starts the core
services as their normal user, without assembling a separate runtime stack.

**Why this priority**: Real local execution is the principal M2 user outcome;
control-plane tests alone cannot satisfy it.

**Independent Test**: On a clean disposable Linux host, install the candidate
GitStore distribution, authorize first use, run the core profile, inspect its
reported endpoints and exercise public catalogue/Git operations. Stop and restart
while verifying retained repository contents.

**Acceptance Scenarios**:

1. **Given** a capable Linux host and no cached assets, **When** an interactive
   user authorizes the displayed profile assets and starts GitStore, **Then** the
   per-user manager obtains verified assets, prepares private credentials and
   enrollment, and reports readiness only after core service checks pass.
2. **Given** automation without cached assets or provisioning authorization,
   **When** startup runs, **Then** it fails before downloading; explicit
   authorization or a verified offline bundle allows startup without prompting.
3. **Given** a verified warm cache, **When** the same profile starts again,
   **Then** it reuses matching assets without unrelated downloads. Interrupted,
   tampered or wrong-platform assets are never activated.
4. **Given** separate deployments, **When** they run concurrently, **Then** their
   service names and retained volumes are isolated, public ports bind to loopback,
   and internal service/datastore ports remain private.
5. **Given** foreground or detached startup, **When** the CLI exits or is
   interrupted, **Then** detached work remains inspectable and foreground
   cancellation requests owned graceful cleanup. Ordinary stop retains data.
6. **Given** a missing host capability or unavailable optional component mapping,
   **When** startup is requested, **Then** it fails before execution with the exact
   unmet requirement and never escalates privileges or substitutes another backend.

### User Story 6 — Maintain persistent deployments safely (Priority: P2)

An operator uses the persistent datastore profile, prepares its schema and
recovers service ownership without losing repository or datastore contents.

**Why this priority**: M2 includes migrations and retained state; their failure
semantics must be proven against real services as well as controlled workloads.

**Independent Test**: Use a disposable persistent datastore and repository volume;
perform a supported migration, reject incompatible/concurrent targets, inject
failure, restart the manager and verify that no replacement overlaps an old writer.

**Acceptance Scenarios**:

1. **Given** a registered compatible migration target and acknowledged backup
   prerequisites, **When** migration starts, **Then** the existing migration
   mechanism runs exclusively as a tracked operation without starting the API.
2. **Given** failed or incomplete migration, **When** dependent startup runs,
   **Then** it is blocked; no downgrade or unsafe cancellation is attempted.
3. **Given** a running deployment and retained resources, **When** the manager
   crashes and restarts, **Then** it discovers actual workloads before replacement,
   restores observation and proves non-overlap before permitting a new Git writer.
4. **Given** uncertainty about writer termination, **When** stop or replacement
   times out, **Then** ownership stays held or uncertain and retained data remains
   untouched until the condition can be resolved.

### Edge Cases

- A stale socket, symlinked runtime directory or second manager targets the same
  state: refuse unsafe reuse without removing another owner's resources.
- A message arrives partially, exceeds limits, contains duplicate fields or
  terminates early: bounded rejection must not disturb previously accepted work.
- State storage fills or the manager crashes immediately before/after acceptance:
  never acknowledge uncommitted intent or discard active ownership to make space.
- A process identifier is reused or a workload cannot be observed: preserve
  uncertain ownership rather than treating the old writer as terminated.
- An old deployment generation was ready: it cannot satisfy a newer start wait.
- A cancelled or failed operation is replayed: return its original outcome,
  without presenting cancellation as rollback or retrying it with a fresh key.
- A caller requests arbitrary execution, mounts, hooks or
  an unregistered migration target: reject before execution or credential use.
- The host lacks execution support: local inspection can still work where its
  control interface is supported; production start remains explicitly unavailable.

## Requirements

### Functional Requirements

- **FR-001**: Provide a per-user local manager and client connection that enforce
  owner-only access, safe runtime/state locations and a single manager per state
  store. Remote catalogue credentials MUST NOT substitute for local peer identity.
- **FR-002**: Negotiate versions, capabilities and limits before operations using
  the accepted [local protocol](../../docs/contracts/daemon-protocol.md). Advertise
  only capabilities actually available in that build and mode; reject unsupported
  capabilities before side effects.
- **FR-003**: Validate registered profile selections without provisioning or
  execution. Report effective non-secret settings and their sources, required
  assets, host requirements and diagnostics. Unperformed backend/component checks
  MUST be identified and MUST NOT produce a runnable-profile success claim.
- **FR-004**: Keep local deployment selection distinct from remote installation,
  account and namespace selection. Require explicit or unambiguous local targets;
  reject unknown components, datastores and unsupported output choices before writes.
- **FR-005**: Provide bounded deployment list/inspect and operation inspect/wait
  using exact identities, documented pagination and the accepted error categories.
  Empty state MUST be useful; missing/expired records MUST remain distinguishable.
- **FR-006**: Preserve operation identity, timestamps, truthful outcome, recovery
  state and safe failure details across client disconnect and manager restart.
  Acceptance, completion and current-generation readiness MUST be separate.
- **FR-007**: Atomically persist accepted mutation intent, deduplication identity
  and ownership before acknowledgment. Same caller/method/key and normalized intent
  MUST return the original operation; changed intent MUST conflict, including after
  restart. A lost acknowledgment MUST be recoverable without untracked retry.
- **FR-008**: Enforce the protocol's retention and capacity policy. Never evict
  active operation, deployment-start or ownership records to accept new work;
  reject new work before side effects if durable safe capacity is unavailable.
- **FR-009**: Serialize workload-changing operations per deployment and enforce
  exclusive repository-volume ownership across different deployments and request
  keys. Replacement and lock release MUST require observed prior-writer termination.
- **FR-010**: Reconcile recorded intent with observed owned resources after restart
  before accepting conflicting work. Corrupt state or unprovable ownership MUST
  fail closed with inspectable uncertainty; absence of a saved process alone is
  insufficient evidence to replace a writer.
- **FR-011**: Preserve detached operations on disconnect or observation timeout.
  Cancellation MUST be explicit, durably tracked, available through the bounded
  control path and refused at unsafe stages. Foreground interruption MUST request
  cancellation and report cleanup status rather than claim automatic rollback.
- **FR-012**: Stop and failed-start cleanup MUST be retry-safe, restricted to
  recorded owners and preserve retained data. Unobserved termination MUST retain
  held/uncertain ownership and an actionable timeout or recovery outcome.
- **FR-013**: Implement the control semantics of migration exclusivity, registered
  target/compatibility and backup preconditions, unsafe cancellation and blocking
  dependent startup after failure. Execute the verified migration mechanism for supported datastore/profile targets;
  reject unverified targets before side effects. No automatic downgrade or arbitrary
  migration script is permitted.
- **FR-014**: Bound logs/events, replay, subscriptions and slow-consumer queues per
  the accepted protocol. Expose stream gaps and cursor expiry explicitly; observer
  failure MUST NOT cancel detached work or erase operation metadata.
- **FR-015**: Enforce all applicable message, parser, connection, concurrency,
  deadline, history and log bounds in the protocol, including rejection before
  oversized allocation and cancellation availability under worker saturation.
- **FR-016**: Reject arbitrary executables, host paths/mounts, hooks, runtime flags
  and privileged settings. No operation may silently elevate privileges or bypass
  the managed GitStore profile boundary.
- **FR-017**: Keep requested results on stdout and diagnostics/progress on stderr;
  honor non-interactive and no-color behavior and existing machine-readable error
  and exit conventions. Document supported formats; reject others before mutation.
- **FR-018**: Exclude credentials from persisted intent, plans, output and logs.
  Preserve separate catalogue, Git and registry identities; use only synthetic
  credentials and disposable state in verification.
- **FR-019**: Provide a test-only controlled workload facility for verifying
  lifecycle, migration and ownership failures. It MUST NOT be registered in normal
  production capability discovery, selectable through production profiles, or
  presented as container, image, network or datastore compatibility evidence.
- **FR-020**: Implement production serve/component-serve, migration and provisioning
  when their concrete prerequisites pass. Unsupported host/profile/component
  combinations MUST fail before side effects with named prerequisites. A global
  open dependency record MUST NOT disable a proven supported subset. Simulated
  deployments MUST NOT be presented as real runtime evidence.
- **FR-021**: Verify client/manager interoperability, malformed input, peer isolation,
  resource bounds, durable recovery and retention before enabling corresponding
  production capabilities. Add applicable build, behavior and memory-safety checks
  with executable targets; distinguish local, simulated and real-backend evidence.
- **FR-022**: Preserve existing remote command behavior without a daemon or runtime
  downloads. Document this increment's implemented subsets and remaining gates;
  neither development checks nor accepted designs establish release support.
- **FR-023**: Tie profile validation to an explicitly recorded server release and
  committed revision. Preserve that version's configuration layering and reject
  removed settings rather than silently applying defaults. Separate controller
  signing-material preparation, enrollment with assigned identity and launch;
  validation MUST NOT generate, enroll, rotate or overwrite credentials.
  Shipped optional telemetry and profiling MUST NOT become mandatory startup
  dependencies or publicly exposed diagnostic endpoints.

- **FR-024**: Execute registered Linux profiles through the selected embedded runtime
  library, including lifecycle, signals, exit collection, bounded restart backoff
  and recovery. Prefer library integrations and compatible host dependencies, provisioning missing
  runtime components on demand; end users
  MUST NOT need Docker, Compose, make, language toolchains or a separate runtime.
- **FR-025**: Provision only authorized profile assets, verified against trusted
  immutable identities and platform compatibility, with atomic cache activation,
  cancellation safety, offline input and bounded eviction of unreferenced assets.
  Interactive first use requires consent; automation requires explicit permission
  or existing verified assets. Preserve trusted-source, platform and digest checks offline.
- **FR-026**: Support private image acquisition using isolated registry credentials
  and the registry's advertised authentication. Reject scope, source or redirect
  changes that could leak credentials; secrets MUST NOT appear in arguments or URLs.
- **FR-027**: Provide rootless service discovery/connectivity and high loopback host
  ports, keep internal ports private and isolate deployments. Preflight required
  host capabilities and clean only owned attachments and transient allocations.
- **FR-028**: Provide retained named local volumes with stable ownership, safe mount
  restrictions, disk exhaustion handling and backup/restore validation. Distinguish
  ephemeral memdb state from retained repository and persistent datastore contents.
- **FR-029**: Start the core readiness graph with private generated credentials,
  separate controller signing preparation/enrollment/launch and reported usable
  endpoints. Dependencies and health, rather than process existence, determine
  readiness; optional components use verified versioned mappings only.
- **FR-030**: Include Linux distribution/host and macOS backend feasibility
  experiments as implementation work. Measure executable, download, installed/cache
  footprint and cold/warm readiness separately. Publish support only for tested
  combinations. Verify digest-bound provenance before publishing artifacts produced
  by `gitstore-dev/cli`; upstream service images require trusted-source/platform/
  digest validation, without requiring upstream attestations.

- **FR-031**: Minimize core distribution size and avoid bundling extra executables.
  Prefer maintained libraries, reuse compatible validated host capabilities and
  provision missing selected-profile dependencies on demand with consent and
  offline support. Report separate core executable, required library, package,
  download/cache and memory footprints; a library preference MUST NOT conceal
  large static additions or imply a measured size result.

### Scope and dependency decisions

The [PRD](../../docs/prd.md), [command matrix](../../docs/contracts/command-matrix.md),
[dependency register](../../docs/contracts/dependencies.md) and ownership rules in
[ADR 0002](../../docs/ADRs/0002-runtime-ownership.md) remain authoritative. This scope
includes work to produce evidence for open internal gates; it does not close them
by specification or require full M1 completion first.

| Area / records | Implementation included | Activation/support condition |
| --- | --- | --- |
| DEP-008; CMD-005, CMD-054–057 | Per-user daemon, validation, inspect/list, stop and logs; durable operations and recovery | Accepted protocol plus DEP-012 executable verification |
| CMD-046–049 | Core profile and API/Git/controller component startup through the same daemon | Verified images, configuration, enrollment, readiness, rootless host capabilities and ownership |
| CMD-050–052 | Optional OIDC/Admin/Storefront integration where published image/config/readiness mappings are verified | Gate each unavailable component independently; do not invent images or endpoints |
| CMD-053 | Tracked Scylla migration using shipped helper, compatibility/backup checks and exclusive ownership | Verify exact helper/image version, target and datastore requirements |
| DEP-013; CMD-083 | Embedded runtime delivery, maintained image pipeline, private registry credentials, online/offline source/digest verification and CLI release provenance | EXP-003A/B/C; registry syntax requires an explicit contract before exposure |
| DEP-014 | Rootless networking and retained local volumes | EXP-004A and local-volume part of EXP-004B; CSI remains M3 |
| DEP-015 | Linux host validation plus Apple/managed-VM feasibility experiments | Host availability bounds evidence; activate only backend/host combinations that pass |
| Other milestones | Preserve existing M1 functionality and its pending gates | No new remote auth/resource scope, File payloads, CSI, scheduler, cloud provisioning or Windows runtime |

Release-specific inputs are recorded in [release-evidence.md](release-evidence.md).
M2 may use shipped credential/configuration/migration helpers without implementing
missing remote CLI login/discovery features. Runtime identity bootstrap remains
separate from the user's saved remote account. Production execution is delivered
incrementally as prerequisites pass; a missing optional backend must not block Linux.
No M0 row changes to implemented or supported until its evidence is recorded.

### Key Entities

- **Local manager identity**: Invoking OS user and owned control/state locations;
  independent of remote installation accounts.
- **Registered profile**: Versioned allowed component/datastore selection with
  required assets, host prerequisites and readiness dependencies.
- **Validation plan**: Effective non-secret selections, origins, checks performed,
  missing requirements and provisioning needs; authorizes no execution itself.
- **Deployment**: Stable local identity, desired and observed generation, components,
  readiness, recovery state and owned retained resources.
- **Operation**: Durable caller/method/key identity, normalized intent fingerprint,
  accepted/updated times, state, cancellation relationship and safe result/error.
- **Ownership record**: Deployment/resource relationship, lifecycle stage and proof
  needed before repository storage can be released or reused.
- **Observation subscription**: Bounded deployment/component logs or events with
  separate identity, ordered observations and explicit replay/gap status.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Every included preflight scenario reports performed and unavailable
  checks accurately, with zero downloads, workload starts or configuration changes.
- **SC-002**: All list/inspect/wait acceptance cases return the selected identities
  and truthful readiness/recovery states; no accepted or stale-ready record is
  reported as a currently ready deployment.
- **SC-003**: At each tested interruption boundary around acceptance, execution,
  stop and restart, replay returns at most one operation for the same intent and
  no accepted operation disappears from retained history.
- **SC-004**: All concurrent-start, writer-replacement and uncertain-recovery cases
  permit at most one writer per repository volume; retained test data is unchanged
  after stop, failed startup and repeated cleanup.
- **SC-005**: Every unauthorized, malformed, over-limit, unsafe-cancellation and
  slow-observer case produces the documented bounded failure; no exercised case
  exposes a synthetic secret, crashes the manager or cancels unrelated work.
- **SC-006**: Automation completes all included observation workflows without
  prompts, and all existing remote workflows remain usable without a local manager.
- **SC-007**: Every delivered command/capability has an evidence record naming its
  checks and limitations; zero real-service, host or release-support claims rely
  solely on controlled test workloads.
- **SC-008**: Every included version-specific configuration and bootstrap validation
  case identifies the selected release and missing prerequisites accurately, with
  zero credential generation, enrollment, rotation or unintended diagnostic exposure.

- **SC-009**: On each claimed Linux host/architecture, a clean installation runs
  the core profile as an ordinary user using only GitStore and documented host
  capabilities; all six real-start acceptance scenarios pass.
- **SC-010**: Each private-registry, wrong-platform, interrupted/tampered/offline
  asset and consent case either activates the authorized verified assets or fails
  before activation, with zero exposed synthetic credentials.
- **SC-011**: Real service stop/restart, migration and manager-crash cases preserve
  all retained test contents and allow at most one repository writer. Failed
  migrations block dependent startup in every tested failure case.
- **SC-012**: Every runtime/network/image/platform experiment records pass, fail or
  unavailable with pinned inputs and observations; size/readiness budgets are
  established from measured builds, and unsupported combinations stay explicit.

- **SC-013**: Each stripped release executable targets at most 5 MiB as the initial
  engineering budget for the requested kilobyte-to-low-megabyte size range; report
  measured bytes and any miss explicitly. Host-present, host-incompatible,
  missing-dependency, denied-download and offline cases resolve consistently, and
  no unused runtime dependency is downloaded by help or remote commands.

## Assumptions

- “Unblocked M2” means implementing M2 where external contracts exist and doing
  the internal integration/selection/validation work needed to enable it. The
  earlier foundation-only restriction is superseded by the user's clarification.
  The existing feature directory remains stable for downstream commands.
- The release review supplements, rather than silently replaces, the M0/M1
  evidence revision `b6c866421dbcf43994565f847553c1f6aec83994`. Source-reviewed
  shipped behavior and release-note claims are distinguished in the review;
  neither establishes live compatibility with this CLI. Future profile execution
  must pin image digests and validate trusted sources independently of the release
  tag. Attestations apply to CLI-published artifacts, not upstream service images.
- M1 has the development subset recorded in [M1 status](../../docs/m1-status.md);
  its completed implementation tasks do not close outstanding upstream/release
  gates. Finishing those remote features is not a prerequisite for M2.
- The existing [protocol](../../docs/contracts/daemon-protocol.md) supplies exact
  methods, types, numeric bounds and errors. This specification reuses it without
  introducing new commands, a general execution interface or a second network API.
- The constitution's implementation language and client/runtime boundaries remain
  binding planning constraints. This document selects no new storage, parser,
  execution, image or networking dependency.
- Developers may use isolated controlled workloads for safety verification; users
  cannot opt into them as a production execution backend. Empty production state
  is expected until a real backend is separately activated.
- Control-interface portability must be verified on each claimed host. Passing
  local control checks on macOS does not establish Linux-container execution there.
- [ADR 0003](../../docs/ADRs/0003-runtime-assets-packaging.md),
  [ADR 0004](../../docs/ADRs/0004-rootless-network-volumes.md) and
  [ADR 0005](../../docs/ADRs/0005-platform-backends.md) govern runtime activation.
  Their M2 experiments and CLI publication checks are included in the implementation plan; they remain
  not-run until executed and recorded. Planning performs none of them.
