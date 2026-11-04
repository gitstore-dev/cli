# Network library selection — development checkpoint

T034 selects libslirp 4.9.7 with the hash-bound private queue-access patch,
versioned `4.9.7-gstd-queueview2`, as an ordinary C code dependency. No networking
executable is bundled or selected. The [native probe](network-library/README.md)
passes the recorded topology with normal and instrumented static/shared libraries.
Unpatched input remains rejected; runtime ownership/attachment integration is T041.

## Actual API and topology results

The ordinary C code dependency is libslirp 4.9.7, pinned by immutable commit and
archive hash in [the source lock](network-library/source-lock.json). Inputs come
from the upstream [QEMU mirror](https://qemu.googlesource.com/libslirp/). Both the
tag and commit archive contain identical member names/types/modes/link targets and
file SHA256 values; their gzip bytes differ. The original source copy is preserved;
the selected private patch is applied only to a separate hash-checked build copy.

Runs use the authorized Ubuntu 24.04 arm64 disposable guest, GCC 13.3.0, and test
UID/GID 11000. Guest-root fixture preparation made the otherwise root-only TUN
device accessible; product preflight must report an inaccessible device instead
of changing its permissions or elevating. No host kernel controls were changed.

An ordinary child creates a single-ID user namespace and private network
namespace, configures its owned TAP/interface/route and passes the TAP descriptor
over a private Unix socket. The libslirp owner stays in the original network
namespace and forwards frames through maintained library APIs. The child service
listens on private 10.0.2.15:8080. A libslirp host forward allocates an OS-selected
high port bound only to 127.0.0.1; the probe observes the actual listener descriptor
instead of reserving and releasing a port. IPv6, DHCP and emulation are disabled;
outbound host-loopback access is disabled. Production profile hosts/DNS naming and
container namespace attachment are not yet integrated.

Static/shared normal callers pass private namespace identity checks, libc DNS
resolution of example.org through 10.0.2.3, loopback forwarding and exact payload
identity, observed child exit, removal of the owned listener and timer cleanup.
Two concurrent instances pass with the same private IP/service port and different
payload identities, without crossed traffic. PATH is `/nonexistent`; no network
helper or privileged executable is called. This is development probe evidence,
not GitStore service readiness, throughput, recovery or host/provider support.

| Initial unpatched normal arm64 comparison | Bytes | External dependency scope |
| --- | ---: | --- |
| Static-libslirp caller, stripped | 133,288 | GLib, PCRE2, platform libc/m/loader |
| Shared-libslirp caller, stripped | 67,504 | Same plus libslirp |
| Release libslirp shared object | 228,768 | Unstripped library build |
| Host GLib shared object | 1,445,440 | Resolved actual Ubuntu SDK runtime |
| Host PCRE2 shared object | 592,328 | Resolved actual Ubuntu SDK runtime |

These are probe measurements, not an incremental/final daemon package size.
Static libslirp linking does not include GLib/PCRE2 costs in the core executable.

## Failing instrumented run

Clang 18.1.3 ASan/UBSan instruments both libslirp and the C caller. Both debug and
optimized **release** library configurations fail with alignment checking enabled:

```text
src/tcp_input.c:263:56: runtime error: member access within misaligned address
... for type 'struct qlink', which requires 8 byte alignment
```

The failure occurs during forwarded TCP processing. The pointer ends in `...7f4`
and is not 8-byte aligned. `UBSAN_OPTIONS=halt_on_error=1` stops the owner before
the full scenario can pass. No sanitizer suppression, alignment exemption,
patched network stack or success claim is substituted. The follow-on client
reports an incomplete reply after the owner exits. The child is bounded and has
a parent-death signal; this is not a successful cleanup/ASan run.

## Selected alignment correction and actual checks

The first candidate packed the original `qlink` type. It passed the simple native
probe and upstream tests, but lowered alignment of containing reassembly types and
produced new GCC packed-pointer conversion warnings. It also did not address the
generic queue helper's separate access type. That candidate is **not selected**;
its reproducible inputs remain under [rejected-qlink1](network-library/rejected-qlink1/).

The [selected patch](network-library/queue-access.patch) instead leaves the original
`qlink`, `ipq`, `ipas` and containing structure declarations unchanged. It adds a
packed packet-access view, uses that view in the two TCP overlay conversion macros,
and packs the existing generic queue helper's access view. Queue insertion/removal,
packet processing and protocol algorithms remain unchanged. The public API is
still opaque `Slirp*`. This is a narrowly maintained source correction, not a
sanitizer exemption or a new network stack. No upstream acceptance is claimed.

[Patch lock](network-library/patch-lock.json) records original/patched SHA256 for
all three headers and the exact unified patch hash. The application script verifies
every input/output before writing any file and refuses unknown/already-patched
source. `-gstd-queueview2` explicitly distinguishes this build from pristine 4.9.7.

Actual Ubuntu 24.04 arm64 checks:

- GCC 13.3.0 release library build with Meson's `werror=true` passes without the
  first candidate's warnings. Normal static/shared native probes and two concurrent
  separate payload identities pass DNS, namespace isolation, high loopback ports,
  observed exits and listener/timer cleanup. All five upstream tests pass.
- Clang 18.1.3 instruments the dependency and callers with ASan/UBSan. Static and
  shared native probes, concurrent static probes and all five upstream tests pass
  with leak checking and `halt_on_error=1`. Alignment checking remains enabled.
- [Queue-layout test](network-library/queue-layout.c) asserts original queue size,
  alignment and member offsets, then calls the actual library insert/remove helpers
  on deliberately unaligned storage with two nodes under ASan/UBSan. It passes.

| Selected normal arm64 artifact | Bytes | Scope |
| --- | ---: | --- |
| Static-libslirp probe, stripped | 198,824 | Library plus harness; GLib/PCRE2/platform libraries remain external |
| Shared-libslirp probe, stripped | 67,504 | Harness; libslirp additionally external |
| Release libslirp shared object | 228,720 | Unstripped comparison library |

These are actual complete artifact sizes, not an incremental/final daemon estimate.
The source/header-only correction and external footprint are recorded in the
[development dependency lock](../dependencies.lock.json). Prefer normal static
libslirp code integration for the development adapter; GLib/PCRE2 delivery and final
combined size still require packaging checks. No pasta/slirp4netns/CNI invocation
is needed for this selected topology.

## Alternatives and remaining integration

- The selected library plus precise source patch passes the reference topology.
  Revisit/remove the patch when an equivalent maintained upstream correction is
  available; do not silently substitute pristine/unverified versions.
- A slirp4netns executable still delegates to libslirp and does not by itself
  resolve this observed alignment failure. It also adds executable supervision and
  delivery costs, so it is not an automatic fallback.
- [pasta](https://passt.top/builds/latest/web/passt.1.html) is a maintained native
  namespace connector with different forwarding internals and an existing-netns
  mode. It is an executable capability, not a stable library API. If library paths
  remain unsuitable, evaluate a compatible trusted host binary or an explicitly
  consented, source/version/hash-bound runtime download. It would stay outside
  default core artifacts; no pasta execution/support result is established yet.
- CNI is a plugin execution protocol, not a library substitute. A generic CNI
  chain introduces extra executable/configuration/IPAM assumptions and is not
  justified for this per-user single-deployment namespace topology.

T041 must implement owned namespaces/attachments, profile hosts mapping, supervision
and reverse cleanup using these inputs. Production recovery, service readiness,
throughput, amd64 and host/provider support remain unverified. No host/on-demand
fallback is selected silently after side effects.
