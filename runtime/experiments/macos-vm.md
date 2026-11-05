# EXP-005B: Managed Linux guest feasibility

An existing owned Linux guest can run the native embedded-libcrun backend through
a maintained host library. This bounded experiment does not establish a managed
VM product backend or support macOS local execution.

The [Apple experiment](apple.md) records immutable package/guest inputs and the
actual private-image, offline startup and manager-loss recovery results. Its
`ContainerAPIClient.createProcess` path uses the installed same-user XPC service,
explicit guest UID/GID 11000, fixed commands and observed process exits. It does
not shell out to the container executable, enable SSH, or mount the host home.
All workload and retained-writer safety remains in the Linux daemon.

| Boundary | Result |
| --- | --- |
| Existing guest identity and same-user host control | Verified on the selected arm64 Mac; guest image digest and UID checked. |
| Private image auth, offline core readiness/logs/stop | Passed inside the owned Linux guest through the ordinary host library. |
| Manager loss, owned cleanup, retained repository restart | Passed inside the guest; this does not test host VM death or disk loss. |
| Missing VM service, kernel/rootfs acquisition and consent | Not run; no clean disposable Mac fixture. Existing service/assets were prerequisites. |
| Authenticated public GitStore host/guest protocol | Not implemented; SDK same-user control is not that protocol. |
| Host endpoint projection and host filesystem semantics | Not implemented/verified; guest ports were not published. |
| Host/VM boot, download, network and stop interruption | Not run as a managed-host lifecycle. Guest daemon interruption is narrower evidence. |
| Lima 2.2.1 on amd64/arm64 Macs | Not run; no isolated installed Lima fixture or amd64 Mac selected. No tool downloaded or bundled. |

The original Lima comparison remains unqualified. Reusing the installed Apple
library offered a bounded test without creating a VM manager or installing another
host stack. Neither path meets the complete delivery/control contract yet. Missing
or incompatible macOS execution must remain actionable unsupported behavior; it
must not silently invoke sudo, Docker or a separately installed VM tool.

Selecting a production path still requires a clean disposable host, verified and
consented guest assets, authenticated control, endpoint projection and the complete
host interruption matrix. DEP-015 remains open. The experimental Swift binaries
are separate from the core `gs`/`gitstored` size measurements.
