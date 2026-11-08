# Reviewed per-platform inputs

Production release jobs require `OS-ARCH/{notices,closure,materials}.json` for
linux-x64, linux-arm64, darwin-x64 and darwin-arm64. The darwin-arm64 and linux-arm64 sets have
source-bound controlled verification; the remaining sets are not committed yet.
The producer fails on missing inputs. Do not substitute empty manifests or
synthetic integration notices to make a release job pass.

The collectors produce proposed inputs from the actual build, not an approval or
support claim. `collect-rust-materials.py` consumes target-filtered Cargo metadata
and matching lockfiles, follows production/build edges, excludes dev-only edges,
checks registry archives against lock checksums and Git archives against the
pinned Git objects, and retains nested native notice files. The observed historical MIT/Apache slash
declarations are normalized to OR for package metadata while retaining the
original declaration and all full license texts. The standard-library
record includes full copyright/third-party texts and a canonical complete rlib
inventory. Its closure digest identifies that inventory, not an individual rlib.
The explicit Cynic supplemental license matches its published crate's VCS commit.

`collect-native-materials.py` adds the actual production C sources/libraries.
Ubuntu system archives are bound to exact authenticated source-package files,
installed copyright text and the referenced full common-license texts. PCRE2's
Debian source diff and libxcrypt's source-generated notice are checked explicitly.
Pinned Jansson/SQLite/crun/slirp/curl sources include the project/distribution
patch recipes. Native review requires a production CMake build and, on Linux,
the actual linker inventory. Hosted review artifacts contain only these input
JSON files; source downloads and temporary objects stay in the build fixture.

`notices.json` uses [the strict notice input](../notices/README.md). Cargo components
also bind `cargoSource` and `sourceArchive`; a changed Git revision or registry
checksum cannot hide behind an unchanged package version.

`closure.json` records actual non-system inputs. `systemArchive` uses the actual
linker's archive basename and its reviewed SHA-256. `buildArchive`/`buildRoot`
select an archive produced from the hash-checked `sourceMaterial`. `dependencyPin`
must also match the current committed source lock, so a changed upstream pin
cannot reuse old notices. Every actual non-application linker input must be covered; only these
compiled libraries permit `@BUILD_SHA256@`, resolved after the build. The toolchain
record uses `toolchainInventory: true` and its pinned rlib-set digest. Generated
metadata is removed before validating the final install-schema libraryClosure.

`materials.json` normally contains `{path,url,sha256}` records. Optional `files`
select exact full notice members and their destination/checksum from tar/ZIP
sources. SQLite's first header comment is an explicit reviewed selector. Legacy
Debian diffs permit only complete newly added files. Toolchain entries select
`toolchainFile` or `toolchainInventory`; their bytes must match the reviewed hash.
libslirp's `libslirp-gitiles` normalization reuses the pinned verifier and changes
only archive timestamps/order before checking the full normalized tar digest.

Source/relink assets are generated from the current production build, never
fetched from an already-published copy of the same release. A component can select
`correspondingSource: {generated: "corresponding-source", releaseAsset: "..."}`
and, where required, `relinkKit: {generated: "daemon-relink", releaseAsset: "..."}`.
Names expand `@VERSION@`, `@OS@`, `@ARCH@`. The producer builds the binary pair,
constructs the complete source asset from reviewed materials and committed project
source, relinks a separate Linux daemon from its actual objects/archives, validates
ordinary/fd/memfd execution, then resolves final hashes and generates package
notices. These assets become separate final signed subjects alongside packages.
No prior release, external registry account or prehosted own kit is required.

Actual full source/license review, all required host checks and final signed
provenance remain release gates. The validators establish identities and coverage;
they cannot establish legal sufficiency by themselves. See open T012/T015.
