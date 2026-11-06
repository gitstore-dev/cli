# Public runtime acceptance (in progress)

These tests use actual `gs` and `gitstored` public interfaces. They require explicit
fixture selection and fail if lifecycle behavior is unavailable. They do not turn
native-probe results into public acceptance. All eight real core CLI cases now pass
on the selected Linux arm64 fixture, covering approval, cold readiness, isolation,
foreground/detached lifetime, replay and pre-effect gates. Asset/cache/security and
cross-host/clean-install gates retain their separate records; this is not M2
acceptance completion.

Select only a dedicated non-root account on a disposable Linux machine, never a
personal account or existing deployment. Its user session/cgroups must meet the
native prerequisites. `GITSTORE_RUNTIME_FIXTURE` selects an owned JSON file with
absolute `gs`, `daemon`, `fixtureParent`, `disposableUid` and
`dedicatedTestAccount: true` fields. An explicit `unsupportedDaemon` selects a control-only own binary for the missing
execution-capability case. Optional `offlineLayouts` and
`wrongPlatformLayouts` map registered core components to already verified OCI
layout fixtures. No unused assets are downloaded to create these test inputs.

```sh
python3 tests/runtime/test_core.py
python3 tests/runtime/test_assets.py
```

The harness installs private single-link copies of the two own executables and
creates private configuration/state/cache and synthetic protected credentials.
It writes the planned T043 owner settings before starting the manager. Personal
configuration is not inherited. `PATH=/nonexistent` makes accidental executable
wrappers fail. Current lifecycle methods remain capability-gated; do not enable
them without the exact owner source and actual resource driver.

Fixtures are retained. Teardown requests stop for its deployment IDs and terminates
only its own manager child. Failed/uncertain stop never authorizes deleting data or
broad PID cleanup. Failures omit raw service/config output. The tamper case changes
only a singly linked blob inside its own private cache.

The configured explicit fallback uses an owner-only directory; the manager derives
one protected file per installation/deployment identity. Credential assertions
compute that identity filename and verify same-deployment reuse. A single literal
file shared by concurrent deployments is not an accepted owner setting.

Run saved scenarios through the explicit dispatcher:

```sh
scripts/test-runtime.sh --scenario core-memdb --fixture /absolute/private-fixture.json
scripts/test-runtime.sh --scenario exp-003b --fixture /absolute/private-fixture.json
scripts/test-runtime.sh --scenario core-offline --fixture /absolute/private-fixture.json
```

`core-memdb` runs the eight real CLI/runtime/network cases. `exp-003b` runs the
real image acquisition/cache/offline/tamper cases through the selected SDK; it
requires actual verified `offlineLayouts` and `wrongPlatformLayouts`. The latter
must contain the other platform's complete bytes, not merely relabeled metadata.
`--case test_METHOD` selects a saved method in the scenario. There is no substitute
backend or automatic container-engine selection. `host-unmet` requires the explicit
`GITSTORE_EXPECT_MISSING_TUN=1` fixture condition; the runner never changes host
permissions itself.

The manifest must be an owner-selected, ordinary mode-0600 file and explicitly
mark `dedicatedTestAccount: true` with the invoking `disposableUid`. Missing
host/fixture inputs emit structured `unavailable` and exit 77. Assertion failures
exit 1. Selected runs save an owner-private `runtime-run-SCENARIO-*.json` under the
disposable parent with binary digests, status, elapsed time and retained-evidence
location. Every scenario root remains available for inspection; no broad cleanup
or deleting retained user data is part of the runner.

`--scenario exp-004b-local` runs the actual public core stop/restart and
retained-volume hash/inode/writer-lease check using selected verified offline
images. See [volume evidence](../../runtime/experiments/local-volumes.md).
The runner snapshots each selected executable before the batch; all snapshots
and scenario roots are retained.

`--scenario exp-003b-sdk` separately tests the actual image library against an
explicit synthetic private registry. The fixture selects `imageSdkTest` and
`registryFixture` executables. Neither is installed or required by product users.

`--scenario clean-install` runs the six US5 acceptance groups plus public raw
GraphQL and library-based Git clone/sync/reclone. Select an ordinary production
candidate pair, the explicit control-only negative fixture and the developer-only
`repositoryProbe` example. The local seed helper remains outside the installed
pair. Candidate observations use a bounded 600-second readiness deadline and
record elapsed time; earlier 60-second failures remain failures in their records.
After memdb restart, catalogue identities intentionally reset. Only the test
harness inspects the known retained commit/tree/blob with libgit2 after observed
stop, under the actual writer lease. This does not implement product file access
or claim public catalogue hydration/persistent metadata.

`--scenario cli-tls` separately verifies the actual release CLI against explicitly
owned HTTPS peers, with valid fixture CA/leaf chains and an unrelated-CA rejection.
It requires selected `gs` and `registryFixture` developer inputs; fixture CAs are
never installed into the user's system.

When copying sources to a VM with an independent clock, extract with fresh
timestamps (GNU tar `-m`) before rebuilding. Preserved older timestamps can leave
Cargo/native test artifacts stale even when the source file contents changed.


The `optional` scenario now includes public OIDC acceptance. Positive cases need
`offlineOidcLayouts` containing the three core and five provider layouts; the
persistent case additionally needs `offlineScyllaLayout`. Missing inputs produce
an explicit unavailable result. Standalone selection uses only the five provider
images. The persistent case pauses an exact bridge PIDfd during core startup to
prove that stale provider health cannot satisfy final readiness. Run large image
cases serially with adequate host disk headroom; preserve failed fixtures and
canonical data, material, journals and inputs.
