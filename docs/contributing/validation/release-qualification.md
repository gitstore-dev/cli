# Release qualification

**[Alpha.6 is published](https://github.com/gitstore-dev/cli/releases/tag/v0.1.0-alpha.6).**
All six reference hosts and 22 package selections passed. The 33 final subjects
and 33 attestation bundles are public. The signed Homebrew formula is on main
through [#29](https://github.com/gitstore-dev/cli/pull/29).

Fresh installations from the public Homebrew tap and npm URL passed on the
maintainer's macOS 26.6.1 arm64 machine. With and without `XDG_RUNTIME_DIR`,
`gs deployment list` returns `UNSUPPORTED_HOST` (exit 6) without creating runtime
state. This corrects alpha.5's misleading diagnostic; macOS remains remote/control
only. T067 is satisfied by the published package checks.

Use the [release runbook](../releases.md) for operating instructions and the
[machine-readable record](alpha6-qualification-review.json) for hashes, receipt
identities, provenance controls and public installation results.

## Release evidence

| Field | Value |
| --- | --- |
| Tag | `v0.1.0-alpha.6` |
| Source | `01440a78153ccb9803556e5560307b40dda73a6f` |
| Release proposal | [#27](https://github.com/gitstore-dev/cli/pull/27) |
| Build | [43220188391](https://github.com/gitstore-dev/cli/actions/runs/43220188391), four platforms and assembly passed |
| Qualification | [43526079380](https://github.com/gitstore-dev/cli/actions/runs/43526079380), 22 selections and complete collector passed |
| Signing and publication | [43532331488](https://github.com/gitstore-dev/cli/actions/runs/43532331488), producer verification and approved publication passed |
| Homebrew delivery | [#29](https://github.com/gitstore-dev/cli/pull/29), signed formula merged through the queue |

Qualification attempt 1 failed during Ubuntu arm64 Homebrew trust setup, before
product installation. A retry of that host and the collector passed with the same
source and candidate; the five successful host results were retained. The failed
attempt remains recorded.

The producer verified all 33 subjects. Independent checks verified the signed Mac
archive, npm package, formula, inventory and checksums, plus five rejection controls.
All 66 public URLs and their API sizes/digests matched the signed candidate; those
five subjects and their bundles were downloaded and checked independently.

## Qualified support

| Host | Architecture | Passing channels | Runtime scope |
| --- | --- | --- | --- |
| Ubuntu 24.04 | x64 and arm64 | Archive, Homebrew, APT, npm | Native rootless Linux |
| Fedora 44 | x64 and arm64 | Archive, Homebrew, DNF, npm | Native rootless Linux |
| macOS 15 | x64 and arm64 | Archive, Homebrew, npm | Remote/control only |

Qualification covers package installation/replacement/removal, Linux native
execution, recovery, integrity and performance gates. Receipts bind the source,
final artifact hashes and measured executable pair. Fixture measurements are not
universal performance guarantees. Additional checks on the maintainer's Mac do
not establish another formal platform or a macOS workload backend.

## Distribution limits

- GitStore and this CLI are early alpha. Upstream schemas change incompatibly.
  Upgrade compatibility, data preservation and recovery are not guaranteed. Use,
  upgrade, migration or removal may lose data. Keep independent backups.
- Daemon bundle reduction remains deferred; runtime images, state and cache are
  separate costs from installed binaries and packages.
- APT/DNF install GitHub Release files; no package registry publication is included.
  npm uses platform-specific release tarballs. Nix remains deferred.
- Named upstream dependencies in the [capability inventory](../../../specs/004-release-unblocked-capabilities/contracts/capabilities.md)
  remain gated. This release completes the selected increment, not every M0–M3 capability.
- GitHub normalizes `~` to `.` in Debian asset names and bundles. Signed logical
  names, package versions and hashes are unchanged. [Installation](../../installation.md)
  explains checksum filenames. Consumer provenance verification is optional.

The [alpha.5 evidence](alpha5-qualification-review.json) remains available;
alpha.5's published binaries and signatures are unchanged.
