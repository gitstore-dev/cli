# Binary sizes

Measured on **2026-11-05**, Linux arm64. These are stripped development
executables, not published releases. The hashes pin the persistent runtime,
standalone migration, bounded restart supervision and public result projection checkpoint. Published-binary optimization is deferred
to the user's follow-up; the original 5 MiB target is a reporting reference.

| Executable | Accepted baseline bytes | Current bytes | Current MiB |
| --- | ---: | ---: | ---: |
| `gs` | 5,590,568 | 5,590,568 | 5.33 |
| `gitstored` | 5,723,600 | 5,920,432 | 5.65 |

The current daemon includes persistent Scylla startup, standalone migration and
observed restart cleanup plus bounded whole-deployment supervision. It was built with test hooks disabled and strict
production compiler warnings enabled.
No size optimization or third-party executable bundle was added. Workload
images, shared libraries, caches, retained volumes and memory are separate costs.

| Current stripped executable | SHA-256 |
| --- | --- |
| `gs` | `8df92a1e12eac89defdeb3c9a22663840b1d0e552b2711f9f120f0498b49d45b` |
| `gitstored` | `d8d9ce9a4473d2321b05fd4f4235dbdd74ce8a287a79a31067eda16c74c7f368` |

The [exact measurement record](../../runtime/packaging/linux-arm64-20261105-restart.json)
also preserves source-binary identities, the unpublished package's digest/entries,
and every unique host shared-library digest. The compressed package is
**5,297,439 bytes**; its unique host-library closure is
**25,611,792 bytes**, reused from this fixture rather than bundled.
The package contains only `gs`, `gitstored` and essential candidate metadata.
These quantities are not total installed OS cost or live-memory measurements.

This artifact-only measurement does not resample live memory or cold/warm
readiness. The runtime samples below retain their own historical fixture scope;
they are not measurements of these new hashes. Current public Scylla lifecycle
validation includes startup, stop and migration in one test, so its overall
duration is not a startup benchmark. Private execution copies are additional
state costs and currently need explicit audited cleanup in disposable testing.

## Reproduction

Build `gs` with Rust 1.96.0 and the workspace release profile (`opt-level=s`, LTO,
one codegen unit, stripped symbols). Build the Linux daemon with `MinSizeRel`,
section garbage collection, image SDK/libcrun/libslirp/bootstrap enabled and
`GITSTORED_TESTING=OFF`. Then select the actual outputs explicitly:

```sh
scripts/measure-runtime-size.sh \
  --gs /absolute/build/release/gs \
  --daemon /absolute/build/daemon-minimal/gitstored \
  --output-directory /absolute/new/measurement-directory
```

The runner requires Linux GNU strip, copies the own binaries before stripping,
refuses an existing output directory, hashes the copied inputs, records the
shared-library closure and builds a local unpublished package. It never modifies
installed/running binaries or downloads tools. Optional `--runtime-costs` reads
only selected fields from an existing protected isolated test sample; it does
not manufacture a benchmark from artifact sizes.

## Separate runtime footprint

The earlier core acceptance fixture measured 84,691,615 verified compressed
image payload bytes across API, Git and controller. SDK transfer counts exclude
HTTP/TLS framing; offline import causes no registry transfer. Its logical asset
cache was 258,759,008 bytes; total logical state after two starts was 776,611,941
bytes, including retained private execution copies. Those are fixture-specific
logical bytes, not a current complete distribution or peak-disk guarantee.

Historical registry cold readiness was 136.208 seconds; another offline first
readiness was 42.353 seconds. The sampled manager RSS/high-water RSS was
13,920 kB, excluding workloads and provisioning-worker peaks. These historical
runtime samples belong to their separately recorded candidate builds in
[packaging evidence](../../runtime/packaging/README.md); they are not a fresh
benchmark of the executable pair above. Matched cold/warm readiness and
whole-workload/worker peak-memory were not resampled in the artifact-only run;
no current performance improvement or peak-memory result is claimed.

The optional Scylla experiment separately acquired 273,983,661 compressed bytes,
619,594,752 merged bytes and an approximately 870 MiB retained stage. Its 2 GiB
physical cgroup ceiling is a limit, not measured consumption; its 64 TiB virtual
address ceiling accommodates a 32 TiB address reservation, not physical RAM.
See [Scylla verification](../../tests/runtime/scylladb.md).

The [older package inventory](../../runtime/packaging/linux-arm64-candidate.json)
is preserved as historical evidence and does not describe the current pair.
