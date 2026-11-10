# Validation Guide: Release and Unblocked Capabilities

The implementation and fixture tools exist. This guide defines the required
validation; it does not claim every host/channel or trusted release gate passed.
See [current evidence](../../docs/contributing/validation/release-qualification.md) for
actual commands, hashes, results and unavailable combinations. Use disposable
test accounts, synthetic credentials and isolated state only.

## Prerequisites and scope

- Rust 1.96.0 and documented CMake/native build dependencies for contributor tests.
- Immutable alpha.9 public server fixture; record profile, full source/image pins,
  independent GraphQL/Git URLs and proxy prefix. Never use an upstream dirty tree.
- Clean Ubuntu 24.04 x86_64/arm64 and maintained Fedora fixtures; macOS arm64/x86_64
  for package/remote checks. Native execution needs selected rootless reference
  hosts with cgroups/namespaces/TUN. No claim follows merely from a cross-build.
- GitHub hosted trusted release run for own artifact signing; no release secrets
  in PR jobs. Synthetic candidate installs need no consumer attestation verifier.

## 1. Baseline checks

For isolated portable checks, the packaging harness creates an owner-only
temporary home/config/state/cache/runtime tree and requires explicitly selected
synthetic credential files and artifacts. See `tests/packaging/README.md`.
Native manifests must follow `runtime/packaging/reference-hosts.md` and
`tests/runtime/README.md`; unavailable fixtures stay not-run. Use separate npm
prefixes and package-manager VMs for each install channel. Do not point these
checks at an existing deployment or inherit user credentials.

From repository root:

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo build --workspace --locked
scripts/test-remote.sh
```

Build production gitstored using [building instructions](../../docs/contributing/building.md),
then select exact built paths:

```sh
scripts/test-daemon.sh --daemon /absolute/build/gitstored --gs /absolute/target/debug/gs
scripts/test-runtime.sh --help
```

Run relevant CTest native/sanitized targets and explicitly selected runtime
scenarios using the existing fixture manifest guide. Never infer hosted/native
results from portable suites. In this session macOS Postman fails inside the
sandbox with `SecItemCopyMatching failed -50`; the status command succeeds outside
it. Run collections with approved Keychain access or on Linux and record actual output.

## 2. Capability acceptance

Use a private disposable configuration and explicitly selected credential input;
never inherit the user's config/credential store. Select
`contract_profile = "gitstore-alpha9-2026-11-05"` in the recorded context for new
capability positives and separately exercise the legacy negative boundaries.
Exercise:

```sh
gs read file --id SYNTHETIC_RELAY_ID -o json
gs read file hero -o yaml
gs create -f /absolute/fixtures/namespace.yaml
gs create -f /absolute/fixtures/repository.yaml
gs read product sample -o yaml
gs read product sample --authoring -o markdown
gs api-resources
gs explain file.spec.source
gs watch product --resource-version FIXTURE_CURSOR -o json
```

Expected: correct selector variables; IDs need no namespace; legacy File ID
unsupported; full spec/status/body projections; authoring round-trip matches
writable fields with Unicode/newline variants. Namespace/Repository nonempty body
fails before writes. Lists remain bounded and expose pagination rather than
claiming an atomic snapshot. api-resources/explain identify local profile evidence.

For each watch kind, verify server-accepted protocol, authentication, ordered
cursor delivery, permitted duplicates after reconnect, explicit expiry, bounded
slow consumer behavior, disconnect cleanup and unsupported Collection watch.
Use fake-server failure injection plus real shipped handler conformance.

For explicit static-users mode, exercise login, rotated refresh and remote logout
using protected synthetic password input. Verify revoked/expired credentials,
old-JTI replay, failed token persistence, unsupported providers, HTTP-200 errors
and proxy prefixes. Logs/stdout must not contain passwords/tokens except explicit
auth token output. Do not retry a refresh mutation after uncertain consumption.
OIDC/device discovery, safe update and uncorrelated waits must still fail before
effects with named gates. Retain library clone/sync regression coverage.

## 3. Installed-package acceptance

After generating a complete candidate asset inventory, install it in
fresh fixtures using the selected channel (placeholder assets below must be
replaced by the candidate's exact version/architecture):

```sh
brew tap gitstore-dev/cli https://github.com/gitstore-dev/cli.git
brew install gitstore-dev/cli/gitstore
sudo apt install ./gitstore_0.1.0~alpha.1-1_amd64.deb
sudo dnf install ./gitstore-0.1.0-0.alpha.1.1.x86_64.rpm
npm install --global --ignore-scripts /absolute/assets/gitstore-npm-0.1.0-alpha.1-linux-x64.tgz
```

These are alternatives on separate fixtures; do not install every channel into
the same user PATH. For npm use a disposable user-owned global prefix. Verify
both public commands resolve to the selected pair and report identical versions.
Audit loader dependencies, private libraries/notices, absent test facilities and
package-manager metadata. Run with no development tools/helper executables available.

Check help/remote commands create no daemon/assets. On qualified Linux execute
first local lifecycle twice concurrently; observe one manager, private socket and
correct UID. Verify fd/memfd launch, logs, stop/restart, explicit socket connect-only
and active mismatched version refusal. Test prefix paths with spaces, Homebrew
Cellar relocation and npm command links. macOS must perform remote/control checks
and actionable unsupported-runtime checks rather than imply container support.

Replace alpha.1 with synthetic alpha.2, test native version ordering and removal,
and capture known effects. Never use real user data. These tests do not promise
preservation or compatibility. Confirm install/upgrade docs and release notes
contain the early-alpha schema/data-loss disclaimer and backup guidance.

## 4. Release automation and provenance

Dry-run Release Please against fixture history: exact alpha.1 initially, alpha.2
after removing release-as, no accidental stable/major release, synchronized Cargo
lock/workspace/daemon/packages. Verify release PR CI and tag build actually trigger
with the configured GitHub App; credential absence is reported explicitly.

Run trusted workflow at the reviewed exact tag. Validate the complete draft
inventory and no test/developer executables; sign final bytes, attach bundles and
check producer/source/workflow policy with the existing verifier. Demonstrate own
signed positive plus altered artifact, missing evidence and wrong-identity failures.
This producer check is independent of package installation: installs also pass
on hosts without gh or any attestation verifier.

Inject failed matrix job/upload and retry: incomplete release stays draft, matching
assets are reused, conflicting digests fail and formula is updated only after
promotion. Never clobber published assets for a test. Use disposable release
fixtures until actual publication is explicitly authorized.

## Evidence record

Record source/version, exact artifact hashes, commands/results, fixture OS/arch,
package dependencies, source/live/simulated distinction, performance and separate
package/library/cache costs. Mark unavailable combinations honestly. Writing or updating this guide establishes no acceptance; only executed checks
with recorded fixture identities and results supply evidence.
