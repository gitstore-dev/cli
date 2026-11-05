# M2 Data Model

**Status**: Implementation model. Core records, acceptance/reservation and bounded
observation have development implementations; assets, workloads and persistent
service maintenance remain planned. Wire representation remains M0 protocol 1.0.
All state is per invoking UID, owner-only, and independent of remote context selection.

## Persistent entities

| Entity | Key / important fields | Relationships and constraints |
| --- | --- | --- |
| DaemonStore | schemaVersion, ownerUid, storeId, lastRecoveryAt | One lifetime lock/control owner; version mismatch/corruption blocks mutations; no unsafe automatic schema downgrade |
| Profile | profileId + profileVersion, serverRevision, manifestDigest, platform, component/datastore enums, readiness graph, config sources, registered targets | Trusted installed metadata; no caller paths, arbitrary mounts/entrypoints or mutable asset tags; exact immutable profile content |
| Deployment | deploymentId, profile FK, desiredState, generation, observedGeneration, readiness, recovery, observedAt, lastOperationId | Components, resources and retained volumes belong to deployment; readiness applies only to current generation |
| Operation | operationId, uid, method, deploymentId?, targetOperationId?, intentFingerprint, normalized non-secret intent, acceptedAt/updatedAt, state, recovery, result/error | State accepted/running/succeeded/failed/cancelled; terminal result/error only; no raw credentials |
| DedupeRecord | uid + method + idempotencyKey, fingerprint, operation FK, expiresAfter | Unique composite identity; active and deployment-start identities not evicted; terminal retention at least seven days |
| Component | deploymentId + component, backendId, immutable imageDigest, desired/observed state, observedAt, generation | Registered component only; runtime identity is not just PID; readiness result separated from process state |
| ResourceOwnership | canonical resourceId, resourceType, deploymentId, operationId, lifecycleStage, backend identity, ownershipState, observedAt | Reserved before side effects; recorded owner required for cleanup; uncertainty cannot be released from PID absence |
| Volume | volumeId, ownerUid, deploymentId, role, canonical local identity, retention, compatibility marker, ownership FK | Named profile volume, no user-selected host mount; one repository writer per canonical volume identity |
| Network | networkId, deploymentId, namespace/backend identity, helper identity, port allocations, attachment stages | Per-deployment isolation; owned reverse cleanup; internal endpoints not publicly bound |
| Asset | digest + platform + kind, trusted manifest/source, integrity policy, staging/verified/active state, bytes, references | Content-addressed cache; active only after trusted-source/platform/digest verification; refcount prevents live asset eviction |
| DependencyResolution | capability, implementation, origin code/host/cache or unresolved, path identity for external components, ABI/version, platform, compatibility result, digest/build identity where available | Prefer declared code dependencies; static/dynamic linking follows measured tradeoffs; normal code dependencies are build inputs, never runtime plugin discovery; external host/cache compatibility requires a registered verifier and actual identity evidence; missing/incompatible external runtime dependencies require authorized provisioning, never silent fallback after side effects |
| Migration | operationId FK, deployment/datastore identity, registered target, prior/target compatibility, backupAcknowledged, stage, outcome | Exclusive against workload mutations and datastore migration mechanism; failure blocks dependent startup |
| LogSegment | deploymentId + component + segmentId, sequence range, bytes, createdAt, expiry | Bounded retention; redacted on ingestion; cursor expiry explicit; metadata retention is independent |

The profile's `scylladb` wire enum maps to upstream `scylla` configuration. Optional
OIDC/Admin/Storefront profiles exist only after verified mappings. Secrets are
references to a credential broker or private runtime files, never database payloads.

## Ephemeral entities

- **Connection**: authenticated peer UID, selected version, active request IDs,
  deadlines and capability requirements. Close subscriptions on disconnect, not work.
- **Subscription**: connection ownership, subscription ID, selected deployment/component,
  ordered event sequence, opaque replay cursor and bounded queue accounting.
- **Worker job**: operation identity, stage, bounded resolved plan and private IPC;
  parent alone commits outcomes. Never trust arbitrary worker paths/commands.
- **Provisioning consent**: approved profile version/manifest digest/source/asset
  set. `allowProvision` authorizes only that immutable plan; change requires renewed
  validation. Offline bundle location is owner-controlled configuration, not wire input.

## Atomic acceptance and recovery

1. Authenticate, negotiate, validate all params and resolve immutable profile/defaults.
2. Canonicalize typed non-secret parameters (sorted UTF-8 keys, explicit defaults,
   integer decimals; array order significant). Hash SHA-256; omit requestId, timeout
   and negotiation version. Apply protocol semantic identity exactly.
3. In one short transaction: check same-key replay/conflict, quotas and deployment
   conflict; record operation/dedupe, generation and resource reservations/job intent.
4. Commit durably before acknowledgment and dispatch. Scheduling capacity must be
   reservable durably; reject new intent before side effects when full.
5. Execute outside the transaction. Persist stage/ownership before new allocation
   and reconcile after each failure. Durable reservations are not proof that an
   old workload terminated; observed backend identity still gates materialization.
6. On daemon restart enter reconciling; discover real workloads/attachments and
   recover each active intent. Use uncertain when proof is unavailable. Do not
   manufacture a terminal result to unblock a new writer.

Filesystem lifetime/volume locks supplement the DB transaction. Surviving containers
can outlive the daemon lock, so reacquiring that lock alone never releases storage.
Database/journal capacity and disk errors must be bounded before acknowledgment.

## State transitions

- Operation: `accepted -> running -> succeeded | failed | cancelled`; cancellation
  before execution permits `accepted -> cancelled`. Terminal outcome is immutable.
- Recovery: `normal -> reconciling -> normal | uncertain`; resolution requires
  current observations, never an assumed success.
- Deployment: desired `running | stopped`; readiness `pending | ready | failed`.
  New accepted start increments generation before acknowledgment; ready requires
  observedGeneration == generation and all required readiness checks.
- Writer ownership: reserve/held before create; remain held/uncertain while stop
  cannot prove termination; release only after observed old-writer termination.
- Asset: `staging -> verified -> active`; invalid/cancelled staging never becomes
  active. Evict only unreferenced entries after owned cleanup.
- Migration: preconditions -> exclusive preparation -> execution -> verification ->
  succeeded/failed. Unsafe stages refuse cancellation; failure blocks startup and
  does not trigger downgrade. Re-execution follows the upstream migration ledger,
  not blind replay of unknown effects.

## Retention and validation

Use [M0 numeric policy](../../docs/contracts/daemon-protocol.md#numeric-resource-policy)
without weakening it: 256 MiB/100,000 operation records, >=7-day terminal retention,
active/deployment-start identity protection, 64 MiB/7-day per-deployment logs and
1 GiB global logs. Enforce all string, integer, depth, collection, queue and job
bounds before unbounded allocation. Refuse new work if no safe eviction exists.

Opaque cursors include sufficient store/generation/position identity to reject
foreign, stale and expired values. No stable snapshot guarantee beyond the accepted
protocol is invented. Backup/restore preserves schema and owner checks; a copied
state store never authorizes taking over an independently running deployment.

### Private observer leases and store upgrade

Daemon metadata schema version 2 adds `observer_slots` without changing the 1.0
wire protocol. At most two log replay readers share the eight runtime worker
slots with lifecycle and health work. A lease binds one immutable subscription
and deployment to a slot; SQL guards prevent collisions with lifecycle jobs.
Disconnect abandons observation, and the lease remains until the private child
has been reaped. Manager startup clears leases belonging to its previous owner
under the store lifetime lock; it does not change terminal operation outcomes.

An exact version-1 schema upgrades in one exclusive transaction. Owner checks,
operation history, deduplication keys and resource ownership survive. Damaged or
future schemas are rejected; no downgrade or partial repair is attempted. Log
segments have their own versioned database and collectors alone create/prune it;
replay opens it read-only and filters expired history without deleting it.


### Migration control metadata implementation

The private migration controller currently stores its bounded journal in one
`resource_ownership` configuration row per accepted migration. Its exact seven
fields are `kind=migration-control`, `targetVersion`, `targetDigest`, immutable
`sourceOperationId`, `generation`, `stage` and `unsafe`. It is control metadata,
not a filesystem receipt, native identity or resource takeover grant. Existing
repository/workload resources retain their independent ownership proofs.

Admission reserves this row in the same transaction as the operation/dedupe/job.
Preparation checks current private keyspace/compatibility/exclusivity/helper-exit
observations and commits the prepared stage atomically; generic transitions
cannot bypass preflight. Dispatch commits `unsafe=true` before effects. Success
requires verified schema and observed cleanup; uncertain cleanup retains an
uncertain owner. A failed/incomplete latest migration blocks new START admission.
Same-key replay returns the original operation even if later observations fail.
Public native migration dispatch remains gated until the persistent graph and
its actual resource owners are implemented.


### Datastore volume ledger implementation

Metadata schema version 3 adds the separate `datastore_volumes` table, with the
same immutable store/root/producer/allocated inode receipt boundaries as retained
repository volumes. Its role is `scylladb` and its fixed compatibility is
`scylla-2026.1:gitstore-06152d-schema-012`. A reserved datastore resource must
belong to an accepted/running Scylla START before initial allocation. Repository
and datastore records must share the canonical store/root and cannot alias an
allocated volume, data directory or writer-lock identity. Allocated receipts and
retained records cannot be rewritten or removed.

The filesystem marker also distinguishes Scylla compatibility from the historical
repository marker; neither role can reopen the other's directory. The actual
writer lock survives through the workload's inherited descriptor. Recovery opens
read-only observation handles without a writer grant; a free flock alone never
proves an old workload terminated.

Exact version 1 and 2 stores upgrade inside the existing exclusive transaction.
Their schema/constraints/triggers are checked before adding version 3; unknown or
damaged stores are refused. Denied journal writes roll back the schema/version,
and terminal operation/dedupe/resource identity remains unchanged. These private
primitives do not activate the Scylla bootstrap graph or provide backup/restore.

Metadata schema version 4 preserves those tables and atomically replaces the
network attachment guard. Ordinary START keeps its existing ownership gate;
standalone MIGRATE additionally requires a stopped, normal, released deployment,
its current held preflight control and canonical network/Scylla/CQL/schema names.
The bounded innocuous `gstd_migration_preflight` SQLite function parses only the
control record; the native dispatcher separately validates the full policy and
kernel namespace identity. The guards remain within the connection's existing
expression-depth limit. Exact versions 1–3 upgrade transactionally; malformed
schemas and denied journal writes retain their original versions and rows.

The private migration schema actor derives observations from journaled,
image-native read-only helpers. It requires the existing GitStore keyspace,
accepts a missing ledger only within that keyspace, validates any existing
migration prefix, and dispatches the schema mutator once. A complete post-helper
ledger is required before the verified/cleanup stages. Bounded anonymous stdout
files avoid signalling a surviving helper through a closed pipe after manager
loss. Source shutdown commits a helper fence before TERM and records retained
termination only after held process death, empty cgroup and non-forced DELETE.
These actors do not yet enable the public migration capability.

Persistent bootstrap records additionally bind `persistent=true` and
`freshDatastore` to their generation. The private startup prefix derives freshness
from the reserved datastore ledger before its first allocation worker: an
unallocated receipt must belong to the current start producer; an allocated
receipt always selects the retained path. The graph cannot be initialized again
after effects. Fresh startup requires datastore readiness, keyspace creation and
migration before application launch; retained startup skips those mutations.
Unknown worker completion retains ownership and never grants replacement.

The full persistent manifest binds the implicit Scylla image and migration target
independently of the unchanged core application profile. Per-image cache
descriptors project application images to their existing identities, allowing
reuse between datastore choices. A Scylla-only cache descriptor is validated
against compiled image/profile definitions, and its reservation requires
an accepted persistent intent; a valid descriptor alone grants no provisioning.
Schema/helper policy metadata remains in the full execution manifest and is
excluded from the per-image descriptor, so changing a migration target does not
duplicate an unchanged Scylla image in the cache.

The private one-shot helper actor creates a paused registered helper, then waits
for the control owner to persist its native identity before permitting START.
Neither repeated polling nor observation commits may dispatch it implicitly.
Success requires normal exit(0), death of the held pidfd, an empty captured
cgroup and non-forced deletion. Cancellation/deadline retains uncertainty and
never signals the mutator, releases ownership or permits dependent startup.
Later observed cleanup is resource evidence; it cannot rewrite an already
recorded uncertain operation outcome. This actor alone grants no durable
operation, filesystem, image or datastore ownership.

Startup helpers have a seven-field `helperControl` journal: `purpose`, `attempt`,
`stage`, `unsafe`, `sourceDatastore`, `termination` and `result`. Dispatch precedes
CREATE; committing the paused native identity precedes START. Identity writes
require actual held kernel handles, recorded rootfs/bundle inodes and matching
held datastore/helper network attachments. A deleted helper has a terminal
native/cgroup/exit receipt before the graph can advance. Readiness retries are
bounded to 128 attempts; keyspace/schema mutations cannot be retried after an
unknown result. The parent coordinator withholds START after any failed identity
commit and retains ownership after failed deletion-result commits. Observer
closure changes neither durable ownership nor uncertain operation outcomes.

Persistent configuration and workload receipts additionally retain the complete
`executionManifest`. Owner reads compare it with the producing operation's
registered policy, including image/config digests and migration target/digest.
This binding is separate from reusable per-image cache identities. An absent or
changed policy fails before dispatch/graph advancement; a successful old helper
cannot authorize a different migration target after a catalogue update.

### Persistent observer restoration

Configuration graph and workload receipts bind the complete execution manifest.
Reopening requires the original store/generation and actual scope, volume,
namespace, process-birth and cgroup identities. Restoration acquires observation
handles, not a writer grant, readiness result or permission to repeat bootstrap.
The datastore service and both helper scopes are separate inventory entries.
Completed helpers require a committed deletion receipt and absence of the original
native instance/process; unresolved mutators preserve uncertainty.

Retained controller enrollment records `controllerIdentitySource` with the prior
successful operation, generation and UID. It must match the same execution policy
and retained datastore. It permits binding to a new actual API process; controller
authentication/readiness is still required. It does not authorize a fabricated
public service-account lookup or repeated creation.

### Standalone migration execution preparation

A private resolver reads the admitted migration under the owner's transaction.
It requires a stopped deployment, released prior resources, the latest source
start's retained configuration policy and the exact registered target digest.
The resolved target carries the migration operation/generation and a separate
immutable source operation; it does not inherit provisioning consent.

The migration datastore reservation requires an allocated canonical Scylla
ledger belonging to that released source. It preserves the original producer
and disk/data/writer identities, rejects missing/unallocated data and repository
roles, and never grants writer authority. Actual filesystem acquisition must
still win the original kernel writer lock. Helper dispatch and mutation stages
remain separate from this preparation.

Native service/helper CREATE receipts now additionally record `population` before
OCI START: schema version 1, creator boot, canonical cgroup path, cgroup device,
cgroup inode and `cgroup.events` inode. The fixed native PID/birth receipt remains
unchanged. Opening the recorded group uses cgroup2 type checks and bounded
`openat2` resolution; a replaced, missing, inaccessible or cross-boot group grants
no ownership or termination claim. Live restoration compares the reopened
population with the original receipt. Legacy receipts without population still
require the existing live PID/cgroup proof or a committed deletion receipt.

A stopped init can be adopted only when the canonical runtime reports the exact
recorded stopped PID/birth and the original recorded cgroup can still be reopened
and proved empty. Cleanup then performs non-forced DELETE and commits its normal
termination receipt; it does not signal a missing/reused PID. If systemd has
already removed the original group, its vanished pathname is insufficient and
ownership remains uncertain. This path does not infer helper success from loss
of its exit status, retry a mutator or resume an incomplete migration.
