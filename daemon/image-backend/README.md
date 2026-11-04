# Internal image code library contract

T035's internal staging adapter and build binding are implemented. This ordinary Rust static library is a C daemon
code dependency, resolved separately from gs to keep TLS features independent.
It is not an executable, plugin, runtime download or new public daemon method.
No lifecycle capability is enabled by linking it. Selection evidence and initial
pins are in `runtime/experiments/image-library.md` from the repository root.

The C boundary is `../src/image_backend.h`. Only a native worker may call staging;
the control owner never initializes its runtime or threads. Parent-only database,
resource ownership and credential brokerage are separate responsibilities.

The <=64 KiB JSON plan has exactly these fields (camelCase), rejecting unknown or
duplicate fields: `schemaVersion` (1), `reference` (registered registry/repository
with an immutable sha256 **platform manifest** digest), `configDigest` (sha256),
`os` (linux), `architecture` (amd64 or arm64), `tokenOrigins`, `blobOrigins`
(arrays of at most eight exact HTTPS origins each), `maximumDownloadBytes`
(1..2 GiB), `maximumMergedBytes` (1 KiB..8 GiB), `timeoutMs` (1..600,000).
The trusted manifest/consent layer supplies this plan; it is not caller wire input.
Origin entries contain no userinfo/path/query/fragment. Registry is always allowed;
additional credential destinations are limited to explicit token origins. Approved
blob-only origins cannot receive Authorization headers. Redirects recheck origin
policy and strip cross-origin credentials through maintained HTTP transport.

The caller lends an already-open, owned, empty mode-0700 staging directory. Linux
access is pinned through a duplicate descriptor, never a new caller-selected host
path. Files are created only under that staging root. The adapter writes verified
manifest/config/blob files, merges through ocirender into a byte-bounded tar sink,
then unpacks through maintained tar with ownership/xattrs/special permissions
disabled. It neither activates assets nor treats partial staging as valid.
Full blob consumption verifies digests using the OCI client's maintained stream;
trusted manifest/config digests and platform must agree before unpack.

The worker must have finite address-space (at most 1 GiB) and file-size (at most
8 GiB) limits and core dumps disabled. The SDK checks these prerequisites before
staging side effects. Merge-library
metadata/hardlink promotion can allocate internally; a bounded output writer alone
is not a memory bound. Worker deadline/cancellation, disk-space reservation and
parent-observed cleanup/activation remain required before runtime enablement.
Worst-case staging reservation includes downloaded bytes plus **twice** merged
bytes (intermediate tar and rootfs), plus bounded metadata. No decompression,
registry, authentication, crypto or OCI whiteout protocol is reimplemented.

Explicit anonymous/Basic credentials are separate borrowed bounded buffers and
never part of saved JSON, argv or error text. The caller/broker owns secret
lifetime. Results contain only byte counts and a stable generic status; the parent
attaches known operation/deployment identity. Failure may leave partial owned
staging for recorded cleanup. Byte counts are meaningful only on success; zeroed
failure output does not claim that no bytes or files were written. Panic output
is suppressed at this boundary.

Tests use private roots and synthetic loopback transports through test-only Rust
constructors. Such constructors are absent from the C ABI and production origin
validation. Production commands never select a test profile/backend by request.

## Development checks

Root workspace checks continue to cover gs separately. Run these additional SDK
checks with pinned Rust 1.96.0:

```sh
cargo fmt --manifest-path daemon/image-backend/Cargo.toml -- --check
cargo clippy --manifest-path daemon/image-backend/Cargo.toml --all-targets --locked -- -D warnings
cargo test --manifest-path daemon/image-backend/Cargo.toml --locked
```

Linux C integration uses `-DGITSTORED_IMAGE_SDK=ON` with CMake, then the normal
build/CTest commands. This explicit development option remains off in the current
control-only build while runtime integration is unfinished. It is not a gs user
option or permission to provision. OpenSSL 3 SDK/runtime libraries are ordinary
linked dependencies. The SDK is not currently ASan-instrumented; C-only sanitizer
checks do not establish Rust/compression-code sanitizer coverage.

Recorded macOS checks pass seven Rust unit tests and Clippy. Linux arm64 checks
pass eight unit tests and Clippy; the ninth, opt-in acquisition scenario passes
against the maintained Go synthetic registry. It tests actual adapter streaming,
digest/config/platform checks, layer merge/unpack, wrong-platform refusal and
pre-download quota refusal. The HTTP client injection exists only in Rust tests;
the production C ABI accepts only HTTPS policies.

For that explicit scenario build the developer registry fixture from
`runtime/experiments/image-library/go-probe`, build SDK tests with `cargo test
--no-run`, then invoke `runtime/experiments/image-library/test_sdk.py` with explicit
`--registry-fixture` and `--sdk-test` executable paths. The runner clears HOME/PATH
and uses only temporary roots and synthetic credentials. The registry executable
is developer tooling, not a product prerequisite. At the original T035 checkpoint, all 15 Linux native C tests
passed with the SDK linked, including borrowed C credential-layout/error checks.
Those T035-only checks do not establish service lifecycle support. Later internal
cache evidence is recorded in `runtime/experiments/provisioning/evidence.json`.


## Offline OCI-layout import (internal T039 checkpoint)

`gitstored_image_import_v1` / `gstd_image_import` borrow a read-only source directory
and an empty private staging directory under the same worker limits. No HTTP client,
credentials or helper is used. Maintained OCI-spec types validate layout/index
format; only the caller's registered manifest pin selects content. All blobs are
size/digest checked, source lookup forbids symlinks/mount crossing, and the same
config/platform and maintained merge/unpack checks run. No external receipt or
pre-extracted rootfs is trusted. Failed imports can leave partial staging.

Linux now passes thirteen unit tests (including three offline scenarios), plus the
separately opt-in synthetic registry scenario recorded above. The real API image
also imports in a network-isolated disposable fixture, as recorded in
`runtime/experiments/provisioning/evidence.json`. This does not enable a public
lifecycle method or establish complete offline provisioning/consent integration.


`gitstored_image_resume_v1` / `gstd_image_resume` are registry-authorized worker
entry points for a separate empty retry target. Complete prior staging blobs are
rehashed/copied/rehashed; incomplete ones restart in full through the OCI client.
A mismatched manifest fails before client construction. There is no unsafe
byte-range append or offline-to-network authorization fallback. The parent must
pin the source producer identity, observe its termination, bind unchanged
plan/consent and reserve the destination. Production lifecycle integration remains
separate. The opt-in synthetic registry test now also verifies exact truncated-
layer refetch, intact-blob reuse and identical reconstruction.


`gitstored_image_materialize_v1` / `gstd_image_materialize` rebuild an independently
owned workload tree from managed cache blobs without any network client or auth.
They use the same digest/config/platform checks and maintained merge/extraction;
no source rootfs content or cross-tree hardlinks are reused. Missing blobs fail.
Source/target directory ancestry must be disjoint for materialization/import/retry.
The real API fixture passes with networking isolated and its entire shared cache
unchanged. This prepares T040; it does not implement or enable the native backend.
