# Shared CLI Contract Design

This defines the behavior M0 must publish, not a command implementation. Exact resource
selectors, GraphQL operations, concurrency fields and capabilities require inventory evidence.

## Routing and identity

| Family | Contract boundary |
| --- | --- |
| context/auth switch, local status, help/version | Local context/identity selection; only documented remote verification contacts server. |
| resource verbs, api-resources/explain, auth provider calls | Public installation capabilities and GraphQL; unsupported mappings fail before writes. |
| api graphql | Public GraphQL using selected installation and credential; preserve errors and partial data. |
| repository sync | Git remote/ref resolution and advertised Git credential mechanism. |
| serve/component serve, deployment, migrate | Versioned daemon API; component commands share daemon ownership. |
| file upload, transfer, publication and CRD operations | Deferred or capability-gated; no invented endpoints or transactional semantics. |

Configuration precedence is flags, environment, selected context/configuration file,
then defaults. Remote context and local deployment identity remain distinct. Credentials
are isolated by installation/issuer/account; registry authentication is a separate domain.
No failed API operation falls back to a Git write or internal RPC.

`repository sync [repository]` infers a target from the current Git worktree and remote,
including subdirectories; an explicit argument overrides inference. Missing or ambiguous
targets require explicit selection. No implicit reset/rebase/commit/force push. Ref and
remote must match the intended installation. Record the exact fetch/push contract in M0.

## Input and output

- Resource create/update accepts Markdown with YAML frontmatter; standalone YAML export
  does not become authoring input by implication. Controller-owned status is not writable.
- Resource read/list defaults to table; GraphQL defaults to JSON. Publish supported
  JSON/YAML/table/Markdown combinations per command. Unsupported combinations fail.
- JSON/YAML preserve top-level raw `body`, metadata, desired `spec`, and observed `status`
  where exposed. YAML body content must preserve newline semantics without lossy folding.
- Authoring Markdown and hydrated inspection are distinct representations. Redirected
  Markdown is plain text; rendered terminal Markdown is not a complete resource export.
- Stdout carries requested results; stderr carries diagnostics and progress. Redact
  secrets except explicit credential-output commands. Honor `NO_COLOR` and non-TTY output.
- Multi-document results identify each input and distinguish success/failure. Retrying
  must not blindly repeat successful writes. Export pagination is bounded and documents
  consistency limits rather than claiming a snapshot without server support.
- Non-interactive mode never prompts or opens a browser. Explicit provisioning permission
  is separate from authentication. Missing credentials/targets fail with actionable errors.
- Direct GraphQL preserves literal `-f` and typed/file-backed `-F` distinctions. M0 must
  specify stdin, variables, operation selection, and conflicting flags without guessing
  server behavior. HTTP success containing GraphQL errors exits nonzero.

## Completion and error categories

Use the PRD's proposed exit policy as the M0 baseline to review and publish:

| Exit | Meaning |
| --- | --- |
| 0 | Requested condition completed, or submission accepted with pending states explicit. |
| 1 | Unexpected failure. |
| 2 | Invalid usage or configuration. |
| 3 | Authentication or authorization failure. |
| 4 | Missing target. |
| 5 | Conflict. |
| 6 | Unsupported capability. |
| 7 | Deadline exceeded. |
| 8 | Remote or runtime operation failure. |

Structured errors include stable code, stage, retryability, safe message, and operation/
revision identity when known. Do not invent an operation identity if submission is unconfirmed.
Precise GraphQL errors remain available alongside partial data and the CLI classification.

Track transport, admission, and reconciliation independently. Submission-only success must
not imply admission/readiness. `--wait=admitted|ready` requires correlation to the submitted
revision/generation; unavailable correlation is unsupported. A timeout preserves existing
submission identity. Retry mutations only with established idempotency. Push progress may
stream while Git's response is open; it is not a resumable admission event service.

## Required review cases

M0 must record expected outputs/classification for ambiguous context/remote, missing auth
discovery, unsupported provider flow, stale revision conflict, multi-document partial
success, HTTP 200 with GraphQL errors, admission pending/failure, stale Ready status,
wait timeout, export newline/Unicode fidelity, and unsupported output format.

These are review fixtures now; executable validation accompanies M1. No plan example may
claim a resource operation exists merely because its command grammar is documented.
