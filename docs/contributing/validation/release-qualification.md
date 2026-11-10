# Release qualification

**Status: not ready for publication.** Alpha.2 has a complete packaged candidate
and 6/22 passing host/channel selections. Linux fixture failures block the
remaining selections. No tag-bound release signing, producer verification or
publication has run. T038, T039 and T064 remain open.

Use the [release runbook](../releases.md) for operating instructions.

## Candidate identity and integrity

| Field | Recorded value |
| --- | --- |
| Tag | `v0.1.0-alpha.2` |
| Source | `a23407463cabb3a309e713a7e291380aa8dacdd7` |
| Build | [42804211944](https://github.com/gitstore-dev/cli/actions/runs/42804211944), all four platform rows and assembly passed |
| Qualification | [42806007675](https://github.com/gitstore-dev/cli/actions/runs/42806007675), failed; collector emitted no complete receipt artifact |
| Frozen subjects | 33 subjects, 468,850,814 bytes; hashes/sizes, checksums and executable/install-inventory identities verified locally |
| Inventory SHA-256 | `d42cb51f794d3612aaeac712d0cf19922b9173da5afacc8c9ec2a1facddc43fd` |
| SHA256SUMS SHA-256 | `77939d20bb02fe65b36740dfb68894bc13ecbc4b49d3fc86506b38f7ca0bde4a` |
| Detailed evidence | [Artifact hashes, receipt identities and performance results](alpha2-qualification-review.json) |

Both alpha.1 and alpha.2 remain unpublished drafts. Alpha.1 builds failed on Rust
component setup; alpha.2's successful build exercised the repair. These records
qualify only their stated source/bytes, never a later candidate automatically.

## Host/channel results

| Host | Architecture | Required channels | Alpha.2 result |
| --- | --- | --- | --- |
| Ubuntu 24.04 | x64 | Archive, Homebrew, APT, npm | Blocked before installation: inaccessible inherited runner directory |
| Ubuntu 24.04 | arm64 | Archive, Homebrew, APT, npm | Same fixture failure |
| Fedora 44 | x64 | Archive, Homebrew, DNF, npm | Blocked before installation: pinned base image unavailable |
| Fedora 44 | arm64 | Archive, Homebrew, DNF, npm | Same fixture failure |
| macOS 15 | x64 | Archive, Homebrew, npm | All three passed |
| macOS 15 | arm64 | Archive, Homebrew, npm | All three passed |

The six macOS receipts bind the same tag/source and all 33 subject hashes.
They cover remote/control behavior; macOS local workload execution is unsupported.
No Linux product failure is inferred from fixture preparation failures.

## Outstanding gates

| Gate | Required evidence |
| --- | --- |
| Linux fixtures | Apply [#16](https://github.com/gitstore-dev/cli/pull/16): available pinned Fedora 44 base and working-directory change after switching to the fixture UID. |
| Full qualification | Rebuild from the reviewed repair/version commit and pass all six hosts/22 selections with exact-byte receipts. Old tags and partial receipts cannot be reused for a new source. |
| T038 | Actual tag-bound signing/producer verification, own signed positive and required negative controls. Release Please proposal/tag events and the four-platform build have passed. |
| T039 | Complete qualified inventory, attestation bundles, support matrix and readiness review. |
| T064 | Final guide, links and delivered inventory review; distinguish qualified support from implemented behavior. |
| Publication | Required maintainer approval of `release-publication` after qualification and producer verification. |

Repair validation: 91 package-policy cases (84 passed, seven explicit fixture
skips), all 29 release-policy cases, workflow lint and documentation checks.
A public-bundle regression covers inaccessible caller/target directories. An
isolated Linux probe verified the actual UID-1001 → UID-11000 transition and
private-home clone/checkout. Both replacement Fedora fixture images built with
all prerequisites on arm64/amd64; image builds do not establish native acceptance.

## Supporting acceptance evidence

| Scope | Evidence | Limit |
| --- | --- | --- |
| CLI capabilities | [Contracts and gap inventory](../../../specs/004-release-unblocked-capabilities/contracts/capabilities.md), [controlled CLI results](remote-cli.md), [shipped-server integration](https://github.com/gitstore-dev/cli/actions/runs/42692602984) | Pinned alpha.9 public contract; unsupported operations remain gated. |
| Development packages | [Package review](alpha1-package-review.json), [all 22 replacement/removal selections](alpha1-upgrade-review.json) | Earlier source/byte identities; not final alpha.2 qualification. |
| Linux native lifecycle/performance | [Ubuntu](https://github.com/gitstore-dev/cli/actions/runs/42669876762), [Fedora](https://github.com/gitstore-dev/cli/actions/runs/42669882585) | Earlier development pairs on both architectures; require fresh tag-bound acceptance. |
| Release Please | [Alpha.1 event](https://github.com/gitstore-dev/cli/actions/runs/42790057056), [alpha.2 event](https://github.com/gitstore-dev/cli/actions/runs/42804184989) | Successful tag/draft creation is not publication. |
| Signing policy | [Own signing preflight and negative controls](https://github.com/gitstore-dev/cli/actions/runs/42630137792) | Branch fixture only; production policy rejects its identity. |

## Requirement trace

| Requirement | Evidence or remaining gate |
| --- | --- |
| FR-001 | Pinned upstream source/release review in capability contracts. |
| FR-002 | M0–M3 gap inventory and explicit external dependencies in capability contracts. |
| FR-003 | Controlled and shipped-server capability checks; unsupported providers/operations stay gated. |
| FR-004 | Command matrix/dependencies reconciled with shipped alpha.9 selector mappings. |
| FR-005 | Public transports, credential isolation, authoring fidelity and bounded-watch regressions. |
| FR-006 | Actual Release Please proposals, synchronized versions, tags and draft events. |
| FR-007 | Homebrew/APT/DNF/npm development acceptance; final matrix remains incomplete. Nix deferred. |
| FR-008 | Matching executable pairs and complete frozen packaging/source/relink inventory; qualified distribution pending. |
| FR-009 | Private first-use directories/socket and daemon-independent remote commands tested. |
| FR-010 | Earlier Linux lifecycle/first-use controls passed; fresh Linux tag qualification blocked. |
| FR-011 | Signing preflight passed; final-subject attestation and producer verification pending T038. |
| FR-012 | Retry/conflict/incomplete-release and identity controls tested; actual final signed evidence pending. |
| FR-013 | Six current macOS selections passed; remaining 16 Linux selections blocked by fixtures. |
| FR-014 | Lifecycle documentation/tests retain no upgrade, data-preservation or recovery guarantees. |
| FR-015 | Earlier native/offline/recovery/integrity/performance evidence; fresh Linux tag acceptance pending. |
| FR-016 | Upload/LFS, CRD/MediaAsset, transfer, catalogue publication and CSI retain named gates. |
| FR-017 | Installation/help/package notices and publisher enforce the early-alpha/schema-change disclaimer. |
| SC-001 | Command-family classification and remaining dependencies in capability contracts. |
| SC-002 | Earlier 22-selection lifecycle matrix passed; final exact-byte matrix incomplete. |
| SC-003 | Earlier Ubuntu/Fedora native acceptance; current macOS remote/control acceptance. |
| SC-004 | Final release provenance verification remains open. |
| SC-005 | Alpha.1 version synchronization tested; alpha.2 train/build verified; no qualified public distribution yet. |
| SC-006 | Controlled success/fault and pinned shipped-server checks for enabled capabilities. |
| SC-007 | Disclaimer coverage tested; final release-note/inventory review remains open. |

Detailed superseded validation is retained in [Git history](https://github.com/gitstore-dev/cli/blob/57296249c1ebe464e51f51a7fb74a98e5b366070/docs/contributing/validation/release-alpha1.md).
Update this summary when evidence or qualification status changes; keep execution
logs and investigation history in CI and pull requests.
