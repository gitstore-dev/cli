# ADR 0004: Rootless Networking and Retained Local Volumes

**Status**: accepted (baseline design and bounded evaluation plan).
**Date**: 2026-10-27. **Owner**: CLI runtime/storage maintainers.
**Evidence**: EVD-001, EVD-002, EVD-022 and primary references below.
**Dependencies**: DEP-012, DEP-014; ADR-0002 defines resource ownership.

## Implemented backend

The current Linux backend integrates libslirp as an ordinary C dependency, with
owned namespaces/TAP and loopback forwarding. It does not load CNI configurations
or execute CNI plugins. The baseline comparisons below are historical design
inputs, not a claim of CNI support. See the [network selection evidence](../../runtime/build-inputs/libslirp.md).

CNI integration remains a separate adapter and validation task. The
[CNI execution protocol](https://www.cni.dev/docs/spec/#section-2-execution-protocol)
invokes plugin binaries; using a CNI client library does not replace those plugins
or prove rootless compatibility. No CNI executable is currently bundled.

## Context

An embedded OCI executor alone supplies neither application networking nor retained
storage. Rootless execution still depends on host namespaces, UID/GID mapping, cgroup
delegation and compatible filesystem/mount behavior. Volume publishing does not supply
File upload, object storage or Git LFS application protocols.

## Decision

Default Linux profiles run as the invoking user with user-owned state and high loopback
host ports. Validate namespace/mapping/cgroup/mount capabilities before setup; document
unavoidable installation-time host configuration separately. Fail unsupported rather
than invoking sudo, a root daemon or disabling host security controls.

Create an isolated profile service network with explicit API/Git/controller discovery,
DNS and published endpoints. Keep internal gRPC/datastore ports private. Record network
namespace/backend/helper/allocation identities before attaching workloads. On partial
failure, stop owned workload attachments then remove only the operation's new transient
network allocations; preserve shared/retained identities. Restart reconciles actual
attachments before retrying cleanup. If a future backend uses CNI, each ADD must
have a safe owned DEL path; attachment alone does not prove network policy enforcement.

EXP-004A compares concrete rootless network paths: [passt/pasta](https://passt.top/passt/about/)
and [slirp4netns](https://github.com/rootless-containers/slirp4netns), using their official
source snapshots reviewed 2026-10-27 and immutable build pins at experiment time.
Also evaluate [CNI plugins](https://github.com/containernetworking/plugins) within a
rootless namespace. The [CNI contract](https://github.com/containernetworking/cni)
is an attachment interface, not proof those plugins work without host-root changes.
A justified external helper/plugin must use a compatible host installation or a
verified on-demand download with consent; no default executable bundle is selected.
Backend-native networking is allowed if it passes equivalent lifecycle and isolation gates.

Use named local persistent volumes initially. Profile IDs map to owned volume paths;
callers cannot choose arbitrary host mounts. Persist volume identity, owner UID, access
mode and retention independent of container IDs. Stop/replacement retains repository and
configured persistent datastore contents; memdb remains explicitly ephemeral. Backup/
restore and version compatibility are prerequisites for irreversible data migrations.
Only one active Git writer may hold a repository volume, regardless of filesystem sharing.

CSI is an M3 extension: capability-discovered attach/stage/publish and inverse operations,
timeouts, mount propagation, privileges, access modes and recovery require a specific
driver profile. Optional privileged CSI must not become a default-profile prerequisite.
Ceph RBD/CephFS are comparison candidates for external storage, not an automatic cluster
installation task. Rook is Kubernetes-operated guidance, not a standalone runtime dependency.

## Alternatives and consequences

- Host-root bridge/firewall setup: cannot satisfy no-sudo default operation; only an
  explicitly separate privileged profile could evaluate it.
- Assume arbitrary CNI works rootless: rejected; plugin capabilities must be tested.
- Delete volumes during stop or reuse a volume with ambiguous ownership: rejected for
  retention/single-writer safety.
- CSI before local volumes or custom LFS over volume mounts: rejected scope and semantics.
- Backend-native network versus CNI: leave the implementation choice to EXP-004A's measured
  result while accepting the required connectivity/cleanup boundary now.

## Experiments and gates

| Experiment | Scope / owner | Required outcome before release |
| --- | --- | --- |
| EXP-004A | Runtime networking maintainers; pasta, slirp4netns and a selected official CNI rootless profile on Linux amd64/arm64 reference hosts | Run core-service DNS/connectivity and loopback publishing; verify internal ports stay private, distinct deployments isolate names, attachment/start/stop/restart failure cleanup is owned/idempotent, bounds hold and ordinary operation uses no elevation. Record immutable versions, rejected host combinations and comparative footprint. Select a path only if all mandatory checks pass. |
| EXP-004B | Runtime/storage maintainers; local volumes on reference hosts, then one separately selected CSI driver in M3 | Prove durable volume identity, stop/restart/replacement retention, exclusive Git writer under concurrent requests/crashes, unsafe mounts rejected, backup/restore and disk exhaustion behavior. For CSI add attach/stage/publish retry and partial-failure recovery, privilege/access-mode evidence and retain local baseline if it fails. |

DEP-014 remains open. This ADR accepts the architecture/evaluation criteria, not a
blanket network/volume support claim. No networking, volumes or storage cluster were
created during M0. The later source checkpoint below records the tested subset.

## 2026-11-05 source implementation checkpoint

[EVD-024](../contracts/README.md) and the
[validation record](../contributing/validation/local-runtime.md) cover the named
Linux arm64 fixture: ordinary libslirp integration, deployment namespace/TAP,
private service discovery, high loopback forwarding, public core lifecycle and
owned cleanup. The selected path does not require a CNI plugin or external
networking executable. TUN-denied and unavailable host combinations fail explicitly.

Named repository and Scylla volumes retain stable identities across observed
stop/restart. Actual concurrent-writer and manager-loss checks refuse replacement
until original termination is observed. Independent backup/restore validation and
bounded disk-full tests are private native probes; they are not public backup or
restore commands. See [volume evidence](../../runtime/experiments/local-volumes.md)
and [Scylla evidence](../../tests/runtime/scylladb.md).

Other Linux architectures/hosts and distribution packages remain unqualified.
DEP-014 stays open for those broader gates. CSI remains M3, and CNI remains the
separate follow-up below.

## Proposed CNI integration — not implemented

Tracked in [GitHub issue #7](https://github.com/gitstore-dev/cli/issues/7) as a
separate follow-up outside the current M2 scope. It is not a dependency of the
current libslirp implementation or M2 completion.

A first-party CNI plugin backed by libslirp is feasible. The proposal makes
`gitstored` a CNI runtime and exposes the existing library backend through a CNI
plugin entry point. The entry point could reuse the `gitstored` executable in a
separate invocation mode, retaining the library integration without bundling a
third-party networking executable. Entry-point packaging and trust checks still
need design and validation; this is not current support or an approved claim of
interchangeability with arbitrary plugins.

The unit of attachment would be the deployment sandbox namespace: API, Git and
controller already share that namespace. `gitstored` creates and holds the
namespace, records attachment intent, then invokes the selected plugin with a
stable sandbox ID, interface name and namespace reference. It persists the
selected plugin identity, configuration and result for CHECK/DEL and recovery.
Changing operator selection applies to new/recreated attachments; cleanup of an
existing attachment must use its original plugin and configuration.

The libslirp plugin's ADD would configure its TAP/address/routes and start or bind
the persistent packet worker. CNI invocation processes return after setup, while
libslirp packet processing must continue for the network's lifetime. The worker
therefore remains supervised and recoverable under gitstored's ownership model.
Worker registration/permission cannot depend solely on the final CNI JSON reply:
a lost ADD acknowledgment must not create an untracked worker. CHECK inspects the
actual namespace, worker, interface and forwarding state. DEL is retry-safe,
including after failed ADD or namespace loss, and cleans only owned resources.
Implement the commands required by the explicitly selected CNI version, with
truthful capability reporting; do not describe an ADD/DEL-only shim as universally
conformant. See the [CNI specification](https://www.cni.dev/docs/spec/).

Operators could select a compatible alternative plugin or chain through trusted,
operator-owned network configuration. Resolve compatible host plugins first;
missing approved components may be downloaded with consent. Lifecycle requests
must not supply arbitrary plugin executables or host namespaces. Compatibility
must cover rootless privileges, IPAM, service DNS, API/Git endpoint publication,
external agents/control planes and recovery. A CNI plugin's feature set does not
prove that it works on this rootless host or replaces those application contracts.
No privilege escalation or live plugin swap is implied by this proposal.
