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
