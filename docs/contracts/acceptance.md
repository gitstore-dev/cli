# M0 Acceptance Evidence

**Review date**: 2026-10-27

**Reviewer role**: Implementation contract reviewer.

**Evidence type**: Document/source review and paper walkthroughs. No executable CLI,
daemon, live endpoint, performance, host support or GitHub-hosted pass is claimed.

## Review record format and rubric

Each SCN record identifies FR/SC references, reviewed artifacts, preconditions,
action, expected observations, actual result (`not-run`, `pass`, `fail`), review
date, reviewer role and open findings. Table rows inherit the date, role and evidence
type above. Expected behavior is never sufficient by itself to record a pass.

M0 requires complete operation coverage, evidence or named prerequisites, owned/gated
findings, complete protocol/ADR decisions and no orphan references. Upstream/M2/M3
release gates may stay open; unresolved M0 decisions may not. All review scenarios
start `not-run` and are updated only after inspection.

## US1 — Command contracts

All rows below were reviewed on the date and by the role recorded above. Preconditions
are the named documents and committed source baseline, not a running CLI. Each action
compares the expected contract against the actual published text/source. `pass` means
that comparison succeeded; listed release gates remain untested.

| Scenario | Requirements | Artifacts / precondition | Action | Expected and observed result | Actual result | Open findings / later gate |
| --- | --- | --- | --- | --- | --- | --- |
| SCN-001 | FR-001, FR-002, SC-001 | All PRD families versus matrix | Enumerated independent family counts and checked each resource/operation | 83 records = 82 proposed/local/representation capabilities plus one explicit File-list exclusion; 30 initial resource operations. No missing family. | pass | None |
| SCN-002 | FR-002, FR-014, SC-002 | Evidence/command/dependency records | Inspected fixed-revision schemas, resolvers, auth and Git hooks; reviewed profile inheritance | Every row supplies/inherits owner, scope, input/output, auth/concurrency/completion and evidence; blocked rows name DEP records. | pass | Open release prerequisites in dependency register |
| SCN-003 | FR-003, FR-005 | cli-behavior.md selection/routing | Traced ambiguous context/remote and endpoint override cases | Exit 2 before writes; no alternate transport/default repository; no credential reuse across installations. | pass | None |
| SCN-004 | FR-004, FR-005 | Auth discovery/base path and EVD-010/019 | Compared static-session response/provider methods with discovery requirements | DEP-001; unsupported unknown flow is exit 6; proxy prefix retained; explicit legacy mode required. Identical token slots do not imply OAuth refresh or device capability. | pass | DEP-001 |
| SCN-005 | FR-002, FR-004 | Collection/File declarations versus matrix | Checked collection Query-only and file read/watch schema/resolver | Collection writes/watch blocked by DEP-002; File list/upload not inferred. CategoryTaxonomy mapping retained; controller mutations excluded. | pass | DEP-002, DEP-007 |
| SCN-006 | FR-005 | Author mutation inputs and client version rules | Checked ObjectMetaInput and NamespaceMetadataInput versus status inputs | No author expected-version field; updates blocked by DEP-009, never silently overwriting or misusing status API. | pass | DEP-009 |
| SCN-007 | FR-005 | Multi-document behavior | Traced success, failure, remaining-not-submitted and retry case | Per-item outcome/identity retained; first nonzero input-order aggregate exit; no retry of successful writes or invented transaction. | pass | M1 executable conformance |
| SCN-008 | FR-005 | Direct GraphQL behavior | Reviewed HTTP 200 errors/partial data and literal/typed/file input rules | Preserve data/errors/extensions; nonzero on errors; -f n=3 string versus -F n=3 number, one stdin consumer, --input conflicts rejected. | pass | M1 executable conformance |
| SCN-009 | FR-006 | EVD-013/018 and completion rules | Traced successful Git submission, synchronous API admission and later reconciliation | Accepted commit reported with pending/unverified states; no Ready inference or fabricated identity. | pass | DEP-004 |
| SCN-010 | FR-006 | Wait correlation/error rules | Traced stale Ready, unsupported wait, timeout and lost response | Require submitted revision/generation; unsupported known wait rejected before submission; exit 7 retains known identity and detached work. Unknown submission stays unknown. | pass | DEP-004 |
| SCN-011 | FR-005 | Export body and author-input policy | Inspected Unicode/multiline/newline examples and literal YAML chomping rules | Body remains top-level; `\|-` / `\|` / `\|+` preserve zero/one/multiple final newlines, author export strips status; Namespace/Repository nonempty body rejected. | pass | DEP-005; semantic M1 round-trip tests not run |
| SCN-012 | FR-005 | Format and non-interactive policy | Reviewed unsupported markdown list/mutation, token output, progress and no-input cases | Exit 2 before side effects for invalid format; stderr diagnostics, no ANSI when redirected, no browser/prompts in automation, provisioning consent separate. | pass | M1 executable conformance |
| SCN-013 | FR-003, FR-006 | Repository sync and EVD-018 | Traced worktree/subdirectory, explicit target, fetch, behind/divergent/concurrent push | No implicit branch/worktree edits or force push; fast-forward push only, buffered server progress recorded as DEP-016. | pass | DEP-016 |
| SCN-014 | FR-001–FR-006, FR-014, SC-001–SC-002 | US1 package | Reconciled matrix, register and behavior after source review | US1 document review passed; missing capabilities have owned/gated prerequisites, not support claims. | pass | No unresolved US1 M0 finding |

### Inventory reconciliation

The independently enumerated PRD categories are: context 3; config validation 2; auth 6;
resource discovery/explain 2; first-release resource pairs 30; direct GraphQL and Git
sync 2; lifecycle/component/deployment/migration 12; later transfer/upload/MediaAsset/
CRD/publication/CSI/LFS capabilities 18; help/version 2; export representations 4; registry
credential setup 1. Total 82. The additional negative File-list inference record brings
the matrix to 83, with no claim that this excluded operation is proposed support.

Each repeated example is reconciled to its canonical row: sync with/without argument,
serve with datastore/detach/add options, -f/-F GraphQL inputs, and read/list format
variants. Read/list export is an overlay rather than another server endpoint. Namespace/
Repository watch are observed upstream but not silently added to the initial product target.

### Clone convention addendum — 2026-10-29

The original inventory above records the 2026-10-27 baseline. CMD-084 adds one
positive client command: the current matrix has 84 records, of which 83 are positive
and one is the File-list exclusion. The direct GraphQL and Git category now has three
commands (GraphQL, sync, clone).

Document review compared README examples, PRD GIT 5, the shared CLI behavior, the
CLI contract design, and CMD-084. They specify `gitstore` by default, `--remote`
overrides including `origin`, tracking of an existing checked-out branch, no extra
remote alias, and rejection of a nonempty destination. Sync selects by remote URL
and installation; renamed remotes remain eligible and `gitstore` does not resolve
ambiguity. Review result: pass for document consistency. M1 must verify these cases
with executable clone/sync checks, including an empty repository and a misleading
`gitstore` remote URL. No executable behavior is claimed.

## US2 — Daemon and runtime decisions

All lifecycle results are paper walkthroughs, using the published messages/ownership
rules. Preconditions and actions are hypothetical deployments, not actual running
workloads. The actual review inspected each stated invariant and its failure path.
Date/reviewer/evidence type are inherited from the record header.

| Scenario | Requirements | Artifacts / precondition | Action | Expected and observed result | Actual result | Open findings / later gate |
| --- | --- | --- | --- | --- | --- | --- |
| SCN-020 | FR-007–FR-010, SC-003 | Start: validated profile and authorized peer; EX-001–003, ADR-0002 | Trace negotiation, host/config/provisioning checks, durable acceptance and readiness graph | No provisioning without consent; intent/dedupe/ownership persists before accepted; workload start follows dependencies; readiness matches deployment generation. Startup failure retains inspectable owned state. | pass | DEP-012–DEP-015 |
| SCN-021 | FR-007–FR-008, SC-003 | Repeated start: EX-004–005/022 and ADR-0002 | Replay same key, change payload, then race distinct keys for same volume | Same request returns op-start; changed payload conflicts; lock/ownership forbids duplicate writer even with different key. Lost ack uses original key/inspection. | pass | DEP-012 |
| SCN-022 | FR-007–FR-008, SC-003 | Disconnect: running detached start, EX-006–007/012 | Close connection, inspect/replay, then request explicit cancellation | Detached work and identity survive; subscriptions close; wait timeout does not cancel; foreground cancellation uses bounded control path independent of runtime-worker/exclusive-lock occupancy. | pass | DEP-012 |
| SCN-023 | FR-008–FR-010, SC-003 | Stop: EX-009–010 and retained repo-dev | Trace authorized stop, cleanup, deadline and lock release | Terminal success only after observed writer termination; owned transient cleanup retry-safe; retained volumes preserved. Timeout retains held/uncertain ownership and known operation identity. | pass | DEP-012, DEP-014 |
| SCN-024 | FR-008–FR-010, SC-003 | Migration: EX-011/013 and ADR-0002 | Trace exclusive migration, compatibility/backup checks, failure and unsafe cancellation | Registered target only, no arbitrary script; unsafe cancellation refused, dependent start blocked on failure, no automatic downgrade. Durable operation can be inspected. | pass | DEP-012 |
| SCN-025 | FR-008–FR-010, SC-003 | Restart: EX-022 and persisted ownership | Discard in-memory state in walkthrough and rediscover backend resources | Reconcile before replacement; uncertain prior writer remains blocked, no synthetic terminal success/failure, retained volume/network identities used for owned cleanup. | pass | DEP-012–DEP-015 |
| SCN-026 | FR-007–FR-008 | Protocol invalid inputs: EX-016–021 and resource table | Inspect incompatible version, absent capability, unauthorized peer, malformed/oversized frame, slow consumer and expired cursor | Mutations fail before effects where possible; parser closes safely; queue caps include global limits, overflow/gap explicit, request/subscription/operation identity distinct. | pass | DEP-012 parser/security tests |
| SCN-027 | FR-007–FR-008, FR-011 | 22 canonical protocol JSON fixtures | Parsed JSON locally, checked IDs, exclusive result/error, methods, mutation keys and state enums | 22 valid fixtures; no undocumented method or uncorrelated paired response; no claim of C/Rust wire execution. | pass | DEP-012 C/Rust/sanitizer/fuzz verification |
| SCN-028 | FR-009–FR-011 | Five ADRs and official candidate sources | Reviewed status/date/context/decision/alternatives/consequences/evidence/owner/gates; compared source-backed version references | All five complete; protocol/ownership designs accepted, packaging/network/platform evaluation bounded with candidates and experiments. Remote delivery independent; no local macOS/runtime support claimed. | pass | DEP-013–DEP-015 |
| SCN-029 | FR-007–FR-011, FR-014 | US2 package and dependency register | Cross-checked dedupe retention/quota, cancellation, readiness and ownership rules | Resolved two review risks before acceptance: cancellation bypasses target execution lock, and ready binds deployment generation. DEP-008 design resolved; runtime gates remain open. | pass | No unresolved US2 M0 finding |


## Contribution policy

| Scenario | Requirements | Artifacts / precondition | Action | Expected and observed result | Actual result | Open findings / later gate |
| --- | --- | --- | --- | --- | --- | --- |
| SCN-030 | FR-012–FR-013, SC-004 | Existing pr-title-lint.yml and CONTRIBUTING.md | Inspected workflow triggers/permissions/action and listed workflow files | Only PR Title Lint exists; Conventional Commit types and optional scopes retained, metadata-only pull_request matching GitStore, pull-requests: read, no checkout, five-minute timeout and merge-group no-op. Contributor guidance defers Markdown/workflow/application CI until useful targets. | pass | GitHub-hosted run not yet verified for the corrected pull_request trigger; fork-token behavior requires separate verification. |


## Final requirement coverage

All rows below record the completed source/document review on 2026-10-27 by the
implementation contract reviewer. A pass is M0 design/coverage acceptance only.

| Requirement | Published artifact / review evidence | Actual result |
| --- | --- | --- |
| FR-001 | command-matrix.md; SCN-001/005/014, 83 records including one explicit negative inference | pass |
| FR-002 | README.md evidence register, matrix profiles/owner refinements and dependencies.md; SCN-002/005/006 | pass |
| FR-003 | cli-behavior.md routing/identity/sync; SCN-003/013 | pass |
| FR-004 | dependencies.md DEP-001–DEP-017; SCN-004/005/008/010 | pass |
| FR-005 | cli-behavior.md input/output/errors/export; SCN-003–SCN-008/011–SCN-013 | pass |
| FR-006 | cli-behavior.md completion and protocol wait contract; SCN-009/010 | pass |
| FR-007 | daemon-protocol.md, ADR-0001 and 22 parsed fixtures; SCN-026/027/029 | pass |
| FR-008 | daemon-protocol.md and ADR-0002; SCN-020–SCN-026/029 | pass |
| FR-009 | ADR-0002–ADR-0005, EXP-003A/B, EXP-004A/B, EXP-005A/B; SCN-028 | pass |
| FR-010 | ADR-0002/0004/0005 privilege/host boundaries; SCN-020/023/025/028 | pass |
| FR-011 | All five ADRs with status/date/context/decision/alternatives/consequences/evidence/owner/gates; SCN-028 | pass |
| FR-012 | Existing pr-title-lint.yml; SCN-030 | pass |
| FR-013 | CONTRIBUTING.md contribution policy and future application/sanitizer/compatibility checks; SCN-030 | pass |
| FR-014 | This map, evidence/dependency records, final structural review; SCN-002/014/029/031 | pass |
| SC-001 | All 82 PRD/local/representation capability records plus one negative File-list row classified; zero omitted/unclassified rows; SCN-001/014 | pass |
| SC-002 | All 14 FRs and four SCs linked here; 17 dependencies have roles/gates and affected records; SCN-002/014/031 | pass |
| SC-003 | Six ownership/completion/recovery walkthroughs; SCN-020–SCN-025 | pass |
| SC-004 | PR-title-only workflow scope and policy inspection; SCN-030 | pass |

## Final publication review

| Scenario | Requirements | Artifacts / precondition | Action | Expected and observed result | Actual result | Open findings / later gate |
| --- | --- | --- | --- | --- | --- | --- |
| SCN-031 | FR-014, SC-001–SC-004 | Complete eleven-document package, README/contributor links, original quality checklist | Executed quickstart review commands, checked internal links/tables/IDs, source object paths, matrix profiles/status/dependencies, ADR fields and JSON fixtures; inspected workflow scope and whitespace | 11 publication documents; 83 sequential CMD records (37 supported-contract, 27 blocked, 19 deferred); 17 evidence records with 29 fixed-revision upstream source references; 17 dependencies; 22 valid JSON fixtures; all six paper walkthroughs and 18 requirement/outcome mappings present; no orphan or broken references. Quality checklist left unchanged. | pass | Live/API/CLI/daemon/host/performance/hosted CI tests not run. |

### Resolved M0 review findings

- Public author-update preconditions are missing: mapped to DEP-009 and blocked rows,
  rather than implying safe updates or substituting controller-status operations.
- Cancellation could deadlock behind the target mutation lock: protocol now defines a
  bounded independent control path, with no second workload writer.
- A prior Ready could satisfy a new operation: deployment generation/observedGeneration
  and intended-generation waits are explicit.
- Per-connection queues could multiply unchecked: global event/log/history quotas and
  safe refusal/eviction are explicit.
- Gated command families needed specific ownership: matrix owner refinements now name
  API/extensibility/storage/controller/daemon roles by record range.
- YAML chomping notation needed escaped table separators: corrected; table structure
  and all relative document links pass local inspection.

No unresolved M0 finding remains. DEP-008 is resolved by protocol design; the other
16 dependencies remain owned release prerequisites. EXP-003A/B, EXP-004A/B and
EXP-005A/B are not-run, as are future C/Rust, auth/export/concurrency and runtime
release tests. This package makes no performance, platform or executable-release claim.

Repository setup review: this is a Git repository with an existing .gitignore; M0 creates
only versioned documents. No Dockerfile, package/tool configuration, Terraform or Helm
project exists, so no unrelated ignore files or language build scaffolding were added.

## Follow-up design amendment — 2026-10-28

Artifact-attestation requirements were added to PRD DIST 1, ADR-0003, DEP-013 and
contributor guidance after the M0 review above. Changed document links and whitespace
were checked locally; the workflow directory still contains only PR-title lint.
EXP-003C is not-run: no artifact was built, attested or verified. The original M0
review outcomes do not claim validation of that future release implementation.
