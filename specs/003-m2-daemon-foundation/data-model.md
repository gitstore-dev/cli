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
