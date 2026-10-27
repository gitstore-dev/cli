# ADR 0003: Runtime Assets and Dependency Delivery

**Status**: accepted (architecture and bounded evaluation plan).
**Date**: 2026-10-27. **Owner**: CLI runtime/distribution maintainers.
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

Runtime/distribution maintainers own both experiments before M2 packaging approval.
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
