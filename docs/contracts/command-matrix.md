# Command and Operation Matrix

**Reviewed**: 2026-10-27. **CLI/daemon implementation**: absent.

Status is `supported-contract`, `blocked`, or `deferred`. Never implies CLI implementation.
A supported-contract row may depend on advertised optional capabilities; absence must fail
before side effects. Blocked means the requested baseline guarantee itself is missing.
Deferred means later scope even where some underlying contract exists. M0 classifies
M1–M3 targets; it does not execute them.

## Profiles inherited by each row

Each row inherits owner, input/output, authorization, concurrency and completion from
its profile below, together with scope, exact mapping and exceptions in the row.
[CLI behavior](cli-behavior.md) supplies common defaults and error categories.
EVD records resolve in the [evidence register](README.md); DEP records resolve in the
[dependency register](dependencies.md). A dash means no additional known prerequisite,
not a verified release. Source evidence is the fixed committed snapshot in the index.

| Profile | Owner / transport | Input → output | Authorization / concurrency / completion |
| --- | --- | --- | --- |
| LOCAL | gs / local files, with advertised verification only when requested | Flags/config → table or JSON/YAML status, redacted except auth token | User-owned store; isolated identities, atomic selection; local completion only. |
| AUTH | gs + API/provider / public GraphQL or advertised OAuth flow | Protected credentials → redacted status; tokens stored | Flow/provider specific; persist rotated credentials atomically; server session result, no inferred flow. |
| DISCOVERY | gs + API/extensibility / public capability/schema contract | Installation/kind/field → table or JSON/YAML schema | Scope/auth visible operations only; built-in mappings version-gated, CRDs explicit; no writes. |
| READ | gs + API / GraphQL | Selector or Relay page, first ≤100 → table/JSON/YAML; Markdown read where body exists | Server read/list policy; preserve returned versions; no multi-page snapshot claim. |
| WRITE | gs + API/admission / GraphQL | Create/update Markdown frontmatter, delete name resolved to exact ID → JSON/YAML or human summary | Server author permission; preserve exposed preconditions. Missing stale-client guard blocks update (DEP-009). Admission and later readiness separate; deletion starts termination only. |
| WATCH | gs + API / public GraphQL subscription | Namespace/selector/cursor → NDJSON events or human stream | Server watch permission and journal readiness; cursor opaque and distinct from object version; expiry requires explicit resnapshot. |
| GRAPHQL | gs + API / GraphQL | query, operationName, typed/literal variables → faithful JSON response by default | Selected credentials and server field authorization; no blind mutation retry; HTTP 200 errors still fail. |
| GIT | gs + public API Git front door / Git Smart HTTP | Worktree/remote/ref → submitted commit plus explicit admission/readiness states | Advertised Git credential mechanism only; fast-forward/no implicit edits; accepted push is not admission. |
| DAEMON | gs + gitstored / owner-only local socket | Profile/deployment/method parameters → operation/state/endpoints or bounded logs | Same OS peer; serialized deployment mutation, durable dedupe; current state distinct from acceptance/readiness. |
| EXPORT | gs + API / underlying read/list | Format and selector → hydrated or authoring representation | Underlying read authorization; preserve versions/body and consistency limits; conformance gate before lossless claims. |
| GATED | Explicit component/driver contract / advertised transport only | Future defined shape → no command offered until gate passes | No guessed credentials, mutation mapping, transfer atomicity or privilege. |

## Per-operation records

For GATED rows, the exact owner/transport refinement is: CMD-058 GitStore catalogue
API/GraphQL; CMD-059–CMD-060 GitStore storage/API with an advertised payload or list
contract; CMD-061–CMD-072 GitStore API/extensibility with explicit discovered mappings;
CMD-073–CMD-074 GitStore API/controllers with a public publication contract; CMD-075
gitstored volume provider/CSI; CMD-076 GitStore Git service/API with LFS transfer;
CMD-083 gitstored image backend/advertised registry authentication. Missing parameter,
authorization and result guarantees are the named dependency, not permission to guess.

| ID | Command / grammar | Purpose | Scope / selector | Profile | Exact mapping / restrictions | Status | Target | Evidence | Prerequisites |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| CMD-001 | gs context list | list saved installation contexts | installation/account | LOCAL | Client context store | supported-contract | M1 | EVD-003 | — |
| CMD-002 | gs context show | show saved installation contexts | installation/account | LOCAL | Client context store | supported-contract | M1 | EVD-003 | — |
| CMD-003 | gs context use | use saved installation contexts | installation/account | LOCAL | Client context store | supported-contract | M1 | EVD-003 | — |
| CMD-004 | gs config validate | Validate effective non-secret CLI configuration | client context/configuration | LOCAL | Client configuration layering and source reporting | supported-contract | M1 | EVD-003 | — |
| CMD-005 | gs <component> config validate | Validate effective non-secret component configuration | selected component and deployment/configuration | LOCAL | Selected component's versioned validation contract and existing layering | supported-contract | M2 | EVD-003, EVD-023 | DEP-012 |
| CMD-006 | gs auth login | login selected identity | installation/issuer/account | AUTH | login(input: LoginInput!) | supported-contract | M1 | EVD-010, EVD-003 | DEP-001 |
| CMD-007 | gs auth logout | logout selected identity | installation/issuer/account | AUTH | logout | supported-contract | M1 | EVD-010, EVD-003 | DEP-001 |
| CMD-008 | gs auth refresh | refresh selected identity | installation/issuer/account | AUTH | refreshToken(input: RefreshTokenInput!) | supported-contract | M1 | EVD-010, EVD-003 | DEP-001 |
| CMD-009 | gs auth status | status selected identity | installation/issuer/account | LOCAL | Local presence; optional server identity verification | supported-contract | M1 | EVD-010, EVD-003 | DEP-001 |
| CMD-010 | gs auth switch | switch selected identity | installation/issuer/account | LOCAL | Select saved identity/context | supported-contract | M1 | EVD-010, EVD-003 | — |
| CMD-011 | gs auth token | token selected identity | installation/issuer/account | LOCAL | Explicit protected credential output | supported-contract | M1 | EVD-010, EVD-003 | — |
| CMD-012 | gs api-resources | Discover supported resource shape and operations | installation; namespace visibility | DISCOVERY | Versioned built-in core mapping; advertised CRDs gated | blocked | M1 | EVD-001, EVD-019, EVD-021 | DEP-001, DEP-006 |
| CMD-013 | gs explain <resource>[.<field>] | Discover supported resource shape and operations | installation; namespace visibility | DISCOVERY | Schema and field documentation | blocked | M1 | EVD-001, EVD-019, EVD-021 | DEP-001, DEP-006 |
| CMD-014 | gs create -f <namespace.md> | create namespace | cluster; name or Relay ID | WRITE | createNamespace(input: CreateNamespaceInput!) | supported-contract | M1 | EVD-011 | DEP-005 |
| CMD-015 | gs read namespace <name> | read namespace | cluster; name or Relay ID | READ | namespace(by: NamespaceBy!) | supported-contract | M1 | EVD-011 | — |
| CMD-016 | gs list namespace | list namespace | cluster; name or Relay ID | READ | namespaces(Relay pagination) | supported-contract | M1 | EVD-011 | — |
| CMD-017 | gs update -f <namespace.md> | update namespace | cluster; name or Relay ID | WRITE | updateNamespace(input: UpdateNamespaceInput!) | blocked | M1 | EVD-011 | DEP-009, DEP-005 |
| CMD-018 | gs delete namespace <name> | delete namespace | cluster; name or Relay ID | WRITE | deleteNamespace(input: DeleteNamespaceInput!); request foreground termination, not physical cleanup | supported-contract | M1 | EVD-011 | — |
| CMD-019 | gs create -f <repository.md> | create repository | namespace/name or Relay ID | WRITE | createRepository(input: CreateRepositoryInput!) | supported-contract | M1 | EVD-012 | DEP-005 |
| CMD-020 | gs read repository <name> | read repository | namespace/name or Relay ID | READ | repository(by: RepositoryBy!) | supported-contract | M1 | EVD-012 | — |
| CMD-021 | gs list repository | list repository | namespace/name or Relay ID | READ | repositorys(Relay pagination) | supported-contract | M1 | EVD-012 | — |
| CMD-022 | gs update -f <repository.md> | update repository | namespace/name or Relay ID | WRITE | updateRepository(input: UpdateRepositoryInput!) | blocked | M1 | EVD-012 | DEP-009, DEP-005 |
| CMD-023 | gs delete repository <name> | delete repository | namespace/name or Relay ID | WRITE | deleteRepository(input: DeleteRepositoryInput!); request foreground termination, not physical cleanup | supported-contract | M1 | EVD-012 | — |
| CMD-024 | gs create -f <product.md> | create product | namespace/name or Relay ID | WRITE | createProduct(input: CreateProductInput!) | supported-contract | M1 | EVD-013 | DEP-003 |
| CMD-025 | gs read product <name> | read product | namespace/name or Relay ID | READ | product(by: ProductBy!) | supported-contract | M1 | EVD-013 | — |
| CMD-026 | gs list product | list product | namespace/name or Relay ID | READ | products(Relay pagination) | supported-contract | M1 | EVD-013 | — |
| CMD-027 | gs update -f <product.md> | update product | namespace/name or Relay ID | WRITE | updateProduct(input: UpdateProductInput!) | blocked | M1 | EVD-013 | DEP-009 |
| CMD-028 | gs delete product <name> | delete product | namespace/name or Relay ID | WRITE | deleteProduct(input: DeleteProductInput!); request foreground termination, not physical cleanup | supported-contract | M1 | EVD-013 | — |
| CMD-029 | gs watch product | watch product | namespace/name or Relay ID | WATCH | watchProducts(namespace?, selector?, resourceVersion?) | supported-contract | M1 | EVD-013, EVD-017 | DEP-017 |
| CMD-030 | gs create -f <category.md> | create category | namespace/name or Relay ID | WRITE | createCategory(input: CreateCategoryInput!); manifest kind CategoryTaxonomy | supported-contract | M1 | EVD-014 | — |
| CMD-031 | gs read category <name> | read category | namespace/name or Relay ID | READ | category(by: CategoryBy!); manifest kind CategoryTaxonomy | supported-contract | M1 | EVD-014 | — |
| CMD-032 | gs list category | list category | namespace/name or Relay ID | READ | categories(Relay pagination); manifest kind CategoryTaxonomy | supported-contract | M1 | EVD-014 | — |
| CMD-033 | gs update -f <category.md> | update category | namespace/name or Relay ID | WRITE | updateCategory(input: UpdateCategoryInput!); manifest kind CategoryTaxonomy | blocked | M1 | EVD-014 | DEP-009 |
| CMD-034 | gs delete category <name> | delete category | namespace/name or Relay ID | WRITE | deleteCategory(input: DeleteCategoryInput!); request foreground termination, not physical cleanup; manifest kind CategoryTaxonomy | supported-contract | M1 | EVD-014 | — |
| CMD-035 | gs watch category | watch category | namespace/name or Relay ID | WATCH | watchCategories(namespace?, selector?, resourceVersion?); manifest kind CategoryTaxonomy | supported-contract | M1 | EVD-014, EVD-017 | DEP-017 |
| CMD-036 | gs create -f <collection.md> | create collection | namespace/name or Relay ID | WRITE | No public mapping | blocked | M1 | EVD-015 | DEP-002 |
| CMD-037 | gs read collection <name> | read collection | namespace/name or Relay ID | READ | collection(by: CollectionBy!) | supported-contract | M1 | EVD-015 | — |
| CMD-038 | gs list collection | list collection | namespace/name or Relay ID | READ | collections(Relay pagination) | supported-contract | M1 | EVD-015 | — |
| CMD-039 | gs update -f <collection.md> | update collection | namespace/name or Relay ID | WRITE | No public mapping | blocked | M1 | EVD-015 | DEP-002 |
| CMD-040 | gs delete collection <name> | delete collection | namespace/name or Relay ID | WRITE | No public mapping; request foreground termination, not physical cleanup | blocked | M1 | EVD-015 | DEP-002 |
| CMD-041 | gs watch collection | watch collection | namespace/name or Relay ID | WATCH | No public mapping | blocked | M1 | EVD-015, EVD-017 | DEP-002, DEP-017 |
| CMD-042 | gs read file <name> | read File metadata | namespace/name | READ | file(namespace!,name!) | supported-contract | M1 | EVD-016, EVD-017 | — |
| CMD-043 | gs watch file | watch File metadata | namespace/name | WATCH | watchFiles(namespace?,selector?,resourceVersion?) | supported-contract | M1 | EVD-016, EVD-017 | DEP-017 |
| CMD-044 | gs api graphql | Execute explicit GraphQL operation | installation and selected identity | GRAPHQL | GraphQL request/response, operationName and variables | supported-contract | M1 | EVD-003, EVD-010, EVD-017 | — |
| CMD-045 | gs repository sync [repository] | Fetch and fast-forward push explicit/inferred repository | namespace/repository/ref and Git remote | GIT | Smart HTTP info/refs, upload-pack, receive-pack | supported-contract | M1 | EVD-018, EVD-003 | DEP-004, DEP-016 |
| CMD-046 | gs serve | Start core profile | explicit local deployment/profile | DAEMON | deployment.start; component selection, never unmanaged process tree | blocked | M2 | EVD-022, EVD-023 | DEP-008, DEP-012, DEP-013, DEP-014, DEP-015 |
| CMD-047 | gs api serve | Start managed api component | explicit local deployment/profile | DAEMON | deployment.start; component selection, never unmanaged process tree | blocked | M2 | EVD-022, EVD-023 | DEP-008, DEP-012, DEP-013, DEP-014, DEP-015 |
| CMD-048 | gs git serve | Start managed git component | explicit local deployment/profile | DAEMON | deployment.start; component selection, never unmanaged process tree | blocked | M2 | EVD-022, EVD-023 | DEP-008, DEP-012, DEP-013, DEP-014, DEP-015 |
| CMD-049 | gs controller serve | Start managed controller component | explicit local deployment/profile | DAEMON | deployment.start; component selection, never unmanaged process tree | blocked | M2 | EVD-022, EVD-023 | DEP-008, DEP-012, DEP-013, DEP-014, DEP-015 |
| CMD-050 | gs oidc serve | Start managed oidc component | explicit local deployment/profile | DAEMON | deployment.start; component selection, never unmanaged process tree | blocked | M2 | EVD-022, EVD-023 | DEP-008, DEP-012, DEP-013, DEP-014, DEP-015 |
| CMD-051 | gs admin serve | Start managed admin component | explicit local deployment/profile | DAEMON | deployment.start; component selection, never unmanaged process tree | blocked | M2 | EVD-022, EVD-023 | DEP-008, DEP-012, DEP-013, DEP-014, DEP-015 |
| CMD-052 | gs storefront serve | Start managed storefront component | explicit local deployment/profile | DAEMON | deployment.start; component selection, never unmanaged process tree | blocked | M2 | EVD-022, EVD-023 | DEP-008, DEP-012, DEP-013, DEP-014, DEP-015 |
| CMD-053 | gs migrate | Run compatible exclusive migration | deployment and datastore | DAEMON | migration.start; not automatic downgrade | blocked | M2 | EVD-022, EVD-023 | DEP-008, DEP-012 |
| CMD-054 | gs deployment list | list managed deployment | local deployment ID | DAEMON | deployment.list | blocked | M2 | EVD-022 | DEP-008, DEP-012 |
| CMD-055 | gs deployment inspect <deployment-id> | inspect managed deployment | local deployment ID | DAEMON | deployment.inspect | blocked | M2 | EVD-022 | DEP-008, DEP-012 |
| CMD-056 | gs deployment stop <deployment-id> | stop managed deployment | local deployment ID | DAEMON | deployment.stop | blocked | M2 | EVD-022 | DEP-008, DEP-012 |
| CMD-057 | gs deployment logs <deployment-id> | logs managed deployment | local deployment ID | DAEMON | stream.subscribe(kind=logs) | blocked | M2 | EVD-022 | DEP-008, DEP-012 |
| CMD-058 | gs transfer product <name> (destination syntax gated) | Transfer Product | source/destination namespace and identity | GATED | No accepted transfer/atomicity contract | deferred | M3 | EVD-001 | DEP-010 |
| CMD-059 | gs file upload -f <blob> (remaining syntax gated) | Upload payload and admit manifest separately | namespace/File and payload endpoint | GATED | No verified public transfer mapping | deferred | M3 | EVD-016, EVD-020 | DEP-007 |
| CMD-060 | gs list file (not offered) | Explicitly exclude inferred File list | namespace | GATED | No list field; not part of first-release target | deferred | M3 | EVD-016 | DEP-007 |
| CMD-061 | gs create mediaasset (syntax/capability gated) | create extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-062 | gs read mediaasset (syntax/capability gated) | read extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-063 | gs list mediaasset (syntax/capability gated) | list extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-064 | gs update mediaasset (syntax/capability gated) | update extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-065 | gs delete mediaasset (syntax/capability gated) | delete extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-066 | gs watch mediaasset (syntax/capability gated) | watch extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-067 | gs create <crd-kind> (syntax/capability gated) | create extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-068 | gs read <crd-kind> (syntax/capability gated) | read extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-069 | gs list <crd-kind> (syntax/capability gated) | list extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-070 | gs update <crd-kind> (syntax/capability gated) | update extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-071 | gs delete <crd-kind> (syntax/capability gated) | delete extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-072 | gs watch <crd-kind> (syntax/capability gated) | watch extension resource | discovered scope | GATED | Explicit advertised callable mapping required | deferred | M3 | EVD-001, EVD-021 | DEP-006 |
| CMD-073 | publish workflow (command grammar gated) | publish catalogue state | explicit resource/revision | GATED | No accepted readiness/rollback contract | deferred | M3 | EVD-001 | DEP-011 |
| CMD-074 | release workflow (command grammar gated) | release catalogue state | explicit resource/revision | GATED | No accepted readiness/rollback contract | deferred | M3 | EVD-001 | DEP-011 |
| CMD-075 | CSI volume profile (configuration, not CRUD command) | Optional driver-backed volumes | deployment/volume/driver | GATED | Driver lifecycle capability contract | deferred | M3 | EVD-022 | DEP-014 |
| CMD-076 | Git LFS integration (through repository sync) | Transfer pointer-referenced payloads | repository/LFS object identities | GATED | Authenticated LFS discovery/batch/transfer required | deferred | M3 | EVD-020 | DEP-007 |
| CMD-077 | gs --help | Local command/version discovery | local client | LOCAL | No daemon/API/download required | supported-contract | M1 | EVD-003 | — |
| CMD-078 | gs --version | Local command/version discovery | local client | LOCAL | No daemon/API/download required | supported-contract | M1 | EVD-003 | — |
| CMD-079 | gs read <kind> <name> -o yaml | Hydrated single-resource export | same selector as underlying read/list | EXPORT | Only fields exposed by supported read/list; no status reconstruction | blocked | M1 | EVD-003, EVD-011, EVD-012, EVD-013, EVD-014, EVD-015, EVD-016 | DEP-005 |
| CMD-080 | gs list <kind> -o yaml | Bounded hydrated list export | same selector as underlying read/list | EXPORT | Only fields exposed by supported read/list; no status reconstruction | blocked | M1 | EVD-003, EVD-011, EVD-012, EVD-013, EVD-014, EVD-015, EVD-016 | DEP-005 |
| CMD-081 | gs read <kind> <name> -o markdown | Present raw/rendered resource body | same selector as underlying read/list | EXPORT | Only fields exposed by supported read/list; no status reconstruction | blocked | M1 | EVD-003, EVD-011, EVD-012, EVD-013, EVD-014, EVD-015, EVD-016 | DEP-005 |
| CMD-082 | Authoring Markdown export (syntax gated) | Re-importable frontmatter plus raw body | same selector as underlying read/list | EXPORT | Only fields exposed by supported read/list; no status reconstruction | blocked | M1 | EVD-003, EVD-011, EVD-012, EVD-013, EVD-014, EVD-015, EVD-016 | DEP-005 |
| CMD-083 | Registry credential setup (syntax gated) | Provide private image credentials | registry/account, separate from API identity | GATED | Image backend challenge/token scope contract | blocked | M2 | EVD-022 | DEP-013 |

## Inventory boundaries

CMD-005 is a parameterized command family for component configuration validation,
not a controller-only operation. Resolve `<component>` to the component's own versioned
configuration contract; do not reuse controller defaults for other components.

The first-release resource target is five Namespace operations, five Repository
operations, six each for Product/Category/Collection, and two File operations: 30 rows.
Namespace/Repository watch exist upstream but are not silently added to the initial
product target. Controller status/final-deletion/bootstrap mutations and `purge` are
excluded from author commands. CRUD grammar uses full names and `read`, never a duplicate
resource-first tree. Specialized sync/upload/lifecycle families remain resource-first.

Exports are representation overlays, not additional server operations. Registry setup,
CSI and LFS rows record PRD capability requirements without inventing CLI subcommands.
The File-list exclusion row documents a tempting but unsupported inference. Flags such
as `--detach`, datastore choice and optional service additions refine the start rows;
they do not imply new independent operations. Both sync forms share one row.

Nonempty Namespace/Repository authoring bodies cannot be sent through the reviewed
mutation inputs; reject instead of dropping content (DEP-005). Public author updates
lack stale-client resourceVersion preconditions even though controller status inputs
have them: those status operations cannot be substituted (DEP-009). Default-branch and
stored provenance restrictions remain authoritative; explicit placement is DEP-003.
