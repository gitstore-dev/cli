# Data Model: M1 Remote CLI

All models below are client-owned configuration, request or result shapes. No new
server schema, database or daemon state is introduced.

## Context and effective selection

A version-1 configuration has `current_context` and a map of named contexts. Each
context holds an exact GraphQL `endpoint`, `contract_profile`, selected `account`,
account records, optional namespace/repository/ref defaults, optional explicit
`git_endpoint`, and optional Git authentication metadata. Config contains no secrets.

An account record has an `issuer` identifier and credential source metadata. An
issuer is a configured identity boundary, not an inferred OAuth discovery URL. An
installation identity is the normalized full GraphQL endpoint, including proxy path.
`https://host/team-a/graphql` and `https://host/team-b/graphql` are different identities.

Validation rules:

- `name`, `endpoint`, `account` and account `issuer` are nonempty.
- Endpoint URLs reject userinfo, query and fragment; require HTTPS except explicit
  loopback development HTTP. Remove trailing slash without dropping a proxy prefix.
- Context/account map keys are unique; selected names must exist. Unknown config
  fields and unsupported config versions fail rather than being silently ignored.
- Effective field precedence is `flags > environment > selected context > defaults`.
  Report a source for each effective non-secret value.
- A configured `contract_profile` is an explicit compatibility selection; this
  increment accepts only `m0-2026-10-27` for built-in resource/Git mappings.
  It is not proof that an arbitrary deployment conforms; live evidence is separate.
- `git_endpoint` is independently configured, never derived from GraphQL ports/path.

Transition: parse and validate candidate selection → persist atomically → make active.
Failure before replacement preserves the prior valid file. No partial selection write.

## Credential identity and material

GraphQL credential key: `(normalized endpoint, issuer, account, purpose=graphql)`.
Git credential key adds the normalized Git endpoint and configured authentication
mechanism; its purpose is `git`. Registry identity is not part of either store.

OS credential entry key is the canonical compact JSON array of these identity
components, under service `dev.gitstore.gs`. This avoids delimiter collisions.
Never key solely by hostname, context alias or remote name. Renaming a context does
not change the protected credential identity.

Credential sources are `os-store` (default), explicit protected file, or explicit
stdin for the invocation. OS errors distinguish absent from unavailable/denied.
Only `auth token` emits a GraphQL token. All other serializers expose source/presence,
not the value; secret types omit Debug/Display and use zeroizing buffers.

For Git, mechanism is explicitly `basic` or `bearer` under a compatible profile.
`basic` material contains username and actual password. `bearer` material contains
a token for the configured Git audience/endpoint. Never convert a GraphQL token
into a Basic password or assume its Git audience. Values never enter persisted Git
configuration, remote URL userinfo or process arguments.

File fallback rules: explicit opt-in; regular user-owned file; no symlink; no group/
other permission bits; bounded UTF-8 input. GraphQL files hold a raw nonempty token
with one optional terminal line ending. Git files hold a mechanism-specific JSON
object. Stdin is consumed at most once across credentials and request/doc input.
No silent file persistence, environment token import or fallback on OS-store errors.

## Resource selection and page

`kind` is one of `namespace`, `repository`, `product`, `category`, `collection`,
`file`; `category` corresponds to `CategoryTaxonomy`. Selection is explicit name
or `--id` (mutually exclusive). Namespaced operations require resolved namespace;
Namespace is cluster-scoped. File currently accepts namespace/name only. The upstream
File lifecycle specification will normalize its `by` selector; that future shape is
not assumed here and must be verified before updating the client mapping.

Name and ID selectors are distinct CLI modes; opaque IDs must not be guessed from
name prefixes. Decode only the verified shared ID encoding to reject wrong-kind
IDs; otherwise use public typed lookup and validate returned kind before mutation.
Deletion always uses the ID returned by lookup, not an invented identifier.

A list result contains items and `pageInfo`, including opaque continuation cursors.
`first` defaults to 100 and is an integer in `1..=100`; `after` is opaque. One page
per invocation. No automatic complete-catalogue accumulation or snapshot guarantee.

## Author document and batch

Each `-f` file contains one Markdown document: opening `---`, YAML frontmatter,
closing `---`, then untouched UTF-8 body. Repeated `-f` supplies multiple documents;
stdin `-` is allowed once. Do not split arbitrary body separators into documents.

Required frontmatter: `apiVersion`, `kind`, `metadata.name`, resolved
`metadata.namespace`, `spec.title`. Supported kinds are `Product` and
`CategoryTaxonomy`; API version is `catalog.gitstore.dev/v1beta1`. Metadata permits
only name/namespace/labels/annotations. Labels and annotations follow public input
shape constraints; spec uses the exact kind-specific input projection in the
upstream contract. Status, owner references, placement hints, unknown fields and
output-only reference identity fields fail validation.

If frontmatter and explicit namespace disagree, fail before submission. A missing
namespace may use effective selection; the resulting request must have one namespace.
The body is the exact byte slice after the closing delimiter's line ending, with
no rendering, trimming or terminal-newline normalization. Empty body is an empty
string; no separate author-provided top-level body field may conflict with it.

Bounds: `100 documents` and `16 MiB total UTF-8 input`, including frontmatter/body.
Validate the entire batch before the first mutation. Parsing failure submits nothing.
Submit valid requests in input order, once each, and stop on first server/transport
failure. Remaining results are explicitly not-submitted.

## Outcome and errors

Per-document outcome contains index, kind/name/namespace, submission certainty
(`not-submitted`, `accepted`, `unknown`), admission state (`succeeded`, `pending`,
`failed`, `unverified`), readiness state (`unverified` in this increment), returned
resource identity/revision when known, and optional error. A failed admission can
follow an accepted commit; a mutation response error does not by itself prove that
nothing was committed. If evidence cannot distinguish stages, submission is unknown.

Clone outcome contains destination, remote name, safe URL and checked-out/tracking
branch if one exists; it has no admission/readiness operation. Sync records exact
local commit and selected remote/ref; accepted pushes retain commit identity and
pending/unverified admission/readiness. Equal refs mean no new submission.

Errors use M0 exit categories 1–8 and fields `code`, `stage`, `retryable`, `message`,
optional `operationId`, `deploymentId`, `revision`. Never invent unavailable identity.
Machine-mode diagnostics are structured stderr; requested data stays on stdout.
For batches, aggregate exit is the first nonzero outcome in input order.

## Git target

Resolve a worktree from any subdirectory. A Git target holds remote name, configured
fetch/push URLs, installation/repository/namespace, local ref and full remote ref.
Validate both effective fetch and push destinations, including pushurl/URL rewrites;
reject destinations outside the configured Git endpoint. No remote-name tie-breaker.

Only branch refs are included in sync; default to the current branch's configured
upstream where unambiguous. Detached HEAD requires explicit `--ref` selecting a
remote branch; the resolved current HEAD commit is the local source. If no such selection exists,
fail with usage guidance. A local tag or arbitrary revision is not a push target.

Transitions: preflight → fetch remote-tracking ref → compare ancestry → equal/no-op,
fast-forward push, or conflict. Do not alter local branch/worktree. If remote advanced,
normal push rejection is a conflict, with no force/retry. Failed/lost push responses
must not report a proven rollback; retain commit and submission uncertainty.
