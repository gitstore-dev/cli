# Implementation Plan: M1 Remote CLI — Unblocked Commands

**Branch**: Existing `main` checkout | **Date**: 2026-10-29 | **Spec**: [spec.md](spec.md)

**Feature directory**: `specs/002-m1-remote-cli`. Spec Kit reports the feature identifier
as its branch value; the actual Git branch remains `main`. No branch switch is needed.

## Summary

Deliver a Rust `gs` CLI in independently testable increments: local selection and
credential inspection, public GraphQL/core resource operations, then Git clone and
submission-only sync. The specification's scope table controls the included subsets;
no open upstream dependency is silently closed. Full M1 and release publication remain
subject to their existing gates.

This plan was prepared before implementation. The unblocked development subset now
has executable code; task state and `docs/m1-status.md` record actual evidence.

## Technical Context

**Language/Version**: Rust 2024 edition; pin Rust 1.96.0 initially (available locally)
and declare the same compiler baseline in Cargo/toolchain/CI. Reassess the minimum
supported compiler separately rather than claiming an untested older version.

**Primary Dependencies**: clap 4 for CLI/env parsing; serde/serde_json and TOML for
configuration/envelopes; reqwest 0.13 blocking client with rustls; url for normalized
endpoints; serde-saphyr 2 for bounded YAML frontmatter and non-resource YAML summaries;
cynic-parser for GraphQL operation selection; git2 0.21/libgit2 for in-process Git
clone/fetch/push and repository inspection; keyring-core with native Apple Keychain and direct DBus
Secret Service access on Linux (no automatic unlock prompt); zeroize for secret buffers; tempfile for atomic file replacement
and fixtures. Verify exact releases, licenses and features and commit Cargo.lock in T001.
No C daemon, generated OpenAPI client, browser login or watch runtime.

**Storage**: User-owned TOML config with atomic selected-context/account persistence;
OS secret stores by default; explicit protected credential-file override. Bootstrap,
identity keys and permissions are defined in [contracts/config.md](contracts/config.md).

**Testing**: cargo unit/integration tests and CLI process tests using isolated config,
credential adapters and disposable repositories. Postman-backed HTTP simulation exercises
the real client for request mapping and upstream failure cases; see research and
quickstart. No production credential or unrelated live resource is needed in CI.

**Target Platform**: Remote CLI on Linux and macOS. Test x86_64 Linux and arm64 macOS
initially; add other architecture evidence before advertising it. Linux Secret Service
requires an available user session; explicit file input supports headless environments.
Windows and daemon/backend support are out of scope. All CLI commands work without
a separately installed Git executable. Package libgit2 and the reqwest/rustls HTTP adapter dependencies
with the CLI; inspect linkage, licenses and supported-host behavior during T001/T030.
No runtime assets are downloaded.

**Project Type**: Cargo workspace with one `gs` binary/library package under `crates/gs`.
The workspace leaves room for future packages without adding speculative crates now.

**Performance Goals**: Local commands do no network I/O; bound input to 16 MiB/100
documents and list pages to at most 100. Apply a 30-second default command deadline,
overridable by `--timeout`; reads have at most two transient retries within that same
budget. Bound HTTP response memory to 16 MiB and redact diagnostics. No latency or
binary-size measurement is claimed before implementation.

**Constraints**: Public interfaces only; explicit endpoint/profile configuration while
discovery is unavailable; credentials isolated by installation/issuer/account, with
separate Git authentication. No implicit updates, waits, exports or daemon access.
No application CI is added during this planning-only pass.

**Scale/Scope**: 30 M0 command records or restricted subsets across three user stories;
blocked options remain explicit errors. Core mappings target the fixed M0 schema
snapshot, subject to the contract verification task rather than guessed latest behavior.

## Constitution Check

Reviewed before research and after design against constitution v1.0.0.

| Principle / gate | Before | After | Enforcement |
| --- | --- | --- | --- |
| I. Public contract boundaries | PASS | PASS | GraphQL resources, public Git front door, no internal transport; snapshot-derived operations. |
| II. Automation and truthful outcomes | PASS | PASS | Preflight selection/format validation, structured errors, submission certainty and distinct states. |
| III. Credential isolation | PASS | PASS | OS stores, explicit file fallback, proxy-prefix identity, separate Git credentials and scoped injection. |
| IV. Recovery and safe state | PASS | PASS | Atomic config selection, no local history edits, uncertain writes are never auto-retried. No daemon state introduced. |
| V. Evidence and verification | PASS | PASS | Local/simulated/live/release evidence separated; behavior CI arrives with code. |
| Rust/C and independent delivery | PASS | PASS | Rust remote CLI only; no runtime dependency or provisioning. |
| Contribution / provenance | PASS | PASS | Existing PR-title policy retained; binary publication held behind DEP-013. |

No constitutional exceptions. Design PASS is not an implementation or release pass.

## Project Structure

```text
specs/002-m1-remote-cli/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/requirements.md
├── contracts/
│   ├── cli.md
│   ├── config.md
│   └── upstream.md
└── tasks.md

# Planned implementation outputs
Cargo.toml
Cargo.lock
rust-toolchain.toml
crates/gs/
├── Cargo.toml
├── src/
│   ├── main.rs
│   ├── lib.rs
│   ├── cli.rs
│   ├── config.rs
│   ├── credentials.rs
│   ├── error.rs
│   ├── output.rs
│   ├── input.rs
│   ├── graphql.rs
│   ├── resources.rs
│   ├── authoring.rs
│   ├── git.rs
│   └── git_http.rs
└── tests/
    ├── local.rs
    ├── graphql.rs
    ├── resources.rs
    ├── authoring.rs
    ├── git.rs
    └── fixtures/
        ├── upstream/
        └── documents/
postman/                      # Local contract requests/mocks/simulation at implementation
scripts/test-remote.sh         # Run the actual gs CLI through the simulation
.github/workflows/cli.yml      # Build/format/lint/test when the workspace exists
```

**Structure Decision**: One package with modules, not a framework or service SDK.
Use explicit checked-in queries and wire DTOs, preserving unknown server extensions
where required. Tests verify actual CLI/transport behavior, not just helper functions.

## Phase 0 — Research

[research.md](research.md) resolves storage, transport, parser, Git and validation
choices. Committed upstream Git objects supply schema evidence; inspect no secret
configuration and do not base behavior on the upstream dirty working tree.

One research agent checks selectors, body mappings, Git routes and authentication as
required by the Spec Kit plan workflow. No application work is delegated in this pass.
Official dependency documentation provides technology-choice evidence; exact dependency
pins and advisory checks happen with Cargo.lock during implementation.

## Phase 1 — Design

[data-model.md](data-model.md) defines context, identity, selection, documents and
outcomes. [contracts/cli.md](contracts/cli.md) defines command grammar and preflight;
[contracts/config.md](contracts/config.md) supplies the missing bootstrap path;
[contracts/upstream.md](contracts/upstream.md) pins public operations and transport
limits. [quickstart.md](quickstart.md) is a future validation guide, not proof of runs.

## Implementation Sequence

1. Scaffold the Cargo workspace, CLI/error/output boundary and meaningful Rust CI.
2. Deliver US1: local context/config/credential inspection using temporary fixtures.
3. Deliver US2: bounded GraphQL transport, core inspection, Product/Category authoring
   and supported deletes. Validate explicit contract mappings and failure behavior.
4. Deliver US3: library-based clone/sync and scoped Git credential handling, tested
   against disposable repositories/public Smart HTTP.
5. Record supported subsets and actual evidence, run cross-story scenarios, and keep
   public release promotion disabled until provenance/host gates are met.

## Requirement Traceability

| Requirements | Design | Validation |
| --- | --- | --- |
| FR-001–004 | config contract, identity model | US1 local/offline/isolation/atomicity/keychain tests |
| FR-005–006 | CLI and upstream contracts | US2 request/response/pagination/error scenarios |
| FR-007–009 | author-document and batch model | US2 field validation/body fidelity/partial-write cases |
| FR-010, 014–015 | CLI output and capability gates | Process-level stdout/stderr/exit/no-side-effect checks |
| FR-011–012, FR-018 | Git library contract | US3 clone/tracking/ambiguity/conflict/no-executable tests |
| FR-013 | operation-outcome model | GraphQL/push response-loss, unsupported-wait and state assertions |
| FR-016–017 | quickstart, provenance decision | CI, controlled/live evidence and release-gate records |

SC-001 maps to US1; SC-002 to US2/US3 controlled installation runs; SC-003 is
cross-cutting; SC-004 maps to US3; SC-005 to all writes; SC-006 to final evidence.

## Complexity Tracking

No exceptions. Public GraphQL schema is the source of truth; runnable Postman
requests/mocks are derived verification artifacts, not a second API specification.
Do not initialize or publish an unrelated cloud workspace during planning.
