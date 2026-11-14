# Contributor documentation

User-facing instructions live in the [user guides](../README.md). Keep those
focused on supported commands, setup, expected results and recovery actions.
Build dependencies, implementation details and test evidence belong here or in
the linked architecture/contract references.

| Topic | Reference |
| --- | --- |
| Build the CLI or native runtime | [Building](building.md) |
| Current stripped executable sizes | [Binary sizes](binary-sizes.md) |
| Contribution policy and required checks | [CONTRIBUTING.md](../../CONTRIBUTING.md) |
| Public command and daemon contracts | [Contract index](../contracts/README.md) |
| Ownership, packaging, networking and platform decisions | [Architecture decisions](../ADRs/) |
| Product scope and milestone criteria | [Product requirements](../prd.md) |
| Remote CLI behavior checks | [Remote validation guide](../../specs/002-m1-remote-cli/quickstart.md) |
| Runtime behavior checks | [Runtime validation guide](../../specs/003-m2-daemon-foundation/quickstart.md) |
| Runtime implementation and acceptance traceability | [Runtime tasks](../../specs/003-m2-daemon-foundation/tasks.md) |
| Published release identities and qualified hosts | [Release qualification](validation/release-qualification.md) |
| Qualify and publish the next release | [Release runbook](releases.md) |

## Validation records

[Remote CLI](validation/remote-cli.md) records scoped implementation checks.
[Local runtime](validation/local-runtime.md) indexes durable source acceptance,
material failures and requirement traceability. Release qualification binds
published support to exact source/artifact hashes and host receipts. Historical
measurements apply to their recorded builds; immutable source links retain earlier
checkpoint details without requiring them for current setup.

When behavior changes, update the user guide and relevant contract first. Record
commands, environment, outcomes and material limitations in contributor evidence;
do not make users reconstruct supported behavior from a progress journal. A local
or simulated pass does not establish published-release or cross-platform support.
