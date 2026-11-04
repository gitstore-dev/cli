# M2 Validation Guide

**Status**: Implementation in progress. Rust/control-plane checks and private
Linux core bootstrap probes have recorded results in [M2 status](../../docs/m2-status.md).
Broader public lifecycle, packaging, recovery and migration scenarios below remain
validation targets. Their presence is not a claim they passed. Daemon build/control
targets exist; the public runtime dispatcher and runner remain in progress.
Run from the repository root. Never use the user's real config, credential store,
existing deployment or retained volumes as fixtures.

## Prerequisites and isolation

Development: pinned Rust 1.96.0; CMake >=3.28, C17 compiler and CTest; Clang 18 for
sanitizers/fuzzing; Python 3 for orchestration. Real backend checks need disposable
Ubuntu 24.04 amd64/arm64 hosts with recorded user namespace/mapping, cgroup and
filesystem capabilities. Distribution users must need only GitStore and documented
host capabilities. No Docker/Compose prerequisite is allowed for runtime acceptance.

The planned runners create mode-0700 temporary configuration/state/runtime/cache
roots, synthetic credentials, unoccupied loopback ports and owned volumes, then
clean only their transient resources. They print root/fixture identities and
preserve redacted failure evidence. Tests that need peer UIDs, registries, Scylla,
VMs or special host capabilities opt in explicitly. Missing access means not-run
or unavailable, never a simulated pass for the real lane.

## 1. Existing client regressions

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo build --workspace --locked
scripts/test-remote.sh
```

Expected: existing remote behavior works with daemon absent; no runtime asset
requests. Existing Postman simulations validate public HTTP only. The installed
Postman CLI default credential initialization fails with `SecItemCopyMatching failed -50`.
Local bootstrap collection lint and one saved readiness assertion pass with an
isolated CLI home and cloud event reporting disabled. Record this limited evidence
separately from full simulation or real collection execution. The existing Compose image runner is separate compatibility
evidence and cannot prove the new default runtime installation path.

## 2. Native build, protocol and controlled recovery

Planned CMake options and test labels must be created with executable targets:

```sh
cmake -S daemon -B build/daemon -DCMAKE_BUILD_TYPE=Debug -DGITSTORED_TESTING=ON
cmake --build build/daemon --parallel 2
ctest --test-dir build/daemon --output-on-failure
scripts/test-daemon.sh --daemon build/daemon/gitstored --gs target/debug/gs
```

The process runner exercises actual Rust/C framed sockets against all 22 M0
examples, negotiation, peer identity, partial framing, unknown fields/capabilities,
duplicate keys, UTF-8, safe integer/depth/length/item boundaries and exact error
categories. Check 32 connections/16 requests, stream quota/overflow/expiry, timers,
metadata/log budgets, eight saturated workers and independent cancellation.

Crash failpoints cover before/after intent transaction commit, dispatch, resource
allocation and acknowledgment. Expected: same-key replay returns original identity;
changed intent conflicts; accepted history survives; disk-full rejects safely;
corrupt/uncertain ownership blocks replacement. Different keys/deployments cannot
bypass canonical volume ownership. Stop/failure/repeated cleanup leave retained
fixture hashes unchanged; stale generations never satisfy ready waits.

## 3. Sanitizers and parser fuzzing

```sh
CC=clang cmake -S daemon -B build/daemon-sanitized \
  -DGITSTORED_TESTING=ON -DGITSTORED_SANITIZERS=ON -DGITSTORED_FUZZING=ON
cmake --build build/daemon-sanitized --parallel 2
ctest --test-dir build/daemon-sanitized --output-on-failure
scripts/test-daemon-fuzz.sh --build-dir build/daemon-sanitized --seconds 60
```

Expected: deterministic malformed-input/resource corpus passes and bounded fuzzing
finds no crash, out-of-bounds access, UB or allocation-limit bypass. Record duration,
compiler, corpus and sanitizer settings; a short smoke run is not a claim of
exhaustive security verification. Production binaries cannot select test backends.

## 4. Runtime selection and trust experiments

Planned experiment entry point (each invocation uses explicit disposable fixtures):

```sh
scripts/test-runtime.sh --experiment EXP-003A
scripts/test-runtime.sh --experiment EXP-003B
scripts/test-runtime.sh --experiment EXP-003C
scripts/test-runtime.sh --experiment EXP-004A
scripts/test-runtime.sh --experiment EXP-004B-local
```

Before execution, lock dependency source/build identities and resolve real image
manifest/per-platform digests. Record trusted upstream sources and platform/digest
checks; upstream image attestations are not required. EXP-003A compares private shared/static library packaging and tests
allocation, fork/signal/reaping, cancellation and native crash recovery. EXP-003B
compares maintained library integrations and justified host/on-demand helper paths under public/private fixtures, wrong/expired/scope
credentials, token/redirect failures, malicious layers, interrupted downloads,
wrong platform/digest, offline import and disk exhaustion. EXP-003C validates CLI-published file/offline release evidence, including wrong
signer/workflow/ref/root and missing required CLI attestations. Add OCI only if
this repository publishes an OCI artifact. This publication check does not gate
startup on upstream image attestations.

EXP-004A evaluates maintained networking libraries first, with pasta/slirp/CNI
fallback comparisons, and tests connectivity, name isolation, loopback publishing,
internal-port privacy and owned attachment cleanup. EXP-004B-local tests retained
volume stop/restart/replacement, writer exclusion, backup/restore and disk-full.
Select only paths passing mandatory criteria; record rejected candidates too.

## 5. Real profile end to end

```sh
scripts/test-runtime.sh --scenario core-memdb
scripts/test-runtime.sh --scenario persistent-scylla
scripts/test-runtime.sh --scenario recovery
scripts/test-runtime.sh --scenario clean-install
```

Each runner provisions its own registered profile, credentials and retained volumes,
then invokes the proposed [CLI](contracts/cli.md). Example commands inside that
owned fixture environment are:

```sh
gs api config validate --deployment m2-fixture --profile core --profile-version m2-alpha8
gs serve --deployment m2-fixture --profile core --profile-version m2-alpha8 \
  --datastore memdb --allow-provision --detach --idempotency-key m2-fixture-start
gs deployment inspect m2-fixture
gs deployment inspect m2-fixture --operation OPERATION_ID --wait=ready
gs deployment logs m2-fixture --component api -o json
gs deployment stop m2-fixture --idempotency-key m2-fixture-stop
```

`OPERATION_ID` is the actual returned ID. Profile/version names are proposed fixture
registrations, not current installed profiles. The logs subscription is bounded
by the runner and explicitly interrupted/unsubscribed. Scylla migration uses the
runner's registered target and backup acknowledgment; never guess an image schema
version or run against an existing datastore.

Expected real checks: public GraphQL and Git operation against reported endpoints;
private controller enrollment and generation readiness; foreground interrupt and
detach; no sudo/extra stack; same-key replay; concurrent writers; manager kill and
restart with live workloads; stop timeout; migration conflict/failure/unsafe cancel;
retained-data hashes and correct memdb ephemeral-state explanation. Migration must
not start API, and API startup must not race schema preparation. Optional service
cases run only with verified mappings; record unavailable mappings by component.

Clean-install cases use only the candidate distribution on fresh host images:
interactive consent/decline, no-input rejection/authorization, verified offline,
cold/warm cache reuse, no unrelated downloads, dependency inventory and trust failures.
Capture stripped binary, required shared-library, package/download/installed/cache
bytes and memory/readiness measurements. Compare each core executable against the
initial <=5 MiB target and report misses with dependency contributors. Test compatible
host-library reuse, wrong ABI/version rejection, missing-library consent/download,
denied provisioning and offline dependency import. Prove unused external runtime components are not fetched. Ordinary compiled
code dependencies do not require runtime discovery or downloads. Establish remaining budgets from these measurements.

## 6. macOS feasibility and support reconciliation

```sh
scripts/test-runtime.sh --experiment EXP-005A
scripts/test-runtime.sh --experiment EXP-005B
```

Run only on explicitly selected disposable compatible Mac/guest fixtures. Record
hardware/OS/framework/backend/kernel identities and any installation elevation.
Test full lifecycle, authenticated guest control, private images, loopback endpoints,
retained volumes, logs and crashes. No Mac access means unavailable evidence;
Linux implementation continues. Never equate control-plane compilation with local
container support or install an unapproved fallback automatically.

## Evidence required before completion

Map SC-001–013 and FR-001–031 to actual checks in planned `docs/m2-status.md`.
Record command subsets, test commands/results, exact dependency/server/image pins,
not-run lanes, experiment outcomes and release exclusions. Update matrix/dependency
rows only from that evidence; partial passes do not close all DEP-012–015. Keep
source review, controlled workloads, native backend and published release results
separate. Publishing is outside this planning operation; final trusted packages
still require ADR 0003 provenance gates.
