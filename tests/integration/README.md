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

The ignored Rust image test seeds a repository through public Git using libgit2,
then exercises real `gs` clone, branch tracking, no-op sync, push, and a second clone
that verifies the pushed revision. It checks the default `gitstore` remote and explicit
`origin` override. CLI subprocesses have no Git executable on PATH. The existing live
suite additionally checks Namespace/Repository inspection and disposable Product and
CategoryTaxonomy create/read/list/delete with Unicode and trailing-newline bodies.

This target checks the memdb stack over loopback HTTP. It does not establish external
provider, HTTPS, native credential-store, admission/readiness correlation, or full M1
release support. Default `cargo test` skips the image test; only this runner provisions
its required disposable installation.
