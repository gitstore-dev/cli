# Feature Specification: M3 Release and Unblocked Capabilities

**Feature Branch**: Existing checkout; no branch hook configured

**Created**: 2026-11-08

**Status**: Specified; package channels agreed; source-reviewed capability scope recorded

**Input**: Review all shipped GitStore releases, implement available M0–M3 gaps,
prepare binary/package releases beginning at v0.1.0-alpha.1 using Release Please
and GitHub Actions attestations. Package installation configures gs and gitstored.
Publishing to external package registries is excluded.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Install a complete distribution (Priority: P1)

A user installs a versioned GitStore package from GitHub using a supported package
manager and receives both executables, their required libraries, safe defaults,
and the integration needed to run the daemon as that user.

**Why this priority**: A release must be usable without a development checkout.

**Independent Test**: Install on a clean supported host, run help/version and a
remote command, then start, inspect and stop a local deployment as an ordinary user.

**Acceptance Scenarios**:

1. **Given** a supported clean host and its package manager, **When** the user
   installs a release, **Then** both executables and dependencies resolve without
   a compiler, manual library copying or manual service-file editing.
2. **Given** an installed package, **When** remote commands run, **Then** no local
   deployment, service-image download or daemon connection is required.
3. **Given** an existing installation, **When** it is upgraded or removed,
   **Then** documentation warns that prereleases guarantee neither upgrade
   compatibility nor preservation of configuration, credentials or stored data.
   Known transition requirements and backup guidance are available; successful
   recovery is not promised.
4. **Given** a system-wide installation performed by an administrator, **When**
   a normal user invokes the runtime, **Then** user-specific state and socket
   access belong to that user and no root daemon is introduced.

### User Story 2 - Obtain a verifiable prerelease (Priority: P1)

A maintainer reviews an automated release proposal and publishes a consistent,
verifiable set of binaries and packages for the selected platforms.

**Why this priority**: Users need provenance and reliable version identity.

**Independent Test**: Exercise a release candidate through generation, verification
and installation, including rejection of altered artifacts and wrong identities.

**Acceptance Scenarios**:

1. **Given** eligible changes, **When** a release proposal is generated, **Then**
   its changelog and all version-bearing artifacts agree; the first public CLI
   prerelease is exactly `v0.1.0-alpha.1`.
2. **Given** final packages, **When** release validation completes, **Then** every
   published executable/archive/package has a checksum and verifiable provenance
   bound to its final bytes, approved workflow, source revision and tag.
3. **Given** a failed build, missing provenance or partial upload, **When** release
   promotion is attempted, **Then** consumers cannot mistake the incomplete set
   for a qualified release and retries preserve existing artifact identities.

### User Story 3 - Use available upstream capabilities (Priority: P2)

A catalogue operator uses commands whose upstream contracts have shipped, without
being blocked solely by an outdated milestone or dependency record.

**Why this priority**: Earlier milestones have useful integration work remaining.

**Independent Test**: For each selected gap, exercise its success and failure cases
against the recorded shipped server version and reconcile the command documentation.

**Acceptance Scenarios**:

1. **Given** a shipped capability with a verified public mapping, **When** its
   client integration is delivered, **Then** supported inputs, outputs, permissions
   and failure outcomes match that version and have conformance evidence.
2. **Given** an implemented command with incomplete release evidence, **When** it
   is assessed, **Then** remaining validation is recorded independently from
   missing implementation and missing upstream contracts.
3. **Given** an unavailable public contract, **When** the user requests the
   affected operation, **Then** it fails before side effects with its named gate.

### Edge Cases

- Installation from a privileged package transaction when no user session exists.
- An older daemon still owns workloads during upgrade; executable/library removal.
- Missing user service manager, unsupported architecture or unavailable macOS backend.
- Prerelease version ordering across package formats and repeated release attempts.
- Correct checksum with wrong producer; valid attestation for different bytes/tag.
- Existing custom configuration, offline runtime assets or denied download consent.
- Expired watch cursors, reconnect duplicates and stale readiness from an older revision.
- Changed upstream migration checksums with retained alpha.8 data.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Review all published upstream release notes through the recorded
  review date and trace relevant claims to immutable public source before enabling
  commands. Preserve historical baselines and record documentation-only proposals.
- **FR-002**: Produce a gap inventory covering M0–M3 with current implementation,
  shipped upstream mapping, remaining client work, failure tests and release gates.
  Implement verified gaps and the experiments that resolve local validation gates;
  unavailable optional capabilities gate only their affected subset.
- **FR-003**: Assess core read/create/delete, hydrated export and authoring fidelity,
  typed watch/reconnect, static-user/OIDC login integration, revision-correlated
  waits, resource discovery and runtime profile compatibility against shipped
  contracts. A release-note headline alone MUST NOT close any dependency.
  The source-reviewed selected increment includes File ID reads, Namespace and
  Repository creation, full core read/list projections and exports, lossless
  supported authoring exports, typed watches, explicit static-user auth lifecycle
  and versioned built-in resource help. Exact supported subsets and remaining
  gates are recorded in [the capability contract](contracts/capabilities.md).
- **FR-004**: Reconcile stale accepted-contract/dependency statements with actual
  code and evidence. In particular preserve existing alpha.9 endpoint and File
  namespace/name compatibility. Upstream PR #523 shipped both Relay ID and
  namespace/name selectors; implement the missing CLI File ID path against that
  available contract, rather than classifying it as an upstream blocker.
- **FR-005**: Preserve public transport boundaries, credential isolation, bounded
  automation, body fidelity and distinct submission/admission/readiness outcomes.
  Generic discovery MUST NOT infer arbitrary callable extension mappings.
- **FR-006**: Use Release Please for reviewed release proposals, version updates,
  changelogs and prereleases starting exactly at `v0.1.0-alpha.1`. Subsequent
  prereleases MUST progress consistently without accidental stable/major promotion.
- **FR-007**: Support Homebrew through a GitHub-hosted tap, APT/DNF through
  GitHub Release `.deb`/`.rpm` assets, and npm through platform-specific npm-format
  tarballs hosted on GitHub Releases. npm packages MUST install both executables
  and required libraries and support the same first-use daemon launch contract.
  Defer Nix. Publish no artifacts to external package-manager registries.
- **FR-008**: Each advertised package MUST install matching gs/gitstored versions,
  required libraries, license notices, completion/reference material and safe
  daemon service integration. Install/upgrade/remove behavior MUST be tested.
- **FR-009**: Configure user-owned runtime directories, owner-only socket defaults
  and an installed daemon launch path without manual service-file editing.
  Package installation MUST NOT invent remote credentials or overwrite user config.
- **FR-010**: Package installation MUST configure the daemon for automatic launch
  as the invoking user on the first local-runtime command. It MUST work when
  package installation runs without a logged-in target user session. Remote
  commands MUST remain independent of daemon startup. Installation MUST NOT start catalogue services or
  provision service images without the existing authorization/consent policy.
- **FR-011**: Attest final release artifacts with GitHub Actions provenance and
  verify exact digest, producer, workflow, tag and revision before promotion.
  Publish signed attestations in the standard format usable by compatible tools,
  with downloadable evidence and optional verification guidance. Package managers,
  installers, gs and gitstored MUST NOT require consumer attestation verification
  or install a mandatory verifier. Release-pipeline validation remains a producer
  quality check. Package checksums alone MUST NOT be described as provenance verification.
- **FR-012**: Validate altered/missing evidence, wrong identities and interrupted
  publication. Release retries MUST NOT silently replace previously published bytes.
- **FR-013**: Build and test each advertised OS/architecture package on clean
  fixtures. macOS remote CLI/control availability and local execution support MUST
  be stated separately; unavailable runtime backends MUST fail actionably.
- **FR-014**: Document known effects of upgrade, migration and removal on running
  workloads, configuration, credentials and stored data. Prerelease software MUST
  NOT promise data preservation or recoverability. Recommend independent backups
  before these operations and publish known destructive/reset requirements.
  Continue implementing and testing ownership, compatibility and cleanup checks
  as engineering safeguards, without representing them as preservation guarantees.
- **FR-017**: User-facing installation and upgrade documentation and every
  prerelease's release notes MUST state that prerelease software makes no upgrade
  compatibility guarantees. Explain that upstream GitStore is also in early alpha
  and its schemas will continue changing incompatibly; the CLI and runtime cannot
  promise compatibility or data preservation across those changes.
  Configuration, public interfaces, daemon protocol and
  persisted state may change incompatibly between prereleases. Automatic migration,
  in-place upgrades and rollback are not guaranteed. Document known transition
  requirements, including backup, manual migration or a fresh deployment where
  applicable. Prereleases also make no data-preservation or recovery guarantees:
  use, upgrade, migration or removal may cause data loss. Recommend independent
  backups and advise against relying on prereleases for irreplaceable data.
- **FR-015**: Qualify the selected distribution with first-use, offline/integrity,
  dependency-resolution and lifecycle recovery checks. Record exact supported hosts,
  package sizes, library costs and known limitations.
- **FR-016**: Payload upload/LFS, CRD/MediaAsset operations, Product transfer,
  publication workflows and CSI remain conditional on their verified contracts.
  Record specific unresolved prerequisites; do not add speculative endpoints.

### Key Entities

- **Capability evidence**: shipped version/revision, public mapping, CLI state,
  test results and remaining gate for one operation.
- **Release**: version, source revision, changelog, selected platforms and promotion state.
- **Distribution artifact**: format, platform, digest, dependencies, provenance and install contract.
- **User installation**: package ownership, configuration, daemon registration,
  retained state and upgrade compatibility.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Every M0–M3 command family has a reviewed gap classification with
  immutable evidence or a named unresolved prerequisite.
- **SC-002**: Every selected package passes clean installation, upgrade and removal
  checks with both executables usable and zero manual library/service-file edits.
  Upgrade checks may pass through an explicit safe refusal of an unsupported
  transition; they do not establish compatibility across prereleases.
- **SC-003**: Every advertised runtime platform passes ordinary-user first startup,
  stop/restart and retained-data ownership checks; unavailable combinations are explicit.
- **SC-004**: All final release artifacts pass provenance verification; all altered,
  missing-evidence and wrong-identity controls fail promotion.
- **SC-005**: The first release reports `0.1.0-alpha.1` consistently across installed
  executables, package metadata, changelog and release tag; a subsequent proposal
  demonstrates correct prerelease advancement.
- **SC-006**: Every newly enabled operation passes its success, authorization,
  partial-failure and applicable reconnect/conflict cases against its declared version.
- **SC-007**: Installation/upgrade documentation and every prerelease's release
  notes carry the no-upgrade-compatibility, no-data-preservation and no-recovery
  guarantee statements and backup guidance; known transition requirements are
  documented without implying a supported migration or rollback.

## Assumptions

- This is one cross-milestone feature; it does not declare M0–M3 fully released.
- The explicit prerelease policy supersedes preservation guarantees inherited
  from earlier milestone wording for this release. Existing retention/ownership
  requirements remain engineering objectives, not user-facing guarantees.
- Homebrew, APT and DNF with first-use user-daemon launch were accepted on
  2026-11-08. npm is also required; Nix is deferred. Consumers may verify the
  published signed attestations with any compatible tool; installation does not
  enforce attestation verification.
- GitHub Release assets are the proposed binary distribution channel. GitHub
  Packages has a different supported-format model; no OCI wrapper is assumed.
- The current Linux arm64 evidence is useful input; broader host qualification is work.
- No publication, release merge or external registry change occurs during specification.
- [Release review](release-review.md) records the source review and its limits.
