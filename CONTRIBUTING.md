# Contributing

The repository is currently in product design. Start with the
[PRD](docs/prd.md) and the [M0 specification](specs/001-m0-contracts/spec.md).
The [M0 contract baseline](docs/contracts/README.md) publishes the operation inventory,
runtime decisions and [acceptance evidence](docs/contracts/acceptance.md). Its reviews
cover design/source consistency, not an executable product or validated runtime release.

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
only initial workflow. Local inspection does not establish a GitHub-hosted pass.

Add CI alongside runnable implementation: build and test the Rust CLI when its
workspace exists, then build and test the C daemon, including sanitizers, when
its targets exist. Reuse applicable GitStore conventions at that point. Each job
should validate implemented behavior or a concrete release requirement.

Before advertising runtime support, add C address/undefined-behavior sanitizer and
protocol fuzz/resource-bound tests, plus identity isolation, recovery, retained-data,
single-writer and host/backend compatibility checks appropriate to the implementation.
Use the M0 [review guide](specs/001-m0-contracts/quickstart.md) to inspect contract
coverage now; do not run intended `gs` commands as though binaries already exist.
