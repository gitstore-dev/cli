# Contributing

The repository has a development implementation of the unblocked M1 Rust CLI.
Start with the [PRD](docs/prd.md), [M1 plan](specs/002-m1-remote-cli/plan.md) and
[task list](specs/002-m1-remote-cli/tasks.md). The [M0 contract baseline](docs/contracts/README.md)
remains authoritative for public operation boundaries and open dependency gates.
See [M1 status](docs/m1-status.md) for evidence; implemented source is not a published
or fully validated product release.

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

## Application CI

Markdown and workflow-manifest lint jobs remain deferred. PR-title validation is the
initial metadata workflow. The CLI workflow now builds, formats, lints and tests
the Rust workspace on Linux x86_64 and macOS arm64, with controlled HTTP simulation
on Linux. Hosted results remain unverified until the workflow runs. Local inspection
does not establish a GitHub-hosted pass.

Extend CI alongside executable changes. Add C build/behavior/sanitizer checks when
daemon targets exist, reusing applicable GitStore conventions. Each job must validate
implemented behavior or a concrete release requirement.

When build/release targets exist, add artifact provenance generation and verification
alongside publication. Follow [ADR 0003](docs/ADRs/0003-runtime-assets-packaging.md)
for trusted build identities, digest binding, first-use/offline verification and the
attestation release gate. No attestation-only workflow is needed before artifacts exist.

Before advertising runtime support, add C address/undefined-behavior sanitizer and
protocol fuzz/resource-bound tests, plus identity isolation, recovery, retained-data,
single-writer and host/backend compatibility checks appropriate to the implementation.
Use the [M1 validation guide](specs/002-m1-remote-cli/quickstart.md) for executable
checks; M2 runtime commands remain unimplemented.

Published-image compatibility CI uses a separate
[Compose target](tests/integration/README.md). Run `cargo build --locked -p gs`, then
`scripts/test-integration.sh` to pull GitStore `:latest`, generate a disposable
installation, test public GraphQL and Git operations, and remove the stack. It needs
Docker Compose v2 and Python 3, requires no external account or sibling source
checkout, and records resolved image digests in the job log. Hosted results remain
unverified until the workflow runs.
