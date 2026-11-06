# M2 Daemon Implementation Contract

The canonical external wire contract is [gitstored protocol 1.0](../../../docs/contracts/daemon-protocol.md).
This artifact binds implementation and verification to it; it introduces no HTTP,
OpenAPI, catalogue CRUD, arbitrary exec endpoint or new wire method.

## Boundary and lifecycle

- User runtime directory mode 0700, socket 0600; peer UID matches effective user.
  Reject symlink substitutions, foreign socket/state owners and unsafe stale paths.
- Linux uses the documented user runtime directory; portable control tests use an
  explicitly owned temporary location. Production state/cache follow user-scoped
  platform directories. Tests always override all locations.
- gs starts the matching installed daemon on demand for serve, with sanitized
  environment and inherited private readiness channel; do not connect to an
  arbitrary executable supplied in remote context configuration. A lifetime lock
  prevents concurrent bootstrap. Other commands report manager unavailability;
  local preflight may initialize control state but never provision workloads.
- Detached workloads survive CLI exit and daemon restart. User-service packaging
  documents manager startup, shutdown and login/session persistence; do not assume
  user lingering/cgroup delegation exists or silently install privileged settings.
- Daemon shutdown observes the 30-second bound and preserves work/ownership for
  recovery. It must not report container termination solely from manager exit.

## Required implementation surface

Implement each canonical method with typed params and declared capability:
`system.negotiate`, `system.capabilities`, `deployment.validate/start/inspect/list/stop`,
`operation.inspect/wait/cancel`, `migration.start`, `stream.subscribe/unsubscribe`.
Advertised capability means the build has the implementation. Per-profile/host
preconditions still apply and give exact errors. An absent optional profile does
not globally disable implemented Linux/control capabilities.

Use four-byte big-endian lengths and strict bounded JSON; responses and events
have separate correlation identities. Unknown optional envelope fields may be
ignored as M0 allows; unknown params and duplicate keys are rejected. Partial
reads/writes and out-of-order responses must remain correct. Preserve all canonical
error codes, stages, retryability and CLI exit mappings without inventing success.

## Persistence and cancellation

[Data model](../data-model.md) defines atomic acceptance and state transitions.
No external runtime call holds the SQLite transaction. Parent control processing
handles observation/cancellation independently of execution capacity. Distinct-key
start/stop/migrate cannot bypass per-deployment serialization or storage ownership.

A wait timeout/disconnect never cancels accepted detached work. A lost acknowledgment
is resolved through same-key replay. A cancellation is its own durable operation;
terminal targets return no-op state, unsafe migration stages return
`UNSAFE_CANCELLATION`. New generation readiness cannot reuse old observations.

## Testability and security

Controlled backends/failpoints compile only into test builds. Normal profile
registration/capabilities cannot expose them. Test through real socket and SQLite
processes; compare C and Rust on all M0 examples and malformed/boundary vectors.

Logs redact known credential material across chunks before persistence. Bound helper
stdout/stderr, event queues and replay. A stream overflow closes only the affected
subscription, returns safe gap guidance where possible and preserves work/history.

Protocol tests must prove each numeric policy bound and cleanup deadline. Runtime
crash tests additionally prove actual backend/workload recovery; controlled tests
alone cannot close DEP-012. See [validation guide](../quickstart.md).

### Interrupted migration cleanup

A separate `deployment.stop` may reconcile a failed, uncertain migration for the
same deployment. It must reopen the recorded native identities, cgroup population
receipts, volume locks and network owner before cleanup. It never starts a workload
or retries the schema helper. A running migration helper must terminate naturally;
unknown or inaccessible ownership remains uncertain. Successful stop releases only
observed owners and preserves the original migration state, result and error.

Queued migration intent can be cancelled only before execution starts and while
its sole owned resource is the undispatched migration control row. Running
migration cancellation currently returns `UNSAFE_CANCELLATION`, including
preparation: the manager does not promise safe cleanup of an in-flight native
worker. Terminal migration cancellation is an independent no-op.

A successful `migration.start` inspection/wait result exposes
`migrationCompleted: true`, the registered `targetVersion` and its matching
`targetDigest`. These are projected only from a successful operation and a
matching compiled target; arbitrary persisted backend fields stay private.

### Bounded restart supervision

A previously ready current generation may restart after its manager observes an
owned native workload or network worker exit. Failed health probes alone are not
permission to replace a live writer. Recovered/uncertain actors never enter this
automatic path. A separately accepted idempotent stop must complete observed
cleanup and release every owner before a new start can be accepted. Both
operations remain visible through ordinary operation inspection; prior terminal
results never change. New starts reuse the original registered intent and existing
verified assets with provisioning disabled.

The manager permits four replacements per manager-owned chain with
250/500/1000/2000 ms delays after observed cleanup, without sleeping its control
loop. Native stop/start deadlines remain independent of the accumulated delay
budget. A failed/uncertain stop, failed replacement start,
exhausted retry budget, newer operator intent or manager loss ends automatic
retries. Recovery retains normal ownership checks and requires explicit operator
stop/start; it does not replay an unobserved automatic replacement. No new wire
method, arbitrary runtime selection or migration retry is introduced.

### OIDC service log selectors

The optional runtime keeps `oidc` as the aggregate log selector and adds the five
fixed service selectors `oidc-smtp`, `oidc-hydra`, `oidc-kratos`, `oidc-ui` and
`oidc-bridge`. Aggregate replay/subscriptions include those five roles; each
record retains its precise service component. Unknown selectors remain invalid.
The private collector has at most nine service inputs (core three, Scylla one,
OIDC five), with the existing per-deployment/global storage and stream bounds.
It reads the complete OIDC material family under its separate broker purpose,
including the client secret, before ingesting any provider output. Standalone
OIDC does not read bootstrap credentials. Serving also requires `deployment.oidc` and the owned runtime proofs.

#### Combined OIDC API configuration

The core/2 coordinator supplies the selected loopback issuer port and the
same public client ID/audience used by the enrollment journal to the isolated core
configuration worker. The worker adds `oidc-jwt` before `anonymous` in the existing
authentication chain and writes `[api.auth.oidc_jwt]` with `issuer_uri` equal to
`http://127.0.0.1:<selected issuer port>/`, `client_id`, and `audience`. No client
secret is mounted in the API. The immutable configuration owner receipt binds
these three public values; changed ports or client mappings reject reuse. The
validator input includes the same table once. Core/1 has no OIDC configuration.
This maps GitStore revision `06152d193e854bc643fa884daa8b9f30b0d75d0f`,
`gitstore-api/internal/config/config.go` and `internal/auth/provider/oidcjwt/provider.go`.
Configuration preparation alone is not discovery/JWKS readiness or public support.

For a current verified core/2 OIDC generation, deployment inspection projects the
owned loopback forwards as `oidcIssuer` (with a trailing slash), `oidcBridge`,
`identity`, `identityUi` and `mail` HTTP URLs. A combined selection also projects
`api` and `git`; a standalone selection has only the five provider URLs. All five
provider ports must be present, valid and distinct. Partial tuples, foreign hosts,
private/admin ports, unknown fields, unready or obsolete generations expose no
endpoint projection. These serving URLs imply no browser/device login command or
Admin/Storefront capability.

Core/2 selects the registered optional OIDC graph explicitly. `components:["oidc"]`
selects its five provider images without API/Git/controller; core selections close
API/Git dependencies, and adding `oidc` adds exactly the five providers. Core/1
keeps its original selection/digest/cache identities. Validation remains read-only
and reports immutable assets, configuration roles and unperformed startup stages.
No missing cache/input permits downloading without explicit provisioning consent.
A runtime-capable manager advertises `deployment.oidc` for clients that require this
mapping; older managers fail the capability check before submission. Admin and
Storefront keep their component-specific external prerequisites.


### Final provider readiness

Core/2 performs a bounded read-only provider/discovery/JWKS sweep after all selected
services start. Its 30-second window includes the readiness commit; it cannot
reuse provider health from before slow Scylla/API/controller startup. A stalled
live bridge keeps readiness pending until fresh checks succeed. Start admission
checks all unresolved owners and recovery state even without a repository volume.
