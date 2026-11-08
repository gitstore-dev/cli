# CLI Capability Contract

All new mappings use GitStore alpha.9 revision
`68f8471edc947e417c98b6e8299a1367ab6b20da` (source root:
[immutable tree](https://github.com/gitstore-dev/GitStore/tree/68f8471edc947e417c98b6e8299a1367ab6b20da)).
This inventory supersedes stale dependency interpretations, not the historical
M0 evidence. Implementation must reconcile CMD/DEP rows with the new receipts.
File ID dispatch, full projections, scoped creation, authoring export and local
resource help, five typed name/label watches and explicit static-user remote auth
are implemented and controlled-tested. Pinned shipped alpha.9 conformance passed
41 named checks, including HTTP/WebSocket proxy prefixes, File Git admission and
ID hydration. This is memdb/static-users evidence, not arbitrary-provider or
binary-release qualification; see the current validation record.

## M0–M3 family inventory

| Family / command IDs | Current status | Work selected / gate |
| --- | --- | --- |
| M0 contracts / CMD-001–084 | Accepted historical baseline; current status scattered | Reconcile matrix/dependencies with these scoped source mappings; preserve baseline provenance |
| Local contexts/config/help/version / CMD-001–005,077–078 | Implemented subset | Package versions, generated help/completions, installed first-use setup qualification |
| Auth / CMD-006–011 | Local credentials plus explicit static-user login/refresh/remote logout implemented | Protected persistence and uncertain rotation tested; arbitrary OIDC/discovery remains DEP-001; no immediate existing-socket revocation claim |
| Resource discovery / CMD-012–013 | Local built-in api-resources/explain implemented and tested | Explicit profile/source/type/operation availability; server discovery and CRDs remain gated |
| Namespace / CMD-014–018 | Read/list/create/delete, typed watch and representable authoring export implemented | Controlled/pinned conformance; safe author update DEP-009; waits DEP-004 |
| Repository / CMD-019–023 | Read/list/create/delete, typed watch and representable authoring export implemented | Controlled/pinned conformance; safe author update DEP-009; waits DEP-004 |
| Product / CMD-024–029 | Create/read/list/delete subset | Full projections/export, authoring export and typed watch; placement flags DEP-003; safe update DEP-009 |
| Category / CMD-030–035 | Create/read/list/delete subset | Full projections/export, authoring export and typed watch; safe update DEP-009 |
| Collection / CMD-036–041 | Read/list subset | Full projections/export; create/update/delete/watch remain DEP-002 |
| File / CMD-042–043 | Legacy name reads; alpha.9 full name/ID reads and typed name/label watch implemented | Public Git admission/read/watch passed; no list/upload inferred; ID watch remains DEP-018 |
| GraphQL/Git / CMD-044–045,084 | Explicit GraphQL and library clone/submission sync | Regression qualify endpoints/proxy/credentials; keep waits DEP-004 and streaming support DEP-016 scoped |
| Runtime / CMD-046–057,083 | Linux core and optional OIDC source-preview; other profiles/backend incomplete | Package installed lifecycle, library closure and host qualification; Admin UI/Storefront external mappings stay gated |
| Transfer / CMD-058 | No verified public transfer contract | DEP-010, external-contract |
| File list/upload/LFS / CMD-059–060,076 | No verified public transfer/list contract | DEP-007, external-contract |
| MediaAsset/CRD / CMD-061–072 | Architectural proposals; no callable mapping | DEP-006, external-contract |
| Publish/release catalogue / CMD-073–074 | No public workflow contract | DEP-011, external-contract; distinct from CLI binary releases |
| CSI / CMD-075 | Optional profile not implemented | DEP-014 driver/privilege/lifecycle experiment; deferred from this release slice |

## File selectors

`gs read file NAME` uses `file(by:{namespacePath:{namespace,name}})` for alpha.9;
`gs read file --id RELAY_ID` uses `file(by:{id:RELAY_ID})`. Exactly one selector;
an ID read does not require namespace. Branch on profile before invoking legacy
validation. Legacy profile keeps its existing flat mapping and rejects File IDs.
Mixed selector errors occur before network effects. Maintain authorization-safe
NOT_FOUND/BAD_USER_INPUT behavior and GraphQL HTTP-200 partial errors.

Evidence: [File schema](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/shared/schemas/admin/file.graphqls),
[PR #523](https://github.com/gitstore-dev/GitStore/pull/523).

## Namespace/Repository creation

Extend `gs create -f` with exact schema inputs. Namespace metadata is cluster-scoped;
Repository requires namespace and permitted spec fields such as defaultBranch.
Reject controller status, unknown author fields, selection conflicts and nonempty
body before submission: these create input types do not carry body. Keep existing
bounded multi-document behavior and partial results; do not retry mutations.

Evidence: [Namespace](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/shared/schemas/admin/namespace.graphqls),
[Repository](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/shared/schemas/admin/repository.graphqls).

## Hydration and authoring exports

Use explicit schema-verified projections for each core kind, including metadata,
desired spec, available status and top-level raw body. JSON/YAML preserve these
fields; table is concise and Markdown presentation does not masquerade as a
manifest. File remains read-only, Collection read/list only. Lists retain bounded
pagination/pageInfo and do not promise an atomic multi-page snapshot.

Add `gs read KIND SELECTOR --authoring -o markdown` for writable projection export.
The profile identifies exact accepted input fields. Product/Category body is
preserved without normalization of Unicode/newlines/frontmatter. Strip observed
status/server identity only according to documented authoring projection; surface
unrepresentable content as an error, never silently drop nonempty body for
Namespace/Repository. Unsupported kinds/formats fail before effects. Required
tests compare read→authoring→parse→create semantic fields rather than byte-equal YAML.

## Typed watches

Extend grammar to `gs watch KIND [NAME | --id ID] [--resource-version CURSOR]`
with namespace/selector support verified for each kind. Emit NDJSON for machine
streams, metadata and progress only on stderr. Implement authenticated
`graphql-transport-ws`, bounded frames/queues and reconnect with a deadline/backoff
budget. Pin exact accepted protocol from shipped handlers before activation.
Kinds: Namespace, Repository, Product, CategoryTaxonomy (CLI category), File.
No Collection watch. Journal cursor is opaque and differs from object version.

Implementation source review found an additional external gate, DEP-018:
typed deletion tombstones omit UID, so read-by-ID does not establish safe ID
watch selection across incarnations. Keep `--id` explicitly unsupported before
effects. Name/label watches are the unblocked slice; this qualification corrects
the initial grammar assumption above rather than inventing a UID mapping.

Default watch observes future events. An explicit cursor resumes with possible
duplicates; document at-least-once delivery. Server cursor expiry is an actionable
WATCH_EXPIRED error requiring explicit resnapshot, not a hidden fresh watch.
Implement per-kind verified bookmark bootstrap before offering snapshot+watch.
Namespace sentinel `__namespace_watch_bootstrap__` cannot be assumed valid for
every kind. Preserve queued events after bookmark while snapshot pages are read;
bound memory/backpressure and fail explicitly if observer cannot keep up.

Evidence: per-kind schemas plus [watch contract](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/specs/050-namespace-watch-contract/contracts/namespace-watch-journal.md).
Tests must cover authentication, frame limits, reconnect duplicates, cross-kind
cursor mismatch, expiry and disconnect. No exactly-once or implicit readiness claim.

## Explicit static-user authentication

Use an explicitly configured alpha.9 static-users auth mode; preserve account/
issuer/installation/purpose isolation. `gs auth login --username NAME` obtains a
password via protected file or dedicated stdin option; never a secret argument.
The implemented increment accepts protected password files or redirected stdin;
it does not prompt. Reject competing stdin consumers. Map public login,
refreshToken and logout operations.
Check actual payload errors, not merely HTTP status; store rotated tokens atomically
and report uncertain rotation if response persistence fails. No blind refresh retry:
the provider consumes old JTI. Local logout stays available with explicit scope;
offer `gs auth logout --remote` for verified server revocation followed by local
deletion, preserving actionable remote-failure status. Unsupported provider operations
fail honestly. Token output remains exclusive to explicit `auth token`.

Default OS-store session changes serialize per credential identity with an owned
private local lock. Concurrent same-identity operations fail before remote writes;
different identities remain independent. This does not make remote token rotation
transactional. Synthetic create/replace/read/remove checks passed separately on
macOS Keychain and an isolated Linux Secret Service; they do not qualify arbitrary
desktop providers. See the release evidence for exact execution scope.

Upstream currently populates accessToken and refreshToken from the same issued
session token; do not infer independent refresh credentials. Reference OIDC
composition does not establish device/refresh/revoke discovery for the CLI.

Evidence: [auth service](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/gitstore-api/internal/admin/graph/resolver/auth_service.go),
[static-user provider](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/gitstore-api/internal/auth/provider/staticusers/provider.go).

## Built-in resource help and remaining gates

`gs api-resources` lists versioned built-in kinds and implemented operations;
`gs explain KIND[.FIELD]` describes recorded schema fields and writable/read-only
status. Include profile/source version and indicate this is local knowledge.
No remote introspection/auth/download is needed. Arbitrary CRDs remain unsupported.

Safe author updates still lack a public stale-client precondition in ObjectMetaInput;
controller status versions do not provide it. Collection explicitly has no public
mutations/watch. Correlated waits need submitted revision/generation evidence,
not merely a Ready condition (Repository currently has no Ready producer).
Evidence: [shared schema](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/shared/schemas/admin/schema.graphqls),
[Collection](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/shared/schemas/admin/collection.graphqls).
