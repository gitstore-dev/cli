# Tasks: M1 Remote CLI — Unblocked Commands

**Input**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md),
[data-model.md](data-model.md), [contracts/](contracts/), [quickstart.md](quickstart.md).

**Status**: Unblocked development implementation complete. Source paths below are relative
to repository root. See `docs/m1-status.md` for evidence and remaining release gates.

**Tests**: Required by FR-016/017 and the constitution. Write behavior scenarios before
implementation; verify meaningful failures, then implement. Controlled HTTP tests run
the actual CLI against the Postman simulation; local adapter tests do not claim live
or native-store evidence. Do not modify the upstream working tree.

## Phase 1 — Setup

**Goal**: A reproducible executable target and checks without any runtime daemon.

- [X] T001 Create `Cargo.toml`, `crates/gs/Cargo.toml`, `crates/gs/src/{main,lib}.rs`, `rust-toolchain.toml` and `Cargo.lock` for one Rust 2024 package pinned to 1.96.0; resolve the plan's dependencies/features including git2/libgit2 and packaged TLS, check advisories/licenses, and ensure `.gitignore` covers build and secret fixtures. Commit no production credentials or runtime assets.
- [X] T002 [P] Build the isolated CLI-process fixture harness in `crates/gs/tests/support/mod.rs` with temporary config/Git directories, synthetic secrets and a credential adapter; clear ambient selection/input variables so tests cannot consume real account state.
- [X] T003 [P] Add `.github/workflows/cli.yml` alongside the executable target for locked build/test, rustfmt and clippy on Linux x86_64 and macOS arm64, using read-only permissions and the pinned toolchain; preserve metadata-only PR-title validation and exclude release publication/attestation credentials.
- [X] T004 [P] Record fixed-revision contract source hashes, minimal nonsecret fixtures and exact selectors/required/null/default/enum constraints in `crates/gs/tests/fixtures/upstream/README.md` and fixtures derived from GitStore `b6c866421dbcf43994565f847553c1f6aec83994`; include `repositories`, nullable create payloads, delete outcomes and separate GraphQL/Git routes, retaining applicable attribution.

**Checkpoint**: Workspace builds; fixture harness cannot read user config or secrets;
CI validates executable behavior rather than Markdown.

## Phase 2 — Foundation

**Goal**: Shared command/input/output/error boundaries before user stories.

- [X] T005 Implement `crates/gs/src/error.rs` with M0 exit categories 1–8, safe structured fields and submission certainty; add unit cases for unknown outcome, missing credentials, ambiguity, conflict and unsupported gates without token/request-header leakage.
- [X] T006 Implement `crates/gs/src/output.rs` for stdout results and stderr diagnostics, applicable table/JSON/YAML summaries, JSON machine diagnostics, raw-token-only output and NO_COLOR/non-TTY behavior; reject unsupported formats before side effects and never serialize secret types.
- [X] T007 Implement bounded UTF-8/duplicate-detecting JSON/YAML utilities in `crates/gs/src/input.rs`: quote/enforce `16 MiB total UTF-8 input`, `100 documents`, `at most 64 levels`, `at most 1,000 references` and `16 MiB expanded content`; disable YAML includes/interpolation/tag execution, reject duplicate keys and multiple stdin consumers, and test limits/malformed input before network code exists.
- [X] T008 Define clap grammar and fail-before-side-effect capability gates in `crates/gs/src/cli.rs` with tests in `crates/gs/tests/gates.rs`; implement `--help`/`--version`, selectors, config/protected-input flags, `--non-interactive`, positive `--timeout`, explicit name-vs-`--id` modes and excluded-operation DEP references per `contracts/cli.md`. Reject unknown spelling as usage and subscriptions/known waits as unsupported.

**Checkpoint**: Local parsing and error/output rules pass without networking/daemon.

## Phase 3 — US1: Select an installation safely (P1)

**Goal**: Usable local configuration/identity inspection independent of upstream gaps.

**Independent test**: Two temporary contexts/accounts, no network or daemon; prove
selection precedence, credential isolation, atomicity and intentional token output.

- [X] T009 [US1] Write CLI cases in `crates/gs/tests/local.rs` for list/show/use/validate, local auth status/logout/switch/token, flags/env/context precedence, proxy-prefix isolation, unknown versions, unsafe credential files, interrupted/concurrent config writes and no network calls; assert prior selection survives rejected changes and only `auth token` exposes synthetic secret text.
- [X] T010 [US1] Implement strict version-1 TOML config and OS default/explicit path discovery in `crates/gs/src/config.rs`; enforce `name, endpoint, account and account issuer are nonempty`, unique selected references, `unknown config fields and unsupported config versions fail`, URL userinfo/query/fragment rejection and explicit loopback-only HTTP.
- [X] T011 [US1] Implement effective selection and field-source reporting in `crates/gs/src/config.rs` using `flags > environment > selected context > defaults`; normalize endpoint without losing proxy prefix, reject contradictory identities, and prevent endpoint overrides inheriting another installation's profile/Git endpoint/credentials.
- [X] T012 [US1] Implement config selection mutation in `crates/gs/src/config.rs` with exclusive lock/reread, same-directory user-only temporary write, flush/atomic rename and directory sync where supported; preserve other contexts and prior valid state on injected failures and concurrent switches.
- [X] T013 [US1] Implement `crates/gs/src/credentials.rs` with zeroizing non-Debug secret types, canonical compact JSON-array keys under `dev.gitstore.gs`, installation/issuer/account/purpose isolation, explicit file/stdin overrides and no automatic fallback; enforce regular user-owned non-symlink files with no group/other permission bits and one optional token terminal line ending.
- [X] T014 [US1] Add Apple Keychain and Linux Secret Service adapters in `crates/gs/src/credentials.rs`; distinguish absent/unavailable/denied and keep Git material separately scoped. Add opt-in native-store smoke cases in `crates/gs/tests/native_credentials.rs` using only disposable synthetic entries and record host results without claiming adapter mocks validate OS integration.
- [X] T015 [US1] Wire context/config/auth local commands through `crates/gs/src/lib.rs` and `cli.rs`; status reports local presence with `verified=false`, auth switch changes only the selected context's account, and token rejects all output flags. Local logout removes only the selected CLI-owned stored credential and reports remote invalidation unsupported; it rejects explicit file/stdin overrides without deleting external files. No provider login/refresh/revoke, network verification or daemon call.
- [X] T016 [US1] Document the secret-free config bootstrap, protected-file/stdin use and native-store entry identities in `docs/configuration.md`; add nonsecret `examples/config.toml`, explain user-owned storage and separate Git endpoint/auth, and run the US1 validation section of `specs/002-m1-remote-cli/quickstart.md`.

**Checkpoint**: US1 is a working local MVP. Credential-provider/discovery gates stay open.

## Phase 4 — US2: Explicit operations without the Admin UI (P1)

**Goal**: Public GraphQL, core inspection and supported create/delete requests.

**Independent test**: Explicit temporary config/protected input plus compatible
controlled HTTP responses; exercise actual CLI requests, not only transport stubs.

- [X] T017 [US2] Load Postman bootstrap/schema/mocking/simulation skills, inspect CLI/local workspace context and reusable owning-team artifacts, then prepare local `postman/` requests/examples/mocks/simulation and a context file from T004's authoritative GraphQL contract. Include success, partial errors, 401/403, 404, 429, timeout, malformed/oversized response and disconnect; publish nothing and bind no unrelated workspace.
- [X] T018 [US2] Write request/response process cases in `crates/gs/tests/graphql.rs` for field encoding, typed/file/stdin inputs, duplicate keys, operation selection, subscriptions, format rejection, partial errors, deadline/retry policy and redirect isolation using T017 fixtures; assert zero mutations on preflight failures.
- [X] T019 [US2] Implement shared bounded reqwest transport in `crates/gs/src/graphql.rs` with explicit full public endpoint, no redirects, bearer identity isolation, `30-second default command deadline`, connect timeout at most 5 seconds/remaining budget, `16 MiB` response cap and at most two additional transient read attempts. Honor Retry-After only within budget and never retry mutations or change transport/credentials.
- [X] T020 [US2] Implement M0 GraphQL envelope grammar in `crates/gs/src/graphql.rs` and `cli.rs`: literal -f, typed/file -F, integers within ±(2^53−1), complete --input envelope, duplicate/stdin/conflict rejection and AST-based selected operation classification; preserve full data/errors/extensions on stdout with precise nonzero error categories and safe stderr.
- [X] T021 [US2] Write typed read/list/delete-selector contracts in `crates/gs/tests/resources.rs` for all six included kinds from T004; prove exact namespace/name/ID mappings, wrong-kind rejection, `repositories` spelling, File read-only using the verified namespace/name selector (normalization to `by` awaits the upstream File lifecycle spec), opaque cursors, no snapshot claim and `first defaults to 100` within `1..=100`.
- [X] T022 [US2] Implement checked-in public query selections/wire DTOs and read/list dispatch in `crates/gs/src/resources.rs`; include real identity/metadata/summary/body fields, require namespace for namespaced kinds, return one bounded page with pageInfo, and emit table/JSON inspection without fabricating hydration or enabling gated export.
- [X] T023 [US2] Write authoring validation/body cases in `crates/gs/tests/authoring.rs` and `tests/fixtures/documents/` for Product/CategoryTaxonomy API version, required title/name/resolved namespace, exact nested inputs, Unicode/newline fidelity, unknown/controller-owned fields, conflicting namespace and body separators; cover all-batch preflight, 100/101 documents and the 16-MiB boundary.
- [X] T024 [US2] Implement strict author projections and ordered batch submission in `crates/gs/src/authoring.rs`: one Markdown document per repeated -f, exact raw body slice, permitted metadata name/namespace/labels/annotations, reference author fields only, Product/Category spec fields per upstream contract and lifecycle ACTIVE/RETIRED; validate whole batch first, stop on first remote failure, mark remainder not-submitted and aggregate first nonzero exit.
- [X] T025 [US2] Implement supported delete lookup/request handling and truthful create/delete outcomes in `crates/gs/src/resources.rs` and `authoring.rs`; mutate only exact resolved ID, preserve server `TERMINATION_STARTED`/`ALREADY_TERMINATING`, null/error/partial-data cases and known revision, never auto-cascade or infer cleanup/readiness, and mark lost responses unknown rather than not-submitted.
- [X] T026 [US2] Add `scripts/test-remote.sh` to launch/reuse the local Postman simulation, wait for actual readiness, drive the built CLI and assert request traces/results, including failure scenarios; clean up only owned processes, keep synthetic secrets out of artifacts and add this meaningful controlled suite to `.github/workflows/cli.yml` after verifying installed CLI support.
- [X] T027 [US2] Extend `crates/gs/tests/gates.rs` and simulation assertions for excluded high-level commands, Namespace/Repository create, unsupported fields, resource YAML/Markdown, watches and waits; assert exact DEP/error category and zero writes/no fallback, including unsupported GraphQL subscriptions and multiple stdin consumers.
- [X] T028 [US2] Add remote examples and outcome/format/compatibility limits in `docs/remote-cli.md`, wire US2 dispatch in `crates/gs/src/lib.rs`, and run the controlled US2 quickstart cases; distinguish simulated success from live admission/provider validation and keep all named gates open.

**Checkpoint**: US2 commands work with existing credentials/resources; no claim of
full catalogue-bootstrap, hydrated export, high-level update or login support.

## Phase 5 — US3: Clone and submit through Git (P2)

**Goal**: Agreed remote naming and safe submission-only sync via public Git.

**Independent test**: Disposable repositories and a compatible public Smart HTTP
fixture; inspect actual remote configuration, refs, library callbacks and outcomes.
Run the built CLI without any Git executable available; fixture setup uses library APIs.

- [X] T029 [US3] Write disposable Git cases in `crates/gs/tests/git.rs` for default/overridden remote, branch tracking, empty clone, nonempty destination, subdirectory sync, no-op/ahead/behind/divergent/dirty, detached/explicit ref, ambiguity, misleading name, pushurl/URL rewrites, mismatched endpoint and concurrent advancement; assert unchanged local history/worktree on failure.
- [X] T030 [US3] Implement the git2/libgit2 adapter and target resolution in `crates/gs/src/git.rs`, with packaged native/TLS dependencies, no Git executable wrapper, scoped public Git root with encoded namespace/repository segments, validated effective fetch/push URLs and full branch refs; require unambiguous remote/upstream selection and never fall back to context-default repository or choose by name.
- [X] T031 [US3] Implement separate Git identity/material using scoped Basic/Bearer credentials in the libgit2 smart-HTTP adapter backed by reqwest in `crates/gs/src/git.rs` and `credentials.rs`; never put secrets in URLs/persistent config, disable external helper/filter/hook execution and ambient auth/header overrides, reject redirects, preserve TLS verification, forbid prompt/browser fallback and verify transport cancellation/deadline behavior. Basic uses actual username/password, never a session token substituted as password.
- [X] T032 [US3] Implement clone in `crates/gs/src/git.rs` with explicit public endpoint/profile/auth, `gitstore` default or valid --remote override through library clone/remote APIs, default destination, no extra alias, checked-out branch tracking when present and nonempty destination rejection; keep clone outcome independent of admission/readiness.
- [X] T033 [US3] Implement submission-only sync in `crates/gs/src/git.rs`: validate clean worktree/ref/target, fetch into remote-tracking ref, compare ancestry, report equal as no submission, normal fast-forward push only, and conflict for behind/divergent/concurrent advancement; no local branch/worktree edits, force retry or alternate write transport.
- [X] T034 [US3] Add public Smart HTTP/auth fixtures and process checks in `crates/gs/tests/git_http.rs` plus `tests/fixtures/git/` for separate/proxied endpoint, Basic/Bearer, 401/403, hostile ambient Git config, credential non-leakage, mismatched push destination and no Git/helper/filter/hook process execution; distinguish filesystem-only ancestry checks from verified HTTP behavior and add applicable checks to CLI CI.
- [X] T035 [US3] Finish Git result/progress/deadline handling in `crates/gs/src/git.rs` and dispatch in `lib.rs`; send only structured results to stdout, relay/flush library progress callbacks to stderr, check per-ref push status rather than transport success alone, reject known waits before pushes, retain commit/submission uncertainty on lost response and report pending/unverified admission/readiness without streaming guarantees. Add corresponding process cases to `tests/git_http.rs` and run US3 quickstart.

**Checkpoint**: Git path meets the agreed clone convention and submission contract;
DEP-004/016 remain open; all commands work without a Git executable. Test fixtures
use library APIs, and no external credential helper/filter/hook runs during Git operations.

## Phase 6 — Cross-cutting validation and documentation

- [X] T036 [P] Update `README.md`, `CONTRIBUTING.md`, `docs/remote-cli.md` and `docs/m1-status.md` to distinguish implemented subsets, setup prerequisites, output restrictions and unresolved full-M1/release gates; do not relabel all M0 support records as implemented or advertise untested architectures/providers.
- [X] T037 [P] Add an opt-in live compatibility harness in `scripts/test-live.sh` plus disposable-fixture instructions in `docs/m1-status.md`; require explicit test-installation/account selection, operate only on fixture-owned resources and record live checks as not-run when access is absent. Do not run against an unrelated deployment or treat simulated fixtures as live evidence.
- [X] T038 Run locked build/test, format/lint and cross-story isolation/input/partial-outcome/Git scenarios from `specs/002-m1-remote-cli/quickstart.md`; record compiler/Git/host/native-store/simulated/live evidence and SC-001–006 outcomes in `docs/m1-status.md`, repair failures and remove any secret-bearing temporary output.
- [X] T039 Reconcile requirements/tasks/evidence and remaining gates in `specs/002-m1-remote-cli/tasks.md` and `docs/m1-status.md`; record DEP-013 producer/workflow/source/digest and negative-case requirements as unresolved publication gates rather than adding an unverified release. Mark only completed work checked; full M1 remains pending upstream capabilities.

## Dependencies & Execution Order

```text
T001 → T002/T003/T004 → T005 → T006 → T007 → T008
    → US1 T009–T016
    → US2 T017–T028
    → US3 T029–T035
    → T036/T037 → T038 → T039
```

- T002–T004 can proceed after the workspace skeleton exists; T004 is authoritative
  input for request/authoring mappings. Shared file mutations run sequentially.
- US1 is the MVP; stop for its verification checkpoint before adding remote behavior.
- US2 and US3 both consume US1's production selection/credential boundary. Their
  behavior fixtures can use explicit temporary config independently; no US2 mutation
  or Namespace/Repository create is required to demonstrate US3.
- T017 artifacts are prepared before the real HTTP tests. T019 transport precedes
  GraphQL/resources/authoring dispatch. Test scenarios precede each behavior change.
- T030's target resolver precedes auth and clone/sync; T034 adds HTTP evidence beyond
  T029's repository-state fixtures. No Git integration is considered proven from only
  filesystem transport.
- T036/T037 can run after all user stories without conflicting files except the final
  evidence log; serialize evidence-log writes. T038 validates before T039 reconciles.

## Parallel Execution Examples

The `[P]` marker identifies eligible file-separated work, not authorization to spawn
agents. Agent use follows the session's delegation instructions.

- **Setup**: after T001, fixture harness T002, CI T003 and source inventory T004 touch
  separate outputs and may proceed together.
- **US1**: after the credential interface in T013 stabilizes, documentation drafting
  for T016 may proceed alongside native-store smoke fixtures in T014; integrate after
  wiring T015. No concurrent edits to `credentials.rs`.
- **US2**: resource contract test drafting in T021 and authoring fixture drafting in
  T023 use separate files once T004 is available. Implement shared dispatch sequentially.
- **US3**: after target/auth interfaces stabilize, Git HTTP fixture preparation in
  T034 can proceed alongside clone/sync test expansion in T029; adapter edits serialize.

## Implementation Strategy

Start with the locally usable US1 MVP and its meaningful CI. Add explicit GraphQL
and one verified resource path before broadening to the remaining included kinds.
Then add Git clone before sync. Preserve the boundaries for each checkpoint; adding
blocked commands to make a larger demo pass is not part of this feature.

Implementation checkboxes track delivered code and applicable local/controlled checks;
ignored native smoke checks and live/hosted evidence remain explicitly unverified. Publication is not
an unblocked implementation task: DEP-013/ADR 0003 and host/compatibility evidence
must pass before a separate trusted release workflow promotes binaries. Full M1
still needs upstream/login/export/wait/watch/update capabilities listed in the spec.

## Requirement Coverage

| Requirements | Tasks |
| --- | --- |
| FR-001–004 | T008–T016 |
| FR-005 | T007, T017–T020 |
| FR-006 | T004, T021–T022 |
| FR-007–009 | T004, T023–T025 |
| FR-010 | T006, T008, T020, T022, T025, T027 |
| FR-011–012, FR-018 | T029–T034 |
| FR-013 | T005, T025, T027, T035 |
| FR-014–015 | T005–T008, T018–T020, T026–T027, T035 |
| FR-016–017 | T001–T004, T009, T014, T016–T018, T021, T023, T026–T029, T034–T039 |

## Implementation adjustments — 2026-10-29

The built-in libgit2 redirect policy permits same-host redirects even with
`RemoteRedirect::None`. T031/T034 therefore use `crates/gs/src/git_http.rs`, a small
reqwest HTTP subtransport for libgit2's maintained smart protocol. It rejects all
redirects, scopes in-memory Basic/Bearer material, streams responses, spools outbound
packs to a temporary file, and uses the remaining command deadline. No Git protocol
or pack implementation was duplicated, and no external Git executable is used.

The Linux generic keyring adapter auto-unlocks locked entries. Production lookup/removal
uses the maintained `dbus-secret-service` API directly, never invoking an unlock prompt;
macOS disables Keychain user interaction. Native smoke checks are opt-in and not-run.
