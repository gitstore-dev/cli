# Implementation Plan: M3 Release and Unblocked Capabilities

**Branch**: `codex/m3-release-unblocked-capabilities` (implementation checkout) | **Date**: 2026-11-08 | **Spec**: [spec.md](spec.md)

**Input**: `specs/004-release-unblocked-capabilities/spec.md`. Setup reports
`004-release-unblocked-capabilities` as a feature identifier, not the actual branch.

## Summary

Deliver an installable `v0.1.0-alpha.1` distribution containing matching Rust gs
and C gitstored through GitHub Releases, a GitHub-hosted Homebrew tap, Debian/RPM
packages and npm-format platform tarballs. Configure first-use user-daemon launch,
publish signed GitHub Actions provenance and retain optional consumer verification.
Release Please manages reviewed alpha release proposals.

Integrate the shipped upstream capabilities enumerated in
[contracts/capabilities.md](contracts/capabilities.md): File ID reads,
Namespace/Repository creation, full core projections/exports, typed watches,
explicit static-user authentication, and versioned built-in resource help.
No speculative M3 endpoints. Release qualification is implementation work;
unavailable optional contracts gate only affected operations.

## Technical Context

**Language/Version**: Rust 1.96.0/edition 2024; C17; CMake >=3.28; Python 3.11+
packaging tools; Ruby formula and npm JSON metadata.

**Primary Dependencies**: Existing clap/serde/git2/rustls/keyring stack, libcrun,
libslirp, image SDK, SQLite and native crypto/network libraries. Reuse immutable
CMake dependency sources. Add a maintained rustls-compatible WebSocket client
for typed subscriptions, with bounded frames and explicit protocol negotiation.
No Git executable, custom container engine or new backend service.

**Storage**: Existing per-user config/credential store, daemon SQLite state,
volumes and cache; file-based release inventories/attestation bundles. No new DB.

**Testing**: Pinned Rust fmt/Clippy/test/build, CTest/ASan/UBSan, controlled
HTTP/WebSocket and Postman contracts, existing native runtime harness, clean
package fixtures and hosted producer attestation checks. Postman Keychain access
fails inside this session's sandbox with `SecItemCopyMatching failed -50`; the
same read-only status check succeeds outside it. Use approved unsandboxed macOS
execution where needed or the Linux contract lane; no Keychain reset is required.

**Target Platform**: Qualify Linux x86_64/arm64 packages (Ubuntu 24.04 baseline
and a selected supported Fedora fixture) and macOS arm64/x86_64 remote/control
packages. macOS workload execution stays unavailable until its backend gate
passes. Windows/Nix deferred. Package format alone proves no distro support.

**Project Type**: CLI, local daemon and release automation.

**Performance Goals**: Preserve PRD help/version p95 <100ms, daemon inspect/version
<250ms on recorded hosts; lists <=100/page; bounded streams. Report package,
library and image bytes separately. Prior accepted binary-size baseline remains;
optimization is deferred.

**Constraints**: No external registry publication; no root runtime or end-user
toolchain; no mandatory consumer verifier. Upstream early-alpha schemas change
incompatibly: no upgrade, data-preservation or recovery guarantees. Continue
engineering/test safeguards without turning them into public guarantees.

**Scale/Scope**: Four package channels, four OS/architecture tuples, one version
train; M0–M3 family classification and seven independent client capability slices.

## Constitution Check

*Before research and after design: pass with explicit user prerelease policy recorded.*

| Principle | Design | Result |
| --- | --- | --- |
| Public contracts | Immutable alpha.9 evidence; GraphQL resources/library Git/daemon lifecycle; no speculative endpoints | Pass |
| Truthful automation | Explicit profiles, structured errors, correlated waits only; no silent mutation retry | Pass |
| Least privilege | Installed sibling pair, cleared environment, private user socket/state; no root daemon | Pass |
| Durable operations | Writer exclusion/cleanup checks remain engineering requirements; no preservation guarantee | Pass with user policy |
| Evidence | Clean-host package tests, scoped native support and own signed release positive | Pass |
| Architecture/scope | Existing Rust/C/library split; macOS backend gated; no generic engine/CSI cluster | Pass |

The constitution requires retained-data safeguards. The user explicitly removed
user-facing compatibility/preservation/recovery guarantees because upstream is
early alpha. Safeguards remain implementation objectives; this clarification
does not authorize destructive operations or weaken writer exclusion. No generated
constitution amendment is needed. Existing source-preview evidence is not release evidence.

## Project Structure

### Documentation (this feature)

```text
specs/004-release-unblocked-capabilities/
├── spec.md
├── plan.md
├── release-review.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── capabilities.md
│   └── distribution.md
└── checklists/requirements.md
```

### Source Code (existing and planned)

```text
crates/gs/src/            # selectors, projections, authoring, auth/watch, startup
crates/gs/tests/          # CLI and controlled transport behavior
daemon/                  # C daemon, version and native library integration
runtime/packaging/       # extend candidate inventory and existing release policy
packaging/               # planned deb/rpm/npm definitions, notices
Formula/                 # planned GitHub-hosted Homebrew tap formula
scripts/                 # planned build/package/install-validation entry points
tests/                   # existing runtime and planned package fixtures
.github/workflows/       # planned release-please.yml and release.yml
postman/                 # existing contracts; extend as HTTP mappings change
docs/                    # install, support, command and upgrade policy
```

**Structure Decision**: Extend the existing workspace and daemon; one production
staging inventory feeds every renderer. Use this repository as the initial tap
via `brew tap gitstore-dev/cli https://github.com/gitstore-dev/cli`; a new repository
is unnecessary. Package manifests are derived from matching artifact builds.

## Execution Order and Delivery Boundaries

1. Reconcile immutable evidence and source mappings. Implement File IDs, create
   projections, full hydrated reads/list exports and lossless authoring exports.
2. Implement static-user lifecycle, typed watches and built-in resource help with
   their independent contract tests. Unavailable discovery/waits remain gated.
3. Synchronize versions, production staging, native library closure and first-use
   startup. Test fd/memfd re-execution before finalizing library linkage.
4. Render/test Homebrew, deb, RPM and npm assets, completion/reference output and
   notices. Validate install, replacement/mismatch handling and removal.
5. Configure Release Please and trusted tag builds; prove alpha.1 bootstrap and
   alpha.2 advancement, final-byte attestations, identity controls and retry behavior.
6. Complete hosted/clean-host qualification, reconcile support docs and prepare
   the release. Actual publication remains a later explicitly authorized action.

Unavailable fixtures gate support/promotion, not implementation. A required failed
package row blocks its promotion rather than silently reducing the advertised set.
No tasks.md is generated here; this command ends after Phase 1 design.

## Complexity Tracking

No architecture exception. Required multi-format renderers share one inventory;
the user-directed prerelease policy is documented above and in the specification.

## Accepted implementation update — 2026-11-09

The user selected isolated GitHub-hosted Ubuntu/macOS runners and pinned Fedora
fixtures for final tag qualification, replacing the unprovisioned six-runner
self-hosted fleet. Linux fixture accounts remain fresh UID 11000; Fedora uses
private cgroups and no host mounts/ports. All 22 host/channel rows, exact trusted
source/tag/byte checks and optional consumer verification remain unchanged.
Separate same-source next-alpha/helper artifacts support qualification but never
enter the release inventory. Existing optional self-hosted development/denial
lanes are independent. Actual trusted tag execution remains an acceptance gate.
