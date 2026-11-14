# Native execution policy candidate

The exact upstream [Moby profile](https://github.com/moby/profiles/blob/4b8214fc4971e06172938a1f1554ccff6647ee80/seccomp/default.json)
and Apache-2.0 license are pinned by `source-lock.json`. Neither the repository root
nor its seccomp directory contains a NOTICE file. This is policy data, not a bundled
executable or a new syscall-filter engine.

`normalize.py` resolves upstream includes/excludes for zero capabilities, the native
amd64/arm64 architecture and minimum Linux 6.8. It deliberately excludes compatibility
architectures and rejects unknown predicates. Generated OCI-native JSON has no
unresolved Docker predicates; libcrun/libseccomp compile and enforce the filter.
The specialization follows the pinned upstream `seccomp/seccomp_linux.go` semantics.

```sh
python3 runtime/profiles/seccomp/normalize.py --check
python3 -m unittest discover -s runtime/profiles/seccomp -p 'test_*.py'
```

Native control builds verify the source/license and generated data, then compile
only their native policy into the daemon core library. Distribution users need no Python or
runtime policy-file selection. Linking this data does not enable execution.

The tiny native arm64 payload passes actual delegated libcrun create/start/state/
TERM/exit/delete and lost-start-ack cases, including Clang 18 ASan/UBSan caller/native
library instrumentation. The fixture initially has Seccomp=0; the payload observes
Seccomp=2, no-new-privileges, zero capabilities and clone3's explicit ENOSYS result.
The older fixture libseccomp does not recognize nine newer/other-architecture allow
names; those remain denied by default. No filter is weakened or alternate runtime
selected. Actual API/Git/controller compatibility and amd64 native execution remain
unverified. See [scoped evidence](../../../tests/runtime/libcrun/delegated-evidence.json).

The private daemon adapter and registered synthetic Git launch now pass the
[adapter scenarios](../../../tests/runtime/libcrun/adapter.md). Actual GitStore service
compatibility, network/volume ownership wiring and public dispatch remain pending;
these checks do not establish runtime support or release readiness.
