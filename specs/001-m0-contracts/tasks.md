---
description: "Dependency-ordered M0 contract and architecture documentation tasks"
---

# Tasks: M0 Contracts

**Input**: Design documents in `specs/001-m0-contracts/`.

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md),
[data-model.md](data-model.md), [contracts/](contracts/artifacts.md),
[quickstart.md](quickstart.md), and constitution v1.0.0.

**Scope**: Publish the M0 documents, not M1/M2 implementations. Preserve existing
PR-title validation. Do not add source scaffolding, build targets, Markdown/workflow
lint jobs, or upstream changes. Upstream inspection and consultation are read-only.

**Verification**: The specification requests contract reviews and lifecycle
walkthroughs. Tasks record those results; no executable application test suite or
TDD cycle is introduced. Source review is not live or release validation.

**Organization**: Two P1 stories, with shared preparation and final consistency review.
Completion markers record the M0 implementation and document reviews, including
inspection of previously created artifacts. They do not mark later release tests passed.

## Format: `[ID] [P?] [Story] Description`

- `[P]` means a task can run alongside another eligible task on different files,
  once its listed prerequisites are complete. It does not authorize upstream edits.
- `[US1]` and `[US2]` map to the specification's user stories.
- All task paths are relative to the repository root; resolve them against the
  repository's absolute path when performing filesystem operations.

## Phase 1: Setup (Shared Documentation Structure)

**Purpose**: Establish the publication index and a place to record actual review outcomes.

- [X] T001 Create `docs/contracts/README.md` as the publication index, linking the planned contract documents and five ADRs listed in `specs/001-m0-contracts/contracts/artifacts.md`; distinguish this M0 package from runnable CLI/daemon releases and establish stable CMD, EVD, DEP, ADR and scenario identifier conventions.
- [X] T002 Create `docs/contracts/acceptance.md` with separate US1, US2, contribution-policy and final-coverage sections; reproduce Acceptance Evidence fields from `specs/001-m0-contracts/data-model.md`, including actual result (`not-run`, `pass`, `fail`), review date, reviewer role and open findings; apply “Planned expectations must not be prefilled as passed results.”

## Phase 2: Foundational (Blocking Publication Rules)

**Purpose**: Make both stories use the same evidence, status and review definitions.

- [X] T003 Capture the read-only upstream baseline in `docs/contracts/README.md`: repository/revision, reviewed date, per-source dirty status, source path/symbol and limitations; define evidence level (`requirement`, `source-reviewed`, `live-validated`) and enforce “A source-reviewed record cannot claim live-validated behavior.” For dirty claims retain a reproducible safe diff/content reference or downgrade the claim to a verification prerequisite; never publish secrets.
- [X] T004 Define the M0 acceptance rubric in `docs/contracts/acceptance.md`, mapping FR-001–FR-014 and SC-001–SC-004 to expected artifacts and review scenarios; require complete operation coverage, owned/gated prerequisites, resolved M0 findings, and “Publication must contain no orphan command, dependency, decision or acceptance references.” Leave actual results `not-run` until each review occurs.

**Checkpoint**: T001–T004 complete before either story. Documentation conventions
and acceptance requirements are stable; no application infrastructure is needed.

## Phase 3: User Story 1 — Establish Supported Command Contracts (Priority: P1, MVP)

**Goal**: Publish a complete operation inventory and deterministic client behavior,
with real evidence or named prerequisites for every proposed operation.

**Independent review**: Compare every PRD command and resource/operation pair with
the matrix. Each has command grammar, scope, ownership, transport, status, milestone,
and evidence or a dependency. Review unsupported, ambiguous, conflict, partial-success
and completion cases without requiring any daemon implementation.

### Implementation and review

- [X] T005 [P] [US1] Populate `docs/contracts/command-matrix.md` from all PRD families, expanding each resource/operation separately, including specialized and deferred proposals; use all Command Contract fields and quote the status rule “supported-contract, blocked, deferred. Never implies CLI implementation.” Verify exact public schema/resolver/auth/completion evidence at the recorded baseline; distinguish Collection read/list from missing mutation/watch and File read/watch from missing list/upload, preserve CategoryTaxonomy mapping, and exclude controller-only mutations. For local commands cite client design; keep daemon commands explicitly unimplemented until M2. Record draft prerequisite IDs for gaps and scope reasons for deferred entries.
- [X] T006 [P] [US1] Publish `docs/contracts/cli-behavior.md` using `specs/001-m0-contracts/contracts/cli.md`: context/config precedence, identity and registry isolation, transport routing, command grammar, exact GraphQL `-f`/`-F` variable/stdin/conflicting-flag rules, repository remote/ref inference and explicit overrides, format defaults/limitations, semantic authoring/export fidelity, stdout/stderr, NO_COLOR, bounded pagination, non-interactive behavior, exit codes 0–8 and structured errors; distinguish acceptance/admission/reconciliation, correlation-gated waits, safe retries and pending/partial results. Define client choices explicitly and mark unverified server guarantees as prerequisites.
- [X] T007 [US1] Publish `docs/contracts/dependencies.md` from T005/T006 gaps and the DEP-001–DEP-008 research seeds; include missing guarantee, affected CMD IDs, responsible component/role, tracking reference, target milestone, resolution gate and unsupported behavior, using status (`open`, `resolved`, `superseded`). Enforce “Resolution requires linked evidence and review of affected command classifications. An issue marked closed alone is insufficient.” Do not invent assignees or request upstream implementation.
- [X] T008 [US1] Reconcile `docs/contracts/command-matrix.md` with the completed dependency register and CLI policy: enforce “One or more Evidence IDs, or a statement that public evidence is missing.” and “Required for blocked entries.” for dependency references; give every blocked operation a named owner/gate, every deferred entry a scope reason, and every source-backed claim a precise revision/date/limitation. Separate unsupported current behavior from later target scope and eliminate contradictory selectors, formats or wait guarantees.
- [X] T009 [US1] Record the shared-client case reviews in the US1 section of `docs/contracts/acceptance.md`: ambiguous context/remote, absent/stale/proxied discovery, unsupported provider flows and refresh semantics, concurrency conflicts, partial multi-document success, GraphQL errors on HTTP 200, admission pending/failure, stale Ready state, wait timeout, unsupported output format, and Unicode/multiline/final-newline export semantics; log actual paper-review outcomes and remaining prerequisites without claiming executable tests.
- [X] T010 [US1] Complete the US1 coverage review in `docs/contracts/acceptance.md`: derive the expected operation count from the PRD, compare matching matrix rows, inspect evidence/dependency links, and record zero missing/unclassified operations, orphan references or unowned prerequisites for FR-001–FR-006 and the US1 portion of FR-014/SC-001–SC-002; resolve M0 findings before marking this story's review passed.

**Checkpoint**: US1 is a useful standalone M0 increment. Its inventory can support
M1 planning while daemon/runtime design remains incomplete. This does not finish M0.

## Phase 4: User Story 2 — Agree on Daemon and Runtime Boundaries (Priority: P1)

**Goal**: Publish a bounded daemon interface and five decision records with explicit
ownership, privilege, recovery and feasibility gates.

**Independent review**: Use protocol and ADRs to walk through start, repeated start,
disconnect, stop, migration and restart. Each must resolve ownership, observable state,
completion and failure/recovery without running a daemon or claiming platform support.

### Implementation and review

- [X] T011 [P] [US2] Publish `docs/contracts/daemon-protocol.md` from `specs/001-m0-contracts/contracts/daemon.md`: finalize method names/typed parameters, four-byte big-endian UTF-8 JSON framing, handshake/version/capability rules, peer authorization, request/result/error/event envelopes, supported operations, deadlines/cancellation and streaming. Accept or explicitly revise every proposed numeric resource bound with rationale, define integer/null/unknown-field behavior, dedupe fingerprint/key lifetime/quotas and safe expiry, and include all required success/error examples. Preserve Operation state transitions `accepted → running → succeeded | failed | cancelled` and “A request ID identifies one exchange and is not a durable deduplication key.” Document recovery uncertainty without inventing success/termination; implement no parser or service.
- [X] T012 [P] [US2] Publish `docs/ADRs/0002-runtime-ownership.md` defining Rust gs/C gitstored boundaries, embedded libcrun library integration, persisted operation/deployment fields from `specs/001-m0-contracts/data-model.md`, ownership-before-side-effects, same-key replay/conflicts, detached-disconnect behavior, exclusive Git writer and restart discovery, retained volumes and migration safety; enforce “Operation success and deployment readiness are separate fields.” and “Replacement requires observed prior-writer termination and exclusive storage ownership, including after daemon restart.” Record alternatives, memory/process ownership risks, consequences and M2 validation gates.
- [X] T013 [US2] Publish `docs/ADRs/0001-daemon-protocol.md` after T011/T012, accepting the final framing, serialization and C/Rust binding strategy with alternatives, interoperability/memory ownership consequences and parser/fuzz/sanitizer gates; link the canonical protocol instead of duplicating it. Apply Decision Record status (`proposed`, `accepted`, `superseded`), date, evidence, responsible role, linked dependencies and “Accepted design does not mean tested implementation.” Require explicit numeric bounds and retention policy before design acceptance.
- [X] T014 [P] [US2] Publish `docs/ADRs/0003-runtime-assets-packaging.md` after T012: compare concrete maintained image-acquisition candidates and libcrun static/dynamic delivery, dependency/license inventory and update obligations, private-registry credential separation/redirect handling, digest/platform selection, safe extraction, first-use consent versus non-interactive authorization, verified versioned cache, interrupted/offline provisioning and separate binary/download/cache measurements. For unproven choices record candidate versions/source evidence, criteria, owner, bounded experiment and M2 release gate rather than unsupported feasibility claims.
- [X] T015 [P] [US2] Publish `docs/ADRs/0004-rootless-network-volumes.md` after T012: define per-user Linux host prerequisites and unsupported-host behavior, concrete rootless network candidates/CNI comparison, isolated service connectivity, DNS/loopback port publishing, allocation/cleanup/restart ownership, retained local-volume identity and exclusive writer rules; distinguish privileged optional profiles and deferred CSI from payload/LFS protocols, and attach concrete evaluation criteria, owner and M2/M3 gates to untested choices.
- [X] T016 [P] [US2] Publish `docs/ADRs/0005-platform-backends.md` after T012: specify the Linux amd64/arm64 validation matrix, Apple backend versus managed Linux VM evaluation with concrete sources/candidates, image/network/volume/log/health/recovery requirements, installation-time privileges versus no-sudo ordinary operation, and supported-host diagnostics; keep Windows deferred and withhold local macOS support claims until the named experiments and release gates pass.
- [X] T017 [US2] Record all six lifecycle walkthroughs and protocol edge-case reviews in the US2 section of `docs/contracts/acceptance.md` after T011–T016 and US1 acceptance writes: start, repeated start including changed payload/concurrent distinct keys, disconnect, stop, migration and restart. Record preconditions, messages, durable state, ownership, completion and failure/recovery; also inspect incompatible versions, unauthorized peers, malformed/oversized frames, queue exhaustion and expired cursors. Keep Acceptance Evidence results (`not-run`, `pass`, `fail`) honest and distinguish paper review from runtime injection.
- [X] T018 [US2] Reconcile runtime prerequisites in `docs/contracts/dependencies.md` after T017: link protocol/ownership decisions and all remaining packaging/network/platform experiments to affected commands, owner roles, criteria and gates; change dependency status only with linked evidence. Confirm all five ADRs include required Decision Record fields, rejected alternatives and explicit feasibility limits, and resolve missing M0 protocol decisions while retaining valid M2/M3 release gates.

**Checkpoint**: US2 passes when the protocol and decision records resolve the six
walkthroughs and all M0 design questions. Accepted design is not runtime validation.

## Phase 5: Polish & Cross-Cutting Concerns

**Purpose**: Integrate publication, preserve the useful contribution policy, and
verify the complete M0 package without expanding implementation scope.

- [X] T019 [P] Link the published contract index and five ADRs from `README.md`, preserving product-design status and the distinction between intended commands and available releases; do not duplicate contract detail or advertise untested support.
- [X] T020 [P] Review and update `CONTRIBUTING.md` against `.github/workflows/pr-title-lint.yml`: retain accepted Conventional Commit types/optional scopes, metadata-only read-only validation and default-branch activation limits; document deferred application CI and the future Rust/C/sanitizer/compatibility gates, and record policy inspection results in `docs/contracts/acceptance.md` without claiming GitHub-hosted execution or adding workflow jobs.
- [X] T021 Execute the complete `specs/001-m0-contracts/quickstart.md` review procedure and finalize `docs/contracts/acceptance.md` after T019/T020: trace every FR-001–FR-014 and SC-001–SC-004 to published evidence and actual outcome, verify complete inventory counts, all six lifecycle walkthroughs, explicit prerequisite owners/gates, consistent examples and no new lint/application CI; resolve all M0 findings and identify later live/runtime tests separately.
- [X] T022 Finalize review status and provenance in `docs/contracts/README.md` after T021, recheck published cross-links/IDs and local whitespace, and summarize remaining upstream/M2/M3 gates; mark M0 complete only if acceptance evidence passes, with no orphan references, unresolved M0 decisions, upstream modifications or product-release claims.

## Dependencies & Execution Order

### Phase dependencies

1. Setup: T001 → T002.
2. Foundation: T003 → T004; both depend on Setup and block all story work.
3. US1: T005 and T006 may run together → T007 → T008 → T009 → T010.
4. US2 design: T011 and T012 may run together; T013 waits for both. T014, T015
   and T016 wait for T012 and may run together alongside T013.
5. US2 review/integration: T017 waits for T011–T016 and T010; T018 waits for T017.
6. Polish: T019 and T020 follow both story checkpoints and may run together;
   T021 waits for both, then T022.

```text
T001 → T002 → T003 → T004
                      ├─ (T005 ∥ T006) → T007 → T008 → T009 → T010 ─┐
                      └─ (T011 ∥ T012) → T013 ──────────────────────┤
                                  T012 → (T014 ∥ T015 ∥ T016) ─────┤
                                                                T017 → T018
                                                                        ↓
                                                               (T019 ∥ T020)
                                                                        ↓
                                                                    T021 → T022
```

### User story dependencies

US1 has no semantic dependency on US2: daemon entries may cite a pending protocol
prerequisite. US2 protocol and ADR drafting can start after Foundation independently
of US1 and can be reviewed directly against its own criteria. T017 waits for T010
only to serialize edits to the shared acceptance document; T018 likewise updates
the register after US1 has finished its writes. Complete US1 first for the smallest
useful increment, or overlap independent US2 drafting when capacity permits.

### Shared-file safety

Do not parallelize tasks writing `docs/contracts/acceptance.md`, the matrix or the
dependency register. T014–T016 must use the completed ownership ADR. Cross-cutting
polish waits for both stories. `[P]` describes eligible parallel work, not automatic
delegation or a requirement to use agents.

## Parallel Example: User Story 1

After T004, work on these different files concurrently:

```text
T005: docs/contracts/command-matrix.md — operation/evidence inventory
T006: docs/contracts/cli-behavior.md — deterministic shared client behavior
```

Join before T007 so the dependency register includes gaps from both documents.

## Parallel Example: User Story 2

After T004, T011 can draft the protocol while T012 defines ownership. After T012,
the following ADRs use separate files:

```text
T014: docs/ADRs/0003-runtime-assets-packaging.md
T015: docs/ADRs/0004-rootless-network-volumes.md
T016: docs/ADRs/0005-platform-backends.md
```

T013 can run alongside them once T011 is also complete. Join all design work before
T017; never parallelize its acceptance writes with US1's T009/T010.

## Implementation Strategy

### MVP first

Complete T001–T010, then review US1 independently. This delivers command coverage,
source evidence, prerequisites and client behavior for M1 planning. Stop to review
that increment if needed, but do not mark the whole M0 milestone complete.

### Incremental delivery

Add US2's protocol and ADR package, record six walkthroughs, reconcile prerequisites,
then complete T019–T022. No deployment step is appropriate for a documentation milestone.
Later CLI/daemon implementation and release validation require their own feature tasks.

## Coverage Summary

| Requirements / outcomes | Task coverage |
| --- | --- |
| FR-001–FR-002, SC-001 | T003, T005, T007–T010 |
| FR-003 | T005–T006, T008–T010 |
| FR-004 | T007–T010, T018 |
| FR-005–FR-006 | T006, T009–T010 |
| FR-007–FR-008, SC-003 | T011–T013, T017–T018 |
| FR-009–FR-011 | T012–T018 |
| FR-012–FR-013, SC-004 | T020–T021 |
| FR-014, SC-002 | T002–T004, T010, T017, T021–T022 |

**Task counts**: 22 total; Setup 2, Foundation 2, US1 6, US2 8, Polish 4.
Nine tasks carry `[P]`: T005, T006, T011, T012, T014, T015, T016, T019, T020.
