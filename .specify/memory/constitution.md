# GitStore CLI Constitution

## Core Principles

### I. Public Contracts and Client Boundaries

GitStore-CLI MUST act as a client of GitStore. Resource operations MUST use the public
GraphQL contract, repository synchronisation MUST use Git, and local service lifecycle
operations MUST use the daemon interface. Transport failures MUST NOT trigger silent
fallback to another write path. The client MUST NOT directly modify server repositories
or datastore files, call internal server interfaces, or duplicate server authorization,
admission, persistence routing, or reconciliation logic.

Every exposed operation MUST map to a verified contract. Missing capabilities MUST have
named prerequisites and explicit unsupported behavior; proposals MUST NOT be represented
as implemented functionality. Required server changes MUST be recorded as upstream
dependencies rather than assumed to be part of a client feature. These boundaries keep
the CLI compatible with the platform and make independently deliverable work explicit.

### II. Automation and Truthful Outcomes

Supported workflows MUST be usable without the Admin UI. Non-interactive execution MUST
never prompt or open a browser; missing credentials, ambiguous targets, and unsupported
choices MUST produce actionable failures. Context, account, namespace, repository, and
deployment selection MUST be explicit or unambiguous. Configuration precedence MUST be
documented and deterministic.

Stdout MUST contain the requested result; progress and diagnostics MUST use stderr.
Machine-readable output and exit categories MUST be stable and documented. Unsupported
output combinations MUST fail explicitly. Non-terminal output MUST omit terminal styling
and honor `NO_COLOR`. Structured resource export MUST preserve metadata, desired state,
observed status, and raw top-level `body` where applicable; rendered Markdown MUST NOT be
presented as a complete resource export.

Transport acceptance, admission, and reconciliation MUST remain distinct outcomes.
Commands MUST preserve submitted operation or revision identity, report partial success,
and return successful wait completion only when the requested condition is verified.
Unavailable correlation MUST produce an explicit unsupported wait result. This prevents
automation from treating an accepted submission as a ready deployment or admitted resource.

### III. Least Privilege and Credential Isolation

Normal local runtime operation MUST run as the invoking user without `sudo`, a root daemon,
or a root-equivalent socket group. Host prerequisites, installation-time changes, and
optional privileged profiles MUST be documented separately. Unsupported hosts MUST fail
with actionable diagnostics and MUST NOT trigger silent privilege escalation.

The daemon MUST authenticate local peers, default to owner-only socket access, and accept
only permitted GitStore profile operations. Parsers, messages, subprocesses, streams, and
concurrent work MUST have documented bounds. Arbitrary host execution, mounts, hooks, or
privileged runtime settings MUST NOT become an unrestricted daemon capability.

Credentials MUST be isolated by installation, issuer, and account; registry credentials
MUST remain separate from catalogue identities. Credential storage MUST use the operating
system credential store by default. File fallback MUST be explicit and permission-restricted.
Logs, status, and diagnostics MUST redact secrets; credential output MUST be an explicit
operation. Authentication flows MUST follow advertised provider capabilities, including
actual refresh and revocation semantics, without inferring support from token shape.

### IV. Durable State and Recoverable Operations

Exactly one Git service writer MUST own repository storage per deployment. Replacement
MUST require observed termination of the prior writer and proven exclusive ownership;
uncertainty MUST fail closed. Shared volumes MUST NOT be described as Git high availability.

Long-running daemon operations MUST have persistent identity and inspectable progress,
terminal outcomes, and structured failures. Duplicate mutations and retries MUST NOT
create duplicate workloads. Client disconnects MUST NOT implicitly cancel detached work.
Daemon restart MUST reconcile existing ownership before starting replacements.

Ordinary stop and failed startup cleanup MUST preserve retained data. Cleanup MUST be
limited to resources owned by the operation and MUST be safe to retry. Migration MUST
enforce exclusive execution and compatibility checks; failed migration MUST block dependent
startup without an unsafe automatic downgrade. Volume mounting, file payload transfer,
manifest admission, and reconciliation MUST retain their separate completion guarantees.
These rules protect data through partial failure rather than relying on process success.

### V. Evidence and Proportionate Verification

Implementation and support claims MUST identify evidence and its limits. Documentation
MUST distinguish intended behavior, implemented contracts, and tested release support.
Unsupported combinations MUST remain gated until their contracts and failure cases pass
validation. Architecture decisions MUST record rationale, alternatives, consequences,
dependencies, and validation needed before release.

Verification MUST target meaningful behavior and material failure modes. New application
CI MUST accompany executable targets or concrete release requirements. Jobs solely for
Markdown or workflow-manifest linting MUST remain deferred. Conventional Commit PR-title
validation MUST remain enabled because contribution policy provides value independently
of application source code. Application checks MUST be added when the relevant code exists,
with contract, integration, recovery, security, and compatibility coverage appropriate to
the change. Checks MUST NOT imply product readiness beyond what they actually validate.

## Product and Architecture Constraints

- `gs` MUST be implemented in Rust and `gitstored` in C. The daemon MUST remain a
  GitStore-profile container runtime, separate from the existing server Git service.
- Linux execution MUST integrate libcrun as a library dependency rather than invoking
  a crun executable. Packaging and linking choices MUST be documented in decision records.
  Maintained runtime, image, networking, and storage implementations MUST be evaluated
  before introducing custom equivalents; low-level container isolation MUST NOT be rebuilt.
- Remote catalogue operations MUST NOT require a local daemon. Remote CLI delivery MUST
  remain independent of local runtime delivery.
- The supported default local profile MUST require only the GitStore distribution and
  documented host capabilities. Docker, Compose, make, language toolchains, and manually
  installed runtime or networking plugins MUST NOT be end-user prerequisites for this path.
- Large service images and optional runtime assets MUST be provisioned on first use,
  verified against pinned identities, and cached. Interactive provisioning MUST obtain
  consent; non-interactive provisioning MUST require explicit authorization or existing
  assets. Offline assets MUST be supported. Help, version, and remote commands MUST NOT
  trigger unrelated runtime downloads.
- Local volumes MUST be the initial persistent storage baseline. CSI and payload-transfer
  extensions MUST remain gated by their own contracts and validation. A mounted volume
  MUST NOT be claimed to implement object storage or Git LFS transfer semantics.
- Platform support MUST be advertised only after backend-specific validation. A Linux
  container runtime on macOS MUST use a validated platform backend or managed Linux VM;
  Linux execution on the macOS kernel MUST NOT be implied. Windows remains deferred.
- The [PRD](../../docs/prd.md) defines milestone scope and detailed product requirements.
  A general-purpose scheduler, Git replication/sharding, and automatic storage-cluster
  installation MUST NOT be added without an explicit scope change.

## Development Workflow and Quality Gates

- Specifications MUST state user outcomes, bounded scope, acceptance scenarios, and
  measurable success criteria. Plans MUST identify applicable principles and dependencies.
  Tasks MUST trace implementation and verification to those requirements.
- M0 MUST establish command coverage, transport boundaries, discovery prerequisites, and
  daemon/runtime decisions. A real contract or named prerequisite MUST cover every proposed
  operation before its implementation is represented as supported.
- Contributors MUST reuse applicable existing contracts, components, and conventions.
  Similar checks MUST serve each repository's actual needs.
- PR titles MUST follow GitStore's accepted Conventional Commit types with optional scope:
  `feat`, `fix`, `docs`, `chore`, `refactor`, `test`, `style`, `perf`, `ci`, `build`, `revert`.
  Title validation MUST use metadata only, read-only permissions, and no contributor code
  checkout or execution.
- Rust build, formatting, lint, and behavior checks MUST accompany the CLI implementation.
  C build, behavior, and address/undefined-behavior sanitizer checks MUST accompany daemon
  implementation. Protocol parsing MUST receive malformed-input and resource-bound tests
  before release. Platform and compatibility checks MUST accompany advertised support.
- Relevant verification MUST cover conflicts, partial failures, credential isolation,
  content fidelity, and recovery when affected by a change. Reports MUST distinguish local,
  simulated, and live results and identify unverified upstream behavior.
- Review MUST assess compliance with this constitution and the applicable specification.
  New abstractions, dependencies, or operational prerequisites MUST have a concrete purpose
  within the approved milestone. Exceptions MUST identify the violated rule, rationale,
  bounded scope, and resolution; permanent exceptions require a constitutional amendment.

## Governance

This constitution governs repository specifications, plans, implementation, and review.
Explicit user instructions take precedence. The PRD supplies product detail; conflicts
between it and this constitution MUST be surfaced and resolved explicitly rather than
silently changing scope. Template examples and unverified upstream proposals MUST NOT
be treated as adopted project rules.

Amendments MUST identify the affected rules, rationale, compatibility impact, and any
required migration or follow-up. They MUST update the version and last-amended date while
preserving the original ratification date. Version changes MUST follow semantic versioning:
MAJOR for incompatible principle removal or redefinition, MINOR for new principles or
materially expanded obligations, PATCH for clarifications without semantic change.

Specification and plan reviews MUST identify conflicts with amended rules before dependent
implementation proceeds. Dependent templates and commands read the constitution at runtime;
constitution updates MUST NOT silently rewrite them. Compliance evidence and explicit
exceptions MUST be recorded with the affected work. A temporary amendment impact report
MUST be removed before committing the constitution.

**Version**: 1.0.0 | **Ratified**: 2026-10-27 | **Last Amended**: 2026-10-27
