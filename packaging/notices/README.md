# Reviewed release notices

Run `runtime/packaging/notices.py` against an explicit reviewed manifest and a
combined Cargo metadata package list for **both** the CLI and image-backend
workspaces. Use target-filtered production/build Cargo edges; dev-only dependencies are not
distributed components. Each third-party `name@version` and each non-system native closure
name must occur exactly once. Include all applicable full license/copyright
texts as ordinary files with SHA-256 identities, SPDX expressions and immutable
HTTPS source pointers. Do not infer native obligations from Cargo metadata.

Manifest shape:

```json
{"components": [{"id": "name@version", "license": "MIT", "source": "https://example.org/source",
"licenses": [{"path": "licenses/name.txt", "sha256": "FULL_SHA256"}]}]}
```

For LGPL static code, include `correspondingSource` and `relinkKit` entries with
`path`, `sha256` and `releaseAsset`. Supply actual patched corresponding source,
build instructions and relinkable objects/material sufficient for the applicable
license; review their contents before selecting the manifest. Sign and publish
those assets alongside the binaries. The generator validates coverage and bytes;
it cannot determine legal sufficiency. A missing kit blocks qualification.

Private Linux linkage includes libcrun, Jansson, SQLite, libslirp, GLib, systemd,
libseccomp, libcap, json-c, libxcrypt, PCRE2, patched curl, OpenSSL, and the Rust
image library's transitive code. Inspect the final linker inputs to establish
the actual closure. macOS control builds have a different closure. System libc,
the OS loader and libgcc are supplied by the host; no loader/glibc is bundled.

Generated notices include the no-compatibility/no-data-preservation/no-recovery
prerelease statement and optional consumer attestation policy. A reviewed full
manifest and LGPL relink assets have not yet been produced for alpha.1.

`scripts/prepare-relink-kit.py` prepares actual Linux production application
objects and selected native archives from CMake's link command. It refuses testing
builds, unknown link flags and unsafe/changing inputs, copies ordinary files,
relinks a separate daemon and runs ordinary/fd/memfd closure checks. Its output is
explicitly unqualified review material. A successful relink does not supply the
missing corresponding patched sources, full notices or an obligation review;
do not add this draft kit to reviewed release inputs without completing those.

The producer also checks source archive bytes against the actual production
Cargo.lock checksum or pinned Git commit. Full standard-library notices are a
separate native-closure component. Source/relink selectors can reference generated
subjects; see [reviewed inputs](../release-inputs/README.md). This avoids requiring
our own release assets to be hosted before the release can be built. Rust source
archives, pinned native sources, distribution patches and committed project source
form the source subject; actual Linux application objects and private archives
form the relink subject. Both are checked before final package hashes are frozen.
