# M0 Documentation and Operation Model

These are documentation records and proposed runtime concepts, not database tables.
No persistence engine or application schema is implemented in M0.

## Command Contract

| Field | Meaning / validation |
| --- | --- |
| id | Unique stable CMD identifier; referenced by dependencies and acceptance evidence. |
| command | Canonical grammar; one resource/operation per record. |
| purpose, scope, selector | User outcome, installation/namespace/deployment scope, identity resolution. |
| owner, transport | Responsible component; local client, GraphQL, Git, daemon, or gated payload transport. |
| input, output | Accepted representation, defaults, applicable formats and restrictions. |
| auth, concurrency | Advertised credential mechanism, permissions and conflict tokens where exposed. |
| completion | Acceptance/admission/reconciliation meanings and supported waits. |
| status | supported-contract, blocked, deferred. Never implies CLI implementation. |
| milestone | M1, M2 or M3 target; M0 describes their contracts. |
| evidence_refs | One or more Evidence IDs, or a statement that public evidence is missing. |
| dependency_refs | Missing capabilities, responsible roles and gates. Required for blocked entries. |

Deferred entries record a scope reason even if evidence exists. Local-only commands
reference an accepted client design rather than pretend to have a server operation.
Daemon-only commands reference protocol design and remain unimplemented until M2.

## Evidence

Fields: `id`, repository/source, relative path and symbol/operation, commit or source
version, reviewed date, working-tree differences, observed guarantee, limitations,
and evidence level (`requirement`, `source-reviewed`, `live-validated`). A source-reviewed
record cannot claim live-validated behavior. Dirty source claims require a reproducible
diff/content snapshot reference before final publication or must be downgraded to a
prerequisite for verification. Do not publish secret-bearing configuration snapshots.

Relationships: many commands may reference one Evidence record; one command may need
several records to establish schema, implementation, authorization, and completion.

## Dependency

Fields: unique `id`, missing guarantee, affected Command IDs, responsible component/role,
tracking reference when known, milestone, resolution gate, expected unsupported behavior,
and status (`open`, `resolved`, `superseded`). Resolution requires linked evidence and
review of affected command classifications. An issue marked closed alone is insufficient.

## Decision Record

Fields: stable number/title, status (`proposed`, `accepted`, `superseded`), date, context,
decision, alternatives, consequences, evidence, feasibility gaps, validation gate,
responsible role and linked commands/dependencies. Accepted design does not mean tested
implementation. Superseded records link their replacement and retain historical rationale.

## Acceptance Evidence

Fields: scenario ID, FR/SC references, reviewed artifacts, preconditions, action,
expected observations, actual result (`not-run`, `pass`, `fail`), review date, reviewer
role and open findings. Planned expectations must not be prefilled as passed results.

## Proposed Daemon Operation

Fields: operation ID, caller identity, operation kind, deployment ID, idempotency key,
canonical request fingerprint, accepted/updated timestamps, state, result/error,
resource ownership, recovery observations and event cursor.

State transitions: `accepted → running → succeeded | failed | cancelled`.
Restart reconciliation is an observation process, not automatic success or failure.
A running operation can remain uncertain until backend ownership is established;
inspection must report that uncertainty and prevent unsafe replacement.

Persist intent and deduplication identity before acknowledging acceptance. A retry with
the same caller/kind/key and same semantic parameters returns the original operation;
different parameters conflict. A request ID identifies one exchange and is not a durable
deduplication key. Wait timeout affects observation, not detached work.

## Proposed Deployment

Fields: deployment ID, profile/version, desired state, component workload identities,
observed readiness, volume identities, network identities, Git writer lock/ownership,
last operation, and recovery status. Operation success and deployment readiness are
separate fields. Stop retains volume identity. Replacement requires observed prior-writer
termination and exclusive storage ownership, including after daemon restart.

## Relationship Summary

- Commands reference Evidence, Dependencies, and Decisions.
- Acceptance Evidence covers requirements using those records.
- Deployment has many Operations and resources; one writer owns each repository volume.
- Operations reference Deployment and preserve caller/deduplication identities across retries.
- Publication must contain no orphan command, dependency, decision or acceptance references.
