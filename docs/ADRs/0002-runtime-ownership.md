# ADR 0002: Profile Runtime Ownership and Recovery

**Status**: accepted (design). **Date**: 2026-10-27.
**Owner**: CLI/daemon maintainers. **Evidence**: EVD-001, EVD-002, EVD-022, EVD-023.
**Dependencies**: DEP-008 (protocol design), DEP-012 (implementation), DEP-013–DEP-015.

## Context

The client must operate GitStore services without becoming a general container engine
or replacing the existing Rust Git service. Losing control of detached work or replacing
a writer while it is alive risks retained repository data. Process existence is not
application readiness, and a shared filesystem does not provide Git high availability.

## Decision

`gs` is Rust: command UX, local context/credentials, public GraphQL/Git clients and
local socket client. `gitstored` is C: per-user profile-scoped images, execution, networks,
volumes, supervision, logs and operation recovery. GitStore API/controllers retain
authentication, authorization, admission, hydration and reconciliation. The server Git
service retains bare repositories and internal gRPC. Remote `gs` does not need a daemon.

On Linux the daemon calls libcrun's C library API through an internal backend boundary;
it does not invoke an external or bundled crun executable. The backend must define C
allocation/free ownership, callback lifetimes, process/fork/thread/signal behavior and
crash recovery; packaging is ADR-0003. No application-level copy of container isolation.

Persist the following before returning accepted mutation identity:

- Operation ID, caller UID, method, canonical intent fingerprint/key, deployment ID,
  accepted/updated timestamps, state, known result/error, ownership and recovery status.
- Deployment ID, profile/version, desired state, workload backend identities, observed
  generation/readiness, retained volume/network identities and writer lock identity.
- Resource owner and lifecycle stage sufficient to resume or safely undo only newly
  allocated transient resources. Never persist raw bearer/password material in intent.

The persistent representation is an M2 implementation choice. It must atomically commit
intent/dedupe/ownership, survive process crash, reject unsafe corruption and provide the
retention/quota rules in the [protocol](../contracts/daemon-protocol.md). An in-memory
map or daemon PID alone cannot satisfy it.

Lock by deployment and repository-volume identity, not only process name. Observe old
backend writer termination before releasing the storage lock or starting its replacement.
Same-key start returns the original operation; changed semantic input conflicts. Different
keys cannot bypass storage ownership. Missing/ambiguous ownership after restart fails
closed. `accepted → running → succeeded | failed | cancelled` describes operation state;
recovery uncertainty remains explicit rather than manufacturing a terminal outcome.

Operation success and deployment readiness are separate fields. A start completes only
after the selected readiness graph reaches ready or records failure. Detached start may
return earlier with durable accepted identity and pending readiness. API/Git mutual
callouts need explicit staged readiness; startup must generate local credentials/signing
material and enroll the controller through existing supported helpers. An ephemeral memdb
datastore and retained repository volume must be described separately.

Disconnect never implicitly cancels detached work. Foreground interruption requests
cancellation explicitly, then observes owned cleanup. Stop waits for observed termination,
retains volumes, and records timeout/uncertainty instead of releasing locks prematurely.
Restart discovers actual workloads before new work; cancellation/cleanup is retry-safe
and restricted to recorded owners. Backoff is bounded by the operation policy.

Migration is an exclusive deployment/datastore operation with explicit target/compatibility
and backup acknowledgment. Block dependent startup while required migration is incomplete
or failed. Unsafe cancellation is refused; failure does not trigger automatic downgrade.
Existing gitctl/config/enrollment semantics are input evidence, not a second unmanaged
process tree or a requirement for users to install Go/make/Compose.

## Alternatives

- Direct CLI-owned child trees: rejected because logs, ownership and recovery must outlive
  the initiating CLI process and component commands must share one owner.
- Replace server Git service with gitstored: rejected; repository semantics and authenticated
  server protocol are separate from profile container execution.
- Bundled crun executable: rejected as the default execution integration; it does not meet
  the selected C-library boundary. A helper requires concrete fault-isolation evidence.
- Assume a stale PID means dead writer: rejected; backend state and exclusive storage
  ownership must prove non-overlap. Shared volumes do not waive the invariant.

## Consequences and feasibility limits

Native libcrun faults can affect the daemon. Memory/process ownership, worker concurrency,
backend discovery and lock recovery are implementation risks, not tested guarantees.
No Linux/macOS runtime was started for this ADR. Store technology and library bindings
must be chosen against this contract during M2; they cannot weaken durable acceptance.

## Validation gate

DEP-012 requires C/Rust wire interoperability, C address/undefined-behavior sanitizers,
parser fuzz/resource bounds, credential redaction and peer authorization tests. Inject
failure at image/network/volume/start/readiness/migration/acknowledgment/restart stages;
demonstrate retained data, inspectable operations, bounded retries and zero overlapping
writers. Paper walkthroughs in [acceptance](../contracts/acceptance.md) establish design
coverage only. Runtime maintainers own this gate before any daemon release claim.
