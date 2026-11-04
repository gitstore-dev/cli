# Image capability experiments

These are isolated candidate API/size probes, outside the Cargo/native production
build. The recorded checkpoint selects Rust code libraries for the development
adapter; it does not enable a product runtime capability, bundle Go/tools, require
upstream attestations or add a runtime command. `go.mod`/`go.sum` pin comparison source modules.
The host Go toolchain for the recorded run is 1.26.4, macOS arm64.

`go-probe` uses go-containerregistry 0.22.1 for synthetic private-registry transfer
and OCI layout creation, and umoci 0.6.0 for unpack. No Docker credential helper or
ambient keychain is selected: auth is an explicit in-memory synthetic identity.
Tests bind only private loopback fixture servers, use owned temporary filesystem
roots, and never fetch an external workload image. Ordinary invocation prints
usage; `selftest` exercises the fixture. The C export runs that same fixture only.

From `go-probe`, set GOENV=off/GOTOOLCHAIN=local and separate GOMODCACHE/GOCACHE under
ignored `build/`, then use `go test -mod=readonly ./...` and
`go build -mod=readonly -trimpath -ldflags=-s`. Building with
`-buildmode=c-archive -o image-probe.a` also produces the header for `../caller.c`.
On the recorded Mac the C linker used CoreFoundation, Security and libresolv.
Basic-auth cross-port redirect isolation, explicit-auth denial/helper avoidance,
malformed tar and traversal/symlink containment tests pass. Linux compilation,
protected IPC, other cross-origin/token-service cases, malicious hardlinks,
resource/disk bounds and cancellation still need separate results. A passing
synthetic unpack is not an extraction/security audit or a production integration.


`cmd/fixture-image` is a developer-only image producer for the explicitly authorized
Linux fixture. It converts that fixture's owned exported filesystem tar into an OCI
layout/archive through the same pinned image library. It does not extract layers,
serve a product command, publish an image or alter upstream workload images.

## Rust comparison

The independent `rust-probe` manifest pins oci-client's supported transport hook
by immutable upstream revision and ocirender 0.2.3 by Cargo.lock. Production root
Cargo dependencies remain unchanged. From the repository root:

```sh
cargo test --manifest-path runtime/experiments/image-library/rust-probe/Cargo.toml --locked
cargo build --manifest-path runtime/experiments/image-library/rust-probe/Cargo.toml \
  --locked --release --no-default-features --features native --target-dir build/rust-native-image-probe
```

Tests require loopback binding; they use only synthetic servers/credentials and
temporary owned roots. On the recorded Mac the actual C link was:

```sh
cc -O2 -mmacosx-version-min=27.0 -Wl,-dead_strip \
  runtime/experiments/image-library/rust-caller.c \
  build/rust-native-image-probe/release/libgitstore_image_library_probe.a \
  -framework Security -framework CoreFoundation -liconv -lSystem -lm \
  -o build/rust-native-image-c-probe
strip -x build/rust-native-image-c-probe
python3 runtime/experiments/image-library/test_rust_bridge.py \
  --registry-fixture build/image-library-probe \
  --rust-probe build/rust-native-image-probe/release/gitstore-image-library-probe \
  --c-probe build/rust-native-image-c-probe
```

`build/image-library-probe` is the Go developer fixture built above, never a
product prerequisite. The Rust C ABI checks explicit buffer lengths, UTF-8 and
unwinds; it is experiment-only. Full-buffer transfer is restricted to tiny known
fixtures and cannot establish production transfer memory limits. See the actual
[comparison and remaining gates](../image-library.md), rather than treating these
tests as provider compatibility or a complete extraction/security audit.

The Linux arm64 run uses the same locked source offline with the pinned Rust
components recorded in `../linux-fixture/rust-toolchain-lock.json`. Cargo vendor
output is development input only. When streaming sources from macOS, set
`COPYFILE_DISABLE=1` and omit xattrs; otherwise AppleDouble `._*.c` metadata files
can be mistaken for source by native build globs. The initial failed attempt was
corrected by checking AppleDouble magic/version before removing only those
transport sidecars in the disposable vendor tree. No dependency source patch was
applied. All seven tests then pass. The C linker needs `-ldl -lpthread -lm -lssl
-lcrypto`; Linux did not need external liblzma.

`bridge.go` retains the cgo export only when cgo is enabled; the same normal Go
fixture also cross-compiles with `CGO_ENABLED=0 GOOS=linux GOARCH=arm64`. Its native
code dependencies, tests and C export remain in the comparison manifest. The
cross-compiled registry fixture is developer tooling, not an installed runtime
requirement.
