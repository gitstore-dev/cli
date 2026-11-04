# Implementation Plan: M2 Runtime — Unblocked Implementation

**Branch**: Existing Git branch `main` | **Date**: 2026-11-03 | **Spec**: [spec.md](spec.md)

**Input**: `specs/003-m2-daemon-foundation/spec.md`, corrected to implement M2 where
its dependencies are unblocked. The directory name is retained for stable discovery.
Setup reports logical feature branch `003-m2-daemon-foundation` from the feature
pointer; actual Git HEAD remains `main`. No branch was created or switched.

## Summary

Implement real Linux M2 runtime alongside the C daemon/control foundation. Reuse
the accepted local protocol, shipped server bootstrap/migration helpers, embedded
libcrun, maintained image/network implementations and retained local volumes.
Open internal gates become ordered implementation/experiment checkpoints; missing
external mappings or host evidence gate only affected subsets. Planning adds no
application code, runs no runtime experiments and establishes no release support.

## Technical Context

**Language/Version**: Rust 1.96.0, edition 2024 for gs; C17 for gitstored.
**Primary Dependencies**: Existing Rust libraries; CMake >=3.28; private Jansson
2.15.1 and SQLite >=3.51.3 candidates; libcrun 1.30.1; library-first image/network evaluation; Skopeo/umoci/crane and pasta/slirp/CNI
remain host/on-demand fallback comparisons, not bundled defaults. CLI artifact attestations
are release-workflow checks, not a packaged runtime dependency. Immutable experiment pins and selection rules are in
[research.md](research.md); all distributed dependencies require a build lock.
**Storage**: One per-user SQLite database (DELETE journal, EXTRA synchronization),
owned local files/directories, durable volume identities/locks, bounded verified
asset cache and bounded log segments. No credentials in intent/history.
**Testing**: Cargo behavior tests; CTest; Clang 18 ASan/UBSan/libFuzzer; actual
Rust/C process interoperability; controlled crash/limit fixtures; real Linux
libcrun/image/network/volume/Scylla tests; clean distribution and provenance cases.
**Target Platform**: Linux Ubuntu 24.04 amd64/arm64 execution targets. macOS
control portability and Apple/managed-VM feasibility are separate evidence lanes.
Windows and CSI deferred. Current macOS developer host cannot prove Linux support.
**Project Type**: CLI plus per-user, profile-scoped runtime daemon.
**Performance Goals**: Enforce all M0 numeric bounds; preserve cancellation and
inspection under worker saturation. Target 5 MiB or less per stripped release executable as an initial interpretation
of the requested kilobyte-to-low-megabyte size range. Measure current release gs
and size contributors before optimization; do not claim this target is achieved.
Measure mandatory libraries, package/download/cache footprint, memory and cold/warm
readiness separately; no hidden footprint reduction by moving bytes to a library.
**Constraints**: No normal-operation sudo, root daemon, Docker/Compose/make/Git
executable prerequisite; no arbitrary execution/mounts; exactly one repository
writer; no implicit data deletion, trust downgrade, transport/backend fallback or
runtime downloads for remote commands.
**Scale/Scope**: M0 policy: 32 connections, 16 outstanding requests per connection,
8 runtime workers, 64 pending jobs, 100,000/256 MiB operation metadata, one mutation
per deployment; other exact frame/stream/log/time bounds remain canonical in M0.
Core memdb and persistent Scylla profiles first; optional components only where
verified mappings exist. M2 experiments EXP-003A/B/C, EXP-004A/local-B, EXP-005A/B
are included work, not successful results.

## Constitution Check

**Pre-research check**: Pass for the corrected scope and planned boundaries.
**Post-design check**: Pass at design level, with executable gates below still open.

| Principle / constraint | Design evidence | Before activation or support claim |
| --- | --- | --- |
| I: public contracts | Reuse M0 socket protocol and pinned upstream helpers; preserve GraphQL/Git ownership | Typed mappings and public readiness checks verified; no internal datastore/repository edits by gs |
| II: truthful automation | Separate acceptance, operation state, generation readiness and recovery; no-input consent rules | Process tests for partial outcomes, waits, loss of response and safe output |
| III: least privilege | Owner-only Unix socket/state, peer credentials, registered profiles, rootless host checks | Unauthorized peer/path/parser bounds and real host isolation checks |
| IV: durability | SQLite atomic acceptance, dedupe, resource reservations, backend observation and retained-volume lock | Crash/stop/migration/disk-full tests; no replacement without termination proof |
| V: evidence | Separate controlled, native backend, provider and release evidence | Per-host reports, experiment decisions and exact digest/provenance evidence |
| Language/runtime | Rust gs, C17 gitstored, libcrun library integration | EXP-003A audits worker/fork/signal topology and records ADR process decision before activation |
| Distribution | Library-first integration, compatible host reuse, verified missing-dependency downloads, offline trust | Library/ABI and size experiments, EXP-003B/C, clean-host and host-reuse checks; no manual helper-stack installation |
| Verification/CI | Extend build/behavior/sanitizer/fuzz lanes with executable targets | Existing Rust checks retained; metadata-only title check unchanged; no doc-only CI |

No constitutional exception is adopted. Worker-process evaluation is required to
keep cancellation responsive around fork-sensitive native execution; it must be
justified and recorded against ADR 0002/0003 before use. A failed checkpoint stops
that backend, not unrelated control/remote work. Host/security restrictions are
reported rather than automatically relaxed.

## Project Structure

### Documentation (this feature)

```text
specs/003-m2-daemon-foundation/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── release-evidence.md
├── quickstart.md
├── checklists/requirements.md
└── contracts/
    ├── cli.md
    ├── daemon.md
    └── runtime.md
```

`tasks.md` is the next phase and is deliberately not generated by planning.

### Source Code (repository root)

All additions below are planned, not present implementation:

```text
crates/gs/src/
├── cli.rs, lib.rs                 # extend dispatch before remote resolution
├── daemon_protocol.rs            # strict typed local wire model
├── daemon_client.rs              # bounded framing, reconnect, event dispatch
├── deployment.rs                 # local UX, consent, waits, foreground cleanup
└── runtime_credentials.rs        # isolated registry/bootstrap credential broker

daemon/
├── CMakeLists.txt
├── include/gitstored/
├── src/
│   ├── main.c, server.c, protocol.c
│   ├── state.c, operations.c, ownership.c, streams.c
│   ├── profiles.c, provisioning.c, credentials.c
│   ├── backend_libcrun.c, networking.c, volumes.c
│   └── bootstrap.c, migration.c, recovery.c
├── tests/                        # core, wire, crash and controlled backend
└── fuzz/                         # framer/parser corpus and harnesses

runtime/
├── dependencies.lock.json        # source/build/license/digest inventory
├── profiles/                     # versioned allowlisted graphs/mappings
├── assets/                       # trusted manifest schema/policy; no image blobs
├── packaging/                    # minimal core packaging + user service files; optional dependency manifests
└── experiments/                  # reproducible EXP-003/004/005 runners/results

tests/runtime/                    # real backend, clean-host and recovery scenarios
scripts/                          # bounded runtime validation/package runners
.github/workflows/                # C/runtime checks alongside executable targets
```

**Structure Decision**: Preserve the existing Cargo workspace. Build C daemon
separately through CMake; do not add C source to the Rust package or introduce a
network service. Rust integration tests discover an explicitly supplied test
binary. Runtime manifests contain no mutable image tags or credential material; no runtime
attestation verifier is added solely for CLI artifact publication.

## Phase 0 — Research complete

[Research](research.md) resolves persistence/parser/build, execution process safety,
image/network candidates, CLI release provenance, profile/bootstrap and host strategy.
Research agents inspected repository integration and primary sources. They ran no
containers or experiments. Source pinning/security inventory is an explicit first
implementation checkpoint; selection experiments do not masquerade as completed
compatibility tests. Postman skill/status check crashed with Keychain error -50;
the installed repository guide and api-engineer guidance were read. This local
framed protocol uses its native conformance harness, not an invented HTTP API or
OpenAPI collection. Existing Postman M1 simulation remains unchanged.

## Phase 1 — Design complete

[Data model](data-model.md), [CLI contract](contracts/cli.md),
[daemon contract](contracts/daemon.md), [runtime contract](contracts/runtime.md)
and [validation guide](quickstart.md) define implementation boundaries. The spec
now includes real startup, assets, networking, volumes and migration requirements
FR-024–031 and outcomes SC-009–013. Its quality checklist is revalidated.

## Implementation sequence for task generation

| Checkpoint | Work and dependency | Completion evidence / bounded gate |
| --- | --- | --- |
| A: reproducible skeleton | Lock dependencies; C target and build checks; profile/source inventory for alpha.8; compare upstream handler/config details | Builds plus committed immutable source evidence; no inferred optional component support |
| B: protocol/state | Strict framed C/Rust interop, peer auth, SQLite transactions, ownership, observation and controlled workers | M0 examples + malformed/limits, crash/dedupe/quota/cancellation tests; controlled evidence only |
| C: execution/provisioning | EXP-003A/B; libcrun library integration, library-first image/network adapters, host resolver, verified dependency/image cache and private auth/offline pipeline | Resolve process/link/pipeline/trust selections and record measured evidence before their production activation |
| D: host resources | EXP-004A/local-004B; namespaces/network, named volumes and writer locks | DNS/ports/isolation, UID mapping, retained-data and disk-failure checks on Linux reference hosts |
| E: actual core profile | Depend on B–D; on-demand daemon, credentials/enrollment graph, foreground/detached serve, readiness/stop/logs | Real memdb core public GraphQL/Git workflow with retained repositories and no extra runtime stack |
| F: persistent/optional profiles | Scylla keyspace/schema lifecycle, migrate compatibility/exclusion and recovery; optional components after mapping | Real exclusive migration/recovery and backup cases; missing optional mappings stay gated independently |
| G: distribution/support | Package candidate distribution, user-service lifecycle, clean installs, real crashes, budgets and EXP-003C CLI publication provenance negative cases | Per-host/architecture evidence before support or publication; DEP records reconciled by proven subset |
| H: macOS feasibility | EXP-005A/B after reusable backend/control boundaries; can run alongside Linux work | Available host experiment reports, authenticated guest path and retained ownership; unsupported where absent/failed |

A–B are independently useful; they are not the feature's final acceptance. C and D
must produce real integrations and evidence, followed by E–G. H does not prevent
Linux completion but is included feasibility work, recorded unavailable where the
required hardware is absent. A missing optional mapping is a contract gate for that component. Upstream
GitStore service images require trusted-source/platform/digest validation, not
attestations. Missing required attestations gate publication of this repository’s
own artifacts only; do not convert that release gate into a startup dependency. Continue unaffected work.

## Traceability and delivery checks

| Requirements | Design / verification |
| --- | --- |
| FR-001–005, FR-015–018 | CLI/daemon contracts; wire, peer, bound and isolated config tests |
| FR-006–014, FR-019 | Data model and daemon/runtime contracts; controlled + real crash/ownership/migration tests |
| FR-020–023 | Release review, capability activation policy, M1 regression and evidence reports |
| FR-024–026 | Runtime contract; EXP-003A/B/C and private/offline/integrity cases |
| FR-027–028 | Runtime resource model; EXP-004A/local-B and isolation/retention cases |
| FR-029–030 | Bootstrap graph, real profiles, clean installation and EXP-005A/B |
| FR-031 | R9 declared code dependencies and external runtime resolution, consent/offline cases and separate size accounting |
| SC-001–008 | Validation guide control/automation/evidence cases |
| SC-009–013 | Validation guide real runtime, migration, assets and host experiments |

Before implementation completion, add `docs/m2-status.md` with exact command
subsets, source/build/image identities, actual checks and not-run/unavailable lanes;
reconcile M0 dependency/command records and CONTRIBUTING/README without rewriting
historical evidence. Keep M1 status and gates independent. No claim that any
future command or build in quickstart has already run.

## Complexity Tracking

No constitutional violations require an exception. The SQLite store, native parser,
execution worker evaluation and optional library/helper adapters each address an
explicit durability, bounded input, process-safety or asset-trust requirement.

## Dependency and size revision — 2026-11-03

The user's packaging preference supersedes earlier default-bundling choices.
Evaluate libraries first; reuse compatible verified host dependencies; download
missing selected-profile assets only with provisioning permission. Use ordinary declared code dependencies; static/dynamic linking follows measured
size and compatibility tradeoffs. Do not require dynamic loading or host-installed
shared libraries solely because libraries are preferred.
Do not bundle image/network/VM executables by default. Justify any unavoidable
helper and prefer host reuse/on-demand delivery. R9 defines resolution and separate
size accounting. A fresh host must still work after authorized provisioning without
manually assembling a runtime stack; offline hosts need the verified offline assets.

Library terminology clarification: a library is a source/build dependency (as in
Cargo.toml, pom.xml or go.mod), not necessarily a shared object. Normal compiled
library reuse is preferred over wrapping executables. Host discovery/download rules
apply to external runtime requirements that remain, not to every code dependency.
