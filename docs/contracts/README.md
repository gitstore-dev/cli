# M0 Contract Baseline

**Review date**: 2026-10-27

**Status**: M0 design/source review complete. [Acceptance evidence](acceptance.md)
records the actual review outcomes and untested release gates.

This package specifies the future `gs` client and `gitstored` daemon. Neither is
implemented or released here. A supported upstream contract is not a supported CLI
command or proof that a deployment passes its release gates.

## Documents

- [Command matrix](command-matrix.md)
- [Dependencies and release gates](dependencies.md)
- [Shared CLI behavior](cli-behavior.md)
- [Daemon protocol](daemon-protocol.md)
- [Acceptance evidence](acceptance.md)
- [ADR 0001: daemon protocol](../ADRs/0001-daemon-protocol.md)
- [ADR 0002: runtime ownership](../ADRs/0002-runtime-ownership.md)
- [ADR 0003: assets and packaging](../ADRs/0003-runtime-assets-packaging.md)
- [ADR 0004: rootless networking and volumes](../ADRs/0004-rootless-network-volumes.md)
- [ADR 0005: platform backends](../ADRs/0005-platform-backends.md)

## Record conventions

CMD identifiers name individual commands or resource/operation pairs. EVD records
identify source evidence; DEP records identify missing contracts or validation gates.
ADRs use stable numbered files. SCN records identify acceptance reviews. IDs remain
stable across revisions; removing a record requires a replacement/supersession note.

Command status is `supported-contract`, `blocked`, or `deferred`. It is independent
of target milestone and evidence level. Deferred entries state the scope reason;
blocked entries name a prerequisite, owner role and resolution gate.

Evidence levels are `requirement`, `source-reviewed`, and `live-validated`. A
source-reviewed record cannot claim live-validated behavior. No record in this M0
package establishes live-validated behavior.

## Source provenance

Planning reviewed GitStore commit `1a76386bdc7c61776dbf6d686a409ce1c8c111c1`.
Execution rechecked the newer committed snapshot
`b6c866421dbcf43994565f847553c1f6aec83994` on 2026-10-27. This newer revision is
the baseline for the evidence below. GitStore's local checkout contains unrelated
uncommitted changes. All upstream claims here are read from the committed Git objects
at that fixed revision, not from modified working-tree content. Therefore every cited
upstream source has provenance `committed snapshot`; per-path working-tree status is
recorded below for reproducibility and does not alter the committed evidence.

No upstream files were edited, services started, or live APIs tested. Issue links
are tracking references; issue closure and deployed compatibility were not verified.
Dirty-only behavior is excluded unless separately captured safely; otherwise it
remains a verification prerequisite. Secret-bearing configuration is never captured.

## Evidence register

The register records source path, symbol, guarantee, limitation and working-tree
differences for each source-backed entry. Public link accessibility is not independently
verified; the cited Git objects were read locally at the fixed commit.

### EVD-010

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [shared/schemas/auth.graphqls](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/shared/schemas/auth.graphqls) — working tree matches snapshot.
- [gitstore-api/internal/graph/resolver/auth_service.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/auth_service.go) — working tree matches snapshot.
- [gitstore-api/internal/auth/provider/staticusers/provider.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/auth/provider/staticusers/provider.go) — working tree matches snapshot.
- [gitstore-api/internal/auth/provider/oidcjwt/provider.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/auth/provider/oidcjwt/provider.go) — working tree matches snapshot.

**Symbols/guarantee and limits**: login/logout/refreshToken, IssueSession/RefreshSession/RevokeSession. Local sessions use the same token in both response slots; OIDC JWT provider validates but cannot issue/refresh/revoke. Provider discovery is a separate gap.

### EVD-011

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [shared/schemas/namespace.graphqls](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/shared/schemas/namespace.graphqls) — working tree matches snapshot.
- [gitstore-api/internal/graph/resolver/namespace.resolvers.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/namespace.resolvers.go) — working tree differs; excluded.

**Symbols/guarantee and limits**: namespace/namespaces/createNamespace/updateNamespace/deleteNamespace. Name or Relay ID; top-level scope. Deletion starts termination, ordinary repositories/catalogue resources block it; owner/admin required. No author expected-version field or nonempty body input.

### EVD-012

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [shared/schemas/repository.graphqls](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/shared/schemas/repository.graphqls) — working tree matches snapshot.
- [gitstore-api/internal/graph/resolver/repository.resolvers.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/repository.resolvers.go) — working tree matches snapshot.

**Symbols/guarantee and limits**: repository/repositories/createRepository/updateRepository/deleteRepository. Namespace/name or ID; Relay pages; system repository is not author-managed. Visibility includes reserved behavior; no author version precondition/body field.

### EVD-013

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [shared/schemas/product.graphqls](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/shared/schemas/product.graphqls) — working tree matches snapshot.
- [gitstore-api/internal/graph/resolver/product.resolvers.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/product.resolvers.go) — working tree matches snapshot.
- [gitstore-api/internal/graph/resolver/service.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/service.go) — working tree matches snapshot.

**Symbols/guarantee and limits**: product/products/createProduct/updateProduct/deleteProduct; CommitProductManifest. Git commit followed by synchronous admission; default-system placement and stored provenance; status/final deletion controller-only; no public stale-client precondition.

### EVD-014

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [shared/schemas/category.graphqls](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/shared/schemas/category.graphqls) — working tree matches snapshot.
- [gitstore-api/internal/graph/resolver/category.resolvers.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/category.resolvers.go) — working tree matches snapshot.
- [gitstore-api/internal/graph/resolver/category_service.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/category_service.go) — working tree matches snapshot.

**Symbols/guarantee and limits**: category/categories/createCategory/updateCategory/deleteCategory; CommitCategoryManifest. GraphQL Category maps to manifest CategoryTaxonomy. Body input supported; own-status mutations not ordinary author operations. No generic stale-client precondition.

### EVD-015

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [shared/schemas/collection.graphqls](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/shared/schemas/collection.graphqls) — working tree matches snapshot.
- [gitstore-api/internal/graph/resolver/collection.resolvers.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/collection.resolvers.go) — working tree matches snapshot.

**Symbols/guarantee and limits**: collection/collections. Read/list only in public schema. Internal CreateCollection/UpdateCollection methods do not establish public mutations or watch.

### EVD-016

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [shared/schemas/file.graphqls](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/shared/schemas/file.graphqls) — working tree matches snapshot.
- [gitstore-api/internal/graph/resolver/file.resolvers.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/file.resolvers.go) — working tree matches snapshot.

**Symbols/guarantee and limits**: file/watchFiles. Read requires namespace/name; typed watch exists. No public list/upload declaration in reviewed schema.

### EVD-017

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [gitstore-api/internal/graph/resolver/watch.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/graph/resolver/watch.go) — working tree differs; excluded.
- [gitstore-api/internal/middleware/security/graphql.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/middleware/security/graphql.go) — working tree differs; excluded.

**Symbols/guarantee and limits**: watchCatalogJournal; GraphQLFieldAuthorizer. Watch journal availability and authorization are required; cursor expiry must resnapshot safely. Per-operation authorization remains server-owned.

### EVD-018

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [gitstore-api/internal/githttp/handler.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/internal/githttp/handler.go) — working tree matches snapshot.
- [gitstore-git-service/src/git/hooks/mod.rs](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-git-service/src/git/hooks/mod.rs) — working tree differs; excluded.

**Symbols/guarantee and limits**: SmartHttpDeps, ReceivePack, run_post_receive. Smart HTTP exists; post-receive admission is asynchronous. ReceivePack returns buffered bytes at this snapshot: end-to-end incremental progress is not verified.

### EVD-019

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [gitstore-api/cmd/server/main.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/cmd/server/main.go) — working tree differs; excluded.

**Symbols/guarantee and limits**: server wiring, provider registry. No verified public CLI installation/flow discovery contract identified in reviewed wiring. Internal issuer/RPC discovery cannot substitute.

### EVD-020

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [docs/resource-storage/lfs-object-storage.md](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/docs/resource-storage/lfs-object-storage.md) — working tree matches snapshot.
- [shared/schemas/file.graphqls](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/shared/schemas/file.graphqls) — working tree matches snapshot.

**Symbols/guarantee and limits**: deferred File source/credential readiness. Payload/LFS source declarations are not executable transfer contracts.

### EVD-021

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [docs/ADRs/0016-custom-resource-definitions.md](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/docs/ADRs/0016-custom-resource-definitions.md) — working tree matches snapshot.

**Symbols/guarantee and limits**: CRD discovery and persistence proposal. ADR proposal, not evidence that arbitrary callable CRD mappings exist.

### EVD-023

**Level**: source-reviewed. **Revision**: `b6c866421dbcf43994565f847553c1f6aec83994`. **Reviewed**: 2026-10-27.

- [scripts/AGENTS.md](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/scripts/AGENTS.md) — working tree matches snapshot.
- [gitstore-api/cmd/gitctl/main.go](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/gitstore-api/cmd/gitctl/main.go) — working tree matches snapshot.
- [compose.yml](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/compose.yml) — working tree matches snapshot.

**Symbols/guarantee and limits**: existing startup/config/migrate/enrollment helpers. Reuse semantics where suitable; existing Compose/gitctl is not gitstored implementation or standalone distribution validation.

### EVD-001

**Level**: requirement. [PRD](../prd.md), reviewed 2026-10-27. Product targets and
milestones, not executable support evidence; current file is versioned with this package.

### EVD-002

**Level**: requirement. [Constitution v1.0.0](../../.specify/memory/constitution.md),
ratified/reviewed 2026-10-27. Architectural and verification constraints.

### EVD-003

**Level**: requirement. [CLI behavior](cli-behavior.md), accepted M0 client design,
2026-10-27. Specifies local selection, input/output and error behavior; unimplemented.

### EVD-022

**Level**: requirement. [Daemon protocol](daemon-protocol.md) and ADRs 0001–0005,
accepted M0 design/evaluation plans, 2026-10-27. No runtime behavior has been tested.

## Review outcome and remaining gates

The completed package contains 83 command/capability records (37 supported-contract,
27 blocked, 19 deferred), 17 evidence records, 17 dependencies, five ADRs and 22 protocol
JSON fixtures. The matrix includes 82 positive/proposed/local representation records
and one explicit negative File-list inference; exports are overlays, not extra endpoints.
All 14 functional requirements and four success criteria have document-review evidence.
See SCN-001–SCN-014, SCN-020–SCN-031 in the acceptance record for exact observations.

DEP-008 is resolved by the protocol design. The other 16 remain explicit release gates:
installation/provider discovery; Collection mappings; placement; completion correlation;
export/body conformance; CRD mappings; File/LFS transfers; safe update preconditions;
transfer/publication semantics; executable daemon/recovery; packaging; rootless networking/
storage; platform integration; end-to-end Git progress; watch conformance.

Experiments EXP-003A/B, EXP-004A/B and EXP-005A/B are designed but not run. No live
endpoint, CLI/daemon implementation, rootless host/backend, runtime performance or
GitHub-hosted workflow validation occurred. Existing PR-title policy remains the only
workflow; no application or Markdown/workflow lint CI was introduced.

M0 is complete as a contract milestone. M1 and M2 remain separate implementation work,
and named upstream prerequisites do not authorize changes to GitStore here.

### Follow-up: artifact attestations (2026-10-28)

PRD DIST 1 and ADR-0003 now extend DEP-013 with build-provenance generation and
verification, starting with the first M1 artifact release. EXP-003C covers file/OCI/offline
verification and negative cases; it is not-run. This is a release-design amendment,
not an attestation-generation result or a new M0 CI workflow.
