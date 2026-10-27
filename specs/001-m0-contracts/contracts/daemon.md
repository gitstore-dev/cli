# Daemon Protocol Design Baseline

**Status**: Proposed M0 design, not an implemented endpoint. M0 execution must accept
and publish a complete versioned contract and ADR before M2 implementation starts.

## Transport and representation

Use a per-user Unix stream socket with owner-only access and OS peer-identity checks.
No unauthenticated TCP listener. Frames contain a four-byte unsigned big-endian byte
length followed by one UTF-8 JSON object. Reject zero-length, oversized, truncated,
invalid UTF-8, duplicate-key or malformed frames. Check the frame bound before allocation.
Partial reads/writes must be supported; framing must not depend on socket packet boundaries.

JSON integer values used for sequences must be within the interoperable integer range;
identifiers are strings. Document null versus absent fields. Unknown optional envelope
fields may be ignored within a compatible version; unknown operations/capabilities and
unknown execution parameters fail rather than silently altering requested behavior.

The initial exchange negotiates protocol major/minor and capabilities. Incompatible
major versions fail before mutations. Minor-version extensions must be additive and
capability-gated. A client must not treat missing capability data as permission to guess.

## Typed envelopes

| Kind | Required fields and behavior |
| --- | --- |
| Request | version, request ID, operation, typed parameters; mutations include idempotency key; optional observation deadline. |
| Response | request ID, result or error exclusively; operation ID/state for durable mutations. |
| Error | code, stage, retryable, safe message; operation/deployment ID when known. |
| Event | subscription ID, operation/deployment ID, monotonically ordered sequence, type, bounded payload. |

Canonical operation coverage: version/capabilities, validate/plan, start, inspect/list,
stop, logs/events, migrate, operation inspection and explicit operation cancellation.
Exact method names and parameter schemas must be finalized in the published contract.
Every method declares authorization, capability, side effects, deadlines, idempotency,
completion semantics, and all error outcomes. No arbitrary host executable/mount/OCI-hook
or privileged-configuration escape hatch is permitted.

## Proposed resource policy for the M0 ADR

| Limit | Proposed default | Excess behavior |
| --- | --- | --- |
| Frame payload | 1 MiB | Reject before allocation; larger log output uses multiple frames. |
| JSON nesting | 32 levels | Reject input. |
| Single string | 256 KiB UTF-8 bytes | Reject input or chunk output before encoding. |
| Array items / object members | 4096 per collection | Reject input; paginate output. |
| Connections | 32 per user daemon | Reject additional connections. |
| Outstanding requests | 16 per connection | Return resource-limit error. |
| Active subscriptions | 8 per connection | Return resource-limit error. |
| Event queue | 256 events or 4 MiB per subscription, whichever first | Report overflow/gap and close subscription. |
| Logs | 64 MiB per deployment, maximum age 7 days | Evict oldest records; report cursor gaps. |
| Runtime workers / concurrent mutations | 8 workers per daemon / 1 mutation per deployment | Bounded queue; reject when full. |
| Pending worker jobs | 64 per daemon | Return resource-limit error. |
| Partial-frame / idle connection timeout | 10 seconds / 5 minutes | Close connection; detached operations continue. |
| Graceful stop / daemon shutdown deadline | 30 seconds each | Record timeout; do not assert writer termination. |

The protocol ADR must accept or explicitly revise each value with rationale. Limits
must be discoverable where clients need them and cannot be silently unlimited.
These values are design bounds, not benchmark results. M2 tests enforcement and tuning.

Slow consumers must receive an explicit overflow/gap result or be disconnected with a
recoverable inspection path. Do not promise infinite retention or resumable log replay.
Cursors outside retained history return a precise expiry/gap outcome. Logs/events use
separate subscription identity and cannot substitute for durable operation inspection.

## Mutation and recovery invariants

Persist accepted intent, operation ID, deduplication identity and ownership before
acknowledgment. Deduplicate by peer identity, operation kind and key using a canonical
semantic parameter fingerprint. Same key/same parameters returns the original operation;
same key/different parameters conflicts. Observation deadlines do not change that fingerprint.

Retain deduplication identity while its deployment or operation is active and for at
least seven days after terminal completion. Deployment-associated start identity remains
until deployment removal. After retention expiry, replay is not guaranteed; clients must
inspect deployment/operation identity before retrying. The ADR must define storage quotas
and safe refusal when durable history cannot be retained. Loss/expiry of history cannot
authorize duplicate workloads when ownership
is uncertain. Concurrent starts targeting the same repository storage must not create
multiple Git writers even when request keys differ.

Operation progression: accepted, running, then succeeded/failed/cancelled. Recovery
reconciles persisted intent against discovered workloads; uncertain outcomes remain
inspectable without asserting termination. Cancellation is explicit, capability-aware,
and may fail when rollback is unsafe; a wait deadline does not cancel detached work.

Stop observes writer termination before releasing ownership and preserves retained
volumes. Migration uses an exclusive lock, compatibility/backup preconditions and
inspectable failures; dependent startup remains blocked after failure, without downgrade.

## Required examples and gates

The published protocol must contain paired request/response examples for negotiation,
start acceptance, operation inspection, same-key replay, changed-payload conflict,
readiness failure, wait timeout, stop, migration, cancellation and log subscription.
Include unauthorized peer, incompatible version, unsupported capability, malformed/
oversized frame, queue exhaustion and expired-cursor examples. IDs, states and error
categories must agree with the [data model](../data-model.md) and [CLI policy](cli.md).

M0 validates example consistency and the six paper walkthroughs. M2 must prove C/Rust
round trips, bounded parsing, peer authorization, fuzz/sanitizer behavior, durable
deduplication and crash recovery. Selecting this design provides none of that evidence.
No parser library or persistence technology is selected in this planning artifact.
