# Distribution and Release Contract

## Artifact set and layout

Each candidate inventory selects required OS/architecture rows before building.
Planned tuples: Linux x86_64/arm64, macOS arm64/x86_64. Linux packages qualify
native core/OIDC where available; macOS packages supply remote/control and clearly
reject workload execution until backend qualification. A required failed row
blocks promotion. Record actual host/ABI limits; do not infer RHEL/Debian support.

Each archive contains ordinary single-link `bin/gs` and `bin/gitstored`, optional
private `lib/`, reference/completion files, notices and `share/gitstore/install.json`.
Production build excludes test probes/symbols, developer helpers and service images.
User config/credentials/volumes never enter the archive. inventory records final
hashes, application version, closure, OS/ABI floor and supported runtime capabilities.

Native system packages install the actual pair under `/usr/libexec/gitstore/bin`
and command links under `/usr/bin`; Homebrew installs the same private pair in its
versioned prefix. Resolve public gs links to the real binary for sibling lookup;
the sibling daemon itself must be ordinary and pass existing identity checks.
Test both command links; no PATH search for a manager/broker is permitted.

Library closure must pass ordinary invocation, Linux pinned-fd startup and
libcrun sealed-memfd re-exec with cleared environment. Do not assume `$ORIGIN`
works under re-exec. Use a tested fixed installation mapping or qualified static
private linkage when needed; publish license/source obligations and ABI floors.
Archives/npm cannot silently rely on undeclared developer-host dependencies.

## User setup and package channels

- Homebrew: formula `gitstore` under `Formula/`, explicit Git URL tap in this repo;
  select exact platform asset URL/checksum. Formula installs binaries and qualified
  dependencies without local compilation. Include optional service definition;
  first local command launches the user daemon, not `brew services start` in a hook.
- APT: GitHub-hosted `gitstore_0.1.0~alpha.1-1_ARCH.deb`; install a downloaded file
  with apt to resolve declared dependencies. No apt repository is promised.
- DNF: `gitstore-0.1.0-0.alpha.1.1.ARCH.rpm`, install by release URL or local file;
  declare ABI dependencies against tested Fedora fixtures. No rpm repo promised.
- npm: per-platform `.tgz` with `package/package.json`, both real executables,
  libraries/notices and bin map `{gs:bin/gs,gitstored:bin/gitstored}`. OS/cpu fields
  reject incompatible tuples. Install from a full GitHub Release URL with `npm
  install --global URL`. No lifecycle download scripts/registry dependencies;
  `--ignore-scripts` must work. Non-default npm prefix and paths with spaces tested.
- Nix: deferred; no flake/registry publishing requirement.

System install scripts own code and optional service templates; they do not select
a target user, start a root manager, set up linger, create credentials, download
workload images or alter an existing user's config. First local invocation creates
private per-user state and starts a matching manager. Remote/help commands have no
daemon/download side effects. Explicit socket overrides remain connect-only.
Concurrent first launches converge to one manager. A mismatched active version
fails actionably; do not terminate workloads merely to upgrade a package.

## Version and prerelease policy

All native binaries report `0.1.0-alpha.1` initially. Package encodings are in
[data-model.md](../data-model.md). Release Please updates all relevant files from
one reviewed version and includes a bootstrap boundary. Require a dry-run showing
first alpha.1 and subsequent alpha.2; remove the temporary release-as override.
Generated docs/completions must match the packaged command definitions.

Every prerelease and install/upgrade guide states:

> GitStore and this CLI are early-alpha software. Upstream schemas will continue
> changing incompatibly. Upgrade compatibility, data preservation and recovery
> are not guaranteed. Use, upgrade, migration or removal may cause data loss.
> Keep independent backups and do not rely on prereleases for irreplaceable data.

Describe known migration/reset/uninstall effects. Package replacement testing
establishes observed behavior only; it cannot establish universal compatibility.

## Producer workflow and publication

Release Please on main uses the upstream GitHub App approach, restricted to
release proposal/tag work. Pin reviewed actions, preserve metadata-only PR checks.
App installation/key availability is a prerequisite for enabling the workflow.
No external registry token or packages write permission is needed.

`.github/workflows/release.yml` runs on reviewed `v*` tags or manual dispatch at
that exact tag. Verify tag/source against the reviewed release PR/version.
Create/keep a draft prerelease until the complete expected set passes. Build
production bytes, package, test, generate manifest/checksums, attest final subjects
with pinned `actions/attest`, upload standard bundles, validate producer policy,
then promote the prerelease and update formula URLs/checksums. Reserve release
mutation credentials for publishing jobs; signing credentials never reach PRs.
No moving latest downloads or component version divergence.

Use existing producer repository ID/workflow/issuer/tag/revision/digest policy.
Attestation generation uses hosted runners with `id-token: write`, `attestations:
write`, `artifact-metadata: write`; separate contents write for publishing. Include
subjects for executable files, archives/packages, manifest/checksums and generated
formula as appropriate. Bundles refer to subjects rather than recursively attesting
themselves. Cryptographic failure or missing subject blocks producer promotion.

Consumers receive ordinary standard signed bundles plus optional verification
instructions for compatible tools. Installers, npm hooks and gs/gitstored neither
enforce attestation verification nor require a verifier executable. Checksums are
integrity metadata, not a substitute for provenance.

Retries compare existing asset names/digests to the inventory, reuse matching
assets, reject conflicts, and resume only the same source candidate. Never use
clobber to replace a published artifact. Concurrent attempts serialize per tag.
Do not mark incomplete draft assets as supported releases or update the tap early.
No publication occurs as part of this plan command.
