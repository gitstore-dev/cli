# Alpha.1 implementation evidence

## CI follow-up — 2026-11-09

Hosted layer auditing isolated the startup failure to cache file permissions:
all four Git-image layer blobs on both architectures had mode `0664`, one link
and matching declared sizes. The SDK correctly refused group-writable source
files; digest checks were deliberately not performed after policy rejection
(a false digest audit field in that case is not evidence of corrupted content).
Logs: `/private/tmp/gs-alpha1-hosted-layer-audit-{x64,arm64}.log`,
[run 42589804349](https://github.com/gitstore-dev/cli/actions/runs/42589804349).
SDK metadata and network blob creation now explicitly request `0600`, rather
than depending on ambient umask. Existing invalid cache inputs remain rejected.
An isolated umask-0002 regression failed before the fix (actual `0664`, expected
`0600`) and passes afterward with an in-memory maintained-client registry stream.
The full Linux SDK suite passes: 15 tests, two explicit opt-in fixture ignores;
all-target release Clippy passes. Logs:
`/private/tmp/gs-alpha1-sdk-file-modes-{before,after,clippy-linux}.log`.
Fresh hosted acquisition/reconstruction and rebuilt production-native acceptance
are required before declaring this host failure resolved.
Fresh SDK acquisition and both C-wrapped/SDK-only reconstruction now pass on both
hosted architectures in
[run 42591136414](https://github.com/gitstore-dev/cli/actions/runs/42591136414).
Each fresh layer is asserted mode `0600`; the earlier candidate's `0664` cache
remains rejected. Retained logs:
`/private/tmp/gs-alpha1-hosted-private-cache-fix-{x64,arm64}.log`.
The fixed Linux SDK also passed all four explicit synthetic-registry cases
(basic authentication/resume/platform/quota, expired auth, denied scope and
redirect credential policy), then the complete 15-case unit/security suite.
Retained `/private/tmp/gs-alpha1-sdk-fixed-controlled-registry.log`.
These are SDK/probe results; exact rebuilt production workload acceptance is
still a separate gate. The opt-in `hosted_native_full` lane now prepares existing
clean-install/recovery/integrity and 100-sample cost suites with exact candidate
and test-source identities; its executable run remains pending.

The exact hosted Linux arm64 candidate from run 42525026876 also passed on the
retained disposable Apple Linux VM (kernel 6.18.35, Ubuntu 24.04): offline core
lifecycle 36.247 s and online cold start/current-generation readiness 45.353 s.
Selected CLI SHA-256 `eb08ebcddea8029de2f4a7644152e39c55e953e63840048e517ee1b06d28fed7`;
daemon `07381a994c6784504150ecedddfcd61cf670c5e3150c0ffdf4abb822c1643265`.
Both were checked against the complete candidate's install inventory before
transfer to new owned experimental paths. Receipts:
`runtime-run-core-offline-ol2eezbc.json` and
`runtime-run-core-memdb-oiltqsvk.json`, retained under
`/home/gitstorefixture/experiments` in `gs-cli-alpha1-linux-20261108`.
Host logs: `/private/tmp/gs-alpha1-hosted-pair-apple-comparison.log` and
`/private/tmp/gs-alpha1-hosted-pair-apple-online-comparison.log`.

Direct pinned-libcrun synthetic lifecycle, delegation/security and start-worker
crash probes passed on both Azure kernel 7.0.0-1017 hosted architectures in
[run 42584603547](https://github.com/gitstore-dev/cli/actions/runs/42584603547).
This establishes the direct library path only; the same run's real production
workload still failed before readiness. The arm64 TLS diagnostic did not run
because rustup attempted to reinstall an existing component and failed; the
previous successful TLS result remains separate. The diagnostic now reuses an
installed pinned compiler. Its next adapter/network attempt initially lacked
Meson; explicit Meson/Ninja build dependencies were added. These developer
diagnostics never enter release packages or qualify a failed workload run.
The corrected registered adapter/network probe subsequently passed both normal
and lost-start-acknowledgement scenarios on both hosted architectures in
[run 42585570950](https://github.com/gitstore-dev/cli/actions/runs/42585570950).
The production workload's bounded milestone counts are all zero (execution,
population, native); failure precedes execution-root recording. This narrows the
remaining investigation to active-cache reopening/verification or private image
materialization, rather than demonstrating a libcrun launch failure. Full logs
are retained at `/private/tmp/gs-alpha1-hosted-adapter-complete-{x64,arm64}.log`.
The selected offline cache probe reports numeric provisioning outcomes only and
does not execute image commands. Diagnostic privacy assertions and the complete
66-case package policy suite pass locally (seven explicit fixture skips).
Corrected exact-cache reconstruction in
[run 42587398198](https://github.com/gitstore-dev/cli/actions/runs/42587398198)
returned provisioning integrity status 4 for the Git image on both architectures,
without HTTP or executing an image. Roots `image-materialization-probe-fjiur4fa`
(x64) and `image-materialization-probe-qkz0xojr` (arm64) stayed in their ephemeral
owned fixtures. Logs are retained at
`/private/tmp/gs-alpha1-hosted-materialization-selected-{x64,arm64}.log`.
The isolated pinned diagnostic Rust installation passed on both architectures;
the former ambient component conflict no longer prevents the arm64 image probe.
The SDK-only mode rejects a missing source before selecting any acquisition path;
its portable C compilation and that negative check passed. SDK-versus-tree audit
comparison and the existing synthetic owned-file reader remain diagnostic work,
not completed runtime acceptance.

Pinned alpha.9 integration passed all 34 tests locally after waiting for initial
Repository reconciliation before fixture cleanup, matching the existing Namespace
readiness check. The previous hosted integration failure was an optimistic-delete
conflict while the Repository controller was writing initial status. Fixtures now
observe public current-generation `Ready` before issuing the single delete;
production mutation/conflict behavior is unchanged. Retained log:
`/private/tmp/gs-alpha1-repository-ready-integration.log`.
The next hosted failure was the corresponding Product controller race; server
logs identify `MarkProductTerminating: datastore: resourceVersion conflict`.
Readiness before fixture deletion now covers all four authored kinds. Category
uses enclosing status observedGeneration because its pinned Ready condition does
not populate that field. All 34 pinned-image checks passed again (30 commands in
3.299 s; four alpha.9 cases in 2.441 s), retained at
`/private/tmp/gs-alpha1-all-resources-ready-integration.log`.
The watch-authoring fixture had a separate cleanup path; the same readiness check
now precedes its single delete. The latest full pinned-image rerun passed all 34
checks (30 command checks in 2.706 s and four capability checks in 2.276 s):
`/private/tmp/gs-alpha1-watch-cleanup-ready-integration.log`.

The previous ordinary amd64 runtime job failed while fetching native corresponding
source with HTTP 503, after native compilation, package checks and actual relinking
passed. Source collection now retries selected transient HTTP responses at most
three times, with 1 s/2 s backoff. HTTPS, byte bounds and authenticated/pinned digest
checks remain required; permanent HTTP errors and policy failures are not retried.
Three download failure/recovery checks and four source/relink binding checks pass.
Retained failed job log: `/private/tmp/gs-alpha1-latest-runtime-failure.log`.

## Complete producer and inspection measurement — 2026-11-08

T019 is implementation-complete as the generated formula target. The task's
literal path is clarified to match the approved plan and `Formula/README.md`:
render the complete real-hash formula for review/testing, then commit
`Formula/gitstore.rb` only after verified matching asset promotion. Committing the
current review formula ahead of the eventual release tag would advertise stale
archive digests because inventories bind the actual build commit. The renderer,
private sibling/completion install, explicit Git URL tap, optional-service boundary,
post-promotion update/idempotence and real-hash macOS installations are verified.
The publication action and qualified release support remain separate open gates.

Repository-level CI variable names, secret names, environments and releases were
read again on 2026-11-08: each list was empty. No secret value was requested/read.
The required protected `release-publication` environment is absent, and no own
trusted tagged attestation/App-trigger positive has been run. Organization-level
App configuration/installation was not established by these repository reads.
No public tag/release/tap update or external registry publication was performed.

T025 is complete for clean macOS 15 remote/control package acceptance. Both
arm64 and Intel Homebrew lanes passed with the exact complete candidates and
real four-platform archive hashes in
[run 42527666842](https://github.com/gitstore-dev/cli/actions/runs/42527666842).
They used independent private prefixes/taps/caches/trust stores, preserved the
inventory and sibling links/completions, verified help/version with no tools on
PATH, checked explicit unsupported local runtime, and removed selected code.
Archive/npm control, relocation and removal already passed on both actual macOS
15 architectures. This establishes that scoped package behavior; it does not
supply the trusted six-host release receipts, complete next-alpha replacement
matrix, native Linux support or publication approval. Scoped receipts/logs are
retained under `/private/tmp/gs-alpha1-homebrew-qualified-control-receipts`:
arm64 macOS 15.8.2 root `brew-lifecycle-vaeni_fw`; Intel macOS 15.7.9 root
`brew-lifecycle-hg10cjyy`. Both report the pinned tooling commit even though shallow
checkout version output is generic. Their exact archive/formula digests match the
four-platform review inventory.

T015 and T018 are complete: all four production tuples built with full actual
source/lock/native bindings, complete notices and source/relink subjects, then
passed exact archive/npm installation/removal and command checks. Selected runs
at branch revision `a682c1961fb42136d247fbf3e051dbf258f777c8`:
[Linux ordinary/sanitized/complete producers](https://github.com/gitstore-dev/cli/actions/runs/42525026876)
and [both macOS 15 complete producers](https://github.com/gitstore-dev/cli/actions/runs/42525026870).
The outputs remain unsigned review candidates, not release-qualified support.
The same-source review coordinator passed in
[run 42527077092](https://github.com/gitstore-dev/cli/actions/runs/42527077092),
confirming actual PR-merge checkout `69983e60d6fad73f03d942405d106f7ab47688ad`
for all four tuples. Real-hash review formula SHA-256:
`05517f36d91d188eb798ff39197b9004a3b90e9712f6645f6d6a87317a362d73`.
Retained `/private/tmp/gs-alpha1-four-platform-review/{gitstore.rb,review.json}`.
The arm64 Homebrew job failed at cache/formula resolution before installation;
its hidden private-home log directory was unintentionally excluded from upload.
The upload now explicitly includes only those selected log/receipt paths, including
their hidden parent, so the next disposable review can expose the actual cause.
The retained log then confirmed Homebrew refused an untrusted formula: the
shallow pinned checkout reports a generic `>=4.3.0` version, so version parsing
incorrectly skipped Homebrew 7's formula trust. The harness now detects the actual
reviewed `cmd/trust.rb` capability and trusts only the exact private fixture formula.
No global Homebrew prefix, trust store, sandbox or user configuration was changed.
Separate hosted-native review run 42527079575 retains workload failures as failures.
Both Azure-kernel 7.0.0-1017 Ubuntu fixtures acquired/activated the first selected
image, then failed workload start. Safe diagnostics show a failed start operation,
owned cleanup success and released workload/resource ownership, not an image/TLS
acquisition failure. The static-OpenSSL bounded TLS builder passed on both
architectures. Artifacts are retained under
`/private/tmp/gs-alpha1-hosted-native-diagnostics`; x64 probe log
`/private/tmp/gs-alpha1-hosted-native-tls-x64.log`. Actual native workload support
on those hosted environments remains unqualified; the Apple kernel 6.18.35 arm64
full acceptance results are separate. Actual fixture/schema/native input review and full-byte
coverage were checked; validators do not establish legal sufficiency by themselves.

Final exact-production measurement passed after the complete acceptance batch:
`/private/tmp/gs-alpha1-complete-native-command-costs-v3.json`, owned root
`gs-public-runtime-ps261uyg`. This uses the complete producer's `327c151e…` CLI /
`82706d81…` daemon, 100 successful current-generation inspections (plus warm-up),
no concurrent service-test batch and selected offline OCI inputs. Offline readiness
was 35.047 s; inspection p95 1.657 ms. Manager RSS/HWM remained 9,367,552 bytes;
asset cost remained 258,759,008 logical / 260,595,712 allocated bytes, state outside
assets 259,156,926 logical / 261,197,824 allocated bytes, cache empty. These remain
independent directory costs and manager-only memory as explained below.

| Exact production warm command | Samples | p95 ms |
| --- | ---: | ---: |
| gs --help | 100 | 0.623 |
| gs --version | 100 | 0.489 |
| gitstored --help | 100 | 6.735 |
| gitstored --version | 100 | 6.515 |

Reference qualification now records these measurements on each selected host,
checks documented distinct help/version/inspection targets and includes the exact
pair hashes/sizes in each receipt. Producer validation rejects missing metrics,
nonfinite/incomplete samples, missed targets, unstopped measurement owners and
metrics for another executable pair. Twenty-four policy fixtures passed including
the actual offline Release Please engine and these new controls; the actual
Linux production and both hosted macOS 15 measurement records also pass the shared
target validator. This implements repeatable measurement gates; T061 still needs
the complete selected reference-host execution set.

T012 is complete for code-library linkage/ABI/license inventory: actual hosted
amd64 and arm64 private-static pairs passed ordinary, pinned-fd and sealed-memfd
execution with no loader environment overrides. Both daemons require at most
GLIBC 2.38 and only host libc/libm/libgcc plus the OS loader; both CLIs require at
most GLIBC 2.39 and host libc/libgcc. Actual macOS 15 arm64/x86_64 control pairs
use OS libraries and deployment target 15.0. All four native/Rust material sets
are committed. This closes the selected linkage design, not clean-host lifecycle,
complete source/notices release acceptance, full tuple producers or signed support.

| Hosted Linux pair (`65d5cc8`) | gs SHA-256 | gitstored SHA-256 |
| --- | --- | --- |
| amd64 | `4f30d98953184d19e39800910addefea3d2cb3d0d73f2b34590fac9ed1fbb60e` | `8663e65daf7d75986aa80f3f7deb94f6e5565e05ce490d5e05cab85e2773a2f7` |
| arm64 | `eb08ebcddea8029de2f4a7644152e39c55e953e63840048e517ee1b06d28fed7` | `d1ee08c9ed417f18302330b5e942021493a953529fbfb78f95f0dad7c2ffb6b4` |

Retained closure packets: `/private/tmp/gs-alpha1-reviewed-hosted-closure-amd64`
and `/private/tmp/gs-alpha1-reviewed-hosted-closure-arm64`. The newer explicit
compression-link setting also passed the clean Linux arm64 producer/core result
below; current hosted runs recheck that exact configuration.

A clean detached checkout of `b300471` completed the full macOS arm64 producer,
including 177 source-bound components, 352 full notice texts, a corresponding
source archive and 361 installed files. Host: macOS 26.6.1 arm64; this does not
substitute for the macOS 15 clean-host matrix. Output:
`/private/tmp/gs-alpha1-production-pipeline-darwin-arm64`.

| Final local candidate | Bytes | SHA-256 |
| --- | ---: | --- |
| archive | 3,519,492 | `874765d598253247d7efb66987a809e7c6ecfa782d9dbabe4ae1c33f6457588e` |
| npm | 3,519,749 | `302efda519dc5fc0609efd7193f2bafcf42132478582bf7d008c54580e15244b` |
| corresponding source | 25,243,662 | `6700bcf07cf43767510d6cd1aa4f791b4a8893a5fffef821f53a251e0d4a6056` |

Archive relocation and npm `--ignore-scripts` installation/removal passed with
these exact complete packages. Receipts are retained under
`/private/tmp/gs-alpha1-production-package-lifecycle`. These are unsigned local
candidates; no tag, registry upload or release support claim follows.

The actual hosted macOS 15 arm64 material packet exactly matches all three
committed arm64 JSON inputs. The Intel packet has 178 components, 354 full texts,
192 material records and zero unresolved entries; it is now committed under
`packaging/release-inputs/darwin-x64`. Both came from the successful
[hosted control/material run](https://github.com/gitstore-dev/cli/actions/runs/42519281219).
Linux ordinary/sanitized behavior and linkage passed, but that run's new material
collection failed on newer CMake's exact dependency-file metadata option. The
parser now omits only that exact output-metadata argument; unknown linker options
remain rejected. A fresh hosted run must establish the collector result.

`tests/packaging/measure-native.py` passed on the dedicated Ubuntu 24.04 arm64
fixture using the exact `327c151e…` CLI and `43b35e76…` daemon recorded below.
It created its own three-service memdb deployment, verified current-generation
readiness for every successful inspection, stopped its own deployment and manager,
and used isolated configuration with no personal data. Receipt:
`/private/tmp/gs-alpha1-native-command-costs-v2.json`; retained fixture
`gs-public-runtime-5f6xzlpy`. Cold readiness including asset preparation: 42.107 s.

| Warm command (100 measured samples plus one warm-up) | p95 ms |
| --- | ---: |
| gs --help | 0.592 |
| gs --version | 0.507 |
| gitstored --help | 8.031 |
| gitstored --version | 8.422 |
| gs deployment inspect performance -o json | 2.121 |

Inspection wall time includes CLI startup and socket negotiation. Manager RSS/HWM
was 9,367,552 bytes, excluding worker/service memory. Asset files occupied
258,759,008 logical / 260,595,712 allocated bytes; state paths outside the asset
subtree occupied 259,156,631 logical / 261,197,824 allocated bytes; cache was empty.
Each category deduplicates allocated bytes by inode internally. State can contain
materialized image/rootfs paths; these independent directory costs must not be
summed as globally deduplicated storage. This is one real fixture sample, not
all-reference-host performance acceptance. T061 remains open.

A first clean Linux producer run failed at final linkage because lzma-sys
selected a host development library rather than embedding its locked source.
The CMake image-library command now explicitly selects static LZMA and zlib
builds; their complete nested notices/sources are already in the Rust packet.
The failed output/log is retained at `alpha1-production-pipeline` in the dedicated
fixture and `/private/tmp/gs-alpha1-production-pipeline-linux-arm64.log`.
The fresh clean producer at `97c00fe` subsequently passed, including exact
source/lock/native archive binding, all full notices, ordinary/fd/memfd execution
and packaging. Output `alpha1-production-pipeline-v2`; log
`/private/tmp/gs-alpha1-production-pipeline-linux-arm64-v2.log`.

| Complete Linux arm64 candidate | Bytes | SHA-256 |
| --- | ---: | --- |
| archive | 11,871,821 | `5daaf1de913906f1d1abeffe9be8f2fb6c39f37721c99d7b48b4582f66b7b5e5` |
| npm | 11,871,920 | `861de1758dddadea18d805e27bc5a0c983c9998a207428790f1688124234f563` |
| Debian | 9,193,112 | `cce3a6feda328472f61a86dc99a92d09de93e87fc8b5b65b1b05f22a0d33c1f6` |
| RPM | 11,996,168 | `343806ed806ab3b4574df0d2c814df4a12c810266103fb0a061d69685e33d728` |
| corresponding source | 92,295,730 | `50cefed99df67e150a13e554509be9c919119623325467ef6b23bb58d0aa7e2c` |
| relink kit | 28,583,874 | `91794925657346b60c01f6f24712cc7ebde7eeecb82771aca5a4d5d4c358d7c1` |

The final daemon is `82706d816848dd9fe0fdad13e843408005b0a91f9582aae69430a1b68349e52f`;
CLI remains `327c151e1e8130c8059b2d1d46139150dc97e6c38889343ac446c3aefb50e3ad`.
The separate relinked daemon (`4c32cccd…`) passed execution checks. Exact complete
archive/npm install/relocate/remove passed; receipt parent
`alpha1-complete-produced-checks-v2`. Distro installation and real service startup
using these new exact bytes subsequently passed too: the actual non-root core
start/current-generation readiness/owned-stop case passed in 45.018 seconds,
retained root `gs-public-runtime-h6n82gzq`; receipt
`/private/tmp/gs-alpha1-complete-native-core-v2-receipt.json`. Exact complete Debian
install/remove passed on Ubuntu 24.04 arm64; receipt
`/private/tmp/gs-alpha1-complete-debian-v2-receipt.json`. Exact complete RPM
install/remove passed on Fedora 44 arm64 userspace/container kernel; receipt
`/private/tmp/gs-alpha1-complete-fedora-rpm-receipt.json`. The latter is package
acceptance only, not native workload support. Both package runners invoked
product checks as UID 11000, never root. The disposable Fedora container
`gs-cli-alpha1-complete-rpm-20261108` used cached image
`sha256:15da97ed9aa94dd3d92c3925eb89520f6c80e07ea7ee24cba848d1f8b6912b3e`,
with no host mounts/ports; only that container was removed after retaining its
receipt. The Apple Linux fixture and its retained roots remain available.

The hosted macOS 15 arm64 complete producer also passed, but its subsequent
archive harness selected system Python through a shell wrapper and lacked the
venv's jsonschema. `check-produced.py` now preserves its exact Python interpreter
when launching the package harness. This was a test-tool dependency error, not
an installed-binary runtime dependency. The next hosted run must pass lifecycle
checks; failed check artifacts are retained as unsigned diagnostics.
With the complete production pair, missing-native-library rejection, running
alpha.2 daemon refusal without stopping/replacing it (exit category 6), and
concurrent first-use autostart through public executable links all passed.
Log `/private/tmp/gs-alpha1-complete-native-package-boundaries.log`; retained
roots `package-version-refusal-aguhibte` and `package-autostart-wy2nocta`.

Truncated offline blob refusal passed; receipt `runtime-run-exp-003b-qbpnm9y8.json`,
root `gs-public-runtime-6iry6moy`. Warm-cache tamper refusal passed in 35.744 s;
receipt `runtime-run-exp-003b-ts5zymdx.json`, root `gs-public-runtime-4xex_3j4`.
Both used the complete production pair and only modified their new private copies;
log `/private/tmp/gs-alpha1-complete-integrity-v2.log`.

Offline core acceptance passed in 35.555 seconds using copies of this turn's
already verified selected cache images, with no new images/network fallback.
Receipt `runtime-run-core-offline-qebbq37w.json`, root `gs-public-runtime-8u0yv1v9`;
log `/private/tmp/gs-alpha1-complete-offline-core-v3.log`. The first fixture
preparation used non-private layout roots and correctly failed at manager startup
before provisioning; that failure remains recorded in
`runtime-run-core-offline-c6podnvt.json`. The helper now creates mode-0700 layout
roots, preserving the daemon owner-settings policy. All seven complete clean-install cases subsequently passed in 327.953 seconds,
including public GraphQL/Git and retained restart through the exact selected pair.
Receipt `runtime-run-clean-install-t6dhcjn_.json`; log
`/private/tmp/gs-alpha1-complete-clean-recovery-v2.log`. All ten recovery scenarios subsequently passed in 554.329 seconds on the same
complete production pair. Receipts were retained locally as
`/private/tmp/gs-alpha1-complete-clean-install-receipt.json` and
`/private/tmp/gs-alpha1-complete-recovery-receipt.json`. The native scenario roots
remain in the dedicated fixture; no personal deployment/data was used.
These are engineering observations, not upgrade/data-preservation guarantees.

Both macOS 15 complete archive/npm lanes now passed in
[run 42521831875](https://github.com/gitstore-dev/cli/actions/runs/42521831875),
including final native source/notices generation, installation/removal, relocation,
unsupported-runtime behavior and 100 warm samples. Homebrew/replacement and the
complete reference matrix remain T025.

The first hosted Linux native smoke preparation passed user/cgroup/TUN setup but
failed when UID 11000 could not traverse the runner's private checkout ancestor.
The helper now copies only committed public test/profile inputs into the new
account's private home; runner-home permissions and .git/token state stay intact.
The next hosted run must prove the corrected native path. A separate manual
unsigned development review lane is prepared to test Homebrew on actual macOS 15
arm64/x86_64 using four real same-source archive hashes. It requires successful
ordinary CLI/runtime run IDs and emits no trusted release qualification. Its
execution is pending the corrected complete Linux producer jobs. New negative
run-selection fixtures pass; release-policy fixtures now total 22 cases.

Integration run 42522472015 exposed a controller/Namespace resource-version race:
the CLI correctly returned CONFLICT. The disposable delete fixture now waits for
public Ready/current-generation status and performs one delete, preserving the
no-mutation-retry policy. Exact pinned upstream schema/reconciler source was
checked at `68f8471…`. The actual pinned alpha.9 image suite passed locally after
this fixture change: 30 command checks plus four alpha.9 capability checks, with
its live contract suite also passing. A later run found the same race in the
separate alpha.9 create/read fixture's cleanup; both now share the same bounded
Ready/current-generation wait. The repeated actual image suite passed too, log
`/private/tmp/gs-alpha1-alpha9-reconciled-cleanup.log`. Both paths still demand an
accepted delete and issue it only once; conflicts were not accepted as success. Log
`/private/tmp/gs-alpha1-stable-namespace-integration.log`. Postman skill status was
current; no endpoint or API contract changed.

The next hosted attempt at `1867800` passed dedicated-user/kernel/cgroup/TUN/
namespace prerequisites on amd64 but the real core start failed in 3.286 seconds
with OPERATION_FAILED/unavailable. No amd64 native support is claimed. Log
`/private/tmp/gs-alpha1-hosted-native-source-failure.log`. Native smoke is now a
separate explicit `hosted_native_run` dispatch consuming an exact successful
unsigned candidate; ordinary package builds and source collection make no workload
claim. Failed native jobs retain only safe diagnostic enums/counts/limits, never
raw database, operation messages, configuration, service logs or credentials.
A sentinel fixture test verifies sensitive intent/result/message content is omitted.
A separate contributor-only `runtime_tls_probe` example can diagnose Tokio/native
TLS builder initialization under the same finite worker limits without configuring
any URL, request or credential. It is never compiled into/installed with gitstored.
The static-OpenSSL probe passed in the owned arm64 fixture; its opt-in hosted step
runs only after a native smoke failure with successful host preparation. This does
not convert the original workload failure into a pass.

The native measurement script is retained for repeatable selected-fixture runs.
The hosted macOS jobs now additionally build complete read-only candidates and
exercise their archive/npm installation, relocation and warm commands. Neither
job signs or publishes PR artifacts. Qualification can be dispatched at the exact
reviewed tag after `main` advances; repository, workflow, event and full source-SHA
checks still apply. No candidate tag was created to exercise that path.

## Hosted CI and material preparation — 2026-11-08

Draft PR #12, source `6e84b822ad911295045a971e79be2766f2a0a60d`, passed all
ordinary hosted checks. [CLI run](https://github.com/gitstore-dev/cli/actions/runs/42513240390)
passed Rust checks on Ubuntu/macOS plus production control-pair policy checks on
actual macOS 15 arm64/x86_64. Both report deployment target 15.0, OS libraries only
and the explicit unavailable-runtime boundary. T021 is complete for this scoped
binary behavior; this is not complete package-manager acceptance under T025.

| Hosted macOS 15 executable | SHA-256 |
| --- | --- |
| arm64 gs | `a584661b4bb414fff94e700d5ccde0d60ba7cdfb8981bc2e4f68a66eefeecbae` |
| arm64 gitstored | `1dda7205444dd89d04ba1ab989182b149c584937cc894f220f5ae66ae21db0a1` |
| x86_64 gs | `977eb2228b13969dbf9f591e760ddeef611de5e9bd9ca063cef8b24162f80452` |
| x86_64 gitstored | `b75125b0018b04b85695842fc9df30bf5ae36eec707221391312c5cd4bce66de` |

[Runtime library run](https://github.com/gitstore-dev/cli/actions/runs/42513240368)
passed normal/sanitized checks on amd64 and arm64, private-static closure, ordinary/
fd/memfd execution and package policy. [Daemon run](https://github.com/gitstore-dev/cli/actions/runs/42513240347)
and [latest/alpha.9 integration](https://github.com/gitstore-dev/cli/actions/runs/42513240345)
also passed. Dedicated native/signing/promotion jobs correctly did not execute
for the PR. These results supersede earlier unavailable hosted-build observations,
not the still-open clean-host release matrix or own attestation gate.

Source material preparation now follows the actual production graph. The macOS
arm64 packet has 174 external Cargo packages, the standard library, Jansson and
SQLite: 177 components, 352 full notice texts, no unresolved entries. The complete
local archive/npm payload was generated with 361 inventoried files plus a separate
source asset in `/private/tmp/gs-alpha1-complete-package-macos`. It is a controlled
source-build checkpoint, not a published/qualified release. The corresponding
input JSON sets are committed under `packaging/release-inputs/darwin-arm64`
and `packaging/release-inputs/linux-arm64` for their respective observed closures.

The Linux packet combines both workspaces and actual native source packages:
271 components, 744 full notice texts, 306 material records, no unresolved entries.
Retained `/home/gitstorefixture/experiments/alpha1-complete-materials-v3`; output
`/private/tmp/gs-alpha1-native-materials-linux-v3.log`. All eight distro source sets
were authenticated through signed Ubuntu source indexes and checked against their
SHA-512 entries. PCRE2's complete added copyright file was reconstructed from its
Debian source diff. Libxcrypt's installed copyright exactly matched the source
template plus LICENSING; referenced native file-header terms were also retained.
Rust registry archives matched lock checksums; the OCI Git archive matched every
pinned Git object. The Cynic license missing from its crate matched its recorded
upstream commit's blob `a612ad9813b006ce81d1ee438dd784da99a54007`.

Producer staging now builds binaries first, creates source/relink subjects from
reviewed inputs and exact objects, resolves final hashes, then generates notices
and packages. It no longer requires a prepublished copy of its own relink kit.
Source subjects retain all selected Rust/native sources and patches, committed
application source and full notices. Static system archives must match their
reviewed hashes; compiled native archives are hashed after building from pinned
sources. Standard-library attribution uses a conservative complete rlib-set index.
A changed Cargo Git revision/checksum with the same package version fails notice
generation. Private linkage keeps OS math/resolver/runtime flags outside -Bstatic;
the rebuilt arm64 daemon passed ordinary/fd/memfd closure with only host libraries.
Its selected real native cold-start/readiness/cleanup case passed in 44.088 s
(`/private/tmp/gs-alpha1-native-host-libraries-core.log`, receipt
`runtime-run-core-memdb-a99dtx9n.json`, owned root `gs-public-runtime-7tr4b230`).
The new daemon digest is `43b35e76419834a9001eb0c524a0c281aab69feabc0fbe34f591f625fe4e781e`.
New hosted collection will provide proposed per-architecture material inputs;
those remain distinct from release qualification and signing.

Changed material checks: 62 portable package cases, 57 passed/five explicit fixture
skips with the actual macOS pair; 21 producer fixtures passed with the pinned
Release Please engine. The actual 177-component macOS source/lock/native binding
passed notice generation. Retained logs: `/private/tmp/gs-alpha1-source-packaging-tests.log`,
`/private/tmp/gs-alpha1-material-release-tests.log`. New tests reject stale Git
commits/registry checksums even when a package version is unchanged, partial
Debian copyright patches, missing relink objects, source-name collisions, altered
source/license bytes and source ZIP/tar links/traversal.

The amd64 packet from the successful collector in
[run 42520966643](https://github.com/gitstore-dev/cli/actions/runs/42520966643)
adds 272 components, 746 full texts and 307 materials with zero unresolved entries.
All four per-platform input sets are now committed; Linux amd64 adds the same
architecture-specific core_detect crate observed on Darwin Intel. Actual complete
Linux producers are now part of ordinary read-only CI, in addition to both macOS
producers. These artifacts remain unsigned review inputs and cannot enter the
trusted release promotion path.

Implementation state: **59/64 checked, 5 open**. T024/T061 still require the
complete selected Linux/reference-host lifecycle and performance matrix; Azure
kernel-7 workload start failed and is not waived. T038 needs the configured,
authorized exact-tag App/signing run and own signed positive. T039/T064 need the
final tag-bound inventory/bundles and remaining qualification/guide review.
Implementation and controlled package acceptance do not make alpha.1 release-ready.
No public tag, release or registry/formula publication occurred.

## Current qualification checkpoint — 2026-11-08

This section supersedes older not-run observations below; their historical hashes
and limitations remain evidence of those earlier checkpoints.

- Rust 1.96.0: fmt, all-target Clippy with warnings denied, full locked workspace
  tests and build passed on macOS arm64. 116 ordinary tests passed; opt-in native/
  live cases stay explicitly ignored in the ordinary target. The dedicated Linux
  UID 11000 run also passed all-target Clippy, 115 ordinary tests (16 explicit
  native/live ignores), and the static-zlib release build.
- `sh scripts/test-capabilities.sh`: 29 focused Rust cases passed; 48 actual CLI
  controlled requests plus three saved Postman collections, 10 assertions, zero
  failed. Full local stdout: `/private/tmp/gs-alpha1-capabilities-current.log`.
  No Postman cloud publication or user credentials were used.
- `scripts/test-integration.sh --alpha9`: **41 named checks passed** (seven Rust
  Git/File cases, 30 existing Python command cases, four alpha.9 capability cases
  with per-kind subcases). Image source labels all matched the pinned SHA below.
  Both direct `/admin/graphql` and reverse-proxied `/gitstore/admin/graphql` runs
  passed. The final proxy run forwarded 97 HTTP requests plus WebSocket tunnels.
  Full local stdout: `/private/tmp/gs-alpha1-live-alpha9-prefix-final.log`.
- Actual File fixtures were authored and submitted through public Git using
  libgit2, never internal RPC/datastores. Typed File ADDED, full read by name and
  namespace-free ID hydration passed, preserving the exact Unicode/trailing-newline
  body. Namespace/Repository creation, full core queries, writable Product/Category
  exports, five bootstrap kinds and four API-authored ADDED events passed.
- Independent synthetic static-user sessions passed login → consuming refresh →
  old-token replay rejection → confirmed remote logout. A revoked session could
  not reconnect its watch and the durable checkpoint stayed unchanged. The existing
  connection remained open at the 200 ms observation point; immediate existing
  socket revocation is **not** claimed. This is memdb/static-users/loopback evidence,
  not external OIDC, native OS credential-store or HTTPS provider qualification.
- Initial failures were confined to test infrastructure: interrupting after stdout
  before checkpoint fsync, duplicate Host header forwarding, and rewriting public
  Host during WebSocket same-origin checks. The harness now waits for the durable
  boundary and preserves one public Host. Product safety checks were not relaxed.
- Each Compose run used its own UUID project, protected generated credentials and
  volumes. Observed cleanup removed only that project's containers/network/volumes.
  The separate authorized Apple Linux fixture remains running.
- Producer fixtures: 21 tests passed, including the actual Release Please 17.3.0
  offline alpha.1 → alpha.2 engine, complete-set/receipt gates, altered bytes,
  signed-identity command constraints, conflicting upload/retry, tap failure after
  promotion and rejection of foreign/PR/wrong-source artifact runs. Mock verifier
  output is **not** an own signed positive. No release API mutation was run.
- `actionlint` 1.7.12 passed for changed release/Release Please/CLI/runtime/
  integration workflows, with declared existing custom reference-runner labels.
- Portable package suite with actual production pairs: 51 cases; macOS 46 passed/
  five explicit fixture skips, Linux 48 passed/three explicit fixture skips; four
  production control daemon setup cases passed. This is macOS 26.6.1 arm64, not
  the still-required macOS 15 arm64/x86_64 acceptance matrix.
- Ordinary C: 64 passed, three platform skips. Three socket cases initially
  sandbox-denied passed outside the sandbox, including the 310.60 s deadline case.
  Sanitized C: 63 passed, three skips with that already-tested slow case excluded.
  No unchanged expensive C workload was repeated.

Pinned image indexes, resolved from `0.1.0-alpha.9` (without a leading `v`), are
committed in [alpha9-images.json](../../../tests/integration/alpha9-images.json):

| Service | Index SHA-256 |
| --- | --- |
| API | `db4625a6ac98807b5481533756959f6bfe7dcd28a3a3843039cdb41e49053760` |
| Git service | `f9477be489b5c734d695764059fcbddf74417a629f06e16f2d733082c718ec97` |
| Controller | `fba3c6ad27eeac7587e75ba59231ba6f6c65a9d6085f9bfb3915aaed537be2da` |

All selected image revision labels:
`68f8471edc947e417c98b6e8299a1367ab6b20da`.

Earlier ordinary installed snapshots (both report `0.1.0-alpha.1`):

| Host / executable | SHA-256 | Native closure |
| --- | --- | --- |
| Linux arm64 gs | `791c9d37f82c8651d794f64703d36a1ab2e8dbebe3e737b56471d977484417da` | libc/libgcc/OS loader; GLIBC 2.39 |
| Linux arm64 gitstored | `1b40a914639e8d9edffe25abacb06a4754190ff8d5adefd50a70d4cdd5ad7dd9` | libc/libm/libgcc/OS loader; GLIBC 2.38 |
| macOS arm64 gs | `0a4d1d2a25c18e42f2e278fb2c01be0b88c74cd5068756fad6a334b35c59d6d4` | OS Security/CoreFoundation, libiconv/libSystem; minOS 15.0 |
| macOS arm64 gitstored | `625af9db838cb5fc2357ebd4df5d5986491761cb0b67b55896a11bdb27952526` | libSystem only; minOS 15.0; control only |

Linux copies are `/home/gitstorefixture/experiments/alpha1-current-pair` in the
dedicated guest; ordinary, pinned-fd and sealed-memfd startup passed for both with
PATH=/nonexistent and no loader overrides. Host copies are
`/private/tmp/gs-alpha1-current-pair-macos`; Mach-O closure and ordinary startup
passed. These snapshots predate the final native credential locking/help-footer/error-category
changes. Final regenerated package checks are recorded below. Earlier real native lifecycle receipts below used the same daemon bytes;
they predate the current CLI and do not qualify every package format.

The current Linux artifact-only measurement ran on disposable copies via
`runtime/packaging/measure.py`: stripped gs 5,983,816 bytes; stripped gitstored
12,371,912 bytes; two-executable measurement archive 8,111,340 bytes; host system
library file cost 2,652,384 bytes. Its daemon copy has a different stripped digest
(`44052f52e037b471eb5fa9f70da255625a0a41f4aa4bbfbc95cad7f864c90554`).
This archive deliberately omits release documentation/notices and is not a release
package. Accepted historical baselines remain gs 5,590,568 and daemon 5,723,600
bytes; current private-static sizes miss those budgets. Optimization remains
deferred. Runtime/cache/image costs and cross-host p95 remain separate open work.

**Release remains unqualified.** Full reviewed license/source/relink materials,
all four tuple packages and clean-host install/replace/remove evidence, complete
execution of the new runtime qualification artifact producer, performance/size receipts and an own
hosted signed positive/App-trigger receipt are still outstanding. The new workflow
fails on missing reviewed inputs or exact-byte receipts. No tags, public release,
registry upload or formula publication was performed. The alpha.0 seed remains
intentional until the generated alpha.1 release PR synchronizes it.

## Final command and package checkpoint — 2026-11-08

This checkpoint supersedes the earlier executable snapshots above. These are
uncommitted source-build observations, not a qualified or published release.

| Production executable | SHA-256 |
| --- | --- |
| Linux arm64 gs | `327c151e1e8130c8059b2d1d46139150dc97e6c38889343ac446c3aefb50e3ad` |
| Linux arm64 gitstored | `1b40a914639e8d9edffe25abacb06a4754190ff8d5adefd50a70d4cdd5ad7dd9` |
| macOS arm64 gs | `5c56eab9692dc567f7b390d433c0b9c7b9181c8ee0badb99ad7f1ab2eaff24a5` |
| macOS arm64 gitstored | `625af9db838cb5fc2357ebd4df5d5986491761cb0b67b55896a11bdb27952526` |

Linux pair: `/home/gitstorefixture/experiments/alpha1-final-command-pair`.
macOS pair: `/private/tmp/gs-alpha1-fixed-command-pair`. Both report full alpha.1.
No runtime image is included. Mac runtime unavailability and automatic application
version mismatch now use contract exit category 6; explicit sockets remain
connect-only. Required Rust fmt/Clippy/test/build passed on both hosts; Linux
production CLI was rebuilt with `LIBZ_SYS_STATIC=1`. Full Linux check output:
`/private/tmp/gs-alpha1-linux-final-rust.log`. Required C/sanitizer results remain
those above; subsequent changes did not alter C execution code.

Native session-store checks used a unique synthetic identity: macOS Keychain
create/replace/read/remove passed in 0.17 s, and the dedicated Linux user with a
fresh private D-Bus/Secret Service passed in 0.29 s. No personal credential was
read. Identity-lock contention, unrelated-account isolation and exposed lock-file
rejection passed. A separately selected public HTTPS Basic-auth refusal probe
passed without a Git executable: it exercised the existing libgit2 smart transport
over reqwest/rustls, with synthetic credentials, not a new TLS backend. A refused
clone may leave an unborn `.git` destination; no successful fetch is claimed.

Actual alpha.1 → alpha.2 operations (all removed selected package code afterward):

| Channel / host | Retained receipt or log | Scope |
| --- | --- | --- |
| APT / Ubuntu 24.04 arm64 | `system-package-md57gytm/receipt.json` in Linux experiments; `/private/tmp/gs-alpha1-apt-replacement.log` | Privileged code install, non-root commands, replacement/removal |
| DNF / Fedora 44 arm64 | `/private/tmp/gs-alpha1-dnf-replacement-receipt.json` | Same operations; container kernel/glibc, no native workload claim |
| npm / Ubuntu 24.04 arm64 | `npm-lifecycle-0fl_5y6u/receipt.json` in Linux experiments | Final CLI grammar, private global prefix, ignore-scripts, no verifier on PATH |
| npm / macOS 26.6.1 arm64 | `/private/tmp/gs-alpha1-package-lifecycles/npm-lifecycle-fitx59zn/receipt.json` | Private prefix, real pair, explicit unsupported runtime |
| Homebrew / macOS 26.6.1 arm64 | `/private/tmp/gs-alpha1-brew-default-api-replacement.log` | Private checkout/tap/Cellar/trust/cache, installed hashes, replacement/removal |
| Archive / Ubuntu 24.04 arm64 | `archive-lifecycle-dg2tq082/receipt.json` in Linux experiments | Full payload relocation into paths with spaces, replacement/removal |
| Archive / macOS 26.6.1 arm64 | `/private/tmp/gs-alpha1-archive-macos-replacement.log` | Same relocation plus unavailable-runtime refusal |

These are deliberately `UNQUALIFIED-ONLY` integration artifacts with incomplete
notices, not canonical release subjects. Synthetic alpha.2 changes only version
identity in separate build inputs; it establishes package-manager operations and
ordering, not data/schema compatibility. Unused Homebrew platform hashes are
synthetic and must never be published. The exact package digests are retained in
these receipts. APT/DNF install commands never started a root product daemon.
The disposable Fedora container was removed after preserving evidence; the
explicitly authorized Apple Linux fixture remains available. No unrelated
workloads, user Homebrew Cellar/taps or real config/credentials were changed.

Homebrew tooling: revision `bf464f57275cae0be106b43d190c98564ac59b0e`, version
7.0.12. The passing fixture lives beneath the private home child
`.gitstore-release-fixtures-20261108-01a1ba97`. Temporary-prefix and Documents
fixture attempts failed and remain recorded; the passing lane kept Homebrew's
sandbox enabled. Homebrew 7's trust was scoped to the exact reviewed private
formula. The first successful install caught completion files being moved out of
libexec: completion entries now use symlinks and retain all inventoried files.
RPM's default build-ID links also violated the private payload allowlist; the
renderer disables those links rather than weakening the installer guard.

Final portable suite: 51 cases; macOS 46 passed/five selected-fixture skips;
Linux 48 passed/three selected-fixture skips. Two separately selected real Linux
failure cases also passed: a copied daemon with an injected nonexistent DT_NEEDED
library failed before product state, and the current CLI refused a running
alpha.2 manager with `INCOMPATIBLE_VERSION`/exit 6 without stopping or replacing it.
Fixture-owned teardown then stopped that exact child; no other manager was touched.
Output: `/private/tmp/gs-alpha1-native-failures.log`. Logs:
`/private/tmp/gs-alpha1-packaging-final.log`,
`/private/tmp/gs-alpha1-linux-final-packages.log`. Negative archive cases reject
extra files, altered bytes, symlinks and setuid modes before extraction. Release
fixtures: 21 passed, including the actual pinned Release Please engine; output
`/private/tmp/gs-alpha1-release-final.log`. Package reference/completions were
regenerated from final grammar on both hosts. Cumulative prerequisites are not
substitutes for the missing clean-host release matrix.

### Local command measurements

`runtime/packaging/measure.py --samples 100` ran on disposable stripped copies of
the final Linux pair, Ubuntu 24.04 / Linux 6.18.35 / aarch64 / glibc 2.39, Apple VM
2 CPUs/4 GiB. Each command had one warm-up, then 100 wall-clock process samples
with isolated empty config/state and PATH=/nonexistent. No daemon/images started.

| Command | p95 milliseconds |
| --- | ---: |
| gs --help | 0.554 |
| gs --version | 0.397 |
| gitstored --help | 6.546 |
| gitstored --version | 6.818 |

Retained `/home/gitstorefixture/experiments/alpha1-final-command-measurement/measurement.json`.
Stripped gs: 5,983,816 bytes; daemon: 12,371,912 bytes; two-binary measurement
archive: 8,113,915 bytes; host libraries: 2,652,384 bytes. It has no release
notices/docs. Existing size baselines remain unmet; optimization is deferred.
Daemon inspection latency, other required hosts and separate current cache/image
costs remain open under T061.

### Qualification producer and relink material

T022's runners now cover all four requested package managers plus archives,
including real missing-library and different-version-manager negative controls.
Acceptance for every required host/architecture remains T024/T025, not implied by
runner implementation. T033 now has a complete trusted receipt producer in
`runtime.yml`: explicit same-source/tag build selection, six dedicated clean
reference hosts, actual installed-byte tests and a full 22-selection collector.
APT maps to Ubuntu, DNF to Fedora; each Linux distribution also tests archive,
Homebrew and npm. macOS tests archive/Homebrew/npm on each architecture. This
removes the accidental APT-on-Fedora / DNF-on-Ubuntu cross-product requirement
without removing any requested manager or actual target OS. All host receipts
bind every final subject digest. No complete reference run has been performed;
the configured runner set and source-bound fixtures remain acceptance work.

The selected Linux relink preparation used real production inputs, not synthetic
objects: 17 unique application/native object or archive materials. Relinked daemon
SHA-256 `823be0ca2a4d9c937cf447bc93f2bcff7a454e44603c1707dca35f925eea2905`
reported alpha.1 and passed ordinary/pinned-fd/sealed-memfd execution with only
system dynamic libraries. The draft kit lives at
`/home/gitstorefixture/experiments/alpha1-actual-relink-kit/UNQUALIFIED-relink-kit.tar.gz`,
SHA-256 `90892b5912cdc712df8823a2936d83d351af86d5ecc94fb1f5c4578c7fdf1f24`.
Output: `/private/tmp/gs-alpha1-actual-relink-kit.log`. Cargo's build archive has
multiple hardlinks; the tool reads through a pinned descriptor with mutation
checks and copies new ordinary kit files, never packaging those hardlinks. It
retains actual application objects, native archives, hashes and a relocatable
relink recipe, and leaves glibc/OS loaders with the host. Full patched sources,
license notices and obligation review remain missing; this draft is not a release
subject and does not close T012/T015.

A second independent output directory reproduced the identical kit and relinked
executable digests. The newly relinked executable also passed the actual selected
`core-memdb` cold-start/current-generation-health/cleanup case in 45.274 s. Native
receipt: `/home/gitstorefixture/experiments/runtime-run-core-memdb-8tdc2gln.json`;
owned runtime root `gs-public-runtime-2ea6ez19`; output
`/private/tmp/gs-alpha1-actual-relink-native.log`. The initial full-scenario attempt
returned unavailable before execution because no unsupported-daemon control was
selected in that manifest; it was not counted as a pass. Only the selected real
cold-start case is claimed for the relinked bytes.

Implementation checkpoint: **53/64 tasks checked, 11 remain open**. Changed
workflows pass actionlint; 21 producer tests pass, including strict same-source
input and actual OS/fixture selection failures. Local Markdown links and
`git diff --check` pass. Production platform assembly now also refuses dirty or
uncommitted inputs; working-tree experiments use the explicitly unqualified smoke
tooling. No public tag, release, registry upload or formula publication has occurred.
A draft implementation PR is authorized to start hosted CI; the remaining
release qualification gates still apply.

### Requirement trace and remaining gates

The spec-quality checklist remains read-only and fully checked (14/14). Its
checks concern specification quality, not implementation acceptance. Following the
implement skill's explicit read-only rule, implementation trace is recorded here
rather than changing checklist markers or implying those markers qualify code.

| Requirement | Actual evidence / remaining work |
| --- | --- |
| FR-001 | T001–T003 immutable release/source review; alpha.9 source receipt and live image revision labels |
| FR-002 | Gap inventory and source-only 84-command receipt; US3 selected increment implemented; named external gates remain |
| FR-003 | T040–T058 controlled and pinned-live read/create/export/watch/static-user auth; arbitrary OIDC/updates/waits remain gated |
| FR-004 | File namespace/name compatibility and namespace-free Relay ID read passed against pinned alpha.9 |
| FR-005 | Public Git/GraphQL/daemon boundaries; body round trips, scoped credentials, bounded watch/backpressure and transport uncertainty tests |
| FR-006 | Release Please 17.3 offline alpha.1→alpha.2, synchronized versions and workflow policy; actual App trigger pending T038 |
| FR-007 | Four manager renderers and arm64 operation checks above; Nix deferred; no registry publication; complete canonical matrix pending |
| FR-008 | Inventory, real sibling pair, inert user-unit and notices guards; complete reviewed notices T015 and all-host evidence T024/T025 pending |
| FR-009 | Private first-use paths, owned socket, credential separation and no-state help/version; selected Linux native receipts above |
| FR-010 | Concurrent first local use converged to one owned manager; explicit socket/mismatch refusal; all packaged native tuples still pending |
| FR-011 | Standard hosted attestation and strict producer verification implemented; consumers optional; own signed positive T038 pending |
| FR-012 | 16 producer fixtures cover incomplete/conflicting/retry/tap-failure and wrong identities; actual hosted negative/positive evidence pending |
| FR-013 | Controlled arm64 checks above; Linux x64, macOS 15 arm64/x64 and remaining manager/host matrix unavailable; no full support claim |
| FR-014 | Lifecycle documentation and actual install/replace/remove effects; no preservation/recovery guarantee; no supported migration claim |
| FR-015 | Selected Linux closure/fd/memfd/native startup receipts; final all-host qualification and T061 measurements remain incomplete |
| FR-016 | Payload/LFS/CRD/MediaAsset/transfer/publication/CSI prerequisites remain explicit; no speculative endpoints |
| FR-017 | CLI/daemon help, generated reference, package notices/descriptions, formula caveats, install guide and per-draft publisher disclaimer |
| SC-001 | Reviewed 84-command classification plus capability contract and named remaining gates |
| SC-002 | Controlled package operations passed on selected arm64 hosts; full canonical exact-byte matrix incomplete |
| SC-003 | Selected dedicated Linux ordinary-user lifecycle passed; other advertised tuples not qualified; macOS workload runtime unavailable |
| SC-004 | Structural/negative controls passed; final own hosted signed positive and complete subjects not yet available |
| SC-005 | Production pair/fixture metadata alpha.1 and engine alpha.2 passed; reviewed public tag/published release not created |
| SC-006 | Selected alpha.9 operations passed controlled failures and pinned live checks; native store checked separately; unsupported providers explicit |
| SC-007 | All selected documentation/package/generated-reference surfaces audited; publisher requires notice in each draft before promotion |

Remaining release prerequisites are concrete: T012/T015 actual closure license,
source and relink materials; T018/T019 final complete canonical packages/formula;
T021/T024/T025 missing host tuples and execution of the new qualification workflow;
T038 configured repository
App and authorized reviewed tag for own hosted attestation/trigger evidence;
T039 final inventory/bundles/readiness; T061 measurements and T064 final guide.
No repository App client-ID variable was configured at the latest read-only check.
The candidate workflow deliberately fails before builds when reviewed inputs are
missing and refuses promotion without complete exact-byte receipts. Publication
is a separate explicitly authorized operation after these gates pass.

## Baseline — 2026-11-08

- Checkout: `main`, `b5c393e37b4787de0407ac3ccc1b058c067d69d8`.
- Host: macOS 26.6.1 (25G76), arm64; Rust 1.96.0
  (`ac68faa20`); Apple clang 21.0.0; CMake 4.4.5.
- Existing `.postman/skills.json` and new Postman skill directories are user-owned
  changes and are preserved. Feature 004 design files were already untracked.
- Spec-quality checklist: 14 checked, zero unchecked; no extension hooks configured.
- Existing candidate packages inventory host libraries but do not bundle them.
  No binary release, qualified package matrix or own signed positive is claimed.
- Upstream alpha.9 revision: `68f8471edc947e417c98b6e8299a1367ab6b20da`.
  Git blob identities were checked through the immutable GitHub tree API. Public
  File/Namespace/Repository/shared/Collection/auth schemas were read. Earlier
  source research also reviewed static-user resolver/provider behavior; conformance
  execution remains separate. The per-command receipt is
  [alpha9-capabilities.json](../../../crates/gs/tests/fixtures/upstream/alpha9-capabilities.json).
- `postman skills status` outside the sandbox: current, exit 0. Sandbox Keychain
  access is the cause of the earlier `SecItemCopyMatching failed -50`; no Keychain
  reset or reinstall was performed. No live collection or signing run yet.

## Prerelease policy

GitStore and the CLI are early alpha; upstream schemas continue changing
incompatibly. There are no upgrade compatibility, data-preservation or recovery
guarantees. Use, upgrade, migration and removal may cause data loss. Keep
independent backups and do not use prereleases for irreplaceable data. Engineering
ownership/cleanup checks remain objectives, not broader guarantees.

## Qualification state

All package/hosted signing/native acceptance combinations start not-run. Build-only,
source review and controlled checks cannot establish live/provider/platform support.
Record exact artifact identities and actual results below as tasks complete.

## Foundation checks — T001–T008

- Added strict source/candidate/artifact/install schemas and validators, an
  immutable-source receipt covering all 84 command records, shared canonical
  package-version encoding and isolated synthetic test harnesses.
- `python3 scripts/validate-capability-evidence.py crates/gs/tests/fixtures/upstream/alpha9-capabilities.json`:
  PASS, 84 records; source-only, not live conformance.
- `python3 -m unittest discover -s tests/packaging -v`: 25 passed; native Debian
  comparison skipped on macOS and checked separately in the disposable fixture.
  Failure controls include source-only false pass claims, duplicate IDs, unreviewed
  references, symlink/hardlink traversal, changed bytes, mismatched executable
  versions, missing notices, unqualified macOS execution and incomplete releases.
- Ubuntu 24.04 image index
  `sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852`:
  dpkg alpha.1 < alpha.2 < stable checks PASS in a disposable arm64 container.
- Fedora 44 image index
  `sha256:15da97ed9aa94dd3d92c3925eb89520f6c80e07ea7ee24cba848d1f8b6912b3e`:
  rpm Lua vercmp alpha.1 < alpha.2 < stable checks PASS in a disposable arm64
  container. No existing workload or VM was stopped or modified.
- `cargo fmt --all -- --check`: PASS.
- `cargo test --locked -p gs --test local`: eight passed outside the sandbox.
  Initial sandbox dependency downloads failed DNS; approved retry downloaded
  locked dependencies and passed. No lockfile update was performed.
- Schemas are producer tooling only. Structural inventory validation is not
  cryptographic verification, promotion authorization or host qualification.

## Production staging and installed checks — T009, partial T010/T011

- Strict staging/archive implementation accepts selected matching native ELF or
  Mach-O pairs, checks selected architecture, executable versions, hashes/modes,
  notices/reference/completions and rejects unlisted payloads, secret-like names,
  helper executables, images and symlinks/hardlinks. Archives are deterministic,
  inventory-only and refuse an existing output. Private libraries need reviewed
  closure/license records; closure qualification is not inferred from this check.
- Added `scripts/build-release-candidate.sh` for explicit production Linux or
  macOS control builds and optional hash-verified source cache. Existing legacy
  candidate measurement behavior is preserved; `--release-stage` selects the
  stricter new archive interface.
- macOS arm64 production control C build: PASS (`build/release-control`,
  `GITSTORED_TESTING=OFF`, all native runtime integrations OFF). Jansson and SQLite
  were taken from existing `build/downloads` after checking both pinned hashes.
  Network configure attempts were interrupted only after identifying this task's
  exact own processes; no unrelated workload was stopped.
- `cargo build --workspace --release --locked`: PASS, version remains 0.1.0;
  release version synchronization belongs to pending T029.
- Selected ordinary production executable copies at
  `/private/tmp/gs-alpha1-pair.HA3ixW`; no services or user data copied.
  `GITSTORE_PACKAGE_PAIR` portable suite: 34 passed, two skips (native lifecycle
  requires a selected fixture; Debian comparator separately passed in Ubuntu).
  The archive smoke uses explicitly synthetic documentation and revision identity;
  it is not a release artifact, license review or provenance receipt.
- Production control daemon/Rust setup run with `--include-ignored`: four passed,
  covering unavailable-runtime negotiation, explicit connect-only no-side-effects
  and application-version mismatch refusal without socket replacement. Initial
  mismatch test failed due to its non-private temp directory; corrected to mode
  0700 and passed. No product permission checks were weakened.
- Native fd/memfd library loading, concurrent first-use Linux launch, clean-host
  package install and all other platform tuples remain not-run. No
  `GITSTORE_RUNTIME_FIXTURE` is selected. A dedicated non-root Linux fixture with
  cgroup-v2/namespaces/TUN and explicit disposable manifest is needed for the next
  native acceptance gate; Docker version-comparator runs do not substitute for it.
- T010/T011 remain unchecked until all of their required installed/staging
  integration checks are complete. No release/tag/package has been published.

## Replacement Linux fixture — 2026-11-08

User authorized a disposable replacement if the original Apple guest was deleted.
The Apple service was initially stopped; after starting it, inventory was empty
and direct lookup of `gitstore-m2-linux-a1a13c` returned not found. The cached
original image remained. New guest: `gs-cli-alpha1-linux-20261108`, Ubuntu 24.04
arm64, 2 CPUs/4 GiB, same pinned image digest
`sha256:0bf2f725e3ee269a2e804c3426e38270e43eaa41960add5ad0ef12e072181178`.
Inspect confirmed no host mounts, published ports/sockets or SSH forwarding.

Guest-root preparation installed build packages and created dedicated UID/GID
11000 with user manager/lingering and TUN group access. A runtime Delegate property
update was rejected by systemd; inspection showed cpu/memory/pids were already
delegated, so the preparation script now verifies them instead of mutating that
property. No host kernel control or existing workload was changed.

Read-only prerequisite checker PASS under UID 11000: private canonical user
runtime and session bus, cgroup delegation, TUN, actual user/network namespaces,
pidfd and openat2. Manifest: `/home/gitstorefixture/experiments/alpha1-fixture.json`,
owned single-link mode 0600. The earlier missing-fixture observation is resolved.

Scoped source snapshot SHA-256:
`234f91df1aefb314fb950b5785ac2946043e319617b75fb13018683da60a4213`;
cached native-source transfer SHA-256:
`1e836b51129d494d6231b339f230e2ea7fcc3360d40fd487042588a73de8a893`.
No Git metadata, host credentials/configuration or build outputs were transferred.
Rust 1.96.0 and production gs/gitstored native builds passed under UID 11000;
libcrun/libslirp/image/bootstrap integrations enabled, testing facilities disabled.
Installed ordinary pair: `/home/gitstorefixture/experiments/alpha1-installed/bin`.
Both report 0.1.0. Dynamic dependencies resolve on this development fixture;
that is not yet clean-host package or private-closure qualification.

## Restored native acceptance and library experiment

- First real production core lifecycle: PASS, one case, 45.408 seconds.
  `runtime-run-core-memdb-mugftj0z.json` under the fixture parent records
  `test_authorized_cold_start_requires_current_generation_health`; retained root
  `gs-public-runtime-a2bz5fv5`. Actual pinned service payloads started, reached
  current-generation readiness and stopped with owned cleanup under UID 11000,
  synthetic credentials and PATH=/nonexistent.
- The initial concurrent installed-link test failed: deployment list did not
  automatically start a manager (NOT_FOUND). Existing auto-start only covered
  deployment.start. `crates/gs/src/deployment.rs` now enables first-use manager
  setup for local commands unless an explicit socket was selected. This implements
  the agreed package behavior while keeping explicit sockets connect-only.
- Rebuilt native CLI and reran concurrent first-use through public executable
  links: PASS, one manager, private UID-owned socket/runtime. Exact manager handles
  were captured via pidfd and birth-time checks; teardown signaled only those
  handles. Retained root `package-autostart-j7vb6_na`. The first failed test root
  `package-autostart-9q84lavs` remains for evidence. No personal manager was used.
- Host Rust fmt/Clippy/workspace tests PASS after the behavior change. T010's
  test implementation is complete; full package/host matrix remains T024/T025.
- `runtime/packaging/probe-origin.py` renamed only a disposable libcap copy to
  prevent accidental resolution from system paths. Baseline production version
  command exited 0; relative `$ORIGIN/../lib` exited 127 after libcrun's sealed
  re-execution with the private library missing. Fixed private-library RPATH exited
  0 and returned the original version. Experiment root:
  `/home/gitstorefixture/experiments/origin-reexec-3owdhony`.
- T012 remains open: fixed-path distro packages have an observed viable lookup
  path, but movable archive/npm private-library loading still needs a qualified
  solution. Do not claim origin-relative relocation works or publish packages on
  the basis of development-host libraries. This failed alternative is preserved
  rather than waived; no install/runtime loader policy was weakened.

Final replacement-fixture executable SHA-256 values after the local-command
startup fix: gs `0cf21f505e5b446c6ea0f9bd8370cf6875fad29569649b7a08cf099efa2dd8ce`;
gitstored `91fa5c7ea0b6e38fed8a07638aa18e0c19f344b23f6e2ac6413debd3512d57ba`.
The dedicated user's test manager socket was absent after observed teardown;
the replacement Apple VM is retained running for further native work.

## Private static closure — follow-up

The relative-lookup failure is resolved for the daemon by statically linking its
private dependencies while retaining the system C runtime. This is not a bundled
loader or glibc. Ubuntu's curl archive pulls in unnecessary Kerberos/LDAP code;
the producer now builds HTTP(S)-only curl from authenticated Ubuntu source
`8.5.0-2ubuntu10.16`, with the exact three source hashes in
`packaging/curl-source.json`. `scripts/build-static-curl.py` applies that security
patch series through dpkg-source. A fresh run of that script passed as UID 11000.
The uniquely named private curl archive prevents earlier system library paths
from selecting the distro curl accidentally.

Jansson hidden visibility alone does not resolve static json-c symbol collisions.
The private Jansson target and consumers now rename its two overlapping public
names at compile time, without accepting duplicate symbols or changing json-c.
The rebuilt production daemon passed with final SHA-256
`fac3b8b3ad2cd860e9404a1fba8bca6cfd5c0b5f881fa8de8477874dd5d823bf`.
`runtime/packaging/closure.py` passed ordinary, pinned-fd and sealed-memfd startup
with PATH=/nonexistent, no loader environment overrides. ELF needs only
`libm.so.6`, `libgcc_s.so.1`, `libc.so.6` and `ld-linux-aarch64.so.1`.
Highest referenced GLIBC version: 2.38, within the selected 2.39 floor.

Real rootless core lifecycle passed again with this exact private-static daemon:
one case in 47.375 seconds, start/current-generation readiness/owned stop.
Receipt: `/home/gitstorefixture/experiments/runtime-run-core-memdb-y4s9kls1.json`;
retained root: `gs-public-runtime-9439leru`. The explicit alternate manifest is
`alpha1-static-fixture.json` in that same fixture parent.

CLI inspection found a remaining dynamic `libz.so.1`; release producer builds now
select `LIBZ_SYS_STATIC=1` and check **both** final binaries' native closure.
That rebuilt CLI pair and clean package lifecycle have not yet been qualified.
Static LGPL dependency source/relink assets and complete reviewed notices remain
required. `notices.py` rejects incomplete Rust/native coverage and missing LGPL
materials. Do not treat these implementation checks as a distributable release.

Generated clap reference and bash/zsh/fish completions pass isolated no-config,
no-credential tests; Rust fmt and Clippy pass. Portable package tooling: 42 cases,
37 passed and 5 explicitly skipped (native/selected-pair fixture requirements).
Debian/RPM/npm renderers and Homebrew formula rendering have controlled checks;
native package installs, full platform matrix and signed release remain pending.

## File ID slice — controlled result

T044 is implemented: alpha.9 dispatch validates FileBy before the legacy path;
an opaque ID needs no namespace. Legacy File ID reads remain unsupported, and
empty/mixed/unscoped name selectors fail before transport. Five focused Rust
selector tests pass. Public GraphQL envelope syntax lint passed with no issues;
collection lint scanned two requests with zero errors/warnings.

The actual CLI ran through the local Postman simulation: 42 controlled requests,
including File ID success and null/not-found responses. Saved collections printed
two requests/two assertions each, all passing. The run printed:

```text
PASS: actual gs CLI exercised 42 controlled GraphQL requests, failure scenarios, authoring/read/delete and zero-write gates.
```

Postman also printed `No authorization data found. Please use the postman login
command.` Local collection execution still passed; no cloud publication occurred.
This proves controlled client behavior, not shipped-image conformance (T057).
