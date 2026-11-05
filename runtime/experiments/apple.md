# EXP-005A: Apple library feasibility

The ordinary Swift API can control the selected existing Linux guest. It is not
selected as a public GitStore backend. Clean installation, service provisioning
and host endpoint projection have not been verified. Linux execution remains
independent of this experiment.

## Inputs and ownership

Test host: macOS 26.6.1 (25G76), arm64, invoking UID 502, Swift 6.4,
Command Line Tools SDK 26.5. The host is an existing development Mac, **not a clean
disposable Mac**. Only the explicitly selected guest and isolated test state are
disposable. No host service was uninstalled or privileged installer tested.

Apple container 1.6.1 is pinned at
`aa8d76b44a834eee3ebf6a0bc577e376f65f6361`; Containerization 0.53.0 at
`f79e412d7c31b00c65c8a6f80dd0b0bb55f6a4f7`. The
[Swift package](apple/library-probe/Package.swift), resolved dependency lock and
[inventory](apple/dependency-inventory.json) contain the actual inputs. Thirty-four
resolved packages have recorded license-file hashes; this is an inventory, not a
completed legal/security review.

The owned running guest is `gitstore-m2-linux-a1a13c`, Ubuntu 24.04 arm64, 2 CPU,
4 GiB, guest UID/GID 11000. Its image is
`sha256:0bf2f725e3ee269a2e804c3426e38270e43eaa41960add5ad0ef12e072181178`.
The observed configuration has no host mounts, SSH forwarding, published ports or
published sockets. Guest tests use synthetic credentials and private temporary
configuration, not host user configuration or credential stores.

## Observations

`AppleProbe` imports `ContainerAPIClient` and `ContainerResource` as ordinary
dependencies. Typed guest lookup and a fixed guest `id -u` process verify the
selected fixture and UID 11000. Fixed guest test modes exercise:

- The real maintained image library against the synthetic private registry.
- Offline verified images, the public core start/readiness/stop path and logs
  inside the guest.
- Killing the guest daemon, reopening owned resources, public stop and retained
  repository restart without replacing a live writer.

These are guest Linux backend results transported through the host library.
They do not establish host loopback reachability, native macOS filesystem locks,
or a production host/guest daemon protocol. The host XPC server checks the audit
token's effective UID against the service UID in the pinned
`Sources/ContainerXPC/XPCServer.swift`; same-UID access passed. Cross-UID rejection
on macOS was not exercised. Guest peer authorization has its separate Linux tests.

`DirectGuard` constructs `Containerization.LinuxContainer` with the selected
seccomp policy and no guest OCI runtime path. The actual constructor rejects it:
the provider's default vmexec path ignores that policy. The guard confirms no
guest boot occurred. This is a limitation of this provider adapter, not an OCI
requirement to implement a particular executable interface. Do not weaken the
policy or infer OCI compatibility from successful VM boot.

The earlier stripped library probes measured 33,504,944 bytes for AppleProbe and
29,850,840 bytes for DirectGuard. Those are experimental checkpoints, not current
core artifact sizes or shipping assets. Swift dependency cost argues against
linking this experimental path into the small core executables by default.

## Unavailable combinations and decision

No clean disposable Mac was available for installer privilege, missing-service,
missing-kernel/rootfs delivery, uninstallation or interrupted provisioning tests.
No amd64 Mac or second macOS version was tested. Host loopback projection and a
GitStore-authenticated host/guest control protocol are not implemented. Existing
Apple service installation cannot prove a self-contained GitStore installation.

Keep local macOS execution unsupported. A future selected integration must deliver
approved assets on demand, authenticate control, project endpoints, preserve
retained data across host/guest failure and pass clean installation checks. There
is no automatic privileged fallback or default container executable bundle.

Reproduction is in [the probe guide](apple/README.md); exact final binary identity
and bounded runner outcomes are in [the evidence receipt](apple/evidence.json).
