# Image library selection — development checkpoint

T032 selects ordinary Rust oci-client/ocirender dependencies linked through an
owned C ABI for the development adapter. T035–T039 must enforce production bounds,
credentials, trusted asset activation and offline/consent behavior before enabling
the capability. This checkpoint delegates registry/layer semantics to maintained
libraries and measures core/external footprint separately.
No Skopeo, crane, umoci or gh executable is added to the default distribution.

## Actual Go API/ABI probe

[Reproducible source/module lock](image-library/README.md) compares library calls,
not tool subprocesses. On macOS arm64 with Go 1.26.4, go-containerregistry v0.22.1
and umoci v0.6.0 successfully perform synthetic private-registry transfer by digest,
OCI layout writing, rootless unpack, layer replacement and whiteout removal. A C
caller of a generated Go archive runs the same fixture successfully. Explicit UID
and GID mappings and an OCI Linux/architecture configuration are required; the
initial missing-mapping and missing-OS cases failed rather than proving support.

| Probe artifact | Bytes | Scope |
| --- | ---: | --- |
| Go executable, `-trimpath -ldflags=-s` | 11,807,378 | Libraries plus synthetic registry/test fixture code |
| C archive | 15,531,720 | Archive size, not installed executable footprint |
| C executable linked to archive | 13,066,120 | Libraries, Go runtime, C bridge and fixture code |

These are harness-inclusive measurements, not an incremental daemon-link estimate.
The executable probes exceed the <=5 MiB core target before daemon control logic;
this does not justify silently embedding them or moving costs into an unreported
shared component. The Mac executable additionally uses system CoreFoundation,
Security, libresolv and libSystem. No stable upstream C ABI is assumed: the generated
C export is our experiment, and umoci explicitly treats its Go API as unstable
before 1.0. See [umoci's API](https://pkg.go.dev/github.com/opencontainers/umoci).

The baseline current release gs is 9,949,824 bytes (8,808,560 after `strip -x` on
an isolated copy); size optimization/complete runtime measurements remain T079.
No release build flags were changed by this measurement.

## Actual Rust API/ABI probe

The separate [Rust manifest/lock](image-library/rust-probe/Cargo.toml) uses
ocirender **0.2.3**, Tokio and oci-client. The initial released oci-client 0.18.0
probe passed synthetic transfer but did not expose an injectable HTTP transport.
That prevents enforcing the approved token-service origin before credentials are
attached. It is not selected for protected product acquisition.

The current probe pins maintained oci-client upstream revision
`71c08729adeb88fbdda43966ece962d32b58fb8d` (unreleased 0.19.0), whose public
[`Client::new_with_transport`](https://github.com/oras-project/rust-oci-client/blob/71c08729adeb88fbdda43966ece962d32b58fb8d/src/transport.rs)
passes all registry and token requests through a caller-supplied HTTP service.
Our policy wraps maintained reqwest transport, checks the exact approved numeric
loopback origin before execution, rejects unapproved redirects and explicitly
chooses its TLS backend. It does not implement OCI/authentication wire semantics.
An unapproved Bearer token realm and cross-port redirect receive **no connection**
in two controlled tests. Real multi-origin registry/token/CDN policies are future
product work; the fixture's single-origin policy does not prove provider support.

Five extraction cases pass through ocirender's merge and maintained tar unpack:
replacement/whiteout, malformed input refusal, symlink containment, hardlink target
containment and parent traversal containment. Caller-controlled output/layer paths
are not a product API. Trusted CAS paths and new private staging are required;
untrusted Docker-save path resolution is not adopted.

On macOS arm64/Rust 1.96.0, the current native-TLS release probe is **3,373,312
bytes**; its C caller is **3,913,384 bytes** after `strip -x`. Both successfully
transfer by digest from the maintained Go synthetic private registry and unpack
replacement/whiteout content with explicit synthetic auth, empty private HOME,
and PATH `/nonexistent`. The C executable links a normal Rust staticlib archive;
it is not a downloaded/exec'd helper or host-discovered library. Its external
dependencies are OS Security/CoreFoundation/libiconv/libSystem on this Mac.
The same seven focused cases pass in Ubuntu 24.04 arm64 using pinned Rust 1.96.0
and offline vendored source dependencies. A real Linux C caller also passes the
private-registry transfer/unpack bridge runner. The optimized Rust executable is
**3,740,544 bytes**; its stripped C caller is **4,004,576 bytes**. Its actual shared
dependencies include OpenSSL 3.0.13 libssl (**737,192 bytes**), libcrypto
(**4,663,464 bytes**), libgcc_s (**133,696 bytes**) and the platform libc/loader.
Compression dependencies are linked code in this run; no external `lzma` library
or executable was needed. These probe sizes do not prove the final daemon target.

The earlier 0.18 rustls executable was 5,840,720 bytes and C caller 6,539,440 bytes,
over the core target before control code. The 0.19 native-TLS measurements include
the policy change; do not attribute the whole difference solely to TLS. Feature
unification with gs's reqwest must not change remote TLS behavior when integrating:
explicit backend selection or a separately scoped maintained transport is needed.

The selected integration is ordinary Rust code dependencies plus a bounded owned
C ABI. The [dependency lock](../dependencies.lock.json) records the decision and
references the [191-package source/license inventory](image-library/rust-source-inventory.json).
This is not final release-license clearance or a native shared-file inventory.
Full-buffer `.pull` is used **only** for tiny known
fixtures; its post-pull size check is not a production memory bound. Production
requires library streaming APIs, enforced body/disk/decompression quotas,
cancellation and activation rules before this capability is enabled. Root Cargo
production dependencies/lock are unchanged. Reproduction is in the experiment
[README](image-library/README.md) and [bridge runner](image-library/test_rust_bridge.py).

## Decision and rejected defaults

Select the native-TLS Rust library adapter, initially for Linux, because both
language/ABI probes pass and its harness-inclusive core measurement leaves more
room under 5 MiB than the measured Go library embedding. This is not proof that
the full gs/gitstored pair meets its size target. Keep the C control owner out of
blocking library work: the native worker owns its Rust runtime, returns bounded
results and never writes the state database. It must not initialize background
library threads in the parent and then fork them.

go-containerregistry/umoci provide usable maintained APIs, but the measured Go/C
embedding exceeds 12 MiB. container-libs also requires Go integration/runtime,
brings broader transport/storage/native integration than this selected registry
pipeline needs, and has no direct stable C ABI that avoids that cost. It is not
selected without an equivalent safe/size advantage; no container-libs binary size
measurement is claimed. image-rs adds Linux snapshot/secure-execution integration
not required by the selected transfer/merge pipeline; it remains a possible
future alternative, not a measured winner. No candidate is rejected merely for
being a code dependency or for static linking. Default Skopeo/crane/umoci/gh tool
bundling is rejected, and no image helper fallback is currently necessary.

## Remaining comparisons and gates

- [container-libs](https://github.com/podman-container-tools/container-libs) owns
  the former containers/image development. It supplies broad transport/storage
  integration, with Go and native dependency costs still to measure for the exact
  selected packages/build tags.
- [go-containerregistry](https://github.com/google/go-containerregistry) has usable
  transfer/layout APIs in the measured probe. Explicit auth avoids its ambient
  credential-helper route; those helper packages still appear in the source module
  graph. They are not bundled or executed by this experiment.
- [Rust oci-client](https://docs.rs/oci-client/0.18.0/oci_client/) supplies registry
  operations, not a complete rootfs unpack pipeline by itself.
- [image-rs](https://github.com/confidential-containers/guest-components/tree/main/image-rs)
  provides a maintained Rust pull/unpack stack with Linux snapshot/path dependencies.
  Default signature/encryption features are unrelated to our upstream-image policy
  and must not become mandatory. Rootless support, minimal features, native C
  integration and actual size require probes.
- [ocirender](https://github.com/edera-dev/ocirender) provides Rust layer merging and
  output. The selected path above uses actual focused API/malicious-layer tests;
  production quotas, activation and a broader extraction review still remain.

The focused fixture tests and ABI/size comparison justify the development choice,
not full production activation. Controlled loopback registry results do not
establish public/private provider support, GitStore workload readiness or an
authorized product download policy.
