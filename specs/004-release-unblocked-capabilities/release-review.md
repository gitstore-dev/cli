# Initial release review — 2026-11-08

## Prerelease upgrade policy

Upstream GitStore is also in early alpha, and its schemas will continue changing
in incompatible ways. This distribution depends on those evolving contracts and
cannot promise compatibility or data preservation across those changes.

Prerelease software makes no upgrade compatibility guarantees. Configuration,
public interfaces, daemon protocol and persisted state may change incompatibly.
Automatic migration, in-place upgrades and rollback are not guaranteed. User-facing
installation/upgrade guidance and each prerelease's release notes must state this
policy and describe known transition requirements. Prereleases also make no
data-preservation or recovery guarantees: use, upgrade, migration or removal may
cause loss of configuration, credentials or stored data. Recommend independent
backups and advise against relying on prereleases for irreplaceable data.
Retention and ownership checks remain engineering objectives, not preservation
guarantees. This explicit user policy takes precedence over earlier milestone
wording that could imply guaranteed preservation.

## Shipped upstream inputs

Read all nine published [GitStore releases](https://github.com/gitstore-dev/GitStore/releases),
alpha.1 through alpha.9, through the GitHub API. The quickstart, gitstore.dev and
cli repositories currently return no published releases. This review concerns
the application releases in the GitStore monorepo, not unpublished main changes.

| Shipped series | Relevant inputs | Consequence |
| --- | --- | --- |
| alpha.1–2 | Release Please setup, alpha version corrections and tag-trigger fixes | Reuse the corrected release pattern, not historical erroneous version seeds. |
| alpha.3–4 | Static users, OIDC reference stack, service-account auth, durable Namespace/Repository watches, Product lifecycle, File boundaries | Audit CLI auth/watch/lifecycle gaps against actual public schemas and resolvers; controller support does not establish client flow discovery. |
| alpha.5 | Controller recovery and bootstrap diagnostics | Available integration inputs; qualify local lifecycle independently. |
| alpha.6–7 | Category mutations, Product admission, namespace/repository lifecycle, canonical auth/config | Existing functionality must not be called absent; stale-client update protection and correlation require separate verification. |
| alpha.8 | File provider selection, lookup diagnostics, controller bootstrap split | Review compatibility and runtime integration; App capability entries are documentation. |
| alpha.9 | Split API endpoints, normalized File lookup, shared metadata, retained Scylla baseline changes | Existing CLI alpha.9 profile already maps File namespace/name reads. ID selection, full field projection and migration compatibility require further assessment. |

Current code inspected: `crates/gs/src/resources.rs` has alpha.9 namespace/name
File mapping but calls the legacy selector first, which rejects File IDs;
`crates/gs/src/lib.rs` leaves watch and other pending commands unsupported.
Upstream [PR #523](https://github.com/gitstore-dev/GitStore/pull/523), merged as
`50f2da6c3f1f1702d9baa543be7c8d6c91b0d1ca`, supplies both selectors.
The [shipped alpha.9 schema](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/shared/schemas/admin/file.graphqls)
defines `FileBy @oneOf` with `id` and `namespacePath`. File ID reads are therefore
unblocked CLI implementation work. The local unsupported error is stale about
upstream availability; it is not evidence of a missing server contract.
These observations supersede earlier discussion that treated alpha.9 File
compatibility as entirely unimplemented. This initial review is not a completed
per-operation conformance audit.

## Upstream Release Please pattern

Inspected at tag alpha.9 (commit `68f8471edc947e417c98b6e8299a1367ab6b20da`):

- [Workflow](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/.github/workflows/release-please.yml): main pushes; serialized non-cancelling release job; GitHub App token; Release Please action v4.
- [Configuration](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/release-please-config.json): manifest mode, simple release type, prerelease versioning, alpha, pre-1.0 minor bump control, changelog sections and extra version files.
- Manifest records `0.1.0-alpha.9`. CLI needs its own first-release seed and history boundary; verify exact alpha.1 output before enabling publication. Do not copy upstream source/version-file paths or assume GitHub App credentials are configured here.
- Existing `runtime/packaging/release-policy.md` already pins the CLI producer and reserved release workflow identity and requires final-byte attestation verification. Preserve that policy when adding actual release automation.

## Package channels

- [Homebrew tap](https://docs.brew.sh/How-to-Create-and-Maintain-a-Tap): GitHub-hosted formula, release archives, dependencies and [service integration](https://docs.brew.sh/Formula-Cookbook#service-files); Linux/macOS. No Homebrew/core submission needed. Installing a formula and starting its service are distinct actions.
- [APT](https://www.debian.org/doc/manuals/debian-reference/ch02.en): install a downloaded release `.deb` with `apt install ./file.deb`; resolves declared dependencies. Native user-service files can ship in the package; a privileged install cannot assume a particular logged-in user session.
- [DNF](https://dnf.readthedocs.io/en/latest/command_ref.html): accepts an RPM URL directly; GitHub Release assets can supply it. Same user-session consideration as Debian packaging.
- [Nix](https://nix.dev/manual/nix/2.27/command-ref/new-cli/nix3-profile-install.html): GitHub flake reference with no nixpkgs submission. Profile installation alone does not configure system/user services; modules or an additional activation path are needed. Reproducible packaging and runtime paths require separate validation.
- [GitHub Packages supported formats](https://docs.github.com/en/packages/learn-github-packages/introduction-to-github-packages): not a generic apt/RPM/Homebrew binary registry. GitHub Releases can host the downloadable packages without another registry account.

Accepted on 2026-11-08: Homebrew plus GitHub-hosted `.deb` and `.rpm`
assets; defer Nix. Configure first-use user-daemon launch. Upgrades without an apt/dnf repository
require installing a newer asset. A GitHub-hosted Homebrew tap supports brew upgrades.
No package manager's ordinary checksum verification should be represented as
automatic GitHub attestation verification without an explicitly tested mechanism.

## npm — accepted

[npm accepts remote package tarballs](https://docs.npmjs.com/cli/v11/commands/npm-install/),
so platform-specific npm-format archives can be hosted as GitHub Release assets
without npm registry publication. Each would contain both executables and required
libraries and expose commands through package metadata. Prefer a complete archive
over an install script that fetches additional executables: disabled scripts,
offline installs and partial downloads otherwise become extra failure cases.
Node/npm becomes an installation prerequisite. npm does not resolve native system
libraries like APT/DNF; package relocation and native library resolution need
their own tests. Existing first-use daemon launch can serve this channel too.
npm is required alongside Homebrew/APT/DNF. Publish standard signed GitHub Actions
attestations for compatible consumer tools. Verification is optional for consumers:
no installer or installed executable enforces it or requires a verifier dependency.
Existing release-pipeline provenance checks validate the producer's own output;
they do not impose an installation-time policy. Document optional verification
and explicit tarball upgrade instructions.
