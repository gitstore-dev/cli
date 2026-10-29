# Upstream Public Contract: M1 Subset

**Evidence**: Local committed GitStore snapshot
`b6c866421dbcf43994565f847553c1f6aec83994`, inspected 2026-10-29 with `git show`.
The upstream working tree is not an input. These are source-reviewed mappings,
not live compatibility or authorization evidence. Never call an internal service.

Canonical files at that revision:

- `shared/schemas/{schema,namespace,repository,product,category,collection,file}.graphqls`
- `gitstore-api/internal/graph/resolver/{product,category,repository}.resolvers.go`
- `gitstore-api/internal/app/server.go`
- `gitstore-api/internal/githttp/{handler,resolver}.go`
- `gitstore-api/internal/middleware/security/secure.go`
- `gitstore-api/internal/auth/provider/staticusers/provider.go`

Implementation records source hashes and reusable nonsecret contract fixtures in
`crates/gs/tests/fixtures/upstream/`; retain provenance/notices when copying material.
Validate client query selections and inputs against that snapshot, not table labels
alone. A deployment must be explicitly configured for the compatible profile;
incompatible schema/auth responses fail without alternate routes or write fallbacks.

## GraphQL route and authentication

Server POST route is `/graphql` on its API listener. External deployments may add a
proxy prefix; configured endpoint is the exact public URL. Bearer headers carry an
existing credential scoped to the selected installation/issuer/account. No issuer
session flow is inferred from an access/refresh field name. Configure no redirect
following. Do not use an internal host/port or attempt discovery by deleting a prefix.

## Operation inventory

All `By` selectors require exactly one field. A namespaced path is
`{namespacePath:{namespace,name}}`; Namespace uses `{name}`. Opaque global IDs use
`{id}`. File currently has a separate direct selector and no list operation.
The upstream File lifecycle specification will normalize File lookup through `by`;
no input type/fields are assumed until that specification ships. Retain the current
verified mapping and update fixtures/mapping together after upstream verification.

| CLI kind | Read | List | Included mutation |
| --- | --- | --- | --- |
| namespace | `namespace(by: NamespaceBy!)` | `namespaces(first,after,last,before)` | `deleteNamespace(input:{id})` |
| repository | `repository(by: RepositoryBy!)` | `repositories(namespace!,first,after,last,before)` | `deleteRepository(input:{id})` |
| product | `product(by: ProductBy!)` | `products(namespace!,first,after,last,before)` | `createProduct(input: CreateProductInput!)`, `deleteProduct(input:{id})` |
| category | `category(by: CategoryBy!)` | `categories(namespace!,filter,first,after,last,before)` | `createCategory(input: CreateCategoryInput!)`, `deleteCategory(input:{id})` |
| collection | `collection(by: CollectionBy!)` | `collections(namespace!,first,after,last,before)` | None |
| file | `file(namespace!,name!)` | None | None |

Only forward pagination (`first`, `after`) is exposed in this increment; return
`edges.node` and pageInfo without interpreting cursors or promising snapshots.
Category filtering is not exposed until a separate accepted CLI grammar defines it;
no parameter is invented from the presence of a server input.

The M0 CMD-021 spelling `repositorys` was a documentation typo; `repositories` is the
verified operation. Correcting that spelling changes neither scope nor server behavior.

## Author input projection

Both create inputs support `apiVersion`, `kind`, `metadata`, `spec` and nullable
`body`. API version is `catalog.gitstore.dev/v1beta1`. Metadata supports name,
namespace, labels and annotations, with no author expected-version/status/owner fields.
Require explicit resolved namespace even though GraphQL metadata has an empty default.

| Kind | Required spec | Additional supported spec |
| --- | --- | --- |
| Product | `title` | `categoryRef`, `tags`, `media`, `options`, `lifecycle` |
| CategoryTaxonomy | `title` | `parentRef`, `media` |

Reference author projection supports apiVersion/kind/name/namespace only; name is
required. Reject returned UID/resourceVersion fields used as author fields. Media
entries use `fileRef` with name/kind/optional. Product options use name/title/values,
with name and values required. Lifecycle state is `ACTIVE` or `RETIRED`; enforce the
exact nested input shape rather than treating lifecycle as a free string. T007/T023
capture all required/null/default/enum constraints in typed DTOs and contract tests.
Do not add repository/path placement hints or project hydrated output wholesale into
mutation input. Raw body maps directly to the public create input.

Create resolvers call the canonical manifest commit/admission services with input
body and server-selected placement. Payload `product`/`category` is nullable; evaluate
errors and returned identity before claiming success. A returned object does not
verify controller readiness. A commit may survive later failure; retain known revision
and mark uncertainty where server response lacks enough evidence.

All included delete inputs require global `id`; resolve name through public typed
lookup first. Payload contains nullable resource and `outcome` of
`TERMINATION_STARTED` or `ALREADY_TERMINATING`. Neither means physical cleanup
completed. Keep namespace/repository dependency restrictions; do not auto-cascade.

## Git front door

GraphQL and Git HTTP listeners have independent `cfg.Api.Port`/`cfg.Api.GitPort` in
`internal/app/server.go`. The public Git endpoint therefore must be configured
independently. Do not strip `/graphql` and assume the resulting host is Git.

Under the configured public Git root, repository routes are:

```text
GET  /<namespace>/<repository>[.git]/info/refs?service=git-upload-pack|git-receive-pack
POST /<namespace>/<repository>[.git]/git-upload-pack
POST /<namespace>/<repository>[.git]/git-receive-pack
```

Encode namespace/repository as individual path segments. Reject traversal, empty
segments or URL userinfo. Optional `.git` is accepted by the resolver. Infer targets
only from URLs matching the configured Git root and valid repository route, including
any proxy prefix. Fetch/push may require separate `repository.contents.read` and
`repository.contents.write` authorization; 401/403 remain distinguishable.

`BasicAuthenticator` forwards the original Authorization header to the configured
provider chain despite its name. Static users support Bearer sessions or Basic
username/actual-password. A session token as a Basic password fails password validation.
The CLI must explicitly select the proven mechanism and Git credential identity;
GraphQL token shape alone establishes neither Git audience nor Basic compatibility.

For Basic, supply the separately selected username/password to the in-process
libgit2 smart-HTTP adapter; Bearer uses its scoped in-memory request header. Reqwest
enforces all-redirect rejection and deadlines while libgit2 owns Git protocol handling.
Do not launch external helpers or use `GIT_CONFIG_*`/process arguments to carry secrets.
Disable external helper/filter/hook execution and untrusted ambient auth headers,
reject redirects, retain TLS verification and test hostile repository/global config.
The library transport must support cancellation/deadlines and per-ref push status;
transport success alone is insufficient evidence that a ref was accepted.

Receive-pack returns buffered bytes in this snapshot. Relay received progress to
stderr faithfully, but leave DEP-016 open. Transport acceptance leaves admission and
readiness pending/unverified; correlated observation is still DEP-004.

## Request and failure bounds

- Input and buffered response: at most 16 MiB each; author batches at most 100 docs.
- YAML/JSON nesting: at most 64 levels; YAML alias expansion budget at most 1,000
  references and 16 MiB expanded content. No includes/interpolation/tag execution.
- One list page: 1–100 objects, default 100, or smaller advertised server maximum.
- Command deadline: 30 seconds default; positive `--timeout` overrides. Connect timeout
  is at most 5 seconds and never exceeds remaining command time.
- Reads: at most two transient retries (three total attempts) within the same deadline;
  honor Retry-After only when it fits. No mutation retry or hidden transport fallback.
- Incomplete/malformed responses: safe diagnostic, no fabricated data or completion.
  Lost mutation/push response: preserve known identity and submission uncertainty.
