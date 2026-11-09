# Reference-host validation lanes

[`runtime.yml`](../../.github/workflows/runtime.yml) separates library/package
checks from actual workload acceptance. Native/library/package jobs have no
signing or publishing credentials. Their measurements are development evidence
and cannot satisfy the [release promotion policy](release-policy.md). Separate
explicit preflight jobs check the configured App and sign only a marked text
fixture; they create no tag, release or registry package.

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
both architectures. Exact rebuilt production-native acceptance subsequently
passed on both architectures; the evidence record retains the earlier failures.
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

## Four-platform Homebrew review

Select successful same-source `review_cli_run` and `review_linux_run` values to
render one exact-hash review formula and install/remove the matching archive on
Ubuntu 24.04 and macOS 15, each on amd64 and arm64. Every job uses a private
Homebrew checkout/prefix, pinned tooling revision, isolated HOME/XDG paths and a
synthetic local tap. It does not update a user's installed tap or publish assets.
Linux results cover Ubuntu only; Fedora Homebrew and the complete trusted
qualification matrix remain separate acceptance work. Missing tooling or runtime
dependencies fail the selected job rather than silently skipping it.

## Explicit Fedora review

`fedora_native_run` selects a successful same-repository unsigned Linux producer
run for separate amd64/arm64 Fedora 44 jobs. The jobs use empty ephemeral GitHub
Docker hosts. A digest-pinned Fedora image boots systemd in a private cgroup
namespace with no host filesystem bind mounts or published ports. Privileged
guest setup is fixture administration; every product invocation remains UID
11000, with no tools on PATH. No App key or signing credential reaches these jobs.
The public source archive excludes checkout credentials, `.git`, environment and
personal configuration. Preparation waits for PID 1 before creating a fresh
dedicated account, user bus, delegated controllers and TUN access.

The runner validates package name/version/architecture, script/trigger absence,
payload paths/modes/links and installed hashes before actual DNF/native checks.
It exercises the real installed public command links, concurrent first launch,
core cold start, the complete clean-install/recovery/integrity/performance suite
and removal. Cleanup targets only its labeled container. Only bounded acceptance
and package-control summaries are uploaded. This is an explicit review lane;
unavailable Docker/kernel prerequisites fail rather than selecting a fallback.
It remains separate from trusted tag qualification and the six-host/channel set.

For a local Apple Fedora VM, the developer-only
[`fedora44-native.Containerfile`](../../tests/fixtures/runtime/fedora44-native.Containerfile)
and [`prepare-fedora-fixture.sh`](../../tests/packaging/prepare-fedora-fixture.sh)
describe the selected disposable fixture. The preparation script refuses any
other hostname/distribution or an existing account. Fedora mounts `/tmp`
independently: copy initial public setup files into `/root`, not its underlying
root-filesystem `/tmp`. The complete runner requires explicit
`--fixture-kind apple-fedora44`; its default Ubuntu selection cannot adopt Fedora.
Receipts distinguish candidate, cold-start-test, complete-test and helper source
revisions. See the evidence record for actual results and recorded kernel/libc/
SELinux scope.

## App and signing preflight

Select `release_preflight=app`, `signing` or `all` on a reviewed branch to run
these independent checks. `none` is the default. Native and release-qualification
selections must remain empty for a preflight run.

The App job uses the inherited `RELEASE_PLEASE_APP_CLIENT_ID` variable and
`RELEASE_PLEASE_APP_PRIVATE_KEY` secret through the pinned token action. It
requests the actual Release Please permissions, scopes the token to `cli` and
checks installation access with a read API call. It changes no repository
content. Repository-level settings alone do not list inherited organization
settings; use the repository's organization-variable/secret metadata endpoints.
The actual CLI-scoped App check passed after installation access was enabled;
see the evidence record. This does not exercise a release PR or tag trigger.

The signing job has its own hosted VM and OIDC/attestation permissions, with no
App key. It generates a non-installable `UNQUALIFIED_RELEASE_PREFLIGHT` text
subject, uses the same pinned signer as `release.yml`, preserves a standard
bundle/trusted root and verifies our immutable repository ID. Altered bytes,
wrong workflow, wrong source and missing evidence must fail. The production
release gate must also reject this branch test fixture. No consumer verification
requirement is introduced. Only the marked subject, public bundle/root and a
bounded result are uploaded. Exact certificate SAN, source ref and digest remain
mandatory; `gh`'s mutually exclusive identity flags must not be combined.

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
