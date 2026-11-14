# Contributing

User-facing setup and command documentation is in [docs/README.md](docs/README.md).
Source builds, runtime internals, contracts and validation archives are indexed in
[docs/contributing/](docs/contributing/README.md). Published prerelease support is limited to the platforms and capabilities in
[release qualification](docs/contributing/validation/release-qualification.md).
Source tests alone do not establish release support.

## Development checks

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo build --workspace --locked
scripts/test-remote.sh
```

The pinned toolchain is Rust 1.96.0. Controlled HTTP tests use Postman CLI 1.75.0 and
Python 3. Git functionality is library-based; tests create fixtures through libgit2
and run `gs` with no Git executable on PATH. Native-store smoke tests are opt-in;
live tests require explicit disposable-installation input to `scripts/test-live.sh`.

## Pull request titles

Use Conventional Commit titles, matching GitStore's accepted types: `feat`, `fix`,
`docs`, `chore`, `refactor`, `test`, `style`, `perf`, `ci`, `build`, and `revert`.
Scope is optional; examples include `docs: define M0 contracts` and
`feat(auth): add login discovery`.

The PR-title workflow validates metadata only, with read-only access and no code
checkout. It uses `pull_request`, matching GitStore, and runs on PR creation, title
edits, updates, and reopening using the PR's workflow definition. Merge-queue entries
report success because the PR title has already been checked. Fork-token restrictions
for the pinned title action require separate verification before promising fork support.

The merge queue preserves commit subjects. Release Please reads those subjects,
so releasable fixes/features need an appropriate `fix:`/`feat:` commit. Changing
only the PR title does not change the release classification of existing commits.

## Application CI

CLI and daemon checks run on pull requests and merge-queue entries. They do not
repeat on the resulting main-branch push. Runtime library checks in
`runtime-ci.yml` run on pull requests and main pushes; they cover the Linux SDK
on both architectures in normal and sanitized builds. Manual host, package and
release qualification remains in the dispatch-only `runtime.yml` workflow.
Release qualification still requires all six hosts and 22 package selections.

Markdown and workflow-manifest lint jobs remain deferred. PR-title validation is the
initial metadata workflow. The CLI workflow now builds, formats, lints and tests
the Rust workspace on Linux x86_64 and macOS arm64, with controlled HTTP simulation
on Linux. Hosted results remain unverified until the workflow runs. Local inspection
does not establish a GitHub-hosted pass.

The daemon workflow builds normal/sanitized C targets, runs Rust/C socket contracts,
checks the image code library and fuzzes bounded protocol inputs. Extend these
checks alongside executable changes. Each job must validate implemented behavior
or a concrete release requirement; local passes do not establish hosted execution.

When build/release targets exist, add artifact provenance generation and verification
alongside publication. Follow [ADR 0003](docs/ADRs/0003-runtime-assets-packaging.md)
for trusted build identities, digest binding, first-use/offline verification and the
attestation release gate. The [release policy](runtime/packaging/release-policy.md) and maintainer-only
offline verifier reject unsigned development candidates. Its fixture tests require
installed developer `gh`; no verifier or helper executable is bundled with the
product. Verified signing/publication runs and remaining limits are recorded in
[release qualification](docs/contributing/validation/release-qualification.md).

Before advertising runtime support, add C address/undefined-behavior sanitizer and
protocol fuzz/resource-bound tests, plus identity isolation, recovery, retained-data,
single-writer and host/backend compatibility checks appropriate to the implementation.
Use the [M1 validation guide](specs/002-m1-remote-cli/quickstart.md) for executable
checks. Native core, retained Scylla, registered migration and owned recovery have
scoped Linux arm64 validation; broader runtime and platform gates remain open.
Run `scripts/test-runtime.sh --scenario persistent-scylla --fixture /absolute/owned.json`
only with dedicated disposable inputs. Its independent restore proof is private
backend verification, not a public backup/restore command. Developer experiments
and probes under `runtime/experiments/` are not installed product binaries.

Published-image compatibility CI uses a separate
[Compose target](tests/integration/README.md). Run `cargo build --locked -p gs`, then
`scripts/test-integration.sh` to pull GitStore `:latest`, generate a disposable
installation, test public GraphQL and Git operations, and remove the stack. It needs
Docker Compose v2 and Python 3, requires no external account or sibling source
checkout, and records resolved image digests in the job log. Hosted results remain
unverified until the workflow runs.
