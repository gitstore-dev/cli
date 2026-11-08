# Research: Release and Unblocked Capabilities

**Date**: 2026-11-08. Decisions are design choices, not release qualification.

## R1 — Shipped capability scope

**Decision**: Pin new work to alpha.9 `68f8471edc947e417c98b6e8299a1367ab6b20da`;
retain historical baseline/legacy profile separately. Implement the available
File ID, Namespace/Repository create, full export, typed watch, explicit static-user
auth and built-in help slices in [capabilities](contracts/capabilities.md).

**Rationale**: Open dependencies include substantial client work; conversely
status fields or OIDC composition alone do not prove correlation or flow discovery.

**Alternatives**: Treat all open dependencies as absent upstream functionality;
use unreleased main; infer arbitrary writes from schema introspection. All rejected.
See [release review](release-review.md) for the nine shipped release-note records.

## R2 — Release Please

**Decision**: Use manifest/simple release type with alpha prerelease versioning,
no component prefix in v-tags, a bounded bootstrap history, manifest seed
`0.1.0-alpha.0` and explicit initial `release-as: 0.1.0-alpha.1`. Remove that
override after the first release; fixture-test alpha.1 and subsequent alpha.2.
Synchronize workspace version, Cargo.lock, daemon full version and packages.
CMake project version stays numeric `0.1.0` with a separate full application
prerelease string. Do not pass alpha syntax as CMake's numeric project VERSION.

**Rationale**: Upstream had bootstrap/version corrections; exact first version
must be tested rather than inferred from a seed. One CLI release owns both binaries.

**Alternatives**: Copy upstream version train; retain release-as forever; manual
versioned tags. None satisfy the agreed release process.

**Sources**: [upstream config](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/release-please-config.json),
[manifest configuration](https://github.com/googleapis/release-please/blob/main/docs/manifest-releaser.md).
Reuse upstream's GitHub App token pattern to trigger PR checks and tag workflows;
App credentials are a deployment prerequisite, not presumed configured.
[Token behavior](https://github.com/googleapis/release-please-action#other-actions-on-release-please-prs).
Permit manual dispatch of the release workflow at a reviewed tag for recovery;
never depend on GITHUB_TOKEN-created events triggering another workflow.

## R3 — Package channels

**Decision**: GitHub Release archives, deb/RPM and npm-format platform tarballs;
Homebrew tap in this repository. Package name `gitstore`, commands `gs`/`gitstored`.
Scope npm names by platform, e.g. `@gitstore-dev/cli-linux-x64`, installed from
full release asset URLs. Include both binaries and private native closure;
no postinstall download or registry publication. Nix/Windows deferred.

**Rationale**: No new registry account/repository needed. npm native bin mappings
can expose both commands without a Node wrapper. Script-disabled installation
works. User-owned first-use daemon launch serves every channel.

**Alternatives**: GitHub Packages apt/RPM registry (not supported); downloader
hooks (extra failure paths); external registry (excluded).

**Sources**: [npm install](https://docs.npmjs.com/cli/v11/commands/npm-install/),
[Homebrew taps](https://docs.brew.sh/How-to-Create-and-Maintain-a-Tap),
[DNF](https://dnf.readthedocs.io/en/latest/command_ref.html),
[APT](https://www.debian.org/doc/manuals/debian-reference/ch02.en).

## R4 — Native library closure and installed launch

**Decision**: Extend production CMake/candidate inventory. Declare supported distro
dependencies for deb/RPM, bundle private non-system libraries for archive/npm,
and use qualified formula dependencies/private closure for Homebrew. Test each
closure with cleared environment, fd startup and libcrun sealed-memfd re-exec.
If relocation fails, statically link private components where supported and
license-compliant. Record chosen linkage per artifact before promotion; do not
rely on LD_LIBRARY_PATH, bundle a glibc loader or ship arbitrary host libraries.

**Rationale**: Existing candidate packages measure ldd dependencies but do not
bundle them. `daemon_start.rs` validates a single-link ordinary sibling ELF and
uses /proc/self/fd with env_clear; relative RUNPATH alone does not prove memfd
re-exec works. This bounded package experiment is implementation, not unresolved
product scope. A failing closure blocks release, not its investigation.

**Alternatives**: Ship candidate unchanged; require toolchains; invoke a container
engine; weaken sibling validation. Rejected.

Use existing Linux first-use startup and optional user-service template; never
guess a logged-in user during privileged package installation. macOS package
commands must distinguish remote/control support from unavailable execution.

Implementation experiment on the restored dedicated arm64 guest confirmed the
lookup risk: renamed private libcap plus `$ORIGIN/../lib` failed with exit 127
after libcrun re-exec; an absolute private-library RPATH passed with matching
version output. See `runtime/packaging/probe-origin.py` and the alpha.1 validation
record. The relocatable private-closure solution remains T012 work; the negative
result cannot be promoted as portable npm/archive support.

## R5 — Platform matrix

**Decision**: Linux amd64/arm64 production and macOS arm64/x86_64 remote/control
packages. Ubuntu 24.04 is the initial Debian-family reference; select a maintained
Fedora fixture satisfying native minima during implementation. Pin exact image
digests and tested OS versions in receipts. No generic Debian/RHEL promise.

**Rationale**: Current scoped runtime evidence is Linux arm64. Native systemd,
seccomp, json-c, curl and crypto minima exceed some older distributions. Portable
macOS control tests do not validate a production Apple/VM backend.

**Alternatives**: Universal Linux ABI; imply macOS container execution. Rejected.

## R6 — Attest and stage releases

**Decision**: Trusted tag workflow `.github/workflows/release.yml`, hosted signing,
commit-pinned `actions/attest` provenance. Grant id-token/attestations/artifact-metadata
write only there, contents write only for publication. Attest final package bytes
and inventory/checksum files; upload standard bundles; verify producer identity
with existing release policy. Draft first, promote after complete inventory and
receipts, then update the tap. Consumer verification remains entirely optional.

**Rationale**: Bind distributed bytes rather than intermediate objects. Package
managers may use any compatible verifier without requiring one at install time.

**Alternatives**: Sign raw binaries only; enforce all consumers; attest PR builds
as releases. Rejected.

**Sources**: [actions/attest](https://github.com/actions/attest), existing
`runtime/packaging/release-policy.md`. Current action requires artifact-metadata
write as well as OIDC and attestation write; re-pin reviewed commits at implementation.

## R7 — Early-alpha scope and API tooling

**Decision**: Explicit no upgrade/data-preservation/recovery guarantees in install
guidance and every prerelease, because upstream schemas change incompatibly.
Recommend independent backups and avoid irreplaceable data. Keep defensive tests.

**Rationale**: Explicit user policy; validation of known behavior is not a broad
compatibility promise. Removing engineering safeguards was not requested.

**Alternatives**: Automatic migration promise; remove writer exclusion. Rejected.

Reuse GraphQL evidence and existing Postman contracts for HTTP traffic; design
HTTP changes before deriving tests. Do not invent REST endpoints or OpenAPI for
the Unix socket/WebSocket stream. `postman skills status` failed inside the sandbox
with `SecItemCopyMatching failed -50` but succeeded outside it, reporting outdated
skills. This is a sandbox Keychain limitation, not evidence that macOS Postman is
generally broken. Use approved unsandboxed execution or Linux for collections;
no live contract or signing run occurred during research.
