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
| Remaining runtime implementation work | [Runtime tasks](../../specs/003-m2-daemon-foundation/tasks.md) |

## Validation records

[Remote CLI](validation/remote-cli.md) and
[local runtime](validation/local-runtime.md) contain historical implementation
checkpoints and environment-specific test results. They are archived evidence,
not user guides or a substitute for current command documentation. Earlier binary
measurements and support statements apply to their recorded builds.

When behavior changes, update the user guide and relevant contract first. Record
commands, environment, outcomes and material limitations in contributor evidence;
do not make users reconstruct supported behavior from a progress journal. A local
or simulated pass does not establish published-release or cross-platform support.
