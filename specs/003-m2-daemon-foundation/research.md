# M2 Implementation Research

**Date**: 2026-11-03. **Status**: Design decisions; no runtime experiment executed.
**Scope correction**: The user requested unblocked M2 implementation. The earlier
foundation-only exclusion is superseded. Internal evaluation and validation work
is included; external missing contracts/evidence gate only affected capabilities.

## R1 — Existing integration and upstream evidence

**Decision**: Retain Rust 1.96.0 and the current blocking CLI. Add a C17 daemon;
route local commands before remote configuration/credential resolution. Split local
operation waits from the current blanket `DEP-004` rejection in `Cli::preflight`.
Use synchronous bounded Unix socket I/O, without adding an async runtime.

**Rationale**: The repository already has reusable output, error, config and
credential code. Remote commands must work without a daemon. Local completion
conditions have a separate accepted contract from server admission correlation.

**Alternatives considered**: Duplicate remote context stores; apply the M1 wait
gate to every command; replace the existing transport architecture. All add scope
or block supported local operations without a contract reason.

**Evidence**: Existing `crates/gs/src/cli.rs`, `lib.rs`, Cargo manifests, M1 status
and [release review](release-evidence.md). Supplemental upstream revision is
`06152d193e854bc643fa884daa8b9f30b0d75d0f`; historical M0/M1 evidence remains unchanged.

At that revision the image-native gitctl includes configuration validation,
signing-material generation, enrollment and Scylla migration. Its help and
implementation may differ: verify typed flags against handlers and fixtures,
rather than copying a help synopsis. M2 translates profile `scylladb` to the
server's `scylla` backend spelling. Keyspace creation remains an explicit profile
bootstrap step; do not assume `gitctl migrate` creates it.

## R2 — Durable state and ownership

**Decision**: Native SQLite C API, one database-owning control loop, short
transactions, foreign keys enabled, DELETE rollback journal and `synchronous=EXTRA`.
Use SQLite 3.51.3 as the minimum candidate baseline, locked source bytes before
build. Keep state on a supported local filesystem. Use a separate lifetime daemon
lock and retained-volume locks plus backend ownership observations.

**Rationale**: One transaction can persist operation/dedupe/generation/resource
reservation before acknowledgment. External runtime I/O must never hold the DB
transaction. SQLite locking protects the state store, not surviving Git writers.
Rollback journaling avoids unnecessary WAL checkpoint/concurrency complexity.
Validate filesystem sync behavior; storage errors fail closed.

**Alternatives considered**: In-memory maps/PID files cannot recover accepted
intent. Ad hoc JSON journals duplicate transactional recovery. WAL provides little
benefit with a single owner and needs additional policy and sidecar handling.

**Sources**: [SQLite atomic commit](https://sqlite.org/atomiccommit.html),
[synchronous policy](https://sqlite.org/pragma.html#pragma_synchronous),
[limits](https://sqlite.org/limits.html). Implementation must bound metadata
including journal overhead, reserve safe transaction space and distinguish log/cache
budgets; no unbounded database growth under quota pressure.

## R3 — Strict framed protocol

**Decision**: Use a private pinned Jansson 2.15.1 build in C and duplicate-rejecting
Serde visitors in Rust. Apply `JSON_REJECT_DUPLICATES`, full-input parsing, reject
all real-number tokens and enforce exact safe integers. Enforce 32-level depth
before recursive materialization through an audited private parser depth setting,
plus bounded allocation, UTF-8/string/item limits and length-aware access.

**Rationale**: Both clients must reject duplicates and numbers consistently; plain
map decoding can erase duplicates before validation. Protocol framing is four-byte
big-endian length plus UTF-8 JSON, not HTTP or OpenAPI.

**Alternatives considered**: Default parser settings, post-parse-only depth checks
and a new general JSON parser fail the bounds/reuse requirements. Boundary fixtures
must verify how the private depth setting counts nested containers.

**Sources**: [Jansson API](https://jansson.readthedocs.io/en/latest/apiref.html),
[decoder](https://github.com/akheron/jansson/blob/v2.15.1/src/load.c),
[release](https://github.com/akheron/jansson/releases/tag/v2.15.1).
The accepted [M0 protocol](../../docs/contracts/daemon-protocol.md) supplies all
wire methods, types, limits, errors and 22 example exchanges.

## R4 — Embedded execution and process safety

**Decision**: Implement the Linux lifecycle adapter with libcrun 1.30.1 linked as
a declared native library dependency called through its C API. Static versus
dynamic linking is an EXP-003A packaging decision based on measured footprint,
compatibility and distribution obligations; neither is required by “library.” The main control loop owns state and cancellation. Evaluate
single-threaded execution workers forked from a single-threaded parent, using the
same gitstored binary/library and bounded private IPC; do not invoke crun CLI.

**Rationale**: libcrun lifecycle is fork/signal sensitive and blocking execution
must not starve cancellation/observation. Process isolation has a concrete purpose:
keep native runtime faults and fork-sensitive calls out of the state-owning loop.
Before adopting the worker topology, EXP-003A must prove signal/reaping, library
allocation ownership, inherited-FD closure, cancellation and crash recovery.
Record the resulting process-boundary decision in ADR 0002/0003; it is not an
unverified permanent exception. Only the parent persists state; workers receive
registered resolved intent, never arbitrary executable/mount options.

**Alternatives considered**: Calling libcrun from an unrestricted thread pool;
wrapping the crun executable; rebuilding isolation. The first needs unsafe fork
assumptions; the latter two violate the selected architecture. Failure of this
checkpoint requires revising the adapter design before runtime activation.

**Sources**: [library guidance](https://github.com/containers/crun/blob/1.30.1/README.md),
[lifecycle header](https://github.com/containers/crun/blob/079ff6a7a16d029460af882f9ea44674e6a510b6/src/libcrun/container.h).
No helper/process safety experiment has passed merely by choosing this design.

## R5 — Image pipeline and provisioning

**Decision**: Evaluate maintained image acquisition/unpack libraries before any
helper-executable path. No Skopeo/umoci/crane executable is selected for default
bundling. Evaluate containers/image (current development in container-libs),
go-containerregistry and umoci library APIs with their actual language/ABI,
maintenance, extraction safety and size costs. A Go library wrapped as a shared
component still brings a Go runtime; do not assume that is smaller or simpler.

Prefer ordinary maintained code dependencies declared in the build manifests.
Evaluate Rust/C-compatible APIs and integration costs first, rather than assuming
Go libraries can be used directly from C. Where the selected implementation still
has external runtime dependencies, reuse compatible host capabilities or provision
missing components with verification and consent. A maintained helper process is a fallback only
when no suitable library integration meets safety and footprint goals; use a
validated host helper or consented download, not mandatory core-package bundling.
Any fallback still uses protected credential input, sanitized environment and
explicit scope/platform/digest checks, with no ambient auth or shell interpolation.

**Rationale**: The user prioritizes small core executables, library reuse and
on-demand dependencies. Avoid both duplicate executable distributions and large
static dependencies. Maintained components must still own registry/extraction
semantics; minimizing binary count does not justify reimplementing those protocols.

**Alternatives considered**: Always bundle Skopeo + umoci; replace them with a
statically embedded Go runtime; assume umoci has a stable C ABI. These are not
approved defaults. Compare library boundaries against host/downloaded executable
fallbacks under EXP-003B, recording core and external footprint separately.

**Sources**: [containers/image library and migration](https://github.com/containers/image),
[current container-libs](https://github.com/containers/container-libs),
[umoci library/API stability](https://pkg.go.dev/github.com/opencontainers/umoci),
[go-containerregistry](https://github.com/google/go-containerregistry).
Prior Skopeo/crane/umoci executable pins below remain comparison inputs only.
Planning inputs alone establish neither a stable foreign ABI nor a passing safety
experiment; the implementation checkpoint below records subsequent actual evidence.

**Implementation checkpoint (2026-11-03)**: T032's
[measured comparison](../../runtime/experiments/image-library.md) selects maintained
oci-client at immutable revision `71c08729adeb88fbdda43966ece962d32b58fb8d` and
ocirender 0.2.3 as ordinary Rust dependencies behind an owned C ABI. macOS/Linux
arm64 private synthetic transfer and focused auth/extraction checks pass. The
released OCI client's missing transport policy hook required the pinned upstream
revision. This updates the candidate selection only: T035–T039's production
adapter, bounds, consent and activation remain required.

## R6 — Network and retained storage

**Decision**: Evaluate maintained library-based rootless networking first, including
libslirp behind a suitable maintained integration. Compare pasta/slirp4netns and
one official CNI rootless profile as fallback candidates. No networking executable
is selected for default bundling; reuse a compatible host capability or provision
an explicitly authorized missing dependency. Initial topology is one owned private network namespace per
deployment, shared by its registered services, with profile-defined service names
and distinct internal ports. Publish only configured high loopback host ports.
Use profile-owned named local volumes and canonical persistent identities.

**Rationale**: Sharing a deployment namespace simplifies core mutual connectivity;
isolation is between deployments, not a promise of network isolation between
components in one profile. DNS/hosts mapping and loopback binding must be verified,
not inferred from successful attachment. Retention is independent of container IDs.

**Alternatives considered**: Automatic helper bundling; host networking; host-root bridge setup; assume CNI
works rootless; CSI as baseline. None satisfies the default-profile constraints.

**Sources**: [libslirp library](https://gitlab.com/qemu-project/libslirp),
[pasta design](https://passt.top/passt/about/),
[slirp4netns controls](https://github.com/rootless-containers/slirp4netns/blob/944fa94090e1fd1312232cbc0e6b43585553d824/slirp4netns.1.md),
[CNI](https://github.com/containernetworking/cni), ADR 0004. Choose the passing path
by measured footprint and maintenance cost; do not enable an untested fallback.

**Implementation checkpoint (2026-11-03)**: The native
[libslirp 4.9.7 experiment](../../runtime/build-inputs/libslirp.md) passes
normal rootless namespace/DNS/loopback/cleanup probes but fails Clang 18 UBSan
alignment checks inside upstream TCP processing in both debug and release builds.
The unpatched input remains rejected. A subsequent hash-bound access-view patch
preserves original container layouts and passes normal GCC Werror plus Clang
ASan/UBSan static/shared/concurrent probes and upstream tests. T034 selects this
ordinary code dependency as `4.9.7-gstd-queueview2`; T041's actual ownership/attachment
adapter remains unimplemented. No executable fallback is selected.

## R7 — CLI release provenance and upstream asset integrity

**Decision**: Attest and verify artifacts produced and published by
`gitstore-dev/cli` in its trusted release pipeline. Do not package GitHub CLI or
an attestation verifier into gs/gitstored for this requirement. Use maintainer/CI
verification tooling for CLI release promotion. EXP-003C is a CLI publication
check, not a prerequisite for starting upstream GitStore service images.

**Rationale**: The user clarified the producing-repository boundary. Upstream
`gitstore-dev/GitStore` has no attestation requirement here. Runtime image checks
use trusted configured registries, immutable platform/digest pins and the CLI
distribution's trusted asset manifest. Offline import enforces the same identities.
Preserve upstream provenance when available, without requiring it or inventing an
external blocker. CLI-published manifests/packages still need their own release
provenance; that does not imply every referenced upstream asset is attested.

**Alternatives considered**: A bundled GitHub CLI verifier and mandatory upstream
image attestations were removed: they added runtime dependencies and blocked M2
based on a release policy intended for this repository's outputs. A custom
cryptographic verifier is unnecessary for this scope.

**Sources**: User clarification on 2026-11-03; corrected
[PRD DIST 1](../../docs/prd.md#automation-and-quality-requirements),
[ADR 0003](../../docs/ADRs/0003-runtime-assets-packaging.md#artifact-attestations)
and [DEP-013](../../docs/contracts/dependencies.md#dep-013).
EXP-003C tests CLI file/offline release evidence and negative cases; include OCI
only if the CLI repository itself publishes an OCI artifact. No release evidence
was generated or verified during planning.

## R8 — Builds, hosts and verification

**Decision**: C17, CMake 3.28 minimum, CTest, Clang 18 Linux sanitizer/fuzz lane;
retain Rust 1.96.0 and existing locked checks. Native execution targets Ubuntu
24.04 amd64/arm64 with recorded namespace, mapping and cgroup configuration.
Portable control tests on macOS do not establish container execution.

Include EXP-005A (Apple container/direct Containerization) and EXP-005B (managed
Lima Linux guest) in M2 work. Use ADR 0005's candidates initially; pin framework,
guest and tool revisions before building experiments. If a required host is absent,
record unavailable evidence and continue Linux work. No automatic privileged fallback.

**Rationale**: Build tools are developer prerequisites; the distribution must carry
runtime dependencies. Linux real-backend recovery and clean-install checks are
separate from controlled workloads and Compose-based M1 compatibility.

**Alternatives considered**: Require full macOS parity before Linux delivery;
claim macOS support from a Unix socket test; add document-only CI. All overstate or
unnecessarily block delivery.

**Sources**: [CMake language policy](https://cmake.org/cmake/help/latest/prop_tgt/C_STANDARD.html),
[Apple container](https://github.com/apple/container),
[Containerization](https://github.com/apple/containerization),
[Lima](https://github.com/lima-vm/lima), ADR 0005.

## Reproducible experiment inputs

| Candidate | Reviewed version | Resolved source commit |
| --- | --- | --- |
| libcrun | 1.30.1 | 079ff6a7a16d029460af882f9ea44674e6a510b6 |
| Skopeo | v1.24.1 | 77f3d92f861271c7cb9175afcf876017bcae9202 |
| umoci | v0.6.0 | 1c44d6f2f9e4b50f5d2a99463f540acf9915dd47 |
| crane | v0.22.1 | 8a72a424fdecb4caa14f2d525e5d2503331442b5 |
| slirp4netns | v1.3.3 | 944fa94090e1fd1312232cbc0e6b43585553d824 |

These are candidate inputs, not vulnerability clearance, measured results or
approved release pins. Implementation first records a dependency lock with source
archive digests, build flags, toolchains, license/transitive inventory and output
identities. Resolve the official pasta snapshot, parser/database source checksums,
macOS candidate identities before their first build. This is a
reproducibility task with a fail-closed rule, not an unresolved product decision.

All architectural questions have planning decisions above. Experiments remain
explicit implementation acceptance checkpoints; no experiment or host result is
invented to make the design look complete.

## R9 — Dependency resolution and small core executables

**Decision**: Core deliverables are gs and gitstored. Prefer maintained code
dependencies declared in build manifests; for remaining external runtime requirements,
reuse compatible host capabilities and provision verified private dependencies only
when required for the selected operation and missing/incompatible locally. No
skopeo, umoci, crane, pasta, slirp4netns, CNI or VM tool is bundled by default.
A helper executable requires recorded library evaluation and justification, then
uses compatible host discovery or an on-demand download where feasible.

“Library” means an ordinary code dependency declared in Cargo.toml or the native
build/dependency manifest, analogous to pom.xml/go.mod. It does not mean a required
shared object, host-installed library or plugin. Select static/dynamic linking
through measured size and deployment tradeoffs. Do not introduce dlopen/plugin
architecture solely from this preference. If an implementation actually uses an
external dependency, resolve its identity/version safely and preserve unrelated
help/remote/control functionality when that optional dependency is unavailable.

**Size objective**: Kilobytes to low megabytes per stripped release executable.
The original 5 MiB reference has measured misses. The user accepted 5,590,568-byte
`gs` and 5,723,600-byte `gitstored` for this increment, deferring published-binary
optimization to their follow-up. Continue reporting uncompressed executable,
mandatory shared libraries, package, downloads, cache and resident-memory costs
independently; moving code to a shared library does not erase its footprint.

**Rationale**: Calling maintained code dependencies avoids wrapping separate tools.
Binary size is an independent optimization/measurement requirement, not a mandate
for shared-library loading. Host reuse and selective downloads apply to external
runtime dependencies that remain after library integration.

**Alternatives considered**: Bundle every backend; fetch dependencies at every
launch; rely on arbitrary host tools; introduce a general plugin ABI or custom
registry/network stack. None is necessary for this profile-scoped runtime.

**Evidence**: User preference on 2026-11-03; no size target or integration is
claimed achieved. Offline and no-input operation retain explicit provisioning
consent, identity verification and actionable missing-dependency behavior.

## Implementation decision — canonical intent SHA-256

Use platform CommonCrypto on macOS and OpenSSL libcrypto ABI 3 on Linux through
CMake library declarations, not an executable or a new cryptographic implementation.
The dependency lock records this platform selection; CMake reports the resolved
OpenSSL version for build evidence. The Linux integration remains unverified until
its lane runs. This is an ordinary code/library dependency; release packaging and
size accounting must include it. Native acceptance tests check a known SHA-256
vector and persisted same-key replay/default normalization across store reopen.
