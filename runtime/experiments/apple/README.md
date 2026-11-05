# Apple library feasibility probe

Development experiment only; neither executable is part of `gs` or `gitstored`.
See [results and limits](../apple.md). The package declares ordinary Swift
dependencies; `Package.resolved` pins their revisions and
`dependency-inventory.json` records the license-file inventory. That inventory
does not constitute distribution approval.

Build from the repository root on the selected Apple-silicon macOS 26 host:

```sh
swift build --package-path runtime/experiments/apple/library-probe \
  --scratch-path "$PWD/build/apple-feasibility/build" \
  --cache-path "$PWD/build/apple-feasibility/cache" \
  --config-path "$PWD/build/apple-feasibility/config" \
  --security-path "$PWD/build/apple-feasibility/security" \
  --configuration release --jobs 2
build/apple-feasibility/build/out/Products/Release/DirectGuard
```

`DirectGuard` rejects the direct provider configuration before any guest boots.
The other probe is intentionally restricted to the already authorized fixture
`gitstore-m2-linux-a1a13c`. It never creates a host service or downloads assets:

```sh
build/apple-feasibility/build/out/Products/Release/AppleProbe --observe gitstore-m2-linux-a1a13c
build/apple-feasibility/build/out/Products/Release/AppleProbe --exec-check gitstore-m2-linux-a1a13c
build/apple-feasibility/build/out/Products/Release/AppleProbe --guest-image-check gitstore-m2-linux-a1a13c
build/apple-feasibility/build/out/Products/Release/AppleProbe --guest-offline-check gitstore-m2-linux-a1a13c
build/apple-feasibility/build/out/Products/Release/AppleProbe --guest-recovery-check gitstore-m2-linux-a1a13c
```

The three guest modes require the existing isolated runtime test checkout and
`/home/gitstorefixture/experiments/mac-library-fixture.json`. They run fixed tests
as guest UID/GID 11000 through `ContainerAPIClient`, check exit status and structured
runner results, and keep raw output in ignored mode-0600 host logs. There is no
arbitrary command or host mount option. Provisioning the development fixture used
the Apple container CLI separately; the probe does not wrap that executable.
