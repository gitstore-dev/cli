# ADR 0004: Rootless Networking and Retained Local Volumes

**Status**: accepted (baseline design and bounded evaluation plan).
**Date**: 2026-10-27. **Owner**: CLI runtime/storage maintainers.
**Evidence**: EVD-001, EVD-002, EVD-022 and primary references below.
**Dependencies**: DEP-012, DEP-014; ADR-0002 defines resource ownership.

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
attachments before retrying cleanup. A CNI ADD must have a safe owned DEL path; attachment
alone does not prove network policy enforcement.

EXP-004A compares concrete rootless network paths: [passt/pasta](https://passt.top/passt/about/)
and [slirp4netns](https://github.com/rootless-containers/slirp4netns), using their official
source snapshots reviewed 2026-10-27 and immutable build pins at experiment time.
Also evaluate [CNI plugins](https://github.com/containernetworking/plugins) within a
rootless namespace. The [CNI contract](https://github.com/containernetworking/cni)
is an attachment interface, not proof those plugins work without host-root changes.
Any selected helper/plugin is GitStore-delivered; no end-user separate install.
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
tested network/volume implementation. No networking, volumes or storage cluster were
created during M0. Tested host/backend combinations must be published before support.
