# Tasks: M3 Release and Unblocked Capabilities

**Input**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md),
[data-model.md](data-model.md), [contracts/](contracts/), [quickstart.md](quickstart.md).

**Tests**: Required by FR-008/012/013/015 and SC-001–007. Add meaningful failure
cases before the corresponding implementation; do not claim planned tests ran.
All tasks start unchecked. Paths below are repository-relative; new paths are
intentional implementation targets. Preserve unrelated Postman/user changes.

## Phase 1: Setup

**Purpose**: Establish source/fixture boundaries without changing upstream or publishing.

- [X] T001 Verify current branch/tree, pinned Rust/C toolchains and applicable instructions; record the baseline and scoped validation environment in `docs/contributing/validation/release-qualification.md`, preserving unrelated edits and current M0 evidence.
- [X] T002 Reconcile `specs/004-release-unblocked-capabilities/contracts/capabilities.md` against immutable alpha.9 revision `68f8471edc947e417c98b6e8299a1367ab6b20da` schemas/resolvers and current client code; record per-command source pointers, client versus external gates and source-only evidence in `crates/gs/tests/fixtures/upstream/alpha9-capabilities.json` without editing upstream.
- [X] T003 [P] Define required OS/architecture/channel rows, exact maintained Fedora and Ubuntu/macOS test versions, ABI floors and fixture inputs in `packaging/platforms.json` and `tests/packaging/README.md`; distinguish macOS remote/control from unavailable local execution and identify missing runtime fixtures without disabling unblocked implementation.
- [X] T004 [P] Extend `specs/004-release-unblocked-capabilities/quickstart.md` with disposable config/credential/package-prefix setup and exact existing native-fixture references; document Postman execution outside the sandbox where Keychain access requires it, without using real user credentials or modifying unrelated skill updates.

## Phase 2: Foundational prerequisites

**Purpose**: Shared record formats, compatibility boundaries and isolated validation.

- [X] T005 Define the capability evidence schema and validator in `packaging/schemas/capability-evidence.schema.json` and `scripts/validate-capability-evidence.py`, including command IDs, full upstream SHA, source paths, implementation/result/gate fields and enum `implemented`, `client-work`, `validation-work`, `external-contract`, `deferred`; enforce “Submission and waits may have different classifications” and “Issue closure or introspection alone cannot establish supported operations.”
- [X] T006 Define candidate/artifact/install schemas in `packaging/schemas/release.schema.json`, `packaging/schemas/install.schema.json` and `runtime/packaging/inventory.py`; include formats, tuples, ABI floors, executable/library identities, notices, sizes, source, subject/bundle digests and receipts; enforce “Both executables report the same full version”, “Digest is calculated after all stripping/relocation/packaging” and “The real daemon payload is never a symlink/hardlink.”
- [X] T007 Implement shared SemVer/package encoding helpers in `runtime/packaging/versions.py` and table-driven checks in `tests/packaging/test_versions.py`: canonical/npm `0.1.0-alpha.1`, tag `v0.1.0-alpha.1`, Debian `0.1.0~alpha.1-1`, RPM Version `0.1.0`/Release `0.alpha.1.1`; enforce “Package rebuild suffixes do not alter the reported application/socket version identity” and test alpha.1 < alpha.2 < stable with native comparators in appropriate fixtures.
- [X] T008 Add isolated package/transport harness entry points in `tests/packaging/support.py` and `crates/gs/tests/support/mod.rs`; require explicit disposable roots, synthetic credentials and exact artifact selections, reuse existing daemon/runtime fixture protocols, and reject accidental personal config or credential-store access.

**Checkpoint**: T001–T008 complete. Stories may proceed independently subject to
the explicit dependencies below; this checkpoint does not qualify a release.

## Phase 3: US1 — Install a complete distribution (P1, MVP)

**Goal**: Install both executables and required libraries through every selected
channel, with ordinary-user first-use daemon launch.

**Independent test**: Install unsigned local candidates in clean channel-specific
fixtures; help/remote commands work without runtime setup; qualified Linux starts,
inspects and stops a deployment; macOS rejects unavailable execution actionably.
This story can pass without publishing or implementing US3 additions.

### Tests

- [X] T009 [P] [US1] Add install-layout tests in `tests/packaging/test_layout.py` for both commands, ordinary sibling files, notices/reference/completion inventory, rejected test helpers/images/secrets, private closure and non-default prefixes/paths with spaces.
- [X] T010 [P] [US1] Add installed-startup cases in `crates/gs/tests/daemon_setup.rs` and `tests/packaging/test_launch.py` for public command links, concurrent first launch, UID/socket permissions, explicit socket connect-only, version mismatch, cleared environment and no daemon/download side effects for help or remote commands.

### Implementation and acceptance

- [X] T011 [US1] Implement production staging in `scripts/build-release-candidate.sh` and extend `runtime/packaging/candidate.py` to emit the validated pair/install inventory from exact production builds (`GITSTORED_TESTING=OFF`); preserve pinned native dependencies and prohibit developer helpers, test facilities and workload images in packages.
- [x] T012 [US1] Resolve native library closure in `daemon/CMakeLists.txt`, `daemon/cmake/Libcrun.cmake`, `daemon/cmake/ImageBackend.cmake` and `runtime/packaging/closure.py`; test ordinary, pinned-fd and sealed-memfd re-execution before selecting private static linkage or qualified installed library paths, record licenses/ABI floors, and reject dependence on LD_LIBRARY_PATH or a bundled glibc loader.
- [X] T013 [US1] Adapt `crates/gs/src/daemon_start.rs` and `daemon/src/dependencies.c` to the qualified installed layout while preserving sibling identity/broker validation and user ownership; enforce “Explicit socket selection remains connect-only” and “Running version mismatch returns an actionable error, never automatic killing/replacement”; no PATH-based daemon discovery or root launch.
- [X] T014 [US1] Add generated command reference and shell completions from clap definitions through `crates/gs/src/cli.rs`, `crates/gs/Cargo.toml` and `scripts/generate-cli-reference.sh`; stage output under `share/` and regenerate after US3 grammar changes before final qualification.
- [x] T015 [US1] Generate complete third-party license/source notices and library inventories in `runtime/packaging/notices.py` and `packaging/notices/README.md`; validate obligations for the actual selected static/shared closure rather than assuming Cargo metadata covers C libraries.
- [X] T016 [US1] Implement Debian rendering in `packaging/deb/control.in`, `packaging/deb/rules.json` and `scripts/package-deb.py`, with `/usr/libexec/gitstore/bin` pair, `/usr/bin` links, tested dependency minima and optional user-unit files; privileged installation must not guess a user session, enable linger, create credentials or start workloads.
- [X] T017 [P] [US1] Implement RPM rendering in `packaging/rpm/gitstore.spec.in` and `scripts/package-rpm.py` from the shared staging inventory, with tested Fedora dependency requirements, matching pair/links/user-unit layout and no implicit generic RHEL support or root daemon.
- [x] T018 [P] [US1] Implement platform npm archives in `packaging/npm/package.json.in` and `scripts/package-npm.py`, using scoped platform names, exact os/cpu, bin maps for both real sibling executables and qualified libraries/notices; no registry dependencies, Node wrapper or lifecycle download scripts, and `--ignore-scripts` must work.
- [x] T019 [P] [US1] Implement the generated `Formula/gitstore.rb` target through `packaging/homebrew/gitstore.rb.in`, `scripts/render-homebrew-formula.py` and post-promotion publication (do not commit an installable formula before its matching assets are published) with explicit platform asset URLs/digests, qualified dependencies/private pair and optional service definition; support this repository's explicit Git URL tap, no compilation during ordinary binary install and no automatic `brew services start` hook.
- [X] T020 [US1] Configure first-use packaging defaults and optional user-unit integration in `runtime/packaging/systemd/gitstored.service.in` and `scripts/install-layout.py`; system installation owns code only, initializes no target user's state and provisions no images; test “Global install owns code; first local invocation creates user state.”
- [X] T021 [US1] Add macOS production remote/control packaging to `scripts/build-release-candidate.sh`, `daemon/CMakeLists.txt` and `crates/gs/src/daemon_start.rs`; qualify arm64/x86_64 binaries and expose an actionable unavailable-runtime boundary without weakening Linux ELF validation or advertising an unvalidated Apple/VM backend.
- [X] T022 [US1] Build clean-host install/replace/remove runners in `tests/packaging/run.py` and `scripts/test-packages.sh` for deb/RPM/Homebrew/npm, native prerelease ordering, command links, missing native libraries, absent verifier/tools and mismatch refusal; use disposable alpha.1→alpha.2 fixtures and record actual effects without preservation/rollback guarantees.
- [X] T023 [US1] Extend `.github/workflows/runtime.yml` and `.github/workflows/cli.yml` with package/closure validation for selected tuples and read-only candidate outputs; keep reference native workloads explicitly selected and separate from hosted build checks, and keep all signing/publication credentials out of PR jobs.
- [X] T024 [US1] Run clean Linux amd64/arm64 package lifecycle, offline assets/integrity/consent, fd/memfd startup, logs/recovery and absent-tool/verifier cases using `scripts/test-packages.sh` and `tests/runtime/run.py`; record exact hashes, commands, dependency closure and unsupported combinations in `docs/contributing/validation/release-qualification.md`.
- [x] T025 [US1] Run clean macOS arm64/x86_64 Homebrew/npm remote/control, relocation and unsupported-runtime acceptance via `scripts/test-packages.sh`; record results and separate package support from native runtime support in `docs/contributing/validation/release-qualification.md`; unavailable fixtures remain unchecked acceptance work.
- [X] T026 [US1] Write `docs/installation.md` and update `docs/local-runtime.md` with all four GitHub-hosted install/upgrade/remove paths, first-use daemon behavior, optional consumer verification, exact supported hosts and the upstream early-alpha schema/no-upgrade/no-data-preservation/no-recovery disclaimer with independent-backup guidance.

**Checkpoint**: Installable candidate MVP; unsigned local packages are not an
attested/published release. Missing required tuple evidence blocks qualification.

## Phase 4: US2 — Obtain a verifiable prerelease (P1)

**Goal**: Prepare consistent Release Please prereleases and final-byte signed
provenance while leaving consumer verification optional.

**Independent test**: Fixture history yields alpha.1 then alpha.2; a complete
candidate passes producer verification, altered/missing/wrong-identity cases fail,
and interrupted publication stays draft. Workflow logic can be tested before US3;
final release qualification needs all required US1/US3 receipts.

### Tests

- [X] T027 [P] [US2] Add Release Please fixture/dry-run tests in `tests/release/test_versions.py` for initial alpha.1, subsequent alpha.2, removed bootstrap override, bounded history, synchronized binary/package versions and rejected accidental stable/major bumps.
- [X] T028 [P] [US2] Add candidate state/retry tests in `tests/release/test_promotion.py`: “Failure leaves an incomplete draft”, “Retries must match source/digests; conflicting assets fail rather than replace published bytes”; assert serialization per tag, incomplete matrix rejection and no early tap update.

### Implementation and acceptance

- [X] T029 [US2] Synchronize full prerelease version reporting in `Cargo.toml`, `Cargo.lock`, `daemon/CMakeLists.txt`, `daemon/src/main.c` and package inventory generation; retain numeric CMake project VERSION with a separate full application string and adjust exact version assertions to require both executables agree.
- [X] T030 [US2] Add `release-please-config.json`, `.release-please-manifest.json` and `CHANGELOG.md` using manifest/simple alpha mode, no component tag prefix, recorded bootstrap SHA, alpha.0 seed and explicit first alpha.1 override; implement/test one-time override removal so subsequent proposals advance rather than remain pinned.
- [X] T031 [US2] Implement `.github/workflows/release-please.yml` with commit-pinned reviewed actions, upstream-style GitHub App token, main-only triggers and serialized non-cancelling runs; document required App variables/secrets and permissions in `docs/contributing/releases.md`, without assuming credentials exist or relying on GITHUB_TOKEN events to trigger PR/tag CI.
- [X] T032 [US2] Implement validated candidate/receipt transitions and checksum manifest generation in `runtime/packaging/release_candidate.py` and `scripts/prepare-release.py`: enforce proposed → version-reviewed → built → packaged → validated → attested → producer-verified → published, complete required matrix, reviewed tag/SHA, final executable/package/inventory digests and the mandatory disclaimer.
- [X] T033 [US2] Implement `.github/workflows/release.yml` for reviewed v-tags or dispatch at that exact tag, with production builds/packages from US1, draft prerelease staging, complete-set gates and separate build/sign/publish permissions; preserve existing read-only metadata PR-title checks and exclude untrusted PR artifacts from release promotion.
- [X] T034 [US2] Add commit-pinned `actions/attest` final-subject signing to `.github/workflows/release.yml`, including standard downloadable bundles for binaries/packages/inventory/checksums and generated formula where applicable; use hosted signing with scoped id-token/attestations/artifact-metadata writes, no external registry write and no mandatory installed verifier.
- [X] T035 [US2] Integrate `runtime/packaging/release_verification.py` and `runtime/packaging/release-policy.md` into producer checks, requiring exact CLI repository ID, signer workflow, issuer, source tag/revision and final digests; add `tests/release/test_attestations.py` controls for altered bytes, missing bundles and wrong producer/workflow/ref/revision while retaining optional consumer use.
- [X] T036 [US2] Implement idempotent draft asset upload/promotion and post-promotion tap update through a protected-branch PR/merge queue in `scripts/publish-release.py`; compare expected names/digests, reuse matching assets, reject conflicts without clobber, resume only the same candidate and serialize per tag; test failure between upload/promotion/formula update using mocked/disposable release fixtures.
- [X] T037 [US2] Document release setup, bootstrap removal, package version encodings, producer verification, optional consumer commands, retry/recovery and standard prerelease disclaimer in `docs/contributing/releases.md`; distinguish signed provenance from vulnerability/security claims and exclude external registry publication.
- [x] T038 [US2] Exercise trusted hosted signing/producer verification and release-PR/tag-trigger behavior using `.github/workflows/release.yml` and `.github/workflows/release-please.yml` only when the repository/App and exact reviewed tag are authorized/configured; record an own signed positive plus negative controls in `docs/contributing/validation/release-qualification.md`; missing credentials/authorization leave this acceptance task open, not falsely passed.
- [x] T039 [US2] Prepare the complete release review inventory and readiness report in `docs/contributing/validation/release-qualification.md` after US1 and US3 qualification, including hashes, bundles, support matrix, failed-job/retry evidence and known limitations; stop before creating/merging a public release/tag or publishing assets without explicit publication authorization.

**Checkpoint**: Verifiable release preparation. Actual publication is a separate
authorized operation; pending hosted evidence stays visible and unchecked.

## Phase 5: US3 — Use available upstream capabilities (P2)

**Goal**: Implement seven independently testable client slices from shipped alpha.9
without relaxing externally blocked operations.

**Independent test**: Controlled HTTP/WebSocket fault cases plus pinned shipped
server conformance for each selected operation, synthetic credentials, exact profile
selection and unchanged explicit unsupported behavior for remaining gates.

### Tests

- [X] T040 [P] [US3] Add `crates/gs/tests/alpha9_resources.rs` and fixtures for File ID/path selectors, namespace-free ID reads, legacy rejection, full spec/status/body projections, scoped Namespace/Repository create and pre-side-effect rejection of nonempty body/status/unknown inputs.
- [X] T041 [P] [US3] Add `crates/gs/tests/export.rs` cases for JSON/YAML hydration and writable Markdown round-trips with Unicode/frontmatter/multiline/terminal-newline variants, unsupported projections, table/body presentation separation and bounded pageInfo without snapshot promises.
- [X] T042 [P] [US3] Add controlled subscription cases in `crates/gs/tests/watch.rs` and `crates/gs/tests/support/watch.rs` for protocol/auth, opaque journal versus object cursors, reconnect duplicates, wrong installation/selector/kind, expiry, bounded frames/queues, slow observers, disconnect and unsupported Collection watch; enforce “Expiry requires explicit resnapshot; do not hide event gaps” and “duplicates are allowed and no exactly-once delivery is promised.”
- [X] T043 [P] [US3] Add `crates/gs/tests/auth_remote.rs` cases for explicit static-user login/refresh/remote logout, isolated token persistence, old-JTI consumption/replay, failed writes and uncertain rotation, HTTP-200 payload failures, non-interactive stdin conflicts, secret redaction and unsupported provider/discovery operations.

### Implementation and acceptance

- [X] T044 [US3] Fix `crates/gs/src/resources.rs` alpha.9 File profile dispatch before legacy validation; support exactly one Relay ID or namespace/name, ID without namespace, existing flat legacy restrictions and authorization-safe GraphQL diagnostics; close only the client ID gap rather than inferring File list/upload.
- [X] T045 [US3] Implement schema-verified full core metadata/spec/status/body selections and bounded JSON/YAML read/list output in `crates/gs/src/resources.rs` and `crates/gs/src/output.rs`; preserve table summaries, no File list and no implicit global snapshot while retaining endpoint/proxy/partial-error handling.
- [X] T046 [US3] Extend `crates/gs/src/authoring.rs` and `crates/gs/src/lib.rs` for exact Namespace/Repository create input fields and scope rules; reject nonempty body/controller status/unknown fields before effects, preserve bounded multi-document partial outcomes and never retry mutations blindly.
- [X] T047 [US3] Implement `--authoring -o markdown` grammar and supported writable export projections in `crates/gs/src/cli.rs`, `crates/gs/src/export.rs` and `crates/gs/src/lib.rs`; preserve Product/Category body semantics, reject unrepresentable data/unsupported kinds/formats and do not serialize observed status as author intent.
- [X] T048 [US3] Verify alpha.9 handler protocol, auth and each kind's cursor/bootstrap semantics in `crates/gs/tests/fixtures/upstream/alpha9-watch.json`; add a pinned maintained rustls WebSocket dependency to `crates/gs/Cargo.toml`/`Cargo.lock` and explicit bounded defaults in `crates/gs/src/watch.rs`; do not apply the Namespace sentinel blindly to every kind.
- [X] T049 [US3] Implement typed watch grammar/routing and NDJSON stream output in `crates/gs/src/cli.rs`, `crates/gs/src/watch.rs` and `crates/gs/src/lib.rs` for Namespace/Repository/Product/CategoryTaxonomy/File using exact verified selectors/auth; default to future changes and reject unsupported kinds/formats before connecting.
- [X] T050 [US3] Implement deadline/backoff reconnect and WatchCheckpoint validation in `crates/gs/src/watch.rs`; enforce “Cursors cannot cross installation/selection”, bounded frame/queue pressure, explicit WATCH_EXPIRED and cleanup on interrupt; offer snapshot/bootstrap only for per-kind verified contracts with bounded queued post-bookmark events.
- [X] T051 [US3] Add explicit versioned static-user auth configuration and secure password-file/stdin grammar in `crates/gs/src/config.rs`, `crates/gs/src/cli.rs` and `crates/gs/src/input.rs`; maintain installation/issuer/account/purpose isolation, non-interactive no-prompt rules and no password arguments or competing stdin consumers.
- [X] T052 [US3] Implement public login/refreshToken/remote logout in `crates/gs/src/auth.rs`, `crates/gs/src/credentials.rs` and `crates/gs/src/lib.rs`; atomically persist rotation, report uncertain consumed-token outcomes without retry, retain explicit local logout scope and auth-token-only secret output, and keep arbitrary OIDC/device discovery gated.
- [X] T053 [US3] Implement local profile-derived `api-resources` and `explain KIND.FIELD` in `crates/gs/src/resource_catalog.rs` and `crates/gs/src/lib.rs`; expose exact implemented operation/field availability with source/profile identity and no network/auth/download dependency; arbitrary CRDs remain unsupported.
- [X] T054 [US3] Extend `crates/gs/tests/gates.rs`, `crates/gs/tests/git.rs` and `crates/gs/tests/git_http.rs` to retain pre-side-effect rejection of safe updates, uncorrelated waits, undiscovered auth, Collection mutations/watch, upload/LFS, CRDs/transfer/publication and unqualified runtime profiles; regress library clone/sync, endpoint prefixes and credential isolation.
- [X] T055 [US3] Update existing `postman/specs/` contracts and `postman/collections/gitstore-alpha9/` fixtures for changed HTTP/auth/selectors after reading current repository API skill instructions; model actual GraphQL envelopes rather than invent REST routes, and add `scripts/test-capabilities.sh` to run controlled HTTP/WebSocket and collection checks with synthetic credentials and no cloud publication.
- [X] T056 [US3] Run controlled success/fault scenarios using `scripts/test-capabilities.sh` and pinned Rust checks, including authorization/partial errors, round-trip fidelity, refresh persistence uncertainty and every stream boundary; record actual Postman output and environment limitations in `docs/contributing/validation/remote-cli.md` without converting source review into live proof.
- [X] T057 [US3] Run the selected command/auth/watch/export slices against disposable shipped alpha.9 fixtures using `tests/integration/README.md` and extend those fixtures as needed; record exact source/image identities, proxy paths, reconnect/revoke outcomes and remaining provider/runtime limits in `docs/contributing/validation/release-qualification.md`.
- [X] T058 [US3] Reconcile `docs/contracts/command-matrix.md`, `docs/contracts/dependencies.md` and `docs/contracts/cli-behavior.md` with completed scoped capability receipts; preserve historical baseline, separate client/validation/external gates and leave safe updates/correlation/discovery/true M3 blockers explicitly open.
- [X] T059 [US3] Update `docs/configuration.md`, `docs/README.md` and `specs/004-release-unblocked-capabilities/contracts/capabilities.md` for actual grammar/profile/fidelity/watch/auth behavior; regenerate packaged command reference/completions through `scripts/generate-cli-reference.sh` and rerun affected US1 package checks before final release qualification.

**Checkpoint**: Each enabled slice has controlled and shipped-version evidence;
missing external contracts remain explicit. Full M0–M3 completion is not implied.

## Phase 6: Polish and cross-cutting qualification

- [X] T060 Run Rust fmt/Clippy/test/build with pinned toolchain and required ordinary/sanitized C suites plus changed package/capability checks; record exact commands/results and unavailable cases in `docs/contributing/validation/release-qualification.md`, without repeating expensive unaffected workloads or touching unrelated services.
- [X] T061 Measure help/version and daemon inspect/version on documented reference hosts plus package/library/cache/image sizes using `runtime/packaging/measure.py`; record sample count/p95 and distinct costs in `docs/contributing/validation/release-qualification.md`, retaining the accepted size baseline and deferred optimization scope.
- [X] T062 Audit all selected package/generated-doc/release-note surfaces in `docs/installation.md`, `docs/contributing/releases.md`, `CHANGELOG.md` and `packaging/` for optional consumer verification, early-alpha incompatible upstream schema rationale, no upgrade/data-preservation/recovery guarantees and independent-backup guidance; retain engineering safeguards without turning them into support promises.
- [X] T063 Trace FR-001–017 and SC-001–007 to actual task/evidence results in `specs/004-release-unblocked-capabilities/checklists/requirements.md` and `docs/contributing/validation/release-qualification.md`; mark only completed tasks checked, identify any missing required tuple/provider/hosted positive and do not waive it merely because other stories pass.
- [ ] T064 Execute the applicable `specs/004-release-unblocked-capabilities/quickstart.md` guide, check local links/whitespace and review final release inventory; report implementation versus qualified support and outstanding publication action in `docs/contributing/validation/release-qualification.md`, without tagging, publishing, merging or uploading to external registries as part of task generation.

## Dependencies and execution order

```text
T001–T004 setup → T005–T008 foundation
                     ├→ US1 T009–T026 → installable candidate MVP
                     ├→ US2 T027–T038 → release automation/producer evidence
                     └→ US3 T040–T059 → client capability slices
US1 + US3 + producer evidence → US2 T039 → T060–T064 final qualification
```

T002 and T003 follow the baseline capture T001; T004 may run alongside them.
T005–T008 run in order before story work. Within US1, T011–T015 establish a
shared staging/closure/reference inventory; T016–T019 then render separate formats.
T020/T021 integrate platform startup; T022/T023 provide execution lanes; T024/T025
require their corresponding artifacts/fixtures. US2 tests and configuration can
start after foundation, but T032–T038 consume US1 artifacts. T039 additionally
requires US3 evidence and all required host receipts. Failure to obtain configured
App/hosted acceptance inputs leaves the relevant acceptance task open while other
implementation proceeds. No simulated receipt substitutes for actual own signing.

US3 test files may be authored concurrently; implementation edits sharing cli.rs,
lib.rs or Cargo.lock must be serialized. File ID → projection → creation →
authoring export establishes fidelity before full integration. Watch T048–T050
and auth T051–T052 can be developed in separate modules after grammar ownership
is coordinated; T055–T059 consolidate contracts/validation/documentation.

## Parallel execution examples

- **US1**: T009 layout tests and T010 startup tests touch separate test files.
  After shared staging/closure T011–T015, RPM T017, npm T018 and formula T019
  renderers can proceed in parallel with coordinated Debian T016 changes.
- **US2**: T027 version-history fixtures and T028 promotion/retry fixtures are
  independent. Workflow implementation and verifier integration should serialize
  edits to release.yml, even when unrelated documentation can proceed separately.
- **US3**: T040 resources, T041 exports, T042 watches and T043 auth test suites
  can be authored independently. Stream and auth module implementation can run
  in parallel only when shared CLI/dispatch and lockfile edits are coordinated.

`[P]` means the task may run beside the other tasks in that stated wave after
its prerequisites pass; it does not authorize concurrent edits to shared files
or mandate additional agents.

## Implementation strategy

Deliver the US1 installable candidate MVP first: complete shared foundation,
package both executables, resolve actual library closure and prove first-use
ordinary-user behavior. Add US2 automation and US3 client slices incrementally;
validate each independently before integrating the final release. If following
the plan's capability-first preference, US3 can precede the US1 implementation
wave after foundation; story grouping remains unchanged. All three stories are
required for the full feature. Do not stop at building package definitions or
shipping only the currently available architecture.

Final release readiness requires actual receipts, not completed templates.
Externally blocked API capabilities and deferred Nix/Windows/CSI/backend work
are not hidden implementation tasks. Prepare a concrete reviewable release;
actual public mutation still requires the user's publication authorization.
