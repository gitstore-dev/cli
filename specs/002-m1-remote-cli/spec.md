# Feature Specification: M1 Remote CLI — Unblocked Commands

**Feature Branch**: Existing checkout; no branch change required.

**Created**: 2026-10-29

**Status**: Development implementation; verification and upstream/release gates tracked separately

**Input**: “M0 is spec'd. Let's start with M1 (items which are unblocked).”

M0 remains the contract baseline. This feature delivers the portions of M1 that
do not require resolving an open upstream capability. It does not declare the
full M1 milestone complete: advertised login flows, author updates, watches,
verified waits, and hydrated export retain their release gates.

## User Scenarios & Testing

### User Story 1 — Select an installation safely (Priority: P1)

An operator selects a saved installation/account and verifies effective settings
before running commands. Scripts inspect the same settings without receiving
credentials or starting a local service.

**Why this priority**: Every remote operation needs deterministic, isolated identity
selection; these commands are useful without server changes.

**Independent Test**: Use two temporary configured installations and saved identities;
list, show, switch and validate them without any server or daemon running.

**Acceptance Scenarios**:

1. **Given** two saved contexts, **When** the operator selects one and shows its
   settings, **Then** the selected installation/account and configuration sources
   are reported and subsequent commands use that selection.
2. **Given** a configured default, **When** flags and environment override it,
   **Then** precedence follows M0 and endpoint overrides cannot reuse credentials
   belonging to another installation.
3. **Given** missing, malformed or ambiguous configuration, **When** validation or
   selection runs, **Then** it fails with the documented category without changing
   the prior valid selection or contacting a server.
4. **Given** an existing saved credential, **When** local auth status runs,
   **Then** presence is reported as local evidence; only explicit `auth token`
   prints the credential. Switching account does not cross installations silently.
5. **Given** a selected CLI-owned stored credential, **When** local logout runs,
   **Then** that credential is removed and remote invalidation is reported unsupported;
   unrelated identities and caller-managed external credential files are untouched.

### User Story 2 — Run explicit operations without the Admin UI (Priority: P1)

An operator uses an existing credential to run explicit GraphQL operations,
inspect catalogue resources and request supported creation/deletion. This enables
remote work before login discovery and the full authoring workflow land.

**Why this priority**: These public operations have contract evidence and do not
require a local runtime or new server endpoints.

**Independent Test**: With a controlled compatible installation and protected
credential input, inspect each included kind, create a Product and CategoryTaxonomy,
and request their deletion. Validate request/response behavior against controlled
fixtures before performing live checks.

**Acceptance Scenarios**:

1. **Given** a selected installation and matching credential, **When** a query uses
   literal, typed or file-backed fields, **Then** the server receives the intended
   values and stdout preserves returned data, errors and extensions.
2. **Given** HTTP success containing operation errors and partial data, **When**
   the result is returned, **Then** partial data remains available and the command
   exits nonzero with a safe diagnostic.
3. **Given** existing Namespace, Repository, Product, CategoryTaxonomy and Collection
   resources, **When** read or bounded list runs, **Then** the selected objects or
   page is displayed with documented identity and scope. File supports read only.
4. **Given** valid Product or CategoryTaxonomy author input, **When** create runs,
   **Then** it submits only supported author fields, uses server-selected placement,
   preserves supported body content and distinguishes admission from readiness.
5. **Given** an existing supported deletion target, **When** delete runs by name
   or typed identity, **Then** it resolves an exact identity and reports requested
   termination, without claiming controller cleanup has completed.
6. **Given** multiple author documents and a malformed later document, **When** a
   batch is processed, **Then** invalid input is rejected before submission. A
   later server failure identifies submitted, failed and unsubmitted items without
   retrying successful writes.

### User Story 3 — Clone and submit through Git (Priority: P2)

An operator clones a GitStore repository, edits it using ordinary Git, and submits
an update while retaining control of branch history and worktree state.

**Why this priority**: Git provides an existing authoring path while higher-level
updates and export fidelity remain gated.

**Independent Test**: Clone and synchronize against a disposable repository through
its public Git path. Inspect remotes, branch tracking, commit identities and
unchanged local history on all rejected cases.

**Acceptance Scenarios**:

1. **Given** a selected repository, **When** clone runs, **Then** the destination
   defaults to its name, the sole remote is `gitstore`, and an existing checked-out
   branch tracks that remote.
2. **Given** `--remote origin` or another valid name, **When** clone runs, **Then**
   only that remote is created and branch tracking uses it. An empty repository
   requires no fabricated checked-out branch.
3. **Given** one eligible remote and a clean worktree, **When** sync runs from a
   subdirectory, **Then** it resolves the intended installation/repository/ref by
   URL and submits only a normal fast-forward update when local is ahead.
4. **Given** multiple eligible remotes, a misleading `gitstore` name, a mismatched
   installation, dirty worktree, behind branch or divergent history, **When** sync
   runs without sufficient explicit selection, **Then** it fails without rewriting
   history or choosing a different repository implicitly.
5. **Given** an accepted push, **When** sync reports success, **Then** it retains
   the submitted commit and marks admission/readiness pending or unverified. Known
   unsupported waits are rejected before any push.

### Edge Cases

- Empty selectors; malformed config; interrupted selection persistence; missing
  keychain entry; unavailable credential store; unsafe credential file permissions.
- Proxy endpoint prefixes, cross-installation overrides and redirects that could
  expose credentials to another host.
- Invalid formats, multiple stdin consumers, conflicting GraphQL input modes,
  unnamed multi-operation requests and oversized input.
- Expired credentials, permission denial, missing targets, rate limits, deadlines,
  malformed responses, partial data and lost mutation responses.
- Empty and changing list pages, opaque cursors and wrong-kind resource IDs.
- Unknown kinds, controller-owned fields, unsupported author fields, Unicode bodies,
  blank lines and zero/one/multiple trailing newlines.
- Invalid clone remote name, nonempty destination, missing branch and concurrent
  remote advancement. No force retry or implicit merge.

## Requirements

### Functional Requirements

- **FR-001**: Deliver context list/show/use, config validation, account switch,
  explicit token output, local-only auth status/logout, help and version without contacting
  a server or initializing a daemon.
- **FR-002**: Select settings with flags > environment > selected configuration >
  defaults, reject ambiguity, retain proxy prefixes, and isolate credentials by
  installation/issuer/account. Registry credentials remain separate.
- **FR-003**: Use the operating-system credential store by default. Accept existing
  credentials through protected stdin or explicitly selected user-only files; file
  persistence requires opt-in. No secrets in normal output, errors or process args.
- **FR-004**: Provide a documented local bootstrap format for saving contexts and
  supplying existing credentials. Do not invent context-create or login commands
  as a prerequisite for included commands.
- **FR-005**: Support direct GraphQL with M0 literal/typed/file/stdin rules, operation
  selection, faithful responses, bounded deadlines and no blind mutation retries.
  Caller-written mutations do not imply supported high-level updates or bypass
  server authorization.
- **FR-006**: Support read/list for Namespace, Repository, Product, CategoryTaxonomy
  and Collection, and read for File metadata, using verified public selectors and
  operation names. Validate mappings against the supported server contract.
- **FR-007**: Support Product and CategoryTaxonomy create from Markdown frontmatter
  and body with strict author-field validation. Product placement stays server-owned;
  reject placement flags until DEP-003 is resolved.
- **FR-008**: Support Namespace, Repository, Product and CategoryTaxonomy delete by
  exact resolved identity; report termination initiation and dependency restrictions.
- **FR-009**: Bound author input at 100 documents and 16 MiB total UTF-8 input,
  validate before writes, and preserve per-document partial outcomes. Aggregate
  exit is the first nonzero outcome in input order.
- **FR-010**: Provide table summaries and JSON for included resource operations,
  and M0 table/JSON/YAML local-status and Git summaries. GraphQL remains JSON-only.
  Hydrated resource YAML/Markdown and authoring export stay gated by DEP-005;
  inspection JSON must not be advertised as a lossless export.
- **FR-011**: Implement clone with `gitstore` as default remote, `--remote` override,
  branch tracking when possible, no extra `origin` alias, and rejection of a
  nonempty destination without overwriting it.
- **FR-012**: Implement sync selection, fetch and fast-forward push without modifying
  local branch/worktree, silently choosing a context repository, force-pushing or
  falling back to GraphQL. Names are not remote identity evidence or tie-breakers.
- **FR-013**: Distinguish transport acceptance, admission and readiness; retain known
  revision identity and mark uncertain submission honestly. Reject gated waits
  before writes; forward received Git progress without a live-streaming guarantee.
- **FR-014**: Honor non-interactive mode, stdout/stderr separation, no-color behavior,
  format validation and M0 exit/error categories. List pages contain at most 100
  objects or the smaller server limit; no catalogue-wide hydration or snapshot claim.
- **FR-015**: Reject excluded operations with their named prerequisite before network
  writes, prompts, browser launches or runtime provisioning.
- **FR-016**: Validate included behavior locally and against controlled public
  contract fixtures; separately record live compatibility, supported hosts and
  credential-store behavior. Simulated evidence cannot establish full M1 release.
- **FR-017**: Add build/format/lint/behavior checks alongside executable targets.
  Published binaries must meet DEP-013 provenance requirements; development builds
  do not by themselves satisfy that release gate.

- **FR-018**: Implement clone/fetch/push and repository inspection through a maintained
  library, without wrapping C Git or another Git executable. End users must not
  require a separately installed Git executable.

### Scope and dependency decisions

The [M0 matrix](../../docs/contracts/command-matrix.md) and
[dependency register](../../docs/contracts/dependencies.md) remain authoritative.
This table narrows delivery without closing open gates.

| Records | Included subset | Excluded or gated behavior |
| --- | --- | --- |
| CMD-001–004, 010–011, 077–078 | Context/config/account/token/help/version | No server discovery or runtime |
| CMD-007, 009 | Local stored-credential removal/presence only | Remote invalidation/verified status: DEP-001 |
| CMD-015–016, 020–021, 025–026, 031–032, 037–038, 042 | Core read/list and File read | File list absent; hydrated export: DEP-005 |
| CMD-024, 030 | Product/Category create with supported fields | Placement override: DEP-003 |
| CMD-018, 023, 028, 034 | Delete requests | No completed-cleanup claim or correlated wait |
| CMD-044 | Explicit GraphQL | No inferred high-level operation support |
| CMD-045 | Submission-only sync and received progress | Waits: DEP-004; streaming guarantee: DEP-016 |
| CMD-084 | Clone and remote configuration | No mutation or admission claim |
| CMD-006, 008, 012–014, 017, 019, 022, 027, 029, 033, 035–036, 039–041, 043, 079–082 | Excluded from this increment | Discovery/auth: DEP-001/006; Namespace/Repository authoring/export: DEP-005; updates: DEP-009; Collection: DEP-002; watch: DEP-017 |

Namespace/Repository create are excluded conservatively because their full authoring
contract is gated by DEP-005. Existing installations supply these resources for
resource/Git scenarios. Conditional legacy static-session login is not selected:
compatibility evidence under DEP-001 is still required. Full M1 exit criteria in
the PRD remain unchanged.

### Key Entities

- **Context**: Saved installation endpoint, account/provider and optional namespace,
  repository/ref defaults, selected for subsequent commands.
- **Credential identity**: Installation/issuer/account associated with protected
  material and separate local-presence/server-verification evidence.
- **Resource selection**: Kind, namespace and name or typed identity selecting one
  object; list selection adds bounded page size and opaque cursor.
- **Author document**: Product/CategoryTaxonomy identity, supported desired fields
  and raw body, without controller-owned state.
- **Operation outcome**: Per-command/document result, submission certainty, known
  revision, admission/readiness states and structured error category.
- **Git target**: Remote URL, installation/repository identity, local/remote refs,
  chosen remote name and submitted commit.

## Success Criteria

### Measurable Outcomes

- **SC-001**: All included local commands work with networking unavailable and no
  daemon installed; selection remains valid after rejected changes.
- **SC-002**: Every included remote command passes its acceptance scenarios on the
  controlled compatible installation; emitted requests match their intended public
  operation and selected identity.
- **SC-003**: Every credential-isolation, invalid-input, ambiguous-target, partial-error
  and unsupported-operation case returns deterministic categories without secret
  leakage or unintended writes.
- **SC-004**: Clone passes default/overridden remote cases; sync passes no-change,
  fast-forward, behind, divergent, dirty, subdirectory and concurrent-push cases
  without altering local history and with no Git executable available.
- **SC-005**: Every included mutation distinguishes submitted/unknown outcomes from
  verified admission/readiness; every gated wait fails before submission.
- **SC-006**: Documentation lists implemented subsets and outstanding release gates;
  development checks pass and any publication has digest-bound provenance evidence.

## Assumptions

- This increment does not resolve upstream server capabilities or implement M2/M3.
- Users already have a compatible installation, existing credentials and, where
  needed, Namespace/Repository resources. Fixtures must not modify unrelated live
  resources or the upstream working tree.
- M0 source evidence is a baseline to verify during planning, not a claim of deployed
  compatibility. No dependency is resolved by this spec.
- Read/list JSON is an inspection representation. Content-fidelity release claims
  wait for DEP-005; normal inspection does not require authoring export.
- Remote-name behavior follows the agreed 2026-10-29 clone contract.
- File selector normalization is pending the upstream File lifecycle specification.
  This increment uses the verified `file(namespace, name)` read; do not invent a
  `FileBy` shape or enable new File lifecycle operations from that future direction.
