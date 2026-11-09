# Package qualification fixtures

Use `packaging/platforms.json` as the required tuple/channel inventory. Ubuntu
24.04 and Fedora 44 are Linux test targets; macOS 15 arm64/x86_64 are remote/control
targets. These are selected fixtures, not tested support claims. Native Linux
needs delegated cgroup-v2 controllers, rootless namespaces, TUN and a dedicated
non-root account. A container build alone cannot qualify workload startup.

Use the selected private fixture manifest described by
`runtime/packaging/reference-hosts.md` and `tests/runtime/README.md` for native
workloads. Never reuse personal deployments, real tokens or unselected host roots.
For portable packaging checks create a fresh temporary owner-only directory,
private config/state/cache/runtime/home directories and an explicitly selected
artifact pair. Credentials must be synthetic protected files and OS-store access
must not be inherited. Install into separate disposable channel fixtures.

Pin actual fixture image digests and package-manager versions in each receipt
when provisioned. See the [current evidence](../../docs/contributing/validation/release-alpha1.md)
for controlled arm64 checks; the complete clean-host matrix remains open.
Do not start/stop unrelated Docker, Apple containers or persistent services.

Run portable tooling checks with `python3 -m unittest discover -s tests/packaging`.
Developer schema validation requires `packaging/requirements.txt`; installed users
do not require Python or jsonschema. No publishing command is part of this suite.

## Replacement Apple fixture — 2026-11-08

The original `gitstore-m2-linux-a1a13c` was absent. With explicit user authorization,
`gs-cli-alpha1-linux-20261108` was created from the cached original image digest
`sha256:0bf2f725e3ee269a2e804c3426e38270e43eaa41960add5ad0ef12e072181178`.
It has 2 CPUs/4 GiB, no host mounts, SSH forwarding, published ports or sockets.
Guest-root capabilities are test infrastructure; production runs as UID/GID 11000.
`prepare-linux-fixture.sh` is restricted to that exact guest and is never packaged.

The selected manifest is `/home/gitstorefixture/experiments/alpha1-fixture.json`,
installed owned mode 0600 from `tests/fixtures/runtime/alpha1-apple-arm64.json`.
Run `python3 /tmp/check-linux-fixture.py --fixture /home/gitstorefixture/experiments/alpha1-fixture.json`
inside the guest as UID 11000. The checker passed private user runtime/bus,
cpu/memory/pids delegation, TUN, user/network namespaces, pidfd and openat2.
This prerequisite result is not service lifecycle acceptance or release support.

## Controlled package operations

`scripts/test-packages.sh --channel archive|npm|deb|rpm|homebrew` requires an
absolute selected artifact, SHA-256, version and private fixture parent. Add all
three `--replacement`, `--replacement-sha256`, `--replacement-version` arguments
for the next alpha. Archive checks relocate the full pair into paths with spaces.
These runners retain receipts and failures; they do not produce release
qualification receipts or imply upgrade/data recovery guarantees.

Debian/RPM runners require an explicitly selected root-owned private
`--system-fixture` on a disposable host, with a dedicated non-root test UID.
They reject pre-existing product installations, maintainer scripts, unexpected
payload paths and mismatched bytes before privileged installation. Product
commands run as the selected user, with no tools on PATH. Removal is scoped to
the package installed by the run.

Homebrew requires `--brew-source` and its full reviewed `--brew-revision`, plus
`--formula`/`--formula-sha256` for each archive (and corresponding
`--replacement-formula` options). It clones tooling into its own prefix, copies
only the tool runtime, and uses private Home/cache/tap/Cellar/trust paths.
Homebrew 7 trusts only the exact validated binary formula in that private store.
Default API metadata remains enabled; forcing a core-tap clone is unnecessary.
On macOS select an owned fixture parent outside system temporary directories
and protected Documents/Desktop/Downloads trees: temporary-prefix sandbox rules
and OS privacy restrictions interfere with isolated Homebrew builds. Never
disable Homebrew's sandbox to make this lane pass.

`prepare-smoke.py` and `build-version-fixture.py` create deliberately
`UNQUALIFIED-ONLY` artifacts for local package-operation checks. Their incomplete
notices or synthetic unused-platform formula hashes must never enter release
inventories. Production candidates require reviewed full notices and real hashes
for every advertised platform.

## Producer replacement checks

Each production candidate CI job also runs `check-produced-replacement.py`.
It requires the candidate's exact current source/version, builds next-alpha
executables in a private copy, and exercises actual original archive/npm packages
through replacement and removal. Linux additionally runs missing-library and
running-version refusal controls against the original production pair. Tooling
must be present; these selected negative controls cannot become passing skips.

Replacement packages deliberately use `UNQUALIFIED-ONLY` names and incomplete
fixture notices. They are uploaded in separate `unqualified-replacement-*`
artifacts, never canonical production subject groups. They can support controlled
APT/DNF/Homebrew replacement tests but cannot be promoted or used as release
qualification receipts. The original source/native build remains unchanged.

## Exact-byte workflow qualification

The manual `runtime.yml` qualification lane consumes a successful same-source
`release.yml` build, never PR artifacts. It prepares six isolated GitHub-hosted
fixtures: Ubuntu 24.04 and macOS 15 on both architectures, plus pinned Fedora 44
systemd guests on matching ephemeral Ubuntu Docker hosts. Linux product processes
run as fresh UID 11000; setup alone has privilege. Fedora has private cgroups,
no host binds/ports, and no App/signing credentials. Qualification manifests,
next-alpha controls/formula and helper bindings are generated from that exact
trusted candidate build. Source transfer uses committed public Git bundles,
excluding checkout credentials/configuration and untracked files. Missing
prerequisites fail the selected host; no fallback or passing skip is used.

For advanced separately managed manual fixture execution, the underlying qualifier
still accepts the explicit manifest described below. This is not a required input
or alternative automatically selected by the hosted workflow.

Copy [the example](../../packaging/qualification-fixture.example.json) to an owned
mode-0600 file at the selected path on each host. Replace every placeholder and
set the actual account/OS tuple. `cleanHost` and `disposableHost` are explicit
maintainer assertions; the script also checks actual OS/version/UID and refuses
pre-existing system installations. Adjust `replacements` to exactly that host's
channels: Ubuntu archive/deb/Homebrew/npm, Fedora archive/rpm/Homebrew/npm, macOS
archive/Homebrew/npm. Every replacement must select actual reviewed next-alpha
artifact bytes and version; Homebrew additionally needs its exact formula digest.
Use isolated version fixture tooling, never rewrite production source/version
inputs or make compatibility claims from those fixtures.

The Linux `systemFixture` must select the same UID/private parent and be readable
by that user; the privileged installer revalidates it. `nativeFixture` must be
private, select the same dedicated user, have the exact candidate sourceRevision,
and contain the offline assets and source-built probes required by the existing
clean-install/recovery scenarios. `mismatchDaemon` selects a different-version
production daemon for refusal checks. Install test tooling (`patchelf`, Python
with venv, Node/npm, selected Homebrew, and APT/DNF as applicable) on the fixture;
none becomes a product dependency. Privileged package operations use noninteractive
sudo only on these selected disposable Linux hosts. macOS receives no runtime
workload claim; its actual macOS major version must be 15.

`scripts/qualify-release.py` writes no pass receipts until every selected channel
and required native case on that host succeeds. Logs and owned state stay in the
private experiment parent. Workflow credentials and user config are not inherited
by test commands. The collector requires all 22 host/channel records, each bound
to every candidate subject digest, before producing `release-qualification` for
promotion. An unavailable lane fails rather than becoming a skip/pass receipt.

## Complete producer and runtime measurements

`check-produced.py --candidate /absolute/producer-output --output /absolute/new-private-root`
exercises the complete producer's exact archive/npm packages and records 100 warm
samples for each executable's help/version commands with isolated configuration.
It does not sign, publish or qualify a host. Hosted macOS 15 jobs retain these
read-only assets and measurements for review.

`measure-native.py --fixture /absolute/owned-private-fixture.json --output /absolute/new-receipt.json`
uses only the selected dedicated non-root Linux account. It creates its own memdb
deployment, measures 100 successful current-generation inspections, records asset,
state and cache directory costs, and stops only its owned deployment/manager.
Without offline layouts, cold readiness explicitly includes asset preparation.
Directory allocations deduplicate inodes within each category; materialized image
paths can appear outside assets, so categories are not a globally deduplicated sum.
Neither manager-only RSS nor these samples establishes full workload peak memory.

`prepare-offline-cache.py --cache /absolute/owned-state/assets --output /absolute/new-root`
prepares developer-only OCI inputs from an explicitly selected prior verified
core cache. It checks the committed profile's architecture-specific manifest,
config and every layer digest/size before copying ordinary files, uses private
mode-0700 layout roots, and downloads no images. Supply its `layouts.json` mapping
as `offlineLayouts` in a fresh dedicated fixture manifest. The native importer
must independently verify the copies; preparation alone is not offline acceptance.

Hosted native smoke is explicitly selected with the runtime workflow's
`hosted_native_run` input. It consumes complete unsigned CI candidates, validates
actual pair hashes, and prepares fresh ephemeral Ubuntu accounts. A failed native
job stays failed. `diagnose-native.py` retains bounded operation enums, resource
counts and limit metadata only; intents, result bodies, raw messages, service logs,
configuration/credentials and database files are never uploaded. A contributor-only
network-free TLS builder probe may run after failure; it is outside installed
packages and requires the explicit dedicated fixture.
