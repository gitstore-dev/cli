# EXP-003B: image adapter evidence

Recorded 2026-11-04 on the explicitly selected disposable Linux arm64 fixture
`gitstore-m2-linux-a1a13c`, UID 11000. These are development checks, not a host
support or publication claim. The Apple VM service is fixture infrastructure;
installed GitStore invokes no container engine, Git executable or registry helper.

The public `gs` → Unix dispatcher → private Rust image library → libcrun/libslirp
path passed all six `Assets` cases through `scripts/test-runtime.sh --scenario
exp-003b --fixture /home/gitstorefixture/experiments/public-runtime-fixture.json`.
The retained result is `/home/gitstorefixture/experiments/runtime-run-exp-003b-sk30onih.json`
(163.408 seconds for the test cases). Sources are the registered immutable core
profile pins, complete verified arm64 OCI layouts, and complete verified amd64
layouts for the wrong-platform case. No upstream attestation is required.

| Case | Observed result |
|---|---|
| No inputs or download permission | `PROVISIONING_REQUIRED`; no assets or credentials |
| Verified offline import | Actual core readiness; owned reverse stop |
| Wrong-platform offline input | Failed unready start; no active assets |
| Truncated API blob | Failed verification; no API activation or credentials |
| Warm restart | Higher current generation, unchanged cached payload hashes and credential hash |
| Tampered warm compressed blob | Recorded failed unready restart; damaged bytes never execute |

Each case retains its isolated configuration, metadata and data. Positive tests
run the actual published service payloads; no test backend manufactures readiness.
The wrong-platform fixture was acquired as bytes only through a test-only probe;
production native-architecture checks remain unchanged.

The earlier registry-backed warm/tamper setup failed its 60-second readiness
observation twice. Keep these failures as failures:
`runtime-run-exp-003b-e9yayktc.json` and `runtime-run-exp-003b-3iatmdgk.json` in the
same fixture parent. The latter assertion log is `assets-latest.log`. Cache tests
now use explicitly selected offline inputs to isolate cache behavior. This does
not establish the cause of, or resolve, the registry timing failures. Prior cold
registry success is recorded separately in `provisioning/bootstrap-evidence.json`.

C/native behavior checks pass 37/37, including asset records, bounded workers,
atomic promotion and fresh tree verification. The explicit `exp-003b-sdk` runner
also passed, record
`/home/gitstorefixture/experiments/runtime-run-exp-003b-sdk-tsfbsd4u.json`:

- Thirteen actual SDK unit/security checks cover malicious symlink/hardlink and
  parent traversal, special-permission stripping, merged-byte quota, offline
  platform/quota/source containment, corrupt layers and verified partial reuse.
- The synthetic private registry uses fresh maintained clients for anonymous,
  wrong-password and wrong-account denial. Destinations stay empty on failure.
- Expired bearer token and issuer-denied repository pull scope both fail before
  destination writes. The issuer confirms it saw `repository:fixture:pull`.
- Authorized streaming, config/layer digest checks, merge/unpack, wrong platform,
  transfer quota and restarting a partial blob without a range validator pass.
- Local TLS peers exercise the actual production HTTPS origin/redirect policy.
  An approved different-port destination receives no Authorization header;
  downgrade, unapproved origins and more than five redirects fail. The test-only
  constructor adds only those fixture CA certificates; no product trust root or
  C ABI option changes.

The synthetic registry is developer tooling rebuilt from locked Go modules, not
an installed dependency. A stale earlier fixture was rejected for incompatible
metadata. Its replacement and the SDK executable are copied/hash-pinned by the
runner. Test-only HTTP acquisition injection is distinct from the selected TLS
policy test. These checks establish controlled failure handling; real private
provider compatibility is not claimed.

Actual filesystem capacity/ENOSPC checks pass separately through `exp-004b-disk`;
see [volume evidence](local-volumes.md). Parent-crash resume, safe eviction of
abandoned staging, live full-disk recovery and real registry timing remain later
recovery/clean-install gates. None is declared complete by these experiments.
