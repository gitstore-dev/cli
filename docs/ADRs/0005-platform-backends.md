# ADR 0005: Platform Backend Validation and Support Claims

**Status**: accepted (platform strategy and bounded evaluation plan).
**Date**: 2026-10-27. **Owner**: CLI platform/distribution maintainers.
**Evidence**: EVD-001, EVD-002, EVD-022 and primary sources below.
**Dependencies**: DEP-012–DEP-015. No local backend is validated by M0.

## Context

Remote catalogue access and local container execution have different platform needs.
Linux kernel container facilities are not available natively on the macOS kernel.
A backend project existing or being open source is not a GitStore distribution contract.

## Decision

Remote Rust gs initially targets Linux/macOS amd64 and arm64 without a daemon.
Linux local execution uses the embedded libcrun backend in a per-user daemon, with
ADR-0004's network/volume requirements. Test both architectures before claiming support.

Evaluate Apple's [container 1.6.1 release](https://github.com/apple/container/releases/tag/1.6.1)
and [Containerization](https://github.com/apple/containerization) integration on supported
Apple-silicon macOS hosts (EXP-005A). The project's
[requirements and installation documentation](https://github.com/apple/container)
describe Apple silicon/macOS requirements and installer setup; do not assume GitStore
can redistribute or integrate that path with no separate user installation. Validate
signed packaging, installation privileges and the profile lifecycle through a versioned
backend boundary. Record the exact Containerization revision used by the experiment.

For unsupported Apple paths or other supported Macs, evaluate a managed Linux VM path
using [Lima v2.2.1](https://github.com/lima-vm/lima/releases/tag/v2.2.1) as the maintained
VM-management candidate (EXP-005B), with its applicable Virtualization.framework/QEMU
backend. GitStore must deliver/control the required components rather than telling
users to install a separate runtime stack. The guest runs the Linux backend and exposes
only the authenticated managed control path. Unvalidated or unavailable virtualization
returns actionable unsupported status, not silent privilege escalation or fallback.

Windows is deferred. A future Linux execution path requires WSL2 or another Linux VM;
this plan does not advertise native Windows runtime execution.

## Reference validation matrix

These are experiment targets, not a supported-platform list. Pin exact OS, kernel,
CPU architecture, backend/dependency versions and host capability configuration in results.

| Target | Initial experiment host | Required boundary |
| --- | --- | --- |
| Remote gs Linux amd64/arm64 | Ubuntu 24.04 LTS reference images on each architecture | API/Git/auth/context/output without daemon or runtime downloads. |
| Remote gs macOS amd64/arm64 | macOS 26 where host/SDK availability permits; publish exact tested hardware | Same remote contract; no local runtime assumed. |
| Linux local amd64/arm64 | Ubuntu 24.04 LTS reference hosts, explicit namespace/mapping/cgroup config | Embedded libcrun, default rootless network/local volumes, no sudo in ordinary operation. |
| Apple local arm64 | Apple silicon, macOS 26, pinned container/framework experiment | Native backend integration/packaging validated across full profile lifecycle. |
| Managed VM macOS amd64/arm64 | Available virtualization-capable Mac hosts, pinned guest kernel/rootfs and Lima backend | Guest Linux backend, authenticated control, host endpoints and retained volumes; report unsupported combinations. |

## Alternatives

- Native crun execution on macOS: rejected; requires Linux execution facilities.
- Require a user-installed Docker/Compose/VM stack: rejected for the default installation
  goal; internal shipped/provisioned components remain possible after validation.
- Advertise all macOS hosts from Apple-source availability: rejected; packaging,
  compatibility, virtualization and privileges need evidence per host/backend.
- Build a VM manager from scratch: rejected before evaluating the maintained candidate.
- Automatically install a privileged fallback: rejected; consent and host requirements
  must be explicit and no-sudo normal operation must remain true.

## Experiments and release gates

| Experiment | Owner / bounded scope | Required evidence |
| --- | --- | --- |
| EXP-005A | Platform/distribution maintainers; Apple container 1.6.1 versus a pinned direct Containerization integration on one supported Apple-silicon reference host | Install only GitStore's evaluated package, disclose any install-time elevation, start as normal user, private image auth, DNS/service discovery, loopback ports, volumes, logs, health, stop/restart and daemon recovery. Measure binary/download/cache footprints separately. Reject the path if it still requires an undisclosed external user installation or weakens profile security. |
| EXP-005B | Platform/distribution maintainers; Lima v2.2.1 managed guest on available amd64/arm64 reference Macs | Pin and verify guest assets, authenticate control, exercise all core-service/profile operations and volume retention, interrupt boot/download/network/stop/restart, verify no writer overlap and actionable unsupported-host behavior. Record backend-specific host privileges and filesystem/locking behavior. |

Before selecting either path, inspect exact dependency/license/security/update inventory
and all upstream release notes at the selected immutable versions. Version references
are dated evaluation baselines, not claims that they are safe or compatible for a release.
DEP-015 stays open until a complete host/backend report passes. If neither candidate
meets the contract, publish remote-only macOS support and explicit local unsupported
behavior rather than claim a seamless installation. No VM or Apple backend was run in M0.
