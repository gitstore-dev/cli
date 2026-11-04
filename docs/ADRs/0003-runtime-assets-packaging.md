# ADR 0003: Runtime Assets and Dependency Delivery

**Status**: accepted (architecture and bounded evaluation plan).
**Date**: 2026-10-27. **Owner**: CLI runtime/distribution maintainers.
**Updated**: 2026-11-03 — clarify CLI attestation scope and library-first, host-first/on-demand dependency delivery.
**Evidence**: EVD-001, EVD-002, EVD-022, EVD-023; official sources below.
**Dependencies**: DEP-012, DEP-013; ADR-0002 ownership is mandatory.

## Context

Calling libcrun does not acquire/unpack images, deliver networking or solve installation.
The distribution must keep executables small without asking users to assemble Docker,
Compose, make, compilers or external crun/CNI tools. Library delivery and first-use image
provisioning are separate problems. Source availability is not tested packaging support.

## Decision

Keep core gs/gitstored executables in the kilobyte-to-low-megabyte range; the
initial M2 engineering target is <=5 MiB each for stripped release builds. Record
actual measurements and any miss rather than claiming this target has been met.
Avoid bundling additional executables by default. Prefer maintained code dependencies called through library APIs. For external
runtime requirements that remain, reuse validated compatible host dependencies
and acquire missing selected-profile components on demand. Downloads require the same verification, consent, cache
and offline rules as other runtime assets. No manual runtime-stack installation
is required of the end user.

Call libcrun's library API for Linux execution. Treat it as a declared native code dependency. “Library” means a dependency
analogous to Cargo.toml/pom.xml/go.mod, not a requirement for a shared object or
dynamic loader. Compare static and dynamic linking under EXP-003A using actual
size, compatibility and delivery measurements. Do not substitute a crun executable. Optional runtime-library
absence must not prevent help, remote commands or available control operations.
Record source/build identity, ABI, transitive inventory and licenses for every
selected dependency; do not trust arbitrary loader/PATH discovery.

Evaluate library-based image acquisition/unpack and networking before helper
processes. EXP-003B compares maintained library integrations, including foreign
runtime/ABI costs, against validated host or on-demand executable fallbacks.
No Skopeo/umoci/crane/network executable is a default bundle. Justify any helper
when no suitable library meets behavior/safety/footprint goals. Do not create a
custom registry client or layer extractor to reduce dependencies. Report core
bytes, required shared libraries, package size, downloads/cache and memory
separately; externalizing bytes is not a total-footprint reduction.

Service images, VM kernels/root filesystems and optional profile assets are downloaded
only when selected, not embedded in gs/gitstored. A trusted versioned manifest identifies
profile, OS/architecture, source, digest, size when available and compatible runtime range.
Verify integrity/authenticity before activation; a transport checksum without a trusted
manifest is insufficient. Stage into a temporary owned cache entry and atomically promote
after verification. Cancellation/integrity failure leaves no active partial installation.

Keep shared verified cache entries immutable. Native mount-target preparation must
operate on a separate owned workload tree, never the shared cached rootfs or
hardlinks to its files. The initial Linux path reconstructs that tree from pinned,
rehash-verified cached blobs through the maintained image library, with no HTTP
fallback; source/destination hierarchies must be disjoint. Reserve/measure its
additional compressed, merged and extracted disk costs separately. A later
maintained overlay/reflink optimization needs equivalent isolation evidence.

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

The attestation requirement applies to artifacts produced and published by
`gitstore-dev/cli`: CLI/daemon binaries, distribution archives, generated runtime
assets/manifests and any OCI images built and published by this repository.
Bind evidence to exact published digests, including the versioned asset manifest.
It gates publication of CLI artifacts beginning with M1 and continuing through M2.

It does not require attestations from `gitstore-dev/GitStore` service images or
other upstream dependencies. Their absence is not a startup or implementation
blocker. Upstream runtime assets use trusted configured sources and pinned
platform/digest identities from the CLI distribution's trusted manifest. Offline
imports enforce the same identities. Preserve upstream provenance when available;
do not require a new upstream attestation program or an exception to use an
otherwise approved digest-pinned image.

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

CLI release verification must require subject digest, expected producer repository
(`gitstore-dev/cli`), approved signer workflow and release source ref/revision.
A valid signature from an unrelated producer is insufficient. Maintainer/CI
verification may use [`gh attestation verify`](https://cli.github.com/manual/gh_attestation_verify).
Missing, invalid or policy-mismatched CLI artifact attestations block CLI release
promotion. This release requirement does not require shipping GitHub CLI or an
attestation verifier inside gs/gitstored.

Preserve upstream source/build identities and license inventory for shipped
third-party components. A CLI repackaging attestation describes that packaging
step, not a claim that the CLI repository built the upstream binary. Offline
verification of CLI-published artifacts can carry attestation bundles and trusted
root material using the same release policy; offline upstream image import needs
trusted manifest/platform/digest checks, not absent upstream attestations.

EXP-003C, owned by distribution maintainers, validates a CLI-published file release
and offline release-verification bundle. Add an OCI attestation case only if
`gitstore-dev/cli` itself builds and publishes an OCI artifact. Check valid evidence,
tampered bytes, wrong producer/workflow/ref, missing required CLI evidence and
untrusted root material before promotion. Status: not-run. This experiment is a
CLI publication gate, independent of ordinary upstream service-image startup.

The 2026-11-03 clarification narrows the earlier ambiguous “GitStore-built” wording
to the producing repository. It supersedes any interpretation requiring upstream
GitStore images to add attestations before M2 can run.

## Candidates and alternatives

Reviewed 2026-10-27; versions are evaluation baselines, not shipped dependency pins.
The 2026-11-03 library-first/host-first/on-demand decision supersedes earlier
wording that favored packaged helper executables; the pipelines below are comparisons.
Recheck advisories and pin immutable source/build identities before an experiment/release.

| Concern | Concrete candidates / source | Evaluation implication |
| --- | --- | --- |
| Embedded runtime | [crun/libcrun 1.30.1](https://github.com/containers/crun/releases/tag/1.30.1), [library build guidance](https://github.com/containers/crun) | Compare static link with private shared library; inspect API/ABI and full transitive/license inventory. |
| Pipeline A | [Skopeo v1.24.1](https://github.com/podman-container-tools/skopeo/releases/tag/v1.24.1) + [umoci v0.6.0](https://github.com/opencontainers/umoci/releases/tag/v0.6.0) | Registry-to-OCI layout then maintained rootfs unpack; evaluate helper size, auth, rootless UID mapping and extraction safety. |
| Pipeline B | [go-containerregistry/crane v0.22.1](https://github.com/google/go-containerregistry/releases/tag/v0.22.1) + umoci v0.6.0 | Compare registry/auth/platform handling and footprint while retaining the same unpack contract. |

The [umoci project](https://github.com/opencontainers/umoci) distinguishes CLI stability
from unstable pre-1.0 Go APIs, requiring explicit API/version risk evaluation before library integration. A helper
remains a possible host/on-demand fallback, not a default bundle. These tools do not replace libcrun execution. Writing a registry client or
extractor from scratch is rejected; embedding all service images is rejected for binary
size/update coupling; requiring Docker or user-installed helpers is rejected for setup.

## Bounded experiments and release gate

| Experiment | Inputs / scope | Required observations | Decision rule |
| --- | --- | --- | --- |
| EXP-003A | Static versus shared libcrun integration × Linux amd64/arm64 on the ADR-0005 reference hosts | Executable/library bytes, transitive inventory, reproducible source/build identity, license/notices review, API lifetime/fork/signal behavior, upgrade and crash recovery | Select a mode only if all safety/distribution obligations pass and publish size/compatibility tradeoff; otherwise no M2 distribution claim. |
| EXP-003B | Maintained library integrations versus host/on-demand helper pipelines × two architectures; public/private test registry; one normal multi-layer and malicious/truncated fixtures | Platform/digest verification, auth expiry/denial/scope, cross-host redirect non-leakage, traversal/symlink/hardlink/whiteout handling, rootless ownership, cancel/resume/offline/cache corruption, disk bounds | Reject any pipeline failing integrity/extraction/auth isolation; choose among passing options by measured footprint and maintenance burden. |

Runtime/distribution maintainers own EXP-003A/B before M2 packaging approval;
EXP-003C gates the first applicable artifact release as described above.
Use test credentials/registries only when implementation validation is explicitly in
scope. Record cold/warm startup readiness time, executable bytes, first-use download,
installed/cache footprint separately. Measure against the user-requested small-core objective and initial 5 MiB per-core
engineering target; establish remaining budgets from actual builds, not invented results.

## Consequences and unverified feasibility

GitStore owns dependency delivery, security updates and recovery across helper failures.
Shipping helpers can increase distribution size even when gs/gitstored stay small.
Static linking and private shared delivery each need concrete distribution review; neither
is assumed compliant or portable merely by architecture. No tool was installed or image
pulled for this ADR. DEP-013 stays open until experiment evidence is published.

## 2026-11-03 library footprint checkpoint

[The libcrun experiment](../../runtime/experiments/libcrun.md) links the actual C
API, not a crun process. On the recorded Linux arm64 fixture a stripped caller
with static libcrun is 659,648 bytes. The shared caller is 67,496 bytes plus an
859,984-byte libcrun shared object; both still require their recorded native system
libraries. The initial development candidate is static libcrun with bounded native
workers inside the same executable. There is no optional dlopen architecture or
additional runtime executable. Complete transitive source/build/license inventory
and package/RSS measurements remain release gates; these caller sizes are not a
measurement of the completed gitstored executable.

[Image library experiments](../../runtime/experiments/image-library.md) remain open.
The Go library/C-archive probe exceeds the core budget with its runtime and test
harness. Maintained Rust transfer/unpack APIs are under comparison; none is selected
or bundled based on library branding alone. The current stripped gs baseline also
misses the initial budget, so T079 must measure real optimization results and keep
that miss visible. These experiments do not add an upstream image-attestation gate
or a default helper bundle.
