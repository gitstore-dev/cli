# OIDC implementation validation

The owned OIDC lifecycle is implemented and publicly accepted on the disposable
Linux arm64 fixture. `gs oidc serve` selects five providers; combined core/2
supports memdb or retained Scylla. See [lifecycle evidence](lifecycle-evidence.json)
for production and sanitizer receipts, including final readiness, retained startup,
manager loss and owned stop. Earlier private milestones below are historical
building blocks; they do not independently establish platform/release support.

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

The [fixed health consumer contract](health-contract.md) records the narrow
per-role private readiness probes. It preserves Hydra's verified JSON predicate
and deliberately treats the other provider bodies as opaque.

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

## Earlier health and migration coordination checkpoint

Fixed-role health reads now run through `GSTD_BOOT_JOB_OIDC_HEALTH`. The worker
enters the already owned namespace, takes no configuration/broker capability,
limits response bytes and deadlines, rejects redirects/proxies, and returns only
a typed status. Hydra retains its existing JSON predicate and rejection semantics.
`bounded_oidc_role_health` covers routes, opaque bodies, failure statuses,
redirect traps, proxy environment, partial/oversized replies and timeouts.
`private_bootstrap_worker_boundary` includes health cancellation/crash/deadline
checks. The preparation probe now exercises all five roles through actual
asynchronous workers against controlled Postman listeners in the owned namespace.

The production helper owner also selects a separate OIDC migration journal.
It binds each single dispatch to its provider, canonical volume producer/data/
writer inodes, original generation, prepared execution and current owned network
attachment. Wrong-provider writers and repeat dispatches fail. Native identity
commits before START and observed exit/empty cgroup/deletion before completion.
A later generation cannot reinterpret a dispatched/uncertain journal as permission
to run again. Completed prior helpers require released owners, matching immutable
provider bindings, deletion receipts and native/process absence before retained
migration dispatch. Allocation freshness alone grants no mutation authority.

At that checkpoint, a fixed service-role catalogue defined provider-before-core start order,
reverse dependency stop order, health roles and provider-volume roles. Core begin
and recovery preparation derive names/counts from the immutable image manifest,
with bounded capacity for all eight service roles. Both architectures and
standalone/combined layouts are covered. That catalogue alone did not enable public dispatch.

These internal building blocks preceded the production lifecycle integration
described below; they did not alone qualify public dispatch.
The [follow-up receipt](coordination-evidence.json) separates controlled HTTP,
metadata policy, actual provider migrations and sanitizer results.

The setup probe supports two additional isolated modes:

```sh
oidc_setup_probe /absolute/empty-owned-root /absolute/installed-gs --migration-journal
oidc_setup_probe /absolute/empty-owned-root /absolute/installed-gs --migration-native /absolute/verified-inputs
```

The first mode uses synthetic execution directories and real owned network/volume
handles to test metadata policy; it never invokes native CREATE. The second needs
four cache-only materializations at `verified-inputs/{first,next}/{hydra,kratos}`,
each produced by `oidc_image_probe PROVIDER NEW_DIRECTORY VERIFIED_CACHE`. It moves
only these disposable rootfs copies into the journaled execution scopes, then runs
four actual image-native migrations through the production helper owner. Retain
inputs, journals and volumes on uncertainty; do not prune a failed fixture. Run
both ordinary GCC Release and Clang ASan/UBSan builds. The OIDC diagnostic plan
now mirrors the core plan's ASan address-space handling while retaining physical
cgroup/core limits; the ordinary plan keeps its finite 1 TiB address limit.

For local collection verification on the disposable Linux fixture:

```sh
python3 runtime/experiments/oidc-client/run-health.py \
  --postman /absolute/postman --mock /absolute/postman/mocks/oidc-hydra \
  --collection /absolute/postman/collections/oidc-health \
  --output-parent /absolute/owned-fixtures
```

The runner refuses occupied fixed ports, owns only its mock process group, uses
an isolated CLI home, saves the exact collection output and disables cloud event
reporting. Controlled collection success proves its five saved reads/six
assertions, not native provider or public lifecycle readiness.

## Production lifecycle follow-up

`oidc_runtime.c` now coordinates private preparation, verified assets, two journaled
migrations, the five native services and one-shot enrollment under the owning
deployment actor. `core/2` selects standalone OIDC or composes it with core services
and optional retained Scylla. `core/1` and its per-image identities remain frozen.
Issuer readiness checks exact discovery and bounded RSA/RS256 public JWKS at two
fixed selected-loopback routes. It does not validate a login or token signature.
The API receives only the public issuer/client/audience overlay.

The V6 metadata upgrade preserves existing rows and adds fixed provider component
names. Log capture redacts both credential families and exposes aggregate `oidc`
and individual fixed-role selectors. Cleanup reverses service dependencies, proves
provider writers released and retains the databases/material. Read-only recovery
proves service populations, namespaces and provider writer inodes; it accepts only
exact completed helper deletion receipts. Incomplete/dispatched helper identities
and uncertain enrollment remain held and block replacement or mutation replay.
Manager-loss observations do not adopt readiness or rewrite a successful start.

Public acceptance is maintained in `tests/runtime/test_optional.py`. The runner
requires explicit non-root dedicated Linux inputs and all eight core/provider
layouts for positive OIDC cases; persistent composition also needs the selected
pinned Scylla layout. Every scenario retains evidence, canonical data and secrets.
See [lifecycle evidence](lifecycle-evidence.json) for exact passing commands,
artifact identities, failed attempts and remaining qualification limits.


Final public acceptance includes the ordinary non-test production build, the full
nine-service retained Scylla graph and a stalled bridge during core startup. The
final provider sweep keeps readiness pending until the bridge resumes. Actual
manager loss then requires read-only reconciliation and explicit owned stop;
original successful operations remain immutable. The standalone graph has fresh,
retained, lost-ACK, redacted log, cache/volume/material/port and sanitizer coverage.

For a candidate already installed on an explicit stopped public fixture:

```sh
python3 runtime/experiments/oidc-client/run-public-retained.py \
  --fixture /absolute/private-fixture.json --root /absolute/gs-public-runtime-fixture \
  --daemon /absolute/production/gitstored --output-parent /absolute/owned-fixtures
```

This driver requires every owner released and no active operation before starting.
It uses public CLI commands and real native libraries, reuses verified assets,
pauses only an exact owned bridge PIDfd, and kills only its own manager PIDfd for
recovery validation. `--standalone` selects an existing stopped `identity` fixture.
It retains canonical data, material, logs and source operations. It does not
substitute a private probe for public lifecycle acceptance.
