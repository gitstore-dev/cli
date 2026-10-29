# Published-image integration checks

Run from the repository root with Python 3, Docker Compose v2, the pinned Rust
toolchain, and access to public GHCR images:

```sh
cargo build --locked -p gs
scripts/test-integration.sh
```

The [CI workflow](../../.github/workflows/ci-integration.yml) runs this target on
relevant pushes and pull requests to main and on manual dispatch. It always pulls
`ghcr.io/gitstore-dev/{api,git-service,controller-manager}:latest` and prints their
resolved digests and source revision labels. A moving tag failure is a compatibility
signal; use the logged digests to identify the tested images.

The stack uses Compose, memdb, random loopback ports, a unique project name, generated
admin credentials, and disposable volumes. Image-native `gitctl` generates user and
signing material and enrolls the controller service account before controller startup.
The bootstrap follows GitStore CI and `gitstore-dev/quickstart`, using the current
cluster-scoped service-account enrollment interface. No sibling checkout is required.
Runtime secrets stay in protected temporary files or project volumes, never command
arguments or committed fixtures. Failure diagnostics include redacted service logs.
The runner removes its containers, network, volumes, and temporary files on exit.

Fixture provisioning uses public GraphQL. The single narrowly retried mutation error
is the documented pre-admission rejection while the namespace system repository is
unavailable. Other mutation errors fail without retries. Repository storage is checked
through authenticated public Git before tests start.

The runner reports **36 individually named tests**: six Cargo tests in
`crates/gs/tests/images.rs` and 30 Python `unittest` cases in
`scripts/test_image_commands.py`. Both suites run even if one fails, and cases within
each suite continue after failures. Names and tracebacks identify the failing command
or Git scenario directly in the CI log. To rerun one case while debugging an already
provisioned target, use Cargo's test-name filter or Python's unittest `-k` filter with
the runner's explicit fixture environment; the normal script owns stack cleanup.

| Command family | Published-image checks |
| --- | --- |
| `context list/show/use`, `config validate` | Each command has a separate case with temporary config |
| `auth status/token/switch` | Protected-file status, explicit raw token output, local account selection |
| `api graphql` | Typed scalar variable query, complete request envelopes for fixtures, rejected invalid credential |
| `read` | Namespace/Repository, Product/Category by name and ID, exact Unicode/trailing-newline body preservation; missing Collection and File responses |
| `list` | Namespace, Repository, Product, Category, Collection; page bound and pageInfo assertions |
| `create` | Separate Product and Category cases; server-returned resource identity and body |
| `delete` | Product/Category by name and ID; disposable Namespace/Repository by ID |
| `repository clone` | Default remote/branch tracking and explicit remote override |
| `repository sync` | No-op, accepted push/reclone, dirty worktree preservation, divergent history preservation |

Each Git case provisions and seeds its own remote repository through public GraphQL
and public Git, using libgit2. No case relies on another case's writes or execution
order. Product/Category cases create unique resources and clean up only their own
server-returned IDs. CLI processes have no Git executable on PATH. Namespace and
Repository creation here is explicit GraphQL fixture setup, not a claim that the
high-level `gs create` commands support those kinds.

`auth logout` and native OS-store behavior remain in the local/native-store suites;
the image target never opens the host credential store. Successful Collection/File
reads retain controlled-fixture coverage: this target currently checks a Collection
operation-error response and File not-found response, not successful object hydration.
Login/refresh, updates, watches, discovery and waits retain their existing unsupported
behavior and separate gate tests; this target does not enable gated functionality.
The caller-provisioned opt-in `scripts/test-live.sh` remains available separately.

This target checks the memdb stack over loopback HTTP. It does not establish external
provider, HTTPS, native credential-store, admission/readiness correlation, or full M1
release support. Default `cargo test` skips the six image tests; only this runner provisions
its required disposable installation.
