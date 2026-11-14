# Local runtime validation reference

For operating instructions, use [local deployments](../../local-runtime.md),
[configuration](../../local-runtime-configuration.md) and
[installation and removal](../../installation.md). Contributor commands are in
[building](../building.md) and the [runtime validation guide](../../../specs/003-m2-daemon-foundation/quickstart.md).

## Current evidence and support

The [release qualification reference](release-qualification.md) and committed
[alpha.6 record](alpha6-qualification-review.json) identify the exact published
source, artifacts, host receipts, failed attempts and provenance controls. Alpha.6
qualifies the core profile on Ubuntu 24.04 and Fedora 44, x86_64 and arm64. macOS
packages provide remote/control commands. OIDC source acceptance below has its
own scope; it does not extend the release's core-profile qualification.

[Runtime tasks](../../../specs/003-m2-daemon-foundation/tasks.md) record the completed
implementation increment. [Capability contracts](../../../specs/004-release-unblocked-capabilities/contracts/capabilities.md)
retain named upstream gates. Admin UI and Storefront remain component-gated;
macOS has no native workload backend. Private restored-copy checks do not provide
a public backup/restore command. Retained volumes and execution copies are costs
outside the shared image-cache budget; automatic execution-copy collection is
not implemented.

## Reproducing checks

Use the commands and isolated fixture requirements in the runtime validation
guide and [contribution policy](../../../CONTRIBUTING.md). `scripts/test-daemon.sh`
checks portable Rust/C socket behavior. Native scenarios through
`scripts/test-runtime.sh` require explicitly selected disposable Linux inputs;
use `--scenario persistent-scylla --fixture /absolute/owned.json` only with owned
test data. `scripts/test-daemon-fuzz.sh` checks bounded protocol inputs. Follow
the [release runbook](../releases.md) for exact-tag qualification and publication.

Controlled actors, native execution, published artifacts and hosted checks each
require their own evidence. C sanitizers do not instrument the Rust image library.
Ignored cases and explicit cross-UID skips are not passing checks. Preserve failed
operations and retained ownership when native termination cannot be proved.

## Durable source acceptance references

These records describe the dated builds and executable/source hashes inside
them. They do not assert that current HEAD was rerun. Original local log paths
identify historical fixtures and are not user prerequisites.

| Area | Committed evidence | Scope |
| --- | --- | --- |
| Contract and build inputs | [Upstream mapping](../../../runtime/profiles/upstream-evidence.json), [dependency lock](../../../runtime/dependencies.lock.json) | Immutable inputs; source mapping alone is not execution evidence. |
| Provisioning | [Cache pipeline](../../../runtime/experiments/provisioning/evidence.json), [bootstrap](../../../runtime/experiments/provisioning/bootstrap-evidence.json) | Consent/offline/cache and actual pinned-image cases. |
| Native execution/network | [Libcrun](../../../tests/runtime/libcrun/delegated-evidence.json), [network adapter](../../../runtime/build-inputs/libslirp/adapter-evidence.json) | Selected rootless Linux fixture and kernel/device capabilities. |
| Ownership/recovery | [Recovery receipt](../../../runtime/experiments/recovery-evidence.json) | Native identities, lost replies, held writers and observed cleanup; early remaining-work fields describe that checkpoint. |
| Persistent datastore | [Scylla report](../../../runtime/experiments/scylladb.md), [persistent acceptance below](#persistent-acceptance) | Migration, retained restart and private independent restore. |
| Restart, 2026-11-05 | [Restart receipt](../../../runtime/experiments/restart-evidence.json) | Real exit/replacement, paused-live health, manager loss and controlled circuit limits. |
| OIDC, 2026-11-06 | [Lifecycle receipt](../../../runtime/experiments/oidc-client/lifecycle-evidence.json), [optional mappings](../../../runtime/profiles/optional-evidence.json) | Five standalone services, eight combined memdb services and nine retained-Scylla services on one Linux arm64 fixture. |
| macOS feasibility | [Apple receipt](../../../runtime/experiments/apple/evidence.json), [managed guest report](../../../runtime/experiments/macos-vm.md) | Library/guest experiments; no public macOS workload backend. |
| Size/packages | [Binary sizes](../binary-sizes.md), [release qualification](release-qualification.md) | Exact identities and separate host-library/image costs. |

On 2026-11-06, the OIDC receipt records 71 passing tests and one explicit cross-UID
skip in each final normal/sanitized 72-case selection. The unchanged slow connection
timer was excluded there and has a separate earlier complete run. The portable
macOS metadata/control build passed 58 cases with two Linux-only skips. Native
production acceptance used testing-disabled binaries, fresh and retained volumes,
public readiness/logs, manager loss, refused replacement and owned stop. Executable
hashes and failed preparation attempts remain in the receipt.

### Persistent acceptance

The 2026-11-05 acceptance used Linux arm64 UID 11000, GitStore source
`06152d193e854bc643fa884daa8b9f30b0d75d0f`, Scylla 2026.1 (observed 2026.1.15)
and target `gitstore-06152d-schema-012` with 12 registered ledger checksums.
`runtime-run-persistent-scylla-zh3v4dcx.json` passed registered-worker policy,
exclusive writer, unchanged ledger under unsafe cancellation, API check mode,
lock conflict, retained restart and independent restore. The historical Scylla
probe SHA-256 was `d05e13a808af5b9a05d7a2187b80f5eb676b6a3e2fd4f5d9d1666aa8d75457c6`;
the backup probe was `683bab0ac38ed245ec60ef1a01ec65648d9d83cdfa9a9bb06f49da478af650fb`.

## Requirement traceability

Identifiers refer to [spec-003](../../../specs/003-m2-daemon-foundation/spec.md).
The quickstart, tasks and dated records above identify the scenarios. This source
acceptance index does not extend the exact release's qualified support.

| Requirements | Checks/evidence | Boundary |
| --- | --- | --- |
| FR-001–004 | Peer UID, framing/capabilities, validation, selection and public native fixtures | Cross-UID skips explicit; preflight does not execute workloads. |
| FR-005–008 | Inspection/pagination, terminal persistence, dropped-ACK replay, retention/capacity | Immutable history; metadata collection does not remove retained data. |
| FR-009–013 | Recovery receipt, concurrent-writer refusal, migration/cgroup identity, stop/retry and persistent acceptance | Missing kernel proof stays uncertain; running migration cancellation refused; no blind mutator retry. |
| FR-014–019 | Stream/replay/log/redaction, real connection timers, bounded fuzz, execution-plan and production-symbol checks | Logs expire; bounded fuzz is not exhaustive security proof; controlled actors do not qualify hosts. |
| FR-020–023 | Public core/persistent/OIDC provisioning, Rust/C and remote runners | Optional evidence scoped independently; Admin/Storefront gated. |
| FR-024 | Restart receipt and native owned recovery | Four attempts; uncertainty/operator intent/manager loss end retries; health failure alone is not native exit. |
| FR-025–027 | Provisioning/registry credential controls and real libslirp adapter | Execution copies outside cache limit; fixed registry policies; no CNI claim. |
| FR-028–029 | Persistent acceptance and OIDC lifecycle | Private restore only; component-specific readiness. |
| FR-030–031 | Platform receipts, dependency lock, binary sizes and release qualification | Source feasibility, core bytes, host dependencies and qualified hosts are distinct. |
| SC-001–002 | Preflight and generation-aware inspect/wait/manager loss | Preflight has no download effects; readiness freshly observed. |
| SC-003–005 | Crash/dedupe/capacity, retained-data/stop, parser/peer/stream/control | Controlled and native results distinguished; bounded security coverage. |
| SC-006–008 | Rust/C and remote runners, command matrix EVD-024, bootstrap/validation | Non-interactive passes do not enable unavailable mappings. |
| SC-009–010 | Installed fixture with PATH unavailable; SDK/public provisioning | UID 11000 source fixture is not clean-host release qualification. |
| SC-011–013 | Persistent acceptance, platform feasibility and binary/release measurements | No public restore/macOS workload backend; package/dependency/image/data costs separate. |

## Material failed attempts and historical detail

The complete checkpoint narrative is retained at immutable commit
[`8fb113e05255912c3e7cc10bceb2b85ade3ec8c7`](https://github.com/gitstore-dev/cli/blob/8fb113e05255912c3e7cc10bceb2b85ade3ec8c7/docs/contributing/validation/local-runtime.md).
Its local fixture identities and intermediate task states are historical evidence.
Material failures remain failures:

- First cancellation fixture selected PID zero; the harness was corrected before
  acceptance. Manual fixture cleanup was not product recovery proof.
- Persistent/restart fixtures selected preparation helpers or assumed deployment
  `one`; corrected selection did not rewrite failed operation history.
- macOS Postman initialization failed with `SecItemCopyMatching failed -50`; later
  status attempts exited 139. Linux contract results have separate evidence.
- OIDC native sanitizer acceptance exposed an address-limit conflict with ASan
  shadow mappings; the diagnostic plan was corrected without suppressing warnings.
- OIDC preparation hit ENOSPC, block I/O failures and a read-only guest journal.
  Accepted subsequent generations have explicit hashes in the lifecycle receipt.
- Alpha.6's first Ubuntu arm64 Homebrew trust setup failed before installation;
  the same-candidate retry passed. Both remain in the release review record.

Read final receipt acceptance fields together with dates and executable hashes;
an early `remaining` entry does not reopen completed spec tasks.
