# Shared CLI Behavior

**Status**: Accepted M0 client design, 2026-10-27; not implemented.

The [matrix](command-matrix.md) controls what can be offered. This document specifies
client behavior, never new server capabilities. EVD-003 identifies this design.

## Selection and transport

Resource CRUD/read/watch and direct GraphQL use the public installation endpoint;
repository sync uses Git; lifecycle/migration uses the daemon. No fallback between
write paths, direct datastore access, server repository writes or internal gRPC calls.
Remote catalogue operations never initialize the local runtime.

Precedence is explicit flags, environment, selected context/configuration, defaults.
Accepted selector flags are `--context`, `--endpoint`, `--account`, `--namespace`,
`--repository`, `--ref` and `--deployment`; their environment equivalents are
`GS_CONTEXT`, `GS_ENDPOINT`, `GS_ACCOUNT`, `GS_NAMESPACE`, `GS_REPOSITORY`, `GS_REF`
and `GS_DEPLOYMENT`. Empty required values and contradictory explicit identities fail
with exit 2. An endpoint override cannot reuse another installation's credentials;
require a matching account entry or protected explicit credential. Normalize trailing
slashes without discarding reverse-proxy base paths. Require HTTPS except an explicit
loopback development installation. Never discover by dropping path prefixes.

Context holds endpoint, account/provider, default namespace and optional repository/ref.
Runtime deployment selection is separate. `auth switch` and `context use` update the
same store atomically; switching account cannot silently cross installations. `context
show` and validation show effective non-secret values and their source. Missing context
or ambiguous accounts fail instead of selecting an arbitrary saved entry. Component
configuration validation must preserve its selected server version's layering rules.

Namespace is cluster-scoped: select by name or exact Relay ID. Namespaced kinds use
namespace/name or their typed Relay ID; resolve names to exact identities before delete.
Reject an ID decoded as another kind. The user-facing `category` maps to manifest
`CategoryTaxonomy`. Deleting Namespace must explain dependency restrictions; ordinary
repositories/catalogue contents block deletion, not an automatic cascade. Deleting a
resource requests termination; final cleanup belongs to controllers.

## Authentication

Use advertised installation/provider/flow capabilities individually. Absence of shared
discovery is DEP-001; explicit documented legacy static-session mode may be selected
by configuration, with server compatibility verified before use. Never infer device,
refresh or revocation from “OIDC” or response field names. No embedded public-client
secret, IdP password emulation or automatic downgrade to a weaker identity.

Store credentials in the OS credential store under installation/issuer/account.
Protected input may be stdin or a permission-restricted explicit credential file;
do not encourage secret command-line arguments. File persistence fallback is opt-in
and user-only. Registry secrets use registry/account records, never catalogue accounts.
Persist rotated credentials atomically, preserving prior state on storage failure.
Device login obeys advertised intervals, slow-down, denial, cancellation and expiry.

`auth status` distinguishes local presence from server verification. Logout removes
local material and reports remote invalidation as completed, unsupported or failed;
local-only logout must say so. `auth token` is the only explicit credential-output
operation and never runs as part of status/diagnostics. Current local sessions may
return identical access/refresh values; the OIDC JWT validation provider cannot issue,
refresh or revoke issuer sessions (EVD-010).

## Input, output and automation

Resource create/update accepts UTF-8 Markdown with YAML frontmatter; require kind,
apiVersion and identity, validate supported fields and reject controller-owned status.
No implicit purge, owner/status mutation or placement hint. Preserve supported raw body;
reject nonempty Namespace/Repository body when the selected mutation cannot carry it.
Missing client-version preconditions block safe author updates (DEP-009); a read-before-
write check alone does not prevent a race. Do not substitute a controller-status API.

`-o`/`--output` accepts json/yaml/table/markdown only where applicable:

| Commands | Default | Applicable formats |
| --- | --- | --- |
| Resource read | table | table/json/yaml; markdown when body is exposed, gated by DEP-005. |
| Resource list | table | table/json/yaml; no markdown list concatenation. |
| create/update/delete | table summary | table/json/yaml; no markdown. |
| context/auth status/validation, deployment inspect/list | table | table/json/yaml with redaction. |
| api-resources/explain | table | table/json/yaml using proven mappings only. |
| api graphql | json | json only, preserving GraphQL response shape. |
| watch and deployment logs | human stream | table denotes human lines; json denotes one event per line. |
| sync/start/stop/migrate | table summary | table/json/yaml, preserving identities and pending state. |
| auth token | raw token | Raw text only; reject output flags rather than leak via generic serializers. |
| help/version | text | Local text; no runtime download or server contact. |

Stdout contains only the requested result. Diagnostics/progress go to stderr. For
machine mode, stderr diagnostics are JSON objects with safe fields; raw GraphQL data
remains on stdout. Honor `NO_COLOR`; redirected output has no ANSI styling. Markdown
is rendered only on a TTY; redirection emits exact raw body. Unsupported formats fail
before side effects. Streaming JSON is NDJSON, not an unterminated JSON array.

`--non-interactive` or `GS_NO_INPUT=1` never prompts or opens a browser. Missing
credentials, target selection or destructive-action choices fail before submission.
Explicit delete and stop with a named target are sufficient intent; scripts cannot
rely on interactive defaults. First-use runtime provisioning requires explicit
`--allow-provision` in non-interactive mode or existing verified offline/cache assets;
interactive first use describes downloads and obtains consent. This grants no privilege
elevation. Other commands and cache-only startup never fetch unrelated assets.

Bound multi-document input at 100 documents and 16 MiB total UTF-8 input by default;
reject excess before submission. Return document index/identity, submitted flag, precise
outcome and revision/operation when known for every attempted item; after a stopping
error, mark remaining items not submitted. No multi-resource transaction. Resume only
failed/unsubmitted items explicitly; never retry successful writes blindly.

## GraphQL request grammar

`-f key=value` supplies a literal UTF-8 string. `-F key=value` recognizes exact `true`,
`false`, `null` and decimal integers in ±(2^53−1); otherwise it is a string. Leading
`@path` reads a UTF-8 file as the value; `@-` reads stdin once. File contents are text,
not automatically parsed as JSON. Invalid/oversized input fails before transmission.
Ordinary keys are variable names; reserved `query` and `operationName` build the envelope.
Use each key once; no dotted/bracket nesting syntax. Arrays/objects and exact unusual
numeric values are provided using `--input <file|->`, a complete JSON envelope with
`query`, optional `operationName`, and `variables`. `--input` cannot be mixed with -f/-F.
Input limit is 16 MiB; query must be nonempty text, variables an object, operationName
text. Reject duplicate JSON keys and multiple stdin consumers.

Examples: `-f query='query($n: Int!) { ... }' -F n=3` makes numeric n; `-f n=3`
makes string n. `-F query=@query.graphql` reads a query. `-f query=@query.graphql`
is a literal string and is not treated as file input. A multi-operation document must
specify operationName. The client sends only the selected installation/credential.

Preserve the server's `data`, `errors` and `extensions`, including partial data. HTTP 200
with nonempty errors fails. Do not print a fabricated all-success result or discard
partial data. Mutations have no automatic retries without proven server idempotency.
Transient read retries are capped at two additional attempts within the caller deadline,
honor Retry-After when it fits, and never change credentials or transport.

## Repository synchronisation

Resolve from the current Git worktree even in a subdirectory. An explicit repository
overrides inference; `--remote` and `--ref` resolve remaining ambiguity. Validate remote
against the selected installation and resolve the full ref. Fail on no worktree/remote,
multiple eligible targets, mismatched installation or dirty worktree. No context fallback
that silently targets another repository.

Fetch the selected remote ref into its remote-tracking ref. Do not modify the worktree
or local branch. If the remote is equal, report no new submission. If remote is an
ancestor of local, push a normal fast-forward update; if local is behind or histories
diverge, report conflict and require explicit user Git action. No commit, reset, rebase,
merge or force push. Concurrent remote advancement is a conflict, never force-retried.
No GraphQL write fallback. Use only supported Git credentials.

Git side-band progress is stderr and flushed as received. The reviewed server snapshot
buffers receive-pack responses, so end-to-end live progress is DEP-016; the CLI must not
claim streaming delivery until server/proxy validation. Closed Git responses are not a
resumable admission tail.

## Completion and errors

Track transport, admission and reconciliation independently. An accepted push exposes
the commit and admission/reconciliation as pending or unverified. GraphQL synchronous
admission still says nothing about controller readiness. `--wait=admitted|ready` requires
correlation to the submitted revision/resource generation (DEP-004), never an old Ready.
Unsupported waits fail before submission when known; timeouts preserve submission identity
and do not cancel detached work. ResourceVersion and watch cursor are distinct opaque values.

| Exit | Category | Examples |
| --- | --- | --- |
| 0 | completed or accepted submission | Pending states remain explicit; waits succeed only on requested condition. |
| 1 | unexpected failure | Client bug or uncategorized internal error. |
| 2 | invalid usage/configuration | Ambiguous selection, malformed input, dirty sync worktree. |
| 3 | authentication/authorization | Missing/revoked credential, denied scope. |
| 4 | missing target | Confirmed absent selected resource/deployment. |
| 5 | conflict | Stale precondition, divergent Git, dedupe key changed payload. |
| 6 | unsupported capability | Missing flow/mapping/wait/backend, incompatible protocol major. |
| 7 | deadline exceeded | Explicit wait/transport deadline, identity retained if known. |
| 8 | remote/runtime failure | GraphQL operation errors not classified above, admission/readiness failure, network/rate-limit exhaustion. |

Errors include `code`, `stage`, `retryable`, safe `message`, optional `operationId`,
`deploymentId` and `revision`. Never invent identity when submission is uncertain:
report `submission=unknown` and inspection guidance. For multi-document aggregate exit,
use the first nonzero outcome in input order; per-item errors remain authoritative.
Server codes are preserved; classify only codes with known semantics, not message guesses.

## Export fidelity

Hydrated JSON/YAML includes apiVersion/kind/metadata/spec/status and top-level body where
the server exposes them; omit absent fields rather than fabricate hydration. YAML uses
literal block scalars and correct chomping: `|-` for no final newline, `|` for one,
`|+` for multiple. Preserve Unicode, embedded blank lines and exact body newline count.
Do not fold Markdown. Structured output and authoring export remain distinct.

Authoring export emits writable frontmatter followed by raw body, strips status and
controller-owned metadata, and preserves identity and applicable supported author fields.
Its exact selection flag is withheld until DEP-005 conformance review. Re-import compares
semantics, not original formatting or key ordering. Datastore-only resources cannot be
exported as Git-ingress-compatible without an explicit mapping.

Page size is at most 100 or the smaller server bound. Never hydrate an entire catalogue
in memory. Preserve object versions and explain that independently paged results are
not an atomic snapshot without a server contract. Watch expiry requires an explicit
resnapshot; report the gap instead of claiming lossless continuation.
