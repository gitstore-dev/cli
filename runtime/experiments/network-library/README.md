# Native network library probe and selected source correction

Developer-only Linux experiment, not a daemon backend or distribution dependency.
Use an explicitly disposable fixture with no user mounts, agent or credentials,
an ordinary test UID, user/network namespaces and accessible `/dev/net/tun`.
The owned VM needed explicit guest-root preparation of its root-only TUN node to
mode 0666. This is a reference fixture prerequisite, never product elevation or a
silent fallback. The probe refuses UID 0. Its only external DNS lookup is
`example.org` through libc's resolver and libslirp's DNS forwarding.

Verify [the source lock](source-lock.json) before extracting into an owned source
directory `libslirp-4.9.7`. Gitiles regenerates archive member timestamps, so its
historical compressed-byte SHA256 is not a stable download identity. The build's
`fetch-source.py` bounds the archive, normalizes timestamps and entry order, then
verifies the entire normalized tar SHA256 before extraction. Source bytes, modes,
ownership metadata and link targets remain part of that hash. The known original
archive and a fresh download normalize identically; changed content is rejected.
This is build-time source preparation, not a runtime download or bundled helper.
Unpatched instrumented input fails as recorded. The selected development build uses
a separate copy and `apply-source-patch.py`; [patch-lock.json](patch-lock.json)
verifies each original/patched header and [queue-access.patch](queue-access.patch).
Keep the original failing source separate. Example selected preparation:

```sh
cp -a libslirp-4.9.7 libslirp-4.9.7-queueview
python3 apply-source-patch.py libslirp-4.9.7-queueview
meson setup libslirp-queueview-release libslirp-4.9.7-queueview \
  --buildtype=release --default-library=both -Dwerror=true -Dversion_suffix=-gstd-queueview2
ninja -C libslirp-queueview-release
```

Use those selected source/build directories in the compiler commands below.
`queue-layout.c` additionally includes internal headers and needs GLib pkg-config
**cflags** plus the static library/GLib link flags. It verifies original queue
layout and executes real insert/remove on deliberately unaligned nodes. The first
packed-original-type candidate is retained only under `rejected-qlink1`.

Build ordinary libraries, then the probe (from the owned experiments directory):

```sh
cc -std=c17 -O2 -Wall -Wextra -Werror -Ilibslirp-4.9.7-queueview/src -Ilibslirp-queueview-release \
  probe.c libslirp-queueview-release/libslirp.a $(pkg-config --libs glib-2.0) -lresolv -o network-probe
env -i PATH=/nonexistent HOME=/home/gitstorefixture ./network-probe
```

The repository copy is `runtime/experiments/network-library/probe.c`; adjust only
that source path if copying the repository hierarchy. No Git/container/network
executable runs from the probe. `HOME` must name the disposable user's own home.
For shared comparison replace the archive/GLib flags with
`-Llibslirp-queueview-release -lslirp` and set `LD_LIBRARY_PATH` to that owned build directory.
Concurrent probes accept separate seven-byte payload identities (`alpha01`,
`beta002`) while using the same guest IP/service port; each client checks its own
identity. Both children are bounded by deadlines, with parent-death signals.

For instrumentation build the dependency itself with `CC=clang-18`, Meson's
`-Db_sanitize=address,undefined -Db_lundef=false`, then link the caller with
`-fsanitize=address,undefined`. Run with `ASAN_OPTIONS=detect_leaks=1` and
`UBSAN_OPTIONS=halt_on_error=1`. Do not disable alignment checks. Actual failures
and selection status are recorded in [the evidence](../network-library.md).
