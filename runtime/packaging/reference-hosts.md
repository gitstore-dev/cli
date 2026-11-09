# Reference-host validation lanes

[`runtime.yml`](../../.github/workflows/runtime.yml) separates library/package
checks from actual workload acceptance. No job has release signing, registry-write
or publishing credentials. Uploaded measurements are unpublished development
evidence and cannot satisfy the [release promotion policy](release-policy.md).

## Hosted build lanes

Ubuntu 24.04 amd64 and arm64 each have normal and Clang 18 sanitizer builds.
They compile the pinned ordinary image/libcrun/libslirp/bootstrap dependencies,
run CTest identity/ownership/protocol/volume/dependency checks, and build a separate
production daemon without test facilities. Normal builds measure the stripped
core pair, package bytes and distinct host-library closure. Production symbol
checks reject test facilities. Build headers/tools are CI prerequisites; no
developer executable or workload image is put into the measured package.

Ordinary/sanitized hosted builds and library checks passed on both architectures;
see the current [evidence record](../../docs/contributing/validation/release-alpha1.md).
Native service smoke is a separate explicit maintainer dispatch using
`hosted_native_run`, selecting a successful unsigned Linux candidate build. It
runs on ephemeral GitHub Ubuntu VMs, validates the exact downloaded pair and
prepares a separate UID 11000. Preparation refuses non-hosted, foreign-repository,
persistent, non-Ubuntu or existing-account hosts. It grants TUN and user-namespace
prerequisites only to that disposable fixture, copies committed public test inputs
into its private home, and runs one real owned core start/readiness/stop case.
This scoped result does not qualify the full Linux package matrix. Missing kernel/
systemd prerequisites and failed workloads fail that explicitly selected job;
none becomes a passing skip. The first amd64 attempt passed prerequisites and
acquired its first image, then failed cache reconstruction. The SDK now creates
private cache files explicitly; fresh acquisition/reconstruction probes pass on
both architectures. Exact rebuilt production-native acceptance remains required.
A bounded diagnostic helper emits only operation enums, resource
counts and host limit metadata, never raw messages, database/configuration or
service/credential contents.

Set `hosted_native_full=true` with an explicit successful `hosted_native_run` to
extend the ephemeral Ubuntu lane after its exact-pair cold start passes. It then
prepares separate developer-only control-negative/maintained-Git probes, copies
only reverified public cache blobs into private offline OCI layouts, and runs
the existing seven clean-install, ten recovery, truncated/tampered-input and
100-sample command/inspection cost checks. Test processes remain UID 11000;
developer helpers stay outside the installed pair. The bounded summary binds
both candidate source and test-source revisions and exact executable hashes.
Only summary/diagnostic JSON is uploaded, never scenario state or credentials.
This remains unsigned native Ubuntu evidence; it does not qualify other package
channels, Fedora, trusted tag receipts or release support.

## Explicit native reference fixtures

Workload jobs run only after a maintainer dispatches reviewed `main` in repository
ID `1431005655`, selects an existing fixture manifest, and has dedicated non-root
reference runners with labels `self-hosted`, `Linux`, `gitstore-runtime`, and
`amd64` or `arm64`. Denial jobs use separately prepared
`gitstore-runtime-denied` runners; an input manifest alone cannot make a healthy
host's TUN inaccessible. Both lanes bind selected fixtures to the reviewed source
before testing. Pull requests/forks and ordinary pushes never select those
persistent hosts. A concurrency group prevents overlapping host batches; a new
run does not cancel a live workload batch.

Each runner needs the documented [source-build dependencies](../../docs/contributing/building.md)
and native namespace/cgroup/systemd/TUN prerequisites. Host preparation is an
explicit administrator/developer fixture action, not something `gs` invokes.
The job builds the reviewed checkout and binds the supplied fixture to those
new `gs`, daemon and native-probe bytes before running scenarios. It preserves
the selected offline layouts, isolated registry test server and dedicated account.
Every scenario snapshots executable hashes again before starting work.

The manifest must be an owned mode-0600 single-link ordinary JSON file, explicitly
selecting `disposableUid`, `dedicatedTestAccount: true`, and an owned private
`fixtureParent`. Supply the scenario-specific image/registry/Git fixture inputs
documented by `tests/runtime/run.py`: verified API/Git/controller layouts, four
persistent offline layout selections, pinned Scylla/API/Git probe images, private
registry test server and repository fixture. No personal configuration, real
account token or pre-existing deployment may be used. Missing inputs fail or
report unavailable; no Docker/VM fallback is selected automatically.

| Behavior | Lane/check |
| --- | --- |
| Compatible present code libraries and ABI descriptors | Native CTest plus real core acceptance |
| Incompatible/missing dependencies | Descriptor rejection CTests and unsupported-daemon clean-install cases |
| Verified host/cache/offline reuse, integrity and consent denial | `core-offline` and `clean-install` |
| Real process loss, stop timeout, writer exclusion and logs | `recovery` |
| Persistent Scylla, migration, failed start, private restore/cancellation | `persistent-scylla` |
| Exact missing optional mappings before effects | `optional` |
| Actual TUN denial before provisioning | Optional separately selected `denied_fixture`; the test verifies denial rather than assuming it |

Product processes run with `PATH=/nonexistent` and synthetic protected credential
material inside these tests. Source-build tools are not end-user runtime helpers.
The workflow does not upload retained fixture directories, credentials or arbitrary
native logs. Keep local failure evidence and inspect remaining ownership before
cleanup. Do not globally prune a reference host to reclaim a failed run.

No self-hosted native reference runner set or six-host release qualification set
is configured or dispatched by this change. The ephemeral hosted core smoke
fixtures described above are a separate scoped lane. Existing local arm64 fixture passes are explicitly scoped in
[the validation record](../../docs/contributing/validation/local-runtime.md).
Unavailable amd64, denial and hosted combinations remain unverified and are not a
release-support list.
