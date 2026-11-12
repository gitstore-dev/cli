# Release qualification

**[Alpha.5 is published](https://github.com/gitstore-dev/cli/releases/tag/v0.1.0-alpha.5).**
All six reference hosts and 22 package selections passed. All 33 final subjects
and their 33 standard attestation bundles are publicly downloadable. The signed
Homebrew formula is supplied by [#25](https://github.com/gitstore-dev/cli/pull/25).

Use the [release runbook](../releases.md) for operating instructions and the
[complete inventory and results](alpha5-qualification-review.json) for exact hashes,
bundle hashes, host receipts, performance and provenance controls.

## Reviewed candidate

| Field | Value |
| --- | --- |
| Tag | `v0.1.0-alpha.5` |
| Source | `968ee1d44ce550c76a05b311c000c87fd910e444` |
| Release proposal | [#24](https://github.com/gitstore-dev/cli/pull/24), merged through the queue with passing required CI |
| Build | [43164227501](https://github.com/gitstore-dev/cli/actions/runs/43164227501), four platforms and assembly passed |
| Qualification | [43165938837](https://github.com/gitstore-dev/cli/actions/runs/43165938837), all 22 selections and complete collector passed |
| Signing and producer verification | [43179378305](https://github.com/gitstore-dev/cli/actions/runs/43179378305), signing/producer verification passed; approved publication failed at draft lookup |
| Frozen subjects | 33 subjects, 468,729,270 bytes, plus 33 standard `.sigstore.jsonl` bundles |
| Inventory SHA-256 | `32602faa4c0491d6e2d184ab21008635207316144b891827abb3669b1cdb7c50` |
| SHA256SUMS SHA-256 | `03c5415ae3276c00d33636a7f3330c98ed0c76f5bbfe82400334ee54f6380761` |

Receipts bind this exact source/tag and all 33 subject hashes. Promotion rechecked
bytes, install inventories, performance identities and complete receipts before
signing. Every final subject passed the issuer, workflow/tag, source revision,
hosted-runner and immutable repository-ID policies. Independent offline checks
revalidated all 33 subject signatures, frozen hashes and install inventories, plus
all 22 executable-bound qualification receipts. Five negative controls rejected
altered bytes, altered bytes with a recomputed digest,
missing evidence, wrong source and wrong workflow.

## Qualified support

| Host | Architecture | Passing channels | Runtime scope |
| --- | --- | --- | --- |
| Ubuntu 24.04 | x64 and arm64 | Archive, Homebrew, APT, npm | Native rootless Linux |
| Fedora 44 | x64 and arm64 | Archive, Homebrew, DNF, npm | Native rootless Linux |
| macOS 15 | x64 and arm64 | Archive, Homebrew, npm | Remote/control only |

Linux acceptance includes first-use startup, concurrent startup, active-version
refusal, library/loader controls, offline native execution, clean installation,
recovery and interrupted/tampered-asset controls. Each host also records 100 warm
command samples; Linux records native startup, inspection latency and separate
runtime assets, state, cache and manager-memory costs. Results are fixture
measurements, not a universal performance guarantee.

On the maintainer's macOS 26.6.1 arm64 machine, the exact archive, npm and private
Homebrew packages also passed isolated install/control/removal checks. These
additional checks did not modify the existing installation and do not add another
formal platform or a macOS workload backend.

## Distribution limits

- GitStore and this CLI are early alpha. Upstream schemas change incompatibly.
  Upgrade compatibility, data preservation and recovery are not guaranteed. Use,
  upgrade, migration or removal may lose data. Keep independent backups; do not
  use prereleases for irreplaceable data.
- Linux `gitstored` is 22,087,920 bytes on x64 and 22,409,424 bytes on arm64.
  Bundle reduction is deferred. Runtime image/state costs are separate from the
  package and daemon sizes; see the machine-readable measurements.
- APT/DNF install downloaded release files; no package registry or repository
  publication is included. npm installs the platform-specific GitHub Release
  tarball. Nix is deferred. macOS local workloads remain unsupported.
- Capability coverage remains the shipped alpha.9 public-contract increment in
  [the capability inventory](../../../specs/004-release-unblocked-capabilities/contracts/capabilities.md).
  Named upstream dependencies remain gated; this release does not complete every
  M0–M3 capability.
- Earlier failed candidates remain unpublished and their receipts were not reused.
  Alpha.5 build and qualification each passed without a retry.

T038, T039 and T064 are satisfied by the signed/public inventory, complete
qualification and delivery checks, and the matching formula supplied by #25.
Homebrew becomes available through the main-branch tap when that change merges.
All 66 public URLs were checked without credentials; 19 package/metadata subjects
and their bundles were downloaded, hashed and independently verified. Direct npm
installation from the public URL passed on the maintainer's Mac, including both
binary identities, isolated state and removal. Consumer verification is optional.

The original approved publication job failed at draft lookup. Maintainer recovery
used that recorded approval and the unchanged signed candidate, retaining full
producer, receipt and byte-equality checks. No tag, package or signature was
replaced. #25 fixes draft discovery/upload and tag retention for future automation.
The failed workflow is retained as failed evidence, not reported as a passing
end-to-end publication run.

GitHub normalizes `~` to `.` in the two Debian download names and their bundles.
The adapter permits only those four bijective aliases and rejects collisions.
Signed logical filenames, package versions and hashes remain unchanged. The
machine-readable inventory records both names; [installation](../../installation.md)
shows how to save the logical filename for checksum verification.
