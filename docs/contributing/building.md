# Build from source

No CLI/daemon binary release is currently published. Builds described here are
for trying the source preview and contributing. Return to the
[user guides](../README.md) for configuration and usage.

## Remote CLI

Use the Rust toolchain pinned in [rust-toolchain.toml](../../rust-toolchain.toml):

```sh
cargo build --release --locked -p gs
```

The executable is `target/release/gs`. Remote commands do not need `gitstored`,
service images, a Git executable or the native runtime dependencies below.

## Native Linux runtime

Build the Rust CLI and C daemon on Linux. A working development environment needs
CMake >=3.28, a C17 compiler, Rust, Python 3, pkg-config, make, Meson and Ninja.
Native development headers/libraries must satisfy the CMake checks:

| Integration | Host development dependencies |
| --- | --- |
| Native execution | libsystemd >=255, libseccomp >=2.5.5, libcap, json-c >=0.17 |
| Networking | GLib 2 |
| Image library / intent hashing | OpenSSL 3 |
| Bootstrap HTTP / password hashing | libcurl >=8.5.0, libxcrypt >=4.4.36 |

These are source-build dependencies. They do not define the intended end-user
installation contract. Pinned Jansson, SQLite, libcrun and libslirp source archives
are verified by the build; libcrun and libslirp are built as code libraries.
The Rust image bridge uses its separate locked Cargo manifest. No crun,
slirp4netns, image-transfer helper or CNI executable is built into the package.

```sh
cargo build --release --locked -p gs
cmake -S daemon -B build/daemon-release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DGITSTORED_IMAGE_SDK=ON \
  -DGITSTORED_LIBCRUN=ON \
  -DGITSTORED_SLIRP=ON \
  -DGITSTORED_BOOTSTRAP=ON \
  -DGITSTORED_TESTING=OFF
cmake --build build/daemon-release --parallel 2
```

Install matching ordinary executable copies together in a trusted directory:

```sh
install -d "$HOME/.local/bin"
install -m 755 target/release/gs build/daemon-release/gitstored "$HOME/.local/bin/"
```

Put that directory on your PATH. Copying into the installation directory also
avoids Cargo build-tree hard links: the installed broker must be a single-link
owned executable. Each build's linked host libraries must be available at runtime;
there is no published missing-library installation bundle yet. See
[local deployment prerequisites](../local-runtime.md#before-you-start).

For offline source builds, `-DGITSTORED_SOURCE_CACHE=/absolute/cache` supplies the
same pinned archives, still hash checked. Rust dependency availability is separate
from service-image offline layouts. Do not replace the immutable pins to bypass a
failed fetch or compatibility check.

## Control-only build

On macOS, or when testing the protocol without workloads, leave native options off:

```sh
cmake -S daemon -B build/daemon -DGITSTORED_TESTING=ON
cmake --build build/daemon --parallel 2
ctest --test-dir build/daemon --output-on-failure
```

This build supports portable control checks; it cannot run Linux containers on
macOS. Manually start a manager only in explicitly selected private directories:

```sh
mkdir -m 700 /absolute/private/runtime /absolute/private/state
build/daemon/gitstored \
  --runtime-dir /absolute/private/runtime \
  --state-dir /absolute/private/state
```

Replace the example paths with existing trusted parent paths. Use `gs
--daemon-socket /absolute/private/runtime/gitstored.sock` to connect. A state-less
control process cannot provide persistent deployment/operation queries. The build
refuses incompatible or corrupt state instead of silently upgrading it.

## Verification

Run the checks in [CONTRIBUTING.md](../../CONTRIBUTING.md). The saved
`scripts/test-daemon.sh --daemon PATH --gs PATH` runner tests actual Rust/C process
interoperability with isolated configuration and synthetic credentials.

For sanitizer builds use Clang 18 and `-DGITSTORED_SANITIZERS=ON`. The
[protocol fuzz guide](../../daemon/fuzz/README.md) provides a bounded smoke command.
Real runtime tests require explicitly selected disposable Linux fixtures; see the
[runtime validation guide](../../specs/003-m2-daemon-foundation/quickstart.md).
Never point tests at a user's existing deployments, credentials or retained data.

The [architecture decisions](../ADRs/), [contracts](../contracts/README.md), and
[validation archive](validation/local-runtime.md) explain implementation boundaries
and the evidence behind current limitations. Historical experiment tooling is
under `runtime/experiments/`; it is not installed with the executables.
