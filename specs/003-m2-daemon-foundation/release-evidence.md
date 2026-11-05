# Shipped GitStore Inputs for M2

**Reviewed**: 2026-11-03
**Latest release supplement**: alpha.9, checked 2026-11-05; see the source review below.
**Purpose**: Reconcile the M2 implementation specification with shipped server features.
**Evidence level**: Release metadata and selected committed sources reviewed;
no binaries, images, credentials, containers or live endpoints exercised.

## Alpha.8 baseline identity

The [release listing](https://github.com/gitstore-dev/GitStore/releases) reports
[v0.1.0-alpha.8](https://github.com/gitstore-dev/GitStore/releases/tag/v0.1.0-alpha.8)
as a prerelease published on 2026-11-03 at 10:56:42 UTC. The tag resolved to
[`06152d193e854bc643fa884daa8b9f30b0d75d0f`](https://github.com/gitstore-dev/GitStore/commit/06152d193e854bc643fa884daa8b9f30b0d75d0f).
GitHub reports the release as non-immutable. Record the resolved revision rather
than relying on the tag alone; it is not an OCI image digest or provenance proof.

This is supplemental evidence. The repository's existing M0/M1 baseline remains
`b6c866421dbcf43994565f847553c1f6aec83994`; no historical contract record is silently
rebased to the new release.

## Impact on this specification

| Shipped input | Evidence reviewed | M2 consequence |
| --- | --- | --- |
| Controller signing preparation, enrollment and launch are separate stages | Alpha.8 release notes; [bootstrap change](https://github.com/gitstore-dev/GitStore/commit/07ec0530c9d260694c222506bf3be26c5ef45bc7); [pinned Makefile](https://github.com/gitstore-dev/GitStore/blob/06152d193e854bc643fa884daa8b9f30b0d75d0f/Makefile) | Treat these helpers as existing integration inputs. Preflight must distinguish missing signing records and enrolled identity; launch must not be modeled as credential generation. Native make/Go workflows are evidence, not end-user prerequisites. |
| Versioned configuration and overlays | [Pinned configuration guide](https://github.com/gitstore-dev/GitStore/blob/06152d193e854bc643fa884daa8b9f30b0d75d0f/docs/configuration.md); alpha.6 and alpha.4 release notes | Preserve per-component layering and reject removed keys. The guide documents additive repeated config files for API/controller, while the Git service accepts one file. Integration still needs executable validation. |
| Collector-routed telemetry and private bounded profiling | Alpha.8 release notes; pinned configuration guide and Makefile | Recognize existing optional observability; do not require it for the core profile, expose private profiling publicly, or confuse telemetry queues with durable operation history. No collector/profile runtime was tested. |
| DigitalOcean infrastructure provisioning | Alpha.8 release-note claim | This is a shipped server-repository feature, outside the local daemon increment. It does not prove rootless local execution, networking, packaging or recovery. Implementation not reviewed here. |
| Missing-resource diagnostics and indexed batch input errors | Alpha.8 breaking-change notes | Record a separate M1 compatibility follow-up. No CLI response mapping is changed from release notes alone. Resolver implementation and actual CLI compatibility remain unverified here. |
| File storage selection moves to spec.provider | Alpha.8 breaking-change notes | Do not infer File upload, list, normalized selectors or payload readiness. File implementation and client compatibility were not reviewed in this pass. |
| App installation/capability contracts and auth alignment | Alpha.8 entries explicitly classified as documentation | Do not infer shipped discovery, device login, refresh or revocation endpoints from these entries. Verify public mappings before changing auth/discovery gates. |

The earlier [alpha.4](https://github.com/gitstore-dev/GitStore/releases/tag/v0.1.0-alpha.4),
[alpha.6](https://github.com/gitstore-dev/GitStore/releases/tag/v0.1.0-alpha.6) and
[alpha.7](https://github.com/gitstore-dev/GitStore/releases/tag/v0.1.0-alpha.7)
notes also report existing authentication, lifecycle, configuration and watch work.
Those entries must not be described collectively as absent server functionality.
They do not individually establish this CLI's end-to-end conformance, completion
correlation or provider compatibility.

## Dependency conclusion

DEP-012 is daemon implementation and verification work. DEP-013–015 contain
packaging, network/storage and host/backend experiments. Their open status is not
proof of an absent upstream server feature and does not prevent development of
those workstreams. Following the user’s clarification, the current specification includes real runtime
implementation and the M2 experiments that produce this evidence. Missing optional
mappings or unavailable platform fixtures gate only the affected subset.

No dependency is closed in this review. Real service integration must still prove
rootless execution, image trusted-source/digest integrity, safe networking/local volumes,
bootstrap readiness and retained-data/single-writer recovery on each advertised
host. Published server features supply integration inputs, not that evidence.

## Additional planning source review

Read the pinned [gitctl handlers](https://github.com/gitstore-dev/GitStore/blob/06152d193e854bc643fa884daa8b9f30b0d75d0f/gitstore-api/cmd/gitctl/main.go),
[base Compose profile](https://github.com/gitstore-dev/GitStore/blob/06152d193e854bc643fa884daa8b9f30b0d75d0f/compose.yml)
and [local overlay](https://github.com/gitstore-dev/GitStore/blob/06152d193e854bc643fa884daa8b9f30b0d75d0f/compose.local.yml)
for helper dispatch, core images and staged bootstrap. The pinned configuration
guide documents standalone gitctl migration, existing-keyspace preparation and
automatic-migration control. These are integration inputs for M2; development
Compose/shell commands are not the production daemon design. Actual protected
credential input, readiness mappings and exact image digests must be verified
before enabling each profile. No helper, container or migration was executed.

## Implementation setup evidence — 2026-11-03

[Runtime evidence](../../runtime/profiles/upstream-evidence.json) now records hashes
and selected handler/configuration lines from the pinned revision plus GHCR index,
platform-manifest and config digests. Core `0.1.0-alpha.8` images exist for Linux
amd64/arm64 and claim the pinned source revision in their OCI labels. The `v`-prefixed
image tags return 404; no `latest` substitution was used. This is source/registry
metadata only, not executed runtime support or build attestation. Optional mappings
remain unverified. `scripts/record-runtime-upstream.py` reproduces the read-only
record with bounded requests and no authorization forwarded across host redirects.

## Alpha.9 source supplement

[Alpha.9](https://github.com/gitstore-dev/GitStore/releases/tag/v0.1.0-alpha.9)
was published on 2026-11-05 at 18:31:43 UTC. Its tag resolves to
[`68f8471edc947e417c98b6e8299a1367ab6b20da`](https://github.com/gitstore-dev/GitStore/commit/68f8471edc947e417c98b6e8299a1367ab6b20da).
The [source receipt](../../runtime/profiles/alpha9-evidence.json) records the
reviewed Git blob identities. This review acquires no service layers and claims
no alpha.9 native execution or retained-data upgrade.

| Shipped input | Verified committed mapping | Consequence |
| --- | --- | --- |
| Admin/Storefront API split | [Router](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/gitstore-api/internal/app/graphql.go) and [listener defaults](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/gitstore-api/internal/config/endpoints.go): `/admin/graphql` on `api.admin_port` (4000), `/storefront/graphql` on `api.storefront_port` (4001), both in `gitstore-api`; no legacy `/graphql` alias | ADR 0012 is now Accepted. A future alpha.9 profile must update listener configuration, public URLs and internal clients together. The optional `gs admin serve` command is for the Admin UI, whose reference runtime mapping remains unverified. |
| Storefront unavailable boundary | [Handler](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/gitstore-api/internal/storefront/handler.go) returns HTTP 503 `STOREFRONT_UNAVAILABLE` with `Cache-Control: no-store` after admission | Publication reads and trusted target binding remain external prerequisites. A bound listener or healthy core API cannot establish Storefront catalogue readiness. |
| Normalized File selector | [Schema](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/shared/schemas/admin/file.graphqls) and [resolver](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/gitstore-api/internal/admin/graph/resolver/file.resolvers.go): `file(by: FileBy!)`, exactly one `id` or `namespacePath:{namespace,name}` | The upstream normalization gate is now satisfied for alpha.9. Existing baseline client queries still require compatibility work before claiming alpha.9 high-level File support. This does not establish list, upload or payload readiness. |
| Scylla partition/fence changes | Existing `002_namespace.cql` and `005_product.cql` changed. The [upstream history policy](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/.claude/rules/scylla-migrations.md) records migration names/checksums and rejects mismatched history | Preserve the alpha.8 volume and migration-target identities. An alpha.9 transition needs its own compatibility evidence; a breaking baseline reset requires a fresh keyspace and explicit reseeding. Never modify the recorded ledger or delete retained data implicitly. |
| Reference OIDC composition | `compose.oidc.yml` has the same Git blob (`ae97eab75bdb16729d5a22465e56326f175c32bc`) in alpha.8 and alpha.9 | Existing provider mapping evidence remains relevant. Production daemon enrollment/lifecycle/recovery and API issuer integration remain local implementation work. |

The executable core/1 and private core/2 profiles retain their original alpha.8
source/image pins. This supplement does not reinterpret an existing deployment,
retarget a migration or change the historical M0/M1 baseline.
