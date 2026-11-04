# EXP-003A: libcrun C library/process boundary

**Recorded:** 2026-11-03. **Scope:** actual tiny owned namespace workloads on the
[disposable Linux arm64 fixture](linux-fixture/evidence.json), not a runnable
GitStore profile, production adapter/network integration, amd64, hosted CI or release support.
A later delegated-cgroup checkpoint is separately scoped below.

[Source lock](libcrun/source-lock.json) pins libcrun 1.30.1, revision
`079ff6a7a16d029460af882f9ea44674e6a510b6`, to the official release archive digest.
The source archive's SHA-256 matches the digest supplied by GitHub release metadata.
No upstream checkout is modified. Both `.a` and `.so` are built with
`--enable-shared --enable-static --disable-crun --disable-dynload-libcrun`.
The crun executable is neither built, installed nor invoked.

## Actual checks

[Probe](libcrun/probe.c), [static tiny payload](libcrun/payload.c),
[runner](libcrun/run.py) and [link recipe](libcrun/build.sh) use library APIs for
load/free, create/start, recorded PID plus process-start-time state, SIGTERM and
delete. Each run has private owned root/state/output directories and a single-UID
rootless mapping. The payload has no networking or user data. Library calls use
short-lived forked workers from one single-threaded parent. Only a bounded reply
struct crosses private CLOEXEC pipes; PATH is `/nonexistent` during execution.

Both shared and static callers pass these two real scenarios:

- Normal create/start; creator/starter exit while payload remains running;
  SIGTERM reaches the payload, which records orderly termination; state observes
  stopped before non-forced deletion.
- Intentional worker SIGSEGV immediately after successful start and before its
  reply. The parent detects the signal, observes the actual surviving payload,
  then signals, observes termination and deletes only that owned runtime identity.

The parent uses `PR_SET_CHILD_SUBREAPER` and collects the actual payload exit code
0 through `waitpid`, including after the lost-start-ack worker crash. An additional
intentional worker crash leaves the running payload observable. A deliberately
non-CLOEXEC sentinel descriptor in the parent must not reach the payload; its
`/proc/self/fd` check runs before creating the ready marker. The expected native
missing-HOME warning exercises a real libcrun callback, verifies its argument is
the still-live stderr FILE, and copies only a bounded message during the call.
Container/config allocations, error values and status members use the paired
upstream frees; no borrowed callback string is retained.

A separate Clang 18.1.3 build instruments both libcrun/libocispec and the C caller
with ASan/UBSan. Both scenarios pass with leak checking enabled and UBSan set to
halt. `ASAN_OPTIONS=handle_segv=0:detect_leaks=1` leaves the deliberate SIGSEGV
visible as an actual signal instead of converting this test fault into an ASan
report. This setting is for the native fault experiment, not the normal daemon
sanitizer suite, which also passes all 14 checks with normal signal handling.

## Footprint comparison

GCC 13.3.0 / Ubuntu 24.04 arm64, measured on isolated stripped copies:

| Artifact | Bytes | Interpretation |
| --- | ---: | --- |
| C caller with static libcrun | 659,648 | libcrun/libocispec included; system dependencies below remain shared |
| C caller with shared libcrun | 67,496 | excludes required libcrun shared object |
| Required libcrun shared object | 859,984 | additional to the shared caller and system libraries |

The source build's unstripped archive is 8,256,384 bytes and shared object is
4,044,592 bytes; archive/debug bytes are not executable footprint. The actual
normal build links libsystemd, libseccomp, libcap, json-c, libm and libc. Systemd
also pulls libgcrypt, libgpg-error, lz4, lzma and zstd plus the ELF loader. These
costs are mandatory for this exact build and are not erased by static libcrun.
The JSON-C build prerequisite is 0.17 (Ubuntu `0.17-1build1`); it does not replace
Jansson's strict wire parser. Product dependency locking/feature minimization and
complete package/RSS measurements remain separate tasks. libcrun headers identify
LGPL-2.1-or-later; packaging must retain the corresponding source/license/relink
materials required by the selected distribution policy.

## Decision and limits

Adopt the proven single-threaded control owner plus forked native execution worker
boundary for development integration. The control parent retains SQLite, scheduling,
cancellation and canonical ownership; it never enters the container namespaces or
runs fork-sensitive runtime calls from an unrestricted thread pool. Subreaper/exit
routing and bounded private replies are required. Use the same daemon executable
with internal child entry points, not a crun helper or public arbitrary-exec method.
A worker failure is not workload termination or permission to release a writer.

Static libcrun is the initial development link candidate: this measured caller is
smaller than the shared caller plus mandatory `.so`, and it avoids an optional
library loader. This is not a finalized redistributable dependency inventory.
Re-evaluate complete transitive/library/package costs during packaging.

The initial comparison probes explicitly set `force_no_cgroup` for a **namespace-only** process/API
comparison. They do not qualify a default profile or authorize disabling a missing
cgroup/network/security prerequisite. Delegated cgroups, retained volumes, actual
images/readiness, restart reconciliation and safe cancellation under a saturated
manager still require their implementation/acceptance tasks. The first fixture
attempt omitted `/dev` and failed create; its private root/state was retained
rather than pretending cleanup/termination. Normal successful roots are deleted
only after observed stop/delete and the payload lifecycle assertion.


## Delegated resource/security checkpoint for T040

The owned fixture now has a real test-user systemd session, canonical private
runtime directory/session bus and delegated cpu/memory/pids controllers. Preparing
that disposable guest used its root account to enable lingering/start the user
manager; product startup never performs that preparation or elevates privileges.
Initially those prerequisites were absent. Their absence must remain an explicit
unmet host condition in the product.

Compiling the same probe with `-DGSTD_PROBE_CGROUPS=1` selects libcrun's systemd
cgroup manager and leaves `force_no_cgroup` false. `run.py --delegated` applies
128 MiB memory, 64 tasks and 50% of one CPU; the parent reads the actual owned
cgroup's `memory.max`, `pids.max` and `cpu.max`. The payload independently checks
NoNewPrivs=1, zero effective capabilities, Seccomp=2 and an explicitly denied
getpriority syscall. This is a small test filter, not a finalized production
service syscall policy.

Both ordinary static linking and direct-ELF Clang 18 ASan/UBSan runs pass normal
and lost-start-ack crash scenarios, including actual payload exit collection and
owned deletion. Sanitizers instrument the caller and pinned native library;
intentional SIGSEGV handling uses the earlier documented option. The shared
sanitizer comparison selects only the exact experimental library directory; no
product loader search or executable fallback is introduced. The development
libtool wrapper was bypassed for the final direct-ELF run under PATH=/nonexistent.

[Exact scoped evidence](libcrun/delegated-evidence.json) records the values and
limits. No GitStore service was executed and T040's production adapter is still
pending. This extends the process-model evidence; it does not close the runtime,
network, service-policy, recovery or release gates.
