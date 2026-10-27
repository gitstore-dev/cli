# Feature Specification: M0 Contracts

**Feature Branch**: `main` (no branch-creation hook configured)

**Created**: 2026-10-27

**Status**: Draft

**Input**: User description: "M0 of the PRD"

**Source**: [Product requirements](../../docs/prd.md), especially “M0 Contracts” in Delivery and acceptance.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Establish Supported Command Contracts (Priority: P1)

As a CLI contributor, I need an inventory of proposed commands and their
dependencies so that implementation follows supported contracts without guessing
server capabilities or duplicating server responsibilities.

**Why this priority**: M0 must distinguish executable contracts from product intent
before remote CLI implementation begins.

**Independent Test**: Review every PRD command family and resource/operation pair;
each has a contract reference or named prerequisite, an owner, target milestone,
and explicit support status.

**Acceptance Scenarios**:

1. **Given** a proposed operation, **When** a contributor consults the matrix,
   **Then** they find command grammar, target scope, owning component, transport,
   evidence, and supported, blocked, or deferred status.
2. **Given** an unconfirmed operation, **When** its entry is reviewed, **Then** it
   names the missing contract, responsible component, resolution gate, and expected
   unsupported behavior without claiming support.
3. **Given** resource, repository synchronisation, and service lifecycle commands,
   **When** routing is reviewed, **Then** each uses its designated public or local
   interface without silent fallback or direct server storage/internal service access.
4. **Given** authentication, discovery, export, and completion requirements,
   **When** dependencies are reviewed, **Then** individual provider capabilities,
   content fidelity, and admission versus readiness guarantees are evidenced or
   recorded as prerequisites.

---

### User Story 2 - Agree on Daemon and Runtime Boundaries (Priority: P1)

As a runtime contributor, I need explicit communication, ownership, packaging, and
privilege decisions so that later work produces a recoverable, profile-scoped
service with a clear security boundary.

**Why this priority**: Unclear ownership and privilege assumptions risk data loss,
duplicate repository writers, or an unusable installation experience.

**Independent Test**: Walk through start, repeated start, disconnect, stop,
migration, and daemon restart using decision records; every case has a defined
owner, observable result, and failure outcome without requiring running software.

**Acceptance Scenarios**:

1. **Given** the proposed daemon interface, **When** its decision record is reviewed,
   **Then** version negotiation, capabilities, request correlation, bounded messages,
   operation tracking, deadlines, cancellation, and duplicate mutations have explicit
   rules and representative success/failure examples.
2. **Given** a detached operation or interrupted client, **When** recovery is
   considered, **Then** operation state remains inspectable and retries cannot create
   duplicate workloads or overlapping Git writers.
3. **Given** an execution or packaging choice, **When** its decision record is
   reviewed, **Then** it states the selected direction, alternatives, tradeoffs,
   platform limits, distribution obligations, and validation gates.
4. **Given** an unsupported host or optional privileged profile, **When** installation
   is reviewed, **Then** ordinary runtime remains per-user, host requirements and
   installation-time privileges are explicit, and local support is withheld until
   the relevant platform passes validation.

---

### Edge Cases

- A kind has a read contract but no list/upload contract: classify each operation
  separately rather than extrapolating capabilities from the kind.
- Discovery is absent, stale, ambiguous, or behind a proxy base path: define
  compatibility errors and conditions for an explicitly configured legacy mode.
- A provider returns identical access/refresh credentials: do not infer refresh,
  revocation, or device authorization capabilities from response shape.
- A submission succeeds but admission/reconciliation is pending or fails: define
  separate states and preserve operation/revision identity.
- An incompatible socket version, oversized request, unauthorized peer, repeated
  mutation, or disconnect occurs: define bounded failure and recovery behavior.
- The daemon restarts while a prior Git writer may still exist: require proof of
  exclusive ownership before replacement.
- A host lacks rootless or virtualization capabilities: document actionable
  unsupported status and fallback validation gates without silent elevation.
- A documentation-only contribution arrives before executable targets exist:
  review the contracts without introducing CI jobs solely for Markdown or workflow lint.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: M0 MUST deliver a command/operation matrix covering all PRD command
  families and resource/operation pairs, marking later-milestone proposals deferred.
- **FR-002**: Each entry MUST identify command grammar, purpose, scope, owner,
  transport, support status, target milestone, and a real contract reference or
  named prerequisite with responsible component and resolution gate. Evidence MUST
  identify the reviewed source revision or date.
- **FR-003**: Routing rules MUST preserve public resource access, repository
  synchronisation, and local lifecycle boundaries. Remote catalogue operations
  MUST remain independent of a local daemon.
- **FR-004**: A dependency register MUST separately cover deployment/provider
  discovery, per-flow authentication, operation mappings, concurrency, admission
  correlation, export fidelity/pagination, and deferred payload/storage capabilities.
  It MUST distinguish proposals from verified behavior.
- **FR-005**: Command contracts MUST define context/identity selection, configuration
  precedence, output modes/defaults, stdout/stderr separation, non-interactive
  behavior, exit categories, and structured errors, including partial success,
  conflicts, unsupported capabilities, and timeouts.
- **FR-006**: Completion contracts MUST distinguish transport acceptance, admission,
  and reconciliation, preserve operation identity, and reject wait guarantees
  unsupported by the available contracts.
- **FR-007**: A daemon protocol decision record MUST define operation coverage,
  framing/serialization, cross-component interoperability, version/capability
  negotiation, request correlation, idempotency, deadlines, cancellation, streaming,
  message/resource limits, and structured terminal/nonterminal outcomes.
- **FR-008**: The daemon decision MUST define peer authorization, default access
  permissions, permitted profile operations, disconnect behavior, durable operation
  identity, restart recovery, and exclusive repository storage ownership.
- **FR-009**: Runtime decision records MUST document component ownership and the
  PRD-mandated language split, library integration, image preparation, network/volume
  boundaries, platform backends, dependency delivery/licensing, first-use
  provisioning, and offline/cache behavior. Unevidenced choices MUST include a
  bounded evaluation and release gate.
- **FR-010**: The privilege model MUST distinguish ordinary per-user operation,
  host prerequisites, installation-time changes, and optional privileged profiles,
  prohibiting silent elevation and premature platform support claims.
- **FR-011**: Decision records MUST state status, context, decision or proposed
  direction, alternatives, consequences, dependencies, and validation gates.
- **FR-012**: Application CI MUST be deferred until runnable targets or concrete
  release requirements exist. M0 MUST NOT introduce jobs solely to validate
  Markdown or workflow definitions. PR-title validation MUST enforce GitStore's
  accepted Conventional Commit types with optional scopes, using metadata only
  and read-only permissions without checking out contributor code.
- **FR-013**: Contributor guidance MUST explain PR-title validation and identify
  application build, test, sanitizer, compatibility, and release checks to consider
  alongside the relevant implementation, reusing applicable GitStore conventions.
- **FR-014**: M0 review MUST trace every requirement to an artifact and acceptance
  evidence, listing unresolved prerequisites with an owner and milestone. Naming a
  prerequisite satisfies coverage but MUST NOT imply operation availability.

### Key Entities

- **Command contract**: An operation, user-visible behavior, transport, owner,
  scope, classification, and supporting evidence or prerequisite.
- **Dependency**: A missing capability/contract, responsible component, affected
  operations, resolution gate, and target milestone.
- **Decision record**: A consequential boundary or choice with status, rationale,
  alternatives, consequences, and validation gates.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Reviewers can classify 100% of PRD operations as supported, blocked,
  or deferred, with zero entries lacking evidence or a named prerequisite.
- **SC-002**: Reviewers can trace 100% of M0 requirements to a planned deliverable
  and acceptance evidence, with every unresolved dependency assigned an owner/gate.
- **SC-003**: Decision records resolve all six walkthroughs—start, repeated start,
  disconnect, stop, migration, and restart—with explicit ownership, completion,
  and failure/recovery outcomes for each.
- **SC-004**: M0 introduces only PR-title validation: all accepted commit types
  permit optional scopes, other types fail validation, and no application,
  documentation, or workflow lint jobs run before relevant targets exist.

## Assumptions

- This invocation specifies M0. Application CI is deferred; PR-title validation
  provides value independently of application source code.
  Detailed inventories and decision records are M0 implementation work. M1 remote commands,
  M2 runtime implementation, and M3 extensions remain outside this feature.
- The PRD is authoritative. Upstream contracts require revision-specific verification
  during M0 execution; this spec does not assert those proposals already exist.
- No CLI, daemon, dependency manifest, or application tests exist at specification
  time, so there is no meaningful application CI to introduce yet.
- GitStore-CLI is a client of GitStore. Coordination with the GitStore agent is
  informational only; this repository's work does not authorize upstream changes.
- The default branch is `main`. No Spec Kit extension hooks are configured. The
  constitution was adopted as version 1.0.0 on 2026-10-27 and governs this plan.
- PRD technology/security constraints remain authoritative; this specification
  defines required decisions and evidence without choosing wire formats or libraries.
