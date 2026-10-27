# M0 Publication Contract

M0 implementation publishes the following documents. These are acceptance rules for
that work, not assertions that the final inventory and ADRs already exist.

| Published artifact | Required content |
| --- | --- |
| docs/contracts/README.md | Index, source baseline, review date, evidence levels, support disclaimer. |
| docs/contracts/command-matrix.md | Every PRD command/resource-operation pair; fields from the data model. |
| docs/contracts/dependencies.md | Named gaps, affected commands, owner role, tracking link, milestone and gate. |
| docs/contracts/cli-behavior.md | Shared CLI contract defined by cli.md here; per-command exceptions explicit. |
| docs/contracts/daemon-protocol.md | Canonical wire rules, typed operations, limits, examples and compatibility. |
| docs/contracts/acceptance.md | FR/SC map, six lifecycle walkthroughs, actual review outcomes and open findings. |
| docs/ADRs/0001-daemon-protocol.md | Framing/serialization and binding strategy; reference canonical protocol. |
| docs/ADRs/0002-runtime-ownership.md | Component boundaries, libcrun, durable operations, exclusive writer and recovery. |
| docs/ADRs/0003-runtime-assets-packaging.md | Linking/delivery, license inventory, image tooling evaluation, verified cache/offline flow. |
| docs/ADRs/0004-rootless-network-volumes.md | Rootless host constraints, networking lifecycle, retained local volumes, CSI gate. |
| docs/ADRs/0005-platform-backends.md | Linux support validation, Apple path evaluation, managed-VM fallback, platform claim gates. |

The inventory must include contexts/config validation, every auth subcommand, resource
discovery/explain, each CRUD/watch pair, GraphQL execution, repository sync, export
formats, every service/component serve command, deployment inspection/logs/stop,
migration, and deferred transfer/upload/publication/CRD/CSI proposals. Do not infer
unmentioned operations from a resource kind. Help/version are local client behavior.

Maintain separate coverage for a CLI command, an upstream contract, and validated release
support. No matrix entry may imply that `gs` or `gitstored` already exists.

Final ADRs must choose the protocol and ownership rules, not leave alternatives unnamed.
For packaging/image/backend choices needing runtime evidence, an ADR may accept an
evaluation plan only if it names candidates, criteria, owner, experiment and release gate.
Implementation must identify and review concrete maintained candidates; “evaluate later”
without those fields fails M0 review. No feasibility claim follows from accepting a plan.

Every FR-001–FR-014 and SC-001–SC-004 must map to a published artifact and acceptance
record. Publication passes when coverage is complete and findings have explicit owners
and gates. Upstream prerequisites may remain open; missing M0 protocol decisions,
unclassified operations, and unowned findings may not.
