# Implementation Plan: M0 Contracts

**Branch**: `main` | **Date**: 2026-10-27 | **Spec**: [spec.md](spec.md)

**Feature directory**: `specs/001-m0-contracts`

**Input**: [Specification](spec.md), [PRD](../../docs/prd.md), and
[constitution v1.0.0](../../.specify/memory/constitution.md).

The setup script reports `001-m0-contracts` as its feature identifier; the actual
Git branch remains `main`. No branch creation or upstream changes are included.

## Summary

Deliver M0 as a reviewed documentation package: a command matrix, source evidence
and dependency register, shared CLI behavior, daemon protocol design, runtime ADRs,
and requirement/acceptance traceability. Inventory every PRD operation while
separating verified upstream contracts, missing capabilities, and deferred scope.
No CLI workspace, C daemon, runtime prototype, or new lint CI is introduced.
Retain the existing metadata-only PR-title workflow.

Use Markdown records with stable identifiers and revision-specific source references.
Planning contracts define what the full M0 documents must contain; M0 implementation
will publish those documents under `docs/contracts/` and `docs/ADRs/`.

## Technical Context

**Language/Version**: Markdown for M0. Rust `gs` and C `gitstored` are product
constraints; compiler versions, editions, and build systems belong to implementation.

**Primary Dependencies**: PRD, constitution, upstream public schemas and source
references, Git contracts, OCI/runtime documentation, existing PR-title action.
No new runtime dependency, API collection, cloud workspace, or generated client.

**Storage**: Versioned documentation. Durable daemon state is a conceptual M2 model;
M0 introduces no database or executable state store.

**Testing**: Contract-coverage review and six lifecycle walkthroughs using
[quickstart.md](quickstart.md). Inspect the existing PR-title policy locally;
no new Markdown/workflow CI or artificial application tests.

**Target Platform**: Platform-independent documents. Product targets remote Linux/macOS
amd64/arm64, rootless Linux runtime, and gated macOS backends. Windows deferred.

**Project Type**: Contract and architecture baseline for a CLI and local runtime daemon.

**Performance Goals**: Complete PRD operation coverage and bounded protocol policies.
PRD latency/scale targets remain later release gates, not M0 measurement claims.

**Constraints**: Public interfaces only; upstream read-only; no API/Git write fallback;
per-user runtime; exclusive Git writer; no unsupported capability/platform claims.

**Scale/Scope**: All PRD command families and resource/operation pairs, including
later proposals; six lifecycle walkthroughs; five focused ADRs; FR-001–FR-014 coverage.

## Constitution Check

Evaluated before research and again after Phase 1 design.

| Gate | Before | After | Evidence / enforcement |
| --- | --- | --- | --- |
| I. Public contracts and client boundaries | PASS | PASS | Per-operation evidence, named prerequisites, routing rules; upstream read-only. |
| II. Automation and truthful outcomes | PASS | PASS | CLI behavior and separate transport/admission/reconciliation states. |
| III. Least privilege and credential isolation | PASS | PASS | Peer authorization, resource bounds, identity isolation, privilege decisions. |
| IV. Durable state and recovery | PASS | PASS | Operation/deployment models and six failure walkthroughs. |
| V. Evidence and proportionate verification | PASS | PASS | Review scenarios; no new application or lint CI. |
| Product architecture constraints | PASS | PASS | Rust/C and libcrun direction retained; untested feasibility gated. |
| Contribution policy | PASS | PASS | Existing PR-title validation retained. |

No exceptions. Post-design PASS means this plan preserves the rules; it does not
mean M0 implementation or runtime release gates have already passed.

## Project Structure

### Documentation (this feature)

```text
specs/001-m0-contracts/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/requirements.md
└── contracts/
    ├── artifacts.md
    ├── cli.md
    └── daemon.md
```

### Source Code (repository root)

No application source changes in M0. Planned implementation outputs:

```text
.github/workflows/pr-title-lint.yml   # Existing; retain
CONTRIBUTING.md                       # Existing; check consistency
README.md                             # Link to published contracts
docs/
├── prd.md                            # Existing product source
├── contracts/
│   ├── README.md                     # Index, evidence provenance, review status
│   ├── command-matrix.md
│   ├── dependencies.md
│   ├── cli-behavior.md
│   ├── daemon-protocol.md
│   └── acceptance.md
└── ADRs/
    ├── 0001-daemon-protocol.md
    ├── 0002-runtime-ownership.md
    ├── 0003-runtime-assets-packaging.md
    ├── 0004-rootless-network-volumes.md
    └── 0005-platform-backends.md
```

**Structure Decision**: Publish durable contracts and decisions under `docs/`;
keep feature planning here. Do not scaffold source trees or build manifests.
`tasks.md` is generated by `$speckit-tasks`, not this command. Protocol details
are canonical in `docs/contracts/daemon-protocol.md`; ADR 0001 explains the choice.

## Execution Approach

### Phase 0 — Research baseline

Resolve evidence classification, document shape, protocol approach, and validation.
Record decisions and alternatives in [research.md](research.md). Exact public
operations receive source verification during M0 execution. Missing upstream
capabilities become named prerequisites, not invented client contracts.

### Phase 1 — Design artifacts

Define entities in [data-model.md](data-model.md), publication requirements in
[contracts/artifacts.md](contracts/artifacts.md), shared behavior in
[contracts/cli.md](contracts/cli.md), protocol design in
[contracts/daemon.md](contracts/daemon.md), and review procedures in
[quickstart.md](quickstart.md). No runnable daemon, generated binding, or prototype.

### M0 implementation sequence for task generation

1. Capture upstream provenance, enumerate PRD operations, and assign stable IDs.
   Classify release deferral separately from capability evidence.
2. Publish matrix, dependency register, and CLI behavior; validate User Story 1.
3. Publish protocol and ownership ADRs, then packaging, networking/volumes, and
   platform ADRs. Separate accepted design from unverified feasibility.
4. Complete six walkthroughs, requirement traceability, and document cross-links;
   retain PR-title policy without adding application or lint workflows.

No step changes GitStore or treats an issue description as an implemented contract.

## Requirement Traceability

Paths below are relative to the planned `docs/` output directory.

| Requirements | Planned artifact | Acceptance evidence |
| --- | --- | --- |
| FR-001–FR-002 | contracts/command-matrix.md, contracts/dependencies.md | Every PRD operation has a row and evidence or prerequisite. |
| FR-003 | contracts/cli-behavior.md, contracts/command-matrix.md | Routing review; remote commands independent of daemon. |
| FR-004 | contracts/dependencies.md | Auth/mapping/concurrency/correlation/export/extension gaps. |
| FR-005–FR-006 | contracts/cli-behavior.md | Context, output, error, partial-success and wait cases. |
| FR-007–FR-008 | contracts/daemon-protocol.md, ADRs/0001 and 0002 | Protocol examples, numeric bounds, recovery walkthroughs. |
| FR-009–FR-011 | ADRs/0002–0005 | Ownership, packaging, privilege and platform decisions/gates. |
| FR-012–FR-013 | Existing title workflow, CONTRIBUTING.md | Contribution policy and deferred application/lint CI. |
| FR-014 | contracts/acceptance.md | Every FR/SC maps to artifact and recorded review outcome. |

SC-001/SC-002 use the inventory and coverage map; SC-003 uses the six walkthroughs;
SC-004 uses policy inspection. M0 execution must record actual review outcomes,
not merely copy these planned mappings.

## Complexity Tracking

No constitutional violations or new application infrastructure. Markdown records
suffice for M0; machine-readable registries require a demonstrated consumer first.
