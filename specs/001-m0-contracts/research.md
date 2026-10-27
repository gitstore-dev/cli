# M0 Research Decisions

**Date**: 2026-10-27 | **Scope**: Planning evidence, not live or release validation.

## 1. Deliver documentation contracts before executable scaffolding

**Decision**: M0 publishes the command inventory, dependency register, shared CLI
behavior, daemon protocol, and five runtime ADRs. Keep planning and published
documents separate; introduce no Rust/C workspace, build tooling, or new lint CI.

**Rationale**: The PRD defines M0 as contract work. A build with no product behavior
or Markdown-only CI does not prove those contracts. Existing PR-title policy remains
useful independently of application source.

**Alternatives considered**: Scaffolding M1/M2 now expands the milestone. A generated
registry or Postman collection adds maintenance without a runnable consumer. Existing
GraphQL schemas remain authoritative; no parallel OpenAPI contract is introduced.

## 2. Separate operation scope, contract evidence, and release support

**Decision**: Use one record per resource/operation or specialized command. Track
`supported-contract`, `blocked`, or `deferred`, plus independent evidence level and
target milestone. Supported-contract means source-backed callable upstream behavior,
not an implemented CLI or supported GitStore release.

**Rationale**: A read contract does not establish writes, watch, list, or upload.
Deferred scope can coexist with an existing upstream capability. Record source revision,
review date, dirty-file status, authorization and completion limits for each claim.

**Alternatives considered**: One row per kind hides missing operations. A single
supported/unsupported flag conflates missing capability with intentionally later scope.

## 3. Use snapshot-specific upstream evidence

**Decision**: Start M0 investigation with GitStore revision
`1a76386bdc7c61776dbf6d686a409ce1c8c111c1`, reviewed 2026-10-27. The local checkout
contains extensive uncommitted changes. Pin committed references where possible;
identify working-tree-only evidence explicitly and recheck before publication.

**Rationale**: Local source inspection establishes neither deployed behavior nor
release compatibility. No live deployment was tested. Issue links in the PRD are
tracking references; their current status was not verified in this planning run.

| Source relative to GitStore | Source-reviewed finding | Consequence |
| --- | --- | --- |
| shared/schemas/auth.graphqls and gitstore-api/internal/graph/resolver/auth_service.go | Login/logout/refresh declarations and provider dispatch; local token response semantics. | Record flow-specific behavior, not assumed OAuth rotation. |
| gitstore-api/internal/auth/provider/oidcjwt/provider.go | External token validation does not provide issuance/refresh/revocation. | Advertise those only through confirmed capability contracts. |
| shared/schemas/namespace.graphqls and repository.graphqls | Lifecycle/read/list/watch declarations exist. | Verify each resolver, selector, authorization and deletion restriction separately. |
| shared/schemas/product.graphqls and category.graphqls | Resource declarations exist; user-facing category maps to CategoryTaxonomy. | Preserve canonical kind mapping; exclude controller-only mutations. |
| shared/schemas/collection.graphqls | Read/list declarations; no mutation/subscription declarations in this file. | Collection write/watch require named public-contract prerequisites. |
| shared/schemas/file.graphqls | Read/watch declarations, no list/upload declaration. | Keep payload transfer and list assumptions separate. |

The research review found auth resolver/provider and auth/product/collection/file schema
files unchanged from HEAD; other relevant admission, datastore, and Git-service files
are modified. The final evidence register must record provenance per source, not apply
one clean/dirty label to all observations.

**Alternatives considered**: Treating all PRD targets as supported or using internal
datastore operations as public mappings would invent contracts. Delegating upstream
changes is outside this client feature; missing capabilities remain dependencies.

## 4. Make prerequisite ownership concrete without inventing assignees

**Decision**: Use component-maintainer roles and explicit release gates.

| Dependency ID | Missing guarantee | Responsible role | Gate |
| --- | --- | --- | --- |
| DEP-001 | Installation/provider/flow discovery including proxy base paths | GitStore API/auth maintainers | M1 discovery/auth |
| DEP-002 | Collection mutation/watch mappings | GitStore catalogue API maintainers | Each affected M1 operation |
| DEP-003 | Explicit Product creation placement | GitStore API/admission maintainers | Placement options |
| DEP-004 | Submitted revision/generation correlation | GitStore admission/controller maintainers | Admission/readiness waits |
| DEP-005 | Hydrated field coverage and pagination consistency | GitStore API/schema maintainers | Export guarantees |
| DEP-006 | CRD callable mappings and persistence routing | GitStore extensibility/API maintainers | M3 CRD operations |
| DEP-007 | Payload/LFS transfer and credential readiness | GitStore storage/API/Git maintainers | M3 payload commands |
| DEP-008 | Final daemon wire contract and limits | CLI/daemon maintainers | M0 protocol review, before M2 implementation |

These are initial planning identifiers; M0 execution verifies, expands, and publishes
the register. Existing PRD issue links provide tracking context without claiming closure.

## 5. Prefer a bounded, explicit local protocol

**Decision**: Plan a four-byte big-endian length prefix followed by a UTF-8 JSON object
over a per-user Unix stream socket. Define typed envelopes and explicit version/capability
negotiation. M0 finalizes the contract and numeric policy; M2 proves C/Rust interoperability.

**Rationale**: This gives an inspectable cross-language representation and allows a
frame bound before payload allocation. This is a project design choice, not a claim
of measured efficiency or a dependency selection. See [daemon design](contracts/daemon.md).

**Alternatives considered**: Newline-delimited JSON couples delimiters and stream
records; protobuf adds code-generation/dependency decisions before a consumer exists;
HTTP/gRPC adds a service stack to a local profile API. Revisit only if implementation
evidence shows the selected approach cannot meet the bounded interoperability contract.

Parser/library versions and daemon persistence implementation are M2 engineering choices.
They do not remain unresolved M0 planning questions. Concrete numeric bounds and the
deduplication retention contract must be accepted during M0 document implementation.

## 6. Record runtime directions separately from feasibility

**Decision**: Keep embedded libcrun and the Rust/C ownership split. Evaluate dependency
delivery and linking, maintained image tooling, rootless networking, local volumes,
and macOS backend delivery in distinct ADRs. No backend support is claimed by M0.

**Rationale**: The [crun project](https://github.com/containers/crun) documents a C
library and shared-library build configuration. This supports the architectural direction,
not a verified GitStore package or host profile. Apple's
[container documentation](https://github.com/apple/container) describes an Apple-silicon
VM-backed path with platform requirements; it is not evidence that GitStore ships it.

**Alternatives considered**: Bundled crun execution conflicts with the selected library
integration. Native Linux execution on macOS is not the fallback; a validated platform
backend or managed Linux VM is required. CSI is later scope, not a prerequisite for
retained local volumes.

Each ADR must separate accepted design, available evidence, feasibility experiments,
responsible role, and release gate. Static/dynamic packaging, image component selection,
and tested host combinations need bounded evaluations rather than fabricated certainty.

## Research Completion

The planning questions are resolved: artifact layout, evidence model, prerequisite
handling, protocol candidate, ADR scope, and review strategy. Upstream gaps remain named
dependencies; runtime experiments remain M2/M3 release gates. Neither is represented as
a passed test. No application implementation or upstream changes were performed.
