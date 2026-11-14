# Native daemon adapter evidence

Recorded 2026-11-04 on the disposable `gitstore-m2-linux-a1a13c` Linux/arm64
fixture, UID 11000. This verifies private daemon library plumbing with synthetic
payloads. It does not verify an actual GitStore service, image authenticity,
public lifecycle dispatch, network/volume recovery, amd64 or a published package.

`daemon/cmake/Libcrun.cmake` builds the pinned 1.30.1 archive with its C library
and systemd/cap/seccomp support. The crun executable, dynamic plugin loading,
BPF, CRIU and language/WASM engines are disabled. No external runtime executable
is invoked. Native JSON-C headers stay in `native_abi.c`; private static Jansson
symbols have hidden visibility to prevent ELF interposition. The same-process
adapter probe exercises both libraries; dynamic exports contain no Jansson
`json_object_get` definition.

The internal factory fixes core commands, config arguments, mount destinations,
UID/GID mapping, no-new-privileges, empty capabilities, native seccomp policy,
read-only root/config files, private tmpfs and resource limits. Source files are
borrowed owner-only read descriptors with one link, bounded size and distinct
identities; repository data is a separate private directory role. The caller
must already hold verified private-image ownership and canonical volume leases.
This is not authorization to mount a caller-selected host path. API-only secrets
belong in its private overlay; configuration content generation and credential
ownership remain bootstrap/dispatch work.

The asynchronous launcher requires a single-threaded control parent/subreaper and
normal SIGCHLD handling. It moves only explicit role/reply descriptors into a
worker, closes unrelated descriptors, clears the environment, uses canonical
owner session-bus settings, disables core files and bounds files/descriptors.
Rootless mapping needs ordinary procfs dumpability: the launcher refuses a hardened
parent rather than changing that setting. Credential/image workers retain their
separate stricter policy. Worker cancellation/deadline/lost replies preserve
uncertainty and do not claim workload termination. The parent remains the only
SQLite owner. Native init `stopped` alone does not prove an empty cgroup or permit
releasing a repository writer.

The selected lane requires non-root Linux >=6.8, pidfd APIs, cgroup v2, delegated
cpu/memory/pids controllers and an owner session bus. Missing prerequisites fail
before native CREATE; there is no elevation or alternate runtime fallback.

From the selected fixture's repository directory, run the explicit developer
probe, never against an existing deployment:

```sh
python3 tests/runtime/libcrun/run.py \
  build/daemon-native/native_adapter_probe \
  build/daemon-native/native_registered_payload \
  --delegated --seccomp runtime/profiles/seccomp/arm64.json --registered
```

Both normal and lost-START-acknowledgement scenarios pass. The registered Git
command runs a **synthetic static payload**, with no test-fixture runtime-policy
bypass: it observes read-only root/config, private writable tmpfs, a writable
repository mount, absent sentinel FD, no-new-privileges, zero effective caps and
clone3 ENOSYS under seccomp. Ordinary workload limits are 1 TiB virtual address
space and 1024 descriptors; its physical memory cgroup limit is 512 MiB. The
ordinary native worker has a 1 GiB soft address limit. The parent rejects a stale
PID/start stamp without signalling, interrupts/reaps an observation worker without
consuming its cancellation byte, then observes/stops the surviving workload and
collects its real exit 0 through the PID/start-time collector before native delete.

The same scenarios pass with Clang 18 ASan/UBSan in the caller and the exact static
libcrun build. Use `ASAN_OPTIONS=detect_leaks=1` and `UBSAN_OPTIONS=halt_on_error=1`.
The deliberate crash failpoint uses SIGKILL so ASan keeps its actual fault
handler enabled. Select the matching build's `native_registered_payload`; its
static code checks the instrumented or ordinary address-limit expectations.
ASan reserves many TiB before payload exec, so this instrumented build explicitly
omits worker/workload RLIMIT_AS. Ordinary runs prove those address bounds;
physical cgroup, file, descriptor and core bounds remain in the instrumented lane.
Initial diagnostic failures are retained as failures, not passing measurements.
Successful roots are removed only after observed exit/delete and expected markers;
failed roots remain for explicit owned-resource reconciliation.
