# M2 Runtime, Assets and Profile Contract

**Status**: Planned implementation contract. No backend or release is validated.
[Research decisions](../research.md) select development paths and acceptance
experiments; ADRs 0002–0005 remain governing constraints.

## Dependency and profile manifests

Lock every built dependency by source revision/archive digest, build toolchain,
flags, platform, license/notices and produced digest before use. Do not treat a
version range as a release lock. Trusted profile metadata resolves component image
manifest and per-platform digests, entrypoints, read-only configuration, safe mount
roles, UID mappings, health checks and dependency stages. Callers select registered
IDs/enums only; raw OCI JSON and arbitrary host options are not exposed.

The asset manifest records schema version, profile/version, OS/architecture,
source allowlist, expected digest, size when known, compatible runtime range and
source/integrity policy and any available upstream provenance identity. Profile and manifest bytes themselves need an
authenticated trust chain. No mutable `latest` image reference is a production pin.
A moving-tag Compose compatibility test remains a different evidence class.

Image/network capabilities prefer maintained code dependencies declared in build
manifests, called through supported library APIs. “Library” does not prescribe
shared objects, plugins or dlopen: choose static/dynamic linking by measured size,
ABI and deployment tradeoffs. Rust/C integration must account for foreign-language
runtime costs. Do not build a new library loader solely to satisfy this preference.

For external runtime dependencies that remain, resolve supported host versions
from trusted locations or provision verified missing components on demand. Record
identity and compatibility; absence of an optional external component must not
break unrelated commands. A helper executable is a justified fallback, not a
default bundle, and may use validated host or authorized downloaded components.
Never silently change implementations after an execution failure.

Fallback helper invocations use fixed verified identities, sanitized environment,
bounded stdio/deadlines and protected auth input, never shell interpolation,
`--creds` secrets or ambient helper/auth discovery. Token services and redirect
authorization stay scoped regardless of library versus process integration.

The Rust credential broker extends existing credential isolation with purpose
and registry/repository scope. The daemon invokes only its installed private
broker entry point over owned inherited descriptors, requesting an exact identity;
secrets return through protected IPC, never argv. OS storage is default, protected
file input is explicit, and failures do not prompt in automation. Separate API
issuer, controller enrollment/signing, Git and registry credentials. Reuse of an
API user token as registry material is forbidden.

## Verification and cache

Provisioning stages: authorized -> download/import to owned temporary entry ->
verify trusted source/digest/platform -> safe unpack -> atomically activate -> retain
by immutable identity. Mark verified roots reusable only after complete success.
Reject traversal, symlink/hardlink escapes, malformed whiteouts, wrong ownership,
wrong platform, disk exhaustion and incomplete extraction. Do not implement custom
layer extraction in the daemon.

Upstream images from `gitstore-dev/GitStore` and other dependencies need no
attestations under this policy. Verify trusted configured source, platform and
immutable digest against the CLI distribution's trusted manifest, online or
offline. Preserve upstream signatures/provenance if available without requiring
them. Offline bundles cannot nominate their own trusted manifest. Cancellation
never activates partial bytes; eviction cannot remove live roots, retained volumes
or active intent.

Attestations apply to artifacts produced and published by `gitstore-dev/cli`,
including its release manifest/packages. Verify their subject digest and approved
producer/workflow/source during CLI release promotion. EXP-003C covers CLI file
and offline release evidence, tampering, wrong identity and missing required CLI
evidence; add OCI only if this repository publishes OCI artifacts. It does not
require upstream images to add attestations and does not require a bundled GitHub
CLI/runtime verifier. An attestation on a CLI repackaging step does not claim an
upstream build was performed by the CLI repository.

## Execution and networking

Linux integration calls libcrun C APIs for create/start/state/kill/delete and exit
collection. Record allocation/free, callbacks, signals, reaping and library ABI
ownership. EXP-003A determines delivery/process topology before activation; no
external crun process substitutes for library integration.

User namespace mappings, cgroup delegation, mount support, seccomp/capability policy
and virtualization restrictions are checked before workload changes. Reject
unsupported combinations without elevation/security-control changes. Root filesystems
are read-only where compatible; writable paths and retained mounts are profile-owned.
Shared verified cache roots stay immutable. Initial native execution uses a separate
owned workload tree reconstructed from digest-verified cached blobs without HTTP,
not mutable bind targets or file hardlinks into the shared cache. Its transient disk
reservation/cleanup is distinct from retained volumes and cache references.

A network identity and every attachment/host port are recorded before use. The
selected rootless implementation provides inter-service discovery, private internal
ports and high loopback public ports. Never use host networking as a silent fallback.
On failure reverse only new owned attachments/allocations; on restart reconcile
actual helpers/namespaces before cleanup. Two deployments must not share names or
unintended reachability. Shared per-deployment namespace is not per-component isolation.

Volumes are stable named local resources. Repository writers require durable
reservation, filesystem ownership lock and proof no prior workload still writes.
Stop retains volumes; transient rootfs/network cleanup must not traverse retained
mounts. Persistent Scylla data is retained, memdb catalogue state is ephemeral,
and retained Git objects do not imply automatic catalogue recovery under memdb.
Document that distinction explicitly in stop/restart results.

## Core startup and migrations

Owner settings are a separate, private `settings.json` under the daemon's explicit
state directory; they are never lifecycle wire parameters. The initial contract
has `schemaVersion: 1`, optional `bootstrapCredentialSource` (`kind: os-store`, the
default, or explicit `kind: protected-directory` with an absolute private directory `path`), and
optional `offlineLayouts` mapping registered core component names to absolute OCI
layout directories. Reject unknown keys, unsafe ownership/modes/links, unsupported
sources and unregistered component mappings before credential/workload effects.
Offline input authorizes only its verified bytes, never fallback network access.
Load a stable validated settings snapshot at manager startup. Do not rewrite it
from client requests or discover ambient credential/helper files. The T043 reader now enforces this contract at explicit-state manager startup.
Lifecycle dispatch remains gated while the resource driver is integrated.

1. Validate profile/configuration/host/port/asset/ownership requirements and consent.
2. Commit accepted intent and reservations; provision verified required assets.
3. Prepare owned network/volumes and protected local user/policy/service material.
4. For Scylla: bring up the registered datastore profile, establish its documented
   keyspace, run the compatible exclusive migration target, then configure API
   startup to check rather than race a second migration.
5. Start Git and API in a staged graph that permits their mutual callouts; verify
   public/API and authenticated Git readiness before dependent controller launch.
6. Prepare controller signing material separately, enroll its existing public key
   through the shipped public mechanism, persist API-assigned identity privately,
   then launch using its signing record. Do not regenerate it on ordinary restart.
7. Verify controller and profile health for the intended generation; report actual
   endpoints and readiness. Optional components add only verified graph nodes.

Use image-native gitctl and registered image entrypoints. Do not require source
checkouts, Go, make or Compose on the user's host. The API issuer private key stays
API-only, outside shared service config. Validation never generates credentials.
Native helper flags and protected credential input require exact handler/source
checks; do not copy development password-in-argv patterns from a Makefile.
If a required protected input is missing, add a named upstream dependency or a
safe existing image-native input adapter before exposing the affected workflow.

Migrations use registered targetVersion -> immutable image/helper/schema mapping,
backup acknowledgment and daemon/datastore exclusivity. Invoke the versioned
helper's actual migration mechanism, preserve its ledger and no-auto-downgrade rule.
No arbitrary script or guessed schema target. An incompatible target, missing
keyspace/backup or uncertain previous migration blocks before unsafe execution.
After failure, dependent startup is blocked and known operation identity retained.

## Packaging and support

The default core distribution contains gs and gitstored plus only essential
packaging/metadata. Avoid extra bundled executables. Prefer maintained libraries,
compatible host capabilities and verified on-demand acquisition of missing selected
runtime dependencies. No manual runtime-stack installation is required; unsupported
host capabilities remain explicit. Images/VM assets are also lazy provisioned.

Target kilobyte-to-low-megabyte stripped core executables (initial engineering
budget <=5 MiB each). Record actual results and misses; do not imply the current
Rust dependency graph already meets this budget. Report mandatory libraries,
package, first-use transfer, installed/cache and memory costs separately. Optional
runtime-library absence must not disable help/remote/control functions that do not
need it. Host library/executable reuse requires version/ABI and behavioral checks;
an arbitrary PATH match is not compatibility evidence.

Linux host validation covers both reference architectures; unavailable runners are
not claimed as passes. Apple container/framework and managed-VM experiments test
packaging, install privileges, guest trust, private images, host networking, logs,
retained volumes and crash recovery. Mac execution remains unsupported until its
backend passes. Windows, CSI, cloud deployment and payload protocols are excluded.


Core dependency selection is shared by validation and asset authorization. Omitted
selection means API/Git/controller; a nonempty unique explicit core selection
always includes the mutual API/Git prerequisites and includes controller only when
requested. Optional/unrecognized selections and explicit empty selections fail
semantically before provisioning. This derived closure does not reorder or rewrite
the original semantic intent/idempotency fingerprint.


The protected-directory owner policy resolves to a normal protected-file broker
source for each installation/issuer/deployment/purpose tuple. The filename is
`bootstrap-<SHA-256 of canonical sorted identity JSON>.json`; issuer is
`gitstore-local/core/1` and purpose is `bootstrap-material`. Different deployments
or installations cannot share the configured fallback file accidentally. Verify
the pinned directory inode still matches its name before resolving a source.
Literal shared protected-file paths are not owner settings; the private broker
still supports explicit single-identity file inputs used by isolated probes.

T043 adds the `configuration` resource kind to the unreleased prototype state
schema. Strict required-schema checking refuses older experimental databases;
there is no implicit repair/adoption or publication compatibility claim. Keep old
fixture data/evidence and select a fresh private state for new runtime tests.
