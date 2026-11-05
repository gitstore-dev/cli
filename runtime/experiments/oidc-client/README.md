# OIDC implementation validation

T050 remains incomplete. The private Linux arm64 building blocks below are
implemented and tested. Public `gs deployment start --components oidc` remains
gated until the managed lifecycle, API issuer integration and recovery are
connected. These checks do not establish amd64 or macOS provider support.

## Contracts and protected enrollment

The consumer [specification](../../../postman/specs/oidc-provider.openapi.json)
and [collection](../../../postman/collections/oidc-provider/README.md) follow
Hydra 2.3.0 revision `ee8c339ddada3a42529c0416897abc32bad03bbb`.
The source [handler](https://github.com/ory/hydra/blob/ee8c339ddada3a42529c0416897abc32bad03bbb/client/handler.go)
returns sensitive creation fields; they stay inside the private libcurl worker.
A matching public GET cannot acknowledge a lost POST reply. The
[journal receipt](enrollment-journal-evidence.json) verifies a single actual
creation attempt and retained uncertainty after a deliberately dropped reply.
The parent-ledger transitions are implemented and tested separately; the public
lifecycle actor is not yet connected to them.

The [controlled HTTP receipt](evidence.json) covers 19 worker cases, including
malformed/oversized replies, mismatched fields, redirects, proxies, timeouts and
connection loss after simulated creation. No mutation is retried. The maintainer
Postman CLI is neither bundled nor a product dependency. Syntax-only OpenAPI lint
and local collection checks pass; cloud governance is unverified while signed out.

To reproduce the controlled HTTP checks, build Linux with image/native/bootstrap
test options and sanitizers. Supply a protected fixture JSON with a dedicated
account, owned `fixtureParent`, `oidcHttpProbe` and `postman` executable paths:

```sh
python3 runtime/experiments/oidc-client/run.py --fixture /absolute/owned/fixture.json
```

The driver uses synthetic material, isolated settings and a guarded local mock;
it rejects an occupied fixture port. Callback policy permits a canonical numeric
loopback address, unprivileged port and `/callback`. This does not implement CLI
login or a browser/device flow.

## Native services and retained ownership

- [SQLite migrations](sqlite-evidence.json): pinned Hydra and Kratos image-native
  migrations in separate rootless libcrun workers; retained databases pass
  read-only integrity checks. No Postgres dependency is inferred.
- [Provider serving](serve-evidence.json): generated protected configuration,
  native readiness and observed shutdown. Kratos readiness uses the source-pinned
  `/admin/health/ready`; redirects are not followed.
- [Combined graph](combined-graph-evidence.json): all five services in one owned
  namespace, actual client creation and reverse-order deletion. The UI uses its
  image-native shell to forward TERM and reap Node; no init binary is bundled.
- [Courier](courier-evidence.json): real synthetic Kratos verification mail reaches
  Mailpit. Eight local Postman requests and five scripted assertion blocks pass.
  Protected provider responses and mail contents stay inside the fixture.
- [Asset planning](asset-plan-evidence.json): production immutable asset planning
  accepts each fixed provider image using cached inputs. Execution copies are
  audited and retired between image checks.
- [Parent preparation](parent-preparation-evidence.json): the actual asynchronous
  parent pipeline commits intent before private configuration/volume work and
  public receipts afterward. Two generations preserve the secret family and
  provider producers/inodes; cancellation retains owners and leaves its marker
  unconsumed. Parent journal policy checks also cover STOP racing an acknowledged
  creation. No service readiness is inferred from completed preparation.
- [Provider volumes](provider-volume-evidence.json): separate fixed provider
  formats, immutable producer/inode receipts, writer exclusion, read-only recovery
  observation and atomic exact-schema upgrades from V1–V4 to V5.
- [Network ownership](network-owner-evidence.json): five fixed public forwards,
  retained ports, bind-conflict refusal and lost-ack recovery, including standalone
  and combined core/2 sanitizer checks. Provider admin APIs and SMTP stay private.
- [Protected preparation](preparation-evidence.json): separate
  `oidc-provider-material` broker purpose and explicit owner source policy.
  Reuse preserves the entire secret family, including the unmounted client secret;
  missing or replaced material cannot authorize regeneration. Only four read-only
  configuration FDs return to the parent. Metadata receipts record file identities
  and fixed ports without reading secret bytes. A later generation pins the original
  completed preparation before read-only reopening; incomplete or changed sources
  fail. Asynchronous worker boundary checks cover cancellation, crashes, deadlines
  and unavailable brokers. Controlled Postman mock requests through the asynchronous
  wrapper also pass normally and under sanitizers: readiness, acknowledged creation,
  public inspection and accepted creation followed by connection loss. No mutation
  is retried or inferred from a public match. Actual-provider execution through this
  wrapper remains unverified.

Private core/2 composition preserves core/1 image cache identities and includes
OIDC only when selected. Standalone OIDC selects five provider services; it does
not implicitly start the core API or Git services. These catalogue and ownership
primitives do not enable the public lifecycle.

Maintained C fixtures live under `daemon/tests/oidc_*`. This directory contains
small validation drivers and receipts. Downloaded tools, images, generated
executables, credentials and provider databases are not tracked in Git. Completed
execution copies are removed only after ownership and native termination checks;
retained databases, credentials, cache inputs and evidence are preserved.

The protected preparation probe also supports the controlled asynchronous HTTP
check without acquiring service images:

```sh
oidc_preparation_probe /absolute/empty-owned-root /absolute/installed-gs /absolute/postman /absolute/owned/oidc-hydra-mock
```

Use a mode-0700 empty fixture root and an ordinary single-link installed copy of
`gs`; Cargo's hardlinked build artifact is refused. The maintainer mock uses the
registered UID-10000 namespace mapping and isolated HOME. It receives only
synthetic fixture requests; its raw console output is discarded. UID-0 mock
startup failures remain recorded and do not establish an OCI provider limitation.
