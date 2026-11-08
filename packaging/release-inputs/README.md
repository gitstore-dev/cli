# Reviewed per-platform inputs

Production release jobs require `OS-ARCH/{notices,closure,materials}.json` for
linux-x64, linux-arm64, darwin-x64 and darwin-arm64. These reviews are not complete;
the producer fails before building when they are missing. Do not substitute empty
manifests or synthetic test notices to make a release job pass.

`notices.json` uses [the strict notice input](../notices/README.md).
`closure.json` is the actual reviewed install-schema libraryClosure array.
`materials.json` is an array of `{path,url,sha256}` records: normalized relative
paths, immutable HTTPS locations and full SHA-256 for every full license text,
corresponding-source archive and relink kit referenced by the notices. Files are
downloaded into a new private input directory and checked before use. Source and
relink assets are separate final signed subjects, never replaced by source links.

Review the real linker inputs and applicable full license/copyright text. Static
LGPL kits need the actual corresponding patched source, relinkable application
objects and instructions sufficient for the applicable license. The manifest
validator cannot establish legal sufficiency. See open T012/T015.
