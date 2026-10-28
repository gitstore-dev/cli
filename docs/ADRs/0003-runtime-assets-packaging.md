# ADR 0003: Runtime Assets and Dependency Delivery

**Status**: accepted (architecture and bounded evaluation plan).
**Date**: 2026-10-27. **Owner**: CLI runtime/distribution maintainers.
**Updated**: 2026-10-28 — artifact provenance generation and verification requirements.
**Evidence**: EVD-001, EVD-002, EVD-022, EVD-023; official sources below.
**Dependencies**: DEP-012, DEP-013; ADR-0002 ownership is mandatory.

## Context

Calling libcrun does not acquire/unpack images, deliver networking or solve installation.
The distribution must keep executables small without asking users to assemble Docker,
Compose, make, compilers or external crun/CNI tools. Library delivery and first-use image
provisioning are separate problems. Source availability is not tested packaging support.

## Decision

Ship gs, gitstored and required internal helpers as a versioned GitStore distribution.
Link the Linux daemon to a pinned libcrun library; evaluate static versus privately
delivered shared-library builds under EXP-003A. Do not substitute a crun executable.
Before selecting a release artifact, record immutable source revision, build options,
transitive libraries, licenses/notices, update ownership, architecture and checksums.
Library updates require rebuilding/revalidating the daemon against the chosen API/ABI.

Use a maintained image transfer/unpack pipeline. EXP-003B compares the two concrete
helper pipelines below. Internal packaged helpers are controlled by gitstored, not
end-user installation prerequisites or arbitrary caller executable paths. No custom
registry client or layer extractor is commissioned by this ADR.

Service images, VM kernels/root filesystems and optional profile assets are downloaded
only when selected, not embedded in gs/gitstored. A trusted versioned manifest identifies
profile, OS/architecture, source, digest, size when available and compatible runtime range.
Verify integrity/authenticity before activation; a transport checksum without a trusted
manifest is insufficient. Stage into a temporary owned cache entry and atomically promote
after verification. Cancellation/integrity failure leaves no active partial installation.

Reuse verified cache entries by immutable identity. Bound cache by configured disk budget;
evict only unreferenced assets, not live workload roots/retained volumes. Support a verified
offline asset bundle with identical manifest validation. Resume partial downloads only if
source validators and final digests still match; otherwise restart the asset safely.
Interactive first use explains source/size and obtains consent; automation requires explicit
allowProvision or preexisting assets. Profile or source changes invalidate prior consent.
Help/version and remote catalogue work never initialize the runtime asset pipeline.

Registry credentials are separate from catalogue identities, scoped to registry/account,
stored securely and supplied through protected helper input. Support the chosen registry's
advertised challenge/token mechanism, including repository scope and expiry. Strip auth
on cross-host redirects unless a separately explicit trusted rule allows it. Reject
unexpected token-service URLs; no Docker credential-helper installation is required.
Private auth failure must be inspectable without logging credentials.

## Artifact attestations

GitStore-built release binaries, distribution archives, shipped helpers and generated
runtime assets must have build-provenance attestations tied to their exact published
digests. Attest a versioned asset manifest too: it is the trust link to first-use
downloads. For GitStore-built OCI images, attest immutable image digests, not mutable
tags. This gate applies to the first M1 CLI release as well as M2 runtime distribution.

Use GitHub Actions artifact attestations as the preferred producer. The current
[`actions/attest`](https://github.com/actions/attest) supports provenance and SBOM
predicates; new implementations use it rather than the older provenance wrapper.
Pin the reviewed action to a full commit when release CI is implemented. Attest the
final bytes after packaging/signing, in the trusted build workflow, before promotion.
If an SPDX/CycloneDX SBOM is produced, bind its SBOM attestation to the same subject.
Do not label provenance alone as vulnerability clearance or a higher SLSA build level.

Scope OIDC and attestation-write permissions to trusted release jobs; grant registry
write only for registry publication and other action-required metadata permissions only
where needed. PR-title validation stays read-only. Untrusted PR jobs must not obtain
release attestation credentials or promote outputs as trusted releases. Recheck action
permissions and repository-plan availability when implementing this gate.
[GitHub's generation guidance](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations)
describes the binary/image workflow integration.

Verification policy must require the subject digest, expected producer repository,
approved signer workflow and release source ref/revision. A valid signature from an
unrelated workflow or repository is insufficient. Human release verification can use
[`gh attestation verify`](https://cli.github.com/manual/gh_attestation_verify);
the runtime needs a packaged verification path, not an end-user GitHub CLI prerequisite.
Missing, invalid or policy-mismatched required attestations block release promotion
or runtime asset activation; do not silently fall back to an untrusted checksum.

Preserve third-party producer provenance/signatures and record approved trust policy
per asset. Where upstream lacks compatible attestations, require a documented alternative
authenticated source/digest policy before including it. A GitStore repackaging attestation
describes that packaging step; it cannot claim GitStore built the upstream binary.
Offline bundles must carry evidence and authenticated trust-policy/root material.
Use the same verification policy offline, with an explicit root refresh/revocation
procedure; a bundle must not define its own trusted identity.
[GitHub's offline guide](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/verify-attestations-offline)
provides the bundle/trusted-root model.

EXP-003C, owned by distribution maintainers, must validate one file release, one OCI
digest and one offline bundle. Check valid evidence, tampered bytes, wrong repository/
workflow/ref, missing evidence, unavailable online verification and stale/untrusted root
material. Record exact trust policy and failure behavior before M1 binary or M2 runtime
publication. Status: not-run. No attestation workflow is added during document-only M0.

## Candidates and alternatives

Reviewed 2026-10-27; versions are evaluation baselines, not shipped dependency pins.
Recheck advisories and pin immutable source/build identities before an experiment/release.

| Concern | Concrete candidates / source | Evaluation implication |
| --- | --- | --- |
| Embedded runtime | [crun/libcrun 1.30.1](https://github.com/containers/crun/releases/tag/1.30.1), [library build guidance](https://github.com/containers/crun) | Compare static link with private shared library; inspect API/ABI and full transitive/license inventory. |
| Pipeline A | [Skopeo v1.24.1](https://github.com/podman-container-tools/skopeo/releases/tag/v1.24.1) + [umoci v0.6.0](https://github.com/opencontainers/umoci/releases/tag/v0.6.0) | Registry-to-OCI layout then maintained rootfs unpack; evaluate helper size, auth, rootless UID mapping and extraction safety. |
| Pipeline B | [go-containerregistry/crane v0.22.1](https://github.com/google/go-containerregistry/releases/tag/v0.22.1) + umoci v0.6.0 | Compare registry/auth/platform handling and footprint while retaining the same unpack contract. |

The [umoci project](https://github.com/opencontainers/umoci) distinguishes CLI stability
from unstable pre-1.0 Go APIs, favoring an explicit versioned helper boundary for this
evaluation. These tools do not replace libcrun execution. Writing a registry client or
extractor from scratch is rejected; embedding all service images is rejected for binary
size/update coupling; requiring Docker or user-installed helpers is rejected for setup.

## Bounded experiments and release gate

| Experiment | Inputs / scope | Required observations | Decision rule |
| --- | --- | --- | --- |
| EXP-003A | Two libcrun delivery modes × Linux amd64/arm64 on the ADR-0005 reference hosts | Executable/library bytes, transitive inventory, reproducible source/build identity, license/notices review, API lifetime/fork/signal behavior, upgrade and crash recovery | Select a mode only if all safety/distribution obligations pass and publish size/compatibility tradeoff; otherwise no M2 distribution claim. |
| EXP-003B | Both pipelines × two architectures; public/private test registry; one normal multi-layer and malicious/truncated fixtures | Platform/digest verification, auth expiry/denial/scope, cross-host redirect non-leakage, traversal/symlink/hardlink/whiteout handling, rootless ownership, cancel/resume/offline/cache corruption, disk bounds | Reject any pipeline failing integrity/extraction/auth isolation; choose among passing options by measured footprint and maintenance burden. |

Runtime/distribution maintainers own EXP-003A/B before M2 packaging approval;
EXP-003C gates the first applicable artifact release as described above.
Use test credentials/registries only when implementation validation is explicitly in
scope. Record cold/warm startup readiness time, executable bytes, first-use download,
installed/cache footprint separately. Establish budgets from initial builds; do not
invent measured size or timing limits in M0.

## Consequences and unverified feasibility

GitStore owns dependency delivery, security updates and recovery across helper failures.
Shipping helpers can increase distribution size even when gs/gitstored stay small.
Static linking and private shared delivery each need concrete distribution review; neither
is assumed compliant or portable merely by architecture. No tool was installed or image
pulled for this ADR. DEP-013 stays open until experiment evidence is published.
