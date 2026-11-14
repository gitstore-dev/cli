# Native dependency build inputs

`libcrun/source-lock.json` pins the ordinary libcrun archive consumed by
`daemon/cmake/Libcrun.cmake`. `libslirp/` supplies the pinned archive, verified
fetcher and queue-access patch consumed by `daemon/cmake/Slirp.cmake` and the
release source-material collectors. Source versions, digests and patch bytes
are independent of these repository locations.

The libslirp source checks and native probe remain beside the verified build
inputs because CMake runs them against those inputs. `rejected-qlink1/` preserves
the failed ABI alternative and its reproduction inputs; it is never applied by
the production build. Runtime/native helpers live under `tests/runtime`, and
maintainer provenance negative controls under `tests/packaging/cli-provenance`.
