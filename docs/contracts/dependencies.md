# Dependencies and Release Gates

**Reviewed**: 2026-10-27. Status: `open`, `resolved`, or `superseded`.
Resolution requires linked evidence and reclassification review; closing an issue is not enough.
Owner roles identify responsibility, not assigned individuals or requests to change upstream.
Tracking issue state is unverified. M0 completion permits owned release prerequisites to remain open.

| ID | Missing guarantee | Affected records | Owner role | Milestone | Status | Tracking / decision |
| --- | --- | --- | --- | --- | --- | --- |
| DEP-001 | Public installation, auth/provider/flow discovery, legacy compatibility and proxy base paths | CMD-006, CMD-007, CMD-008, CMD-009, CMD-012, CMD-013 | GitStore API/auth maintainers | M1 | open | [reference](https://github.com/gitstore-dev/GitStore/issues/472) |
| DEP-002 | Public Collection mutation/watch mappings | CMD-036, CMD-039, CMD-040, CMD-041 | GitStore catalogue API maintainers | M1 affected operations | open | [reference](https://github.com/gitstore-dev/cli/issues/4) |
| DEP-003 | Explicit Product creation placement | CMD-024 | GitStore API/admission maintainers | M1 placement options | open | [reference](https://github.com/gitstore-dev/GitStore/issues/490) |
| DEP-004 | Submitted-revision/generation admission/readiness correlation | CMD-045, CMD-014–CMD-043 (requested resource waits) | GitStore API/admission/controller maintainers | M1 waits | open | [reference](https://github.com/gitstore-dev/cli/issues/4) |
| DEP-005 | Export/authoring fidelity and supported mutation field coverage | CMD-014, CMD-017, CMD-019, CMD-022, CMD-079, CMD-080, CMD-081, CMD-082 | CLI + GitStore API/schema maintainers | M1 export and body authoring | open | [reference](https://github.com/gitstore-dev/cli/issues/1) |
| DEP-006 | Capability-driven callable CRD/MediaAsset mappings | CMD-012, CMD-013, CMD-061, CMD-062, CMD-063, CMD-064, CMD-065, CMD-066, CMD-067, CMD-068, CMD-069, CMD-070, CMD-071, CMD-072, CMD-012–CMD-013 (core/extension discovery distinguished) | GitStore extensibility/API maintainers | M3; core discovery subset M1 | open | [reference](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/docs/ADRs/0016-custom-resource-definitions.md) |
| DEP-007 | File list/upload, payload/LFS and credential readiness | CMD-059, CMD-060, CMD-076 | GitStore storage/API/Git maintainers | M3 | open | [reference](https://github.com/gitstore-dev/GitStore/blob/b6c866421dbcf43994565f847553c1f6aec83994/docs/resource-storage/lfs-object-storage.md) |
| DEP-008 | Final local daemon protocol design | CMD-046, CMD-047, CMD-048, CMD-049, CMD-050, CMD-051, CMD-052, CMD-053, CMD-054, CMD-055, CMD-056, CMD-057, ADR-0001 | CLI/daemon maintainers | M0 design before M2 implementation | resolved | [reference](../ADRs/0001-daemon-protocol.md) |
| DEP-009 | Public stale-client author-update precondition | CMD-017, CMD-022, CMD-027, CMD-033 | GitStore API/admission maintainers | M1 author updates | open | No existing tracking reference verified |
| DEP-010 | Product transfer destination/conflict/atomicity contract | CMD-058 | GitStore catalogue API maintainers | M3 | open | [reference](https://github.com/gitstore-dev/cli/issues/4) |
| DEP-011 | Publication/release readiness and rollback contract | CMD-073, CMD-074 | GitStore catalogue/controller maintainers | M3 | open | No existing tracking reference verified |
| DEP-012 | Daemon execution, protocol and recovery validation | CMD-005, CMD-046, CMD-047, CMD-048, CMD-049, CMD-050, CMD-051, CMD-052, CMD-053, CMD-054, CMD-055, CMD-056, CMD-057 | CLI/daemon maintainers | M2 | open | [reference](../ADRs/0002-runtime-ownership.md) |
| DEP-013 | Artifact provenance and distribution/image/library packaging experiments | First M1 CLI release; CMD-046, CMD-047, CMD-048, CMD-049, CMD-050, CMD-051, CMD-052, CMD-083 runtime assets | CLI runtime/distribution maintainers | M1 artifact provenance; M2 runtime packaging | open | [reference](../ADRs/0003-runtime-assets-packaging.md) |
| DEP-014 | Rootless networking/volumes and optional CSI experiments | CMD-046, CMD-047, CMD-048, CMD-049, CMD-050, CMD-051, CMD-052, CMD-075 | CLI runtime/storage maintainers | M2 baseline; M3 CSI | open | [reference](../ADRs/0004-rootless-network-volumes.md) |
| DEP-015 | Platform backend packaging and support validation | CMD-046, CMD-047, CMD-048, CMD-049, CMD-050, CMD-051, CMD-052 | CLI platform/distribution maintainers | M2 macOS; Linux host matrix | open | [reference](../ADRs/0005-platform-backends.md) |
| DEP-016 | End-to-end incremental Git progress through API/proxy | CMD-045 | GitStore API/Git and deployment maintainers | M1 streaming claim | open | [reference](https://github.com/gitstore-dev/cli/issues/4) |
| DEP-017 | Journal-ready typed watch and reconnect conformance | CMD-029, CMD-035, CMD-041, CMD-043 | CLI + GitStore API/watch maintainers | M1 watch release | open | No existing tracking reference verified |

## DEP-001

Advertise each flow, usable endpoints/client metadata/scopes and installation identity independently; test proxy prefixes and explicit legacy mode. Unknown discovery fails compatibility before prompts/writes; never assume OIDC implies device/refresh/revoke.

## DEP-002

Provide explicit public schema, authorization, input/version/completion guarantees for each write/watch. Until then only read/list are offered; internal methods are insufficient.

## DEP-003

Validate a public placement-hints contract. Baseline create uses server placement; do not offer repository/path override flags or rewrite stored update provenance.

## DEP-004

Verify correlation after both API and Git submission, including old Ready and failures/timeouts. A known unsupported wait fails before submission; submission-only reports accepted commit and unverified completion.

## DEP-005

Round-trip Unicode/multiline/newline variants and all exposed fields, define writable projection and list consistency limits. Namespace/Repository inputs omit body: reject nonempty author body instead of discarding it. No atomic multi-page snapshot promise.

## DEP-006

Publish per-operation names/scope/schemas/persistence/mappings and authorization. Versioned built-in core mappings may be used only against verified server versions; arbitrary kinds fail unsupported.

## DEP-007

Confirm public list separately from upload negotiation; validate transfer auth/checksum/size/retention, manifest partial failure and LFS batch/missing objects. No payload command inferred from metadata/source types.

## DEP-008

Resolved 2026-10-27 by EVD-022, ADR-0001 and SCN-027/029: exact operations, types, framing, limits, versions, dedupe/expiry/cancellation and examples are published. Runtime interoperability remains DEP-012; design acceptance alone cannot unblock shipped daemon commands.

## DEP-009

Provide expected resource version or equivalent server-enforced client precondition for Namespace/Repository/Product/Category update. Current ObjectMetaInput has name/namespace/labels/annotations, not resourceVersion; internal commit races/controller-status checks do not protect a stale client. Until then author update fails unsupported, not best-effort overwrite.

## DEP-010

Specify public transfer semantics and permissions before finalizing flags; reject unsupported workflow.

## DEP-011

Specify publish/release public operations, revisions, readiness and failure/rollback guarantees; no speculative command tree.

## DEP-012

Prove C/Rust wire round trips, peer auth, parser bounds/fuzz/sanitizers, durable dedupe, crash/stop/migration safety and no overlapping writers. Until implementation/release gates pass, daemon command rows remain blocked.

## DEP-013

Execute EXP-003A/B: compare linking/license/size and maintained image pipelines, private auth redirects, safe unpack, offline/integrity/cache failures. Provision only verified profile assets; no claimed standalone installation yet.

Added 2026-10-28: EXP-003C also gates artifact provenance beginning with the first M1
CLI release. Generate digest-bound evidence in the trusted build, enforce producer/
workflow/source policy before publication or activation, and prove tampering/wrong-
identity/missing-evidence/offline-trust failures. Preserve third-party provenance without
misrepresenting repackaged assets as GitStore-built. All three experiments remain not-run.

## DEP-014

Execute EXP-004A/B against chosen host matrix, DNS/ports/isolation/restart cleanup and retained volumes. CSI needs separate privileged/driver evidence and cannot become a default prerequisite.

## DEP-015

Execute EXP-005A/B for Apple/container and managed-VM path; publish supported hosts only after startup/private image/network/volume/log/stop/recovery and installation privilege tests. Linux-only operation must not be advertised as native macOS.

## DEP-016

Reviewed SmartHttp GitClient.ReceivePack returns []byte. Verify actual negotiated progress streaming and proxy flushing before advertising live output; forward received output faithfully, never invent resumable admission tail.

## DEP-017

Verify selected server journal readiness, authorization, opaque cursor continuity and WATCH_EXPIRED resnapshot for each typed watch. Declared subscription is source evidence; deployment readiness/reconnect behavior remains untested.

## Design versus release resolution

DEP-008 may become resolved when the local protocol is accepted in M0. DEP-012 stays
open until executable verification. DEP-013–DEP-015 remain open after accepting bounded
evaluation plans: choosing experiments is not evidence that they have passed.
DEP-005 includes client conformance work and field-shape gaps; publishing this design
does not promise that an existing server supports arbitrary authoring bodies or snapshots.

## Runtime evaluation reconciliation

EXP-003A/B, EXP-004A/B and EXP-005A/B are specified in ADRs 0003–0005 with owner,
candidates, bounded scope, required observations and reject/pass criteria. Their status
is not-run. Accepting evaluation plans leaves DEP-013–DEP-015 open. Protocol/ownership
references remain available to every daemon row; none is reclassified as executable
support because DEP-008 is resolved. Live compatibility and release claims require
new evidence before changing command support classifications.
