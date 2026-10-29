# M1 Development Implementation and Evidence

**Recorded**: 2026-10-29. **Scope**: the unblocked increment in
[specs/002-m1-remote-cli](../specs/002-m1-remote-cli/spec.md), not full PRD M1 release.

The 30 command records/restricted subsets have development implementations in the
Rust workspace. [Tasks](../specs/002-m1-remote-cli/tasks.md) track code and applicable
checks. [Remote command documentation](remote-cli.md) lists supported behavior;
[configuration](configuration.md) supplies existing-credential bootstrap.

## Checks and environment

Local host: macOS 26.6.1, arm64. Rust 1.96.0, git2 0.21.0, libgit2 1.9.7,
reqwest 0.13.5 with rustls, Postman CLI 1.75.0. The exact dependency resolution is
`Cargo.lock`; [license inventory](dependencies.json) covers all resolved platforms.
No server working-tree changes or production credentials were used.

| Check | Evidence / result |
| --- | --- |
| Locked Rust build, rustfmt, Clippy with warnings denied | Pass locally |
| Rust unit/process/loopback suite | 46 tests pass; native-store and published-image checks intentionally ignored in the default suite |
| Real `gs` through Postman simulation | 38 CLI requests pass, including real serialization, read/list/create/delete, ordered partial batch failure, no-write gates and transport failures |
| Saved Postman collection | 2 requests and 2 assertions pass; zero failures |
| Postman collection lint | 2 request files; zero errors/warnings |
| RustSec audit | Zero reported vulnerabilities and maintenance warnings at review time |
| Dependency license inventory | 266 resolved packages; all have license identifiers. This is not a release notices/packaging approval |
| CI configuration | Linux x86_64/macOS arm64 build/format/lint/tests; controlled HTTP suite and published-image Compose target on Linux. Hosted jobs not run in this session |
| Native Keychain/Secret Service smoke check | Not run; explicit ignored test uses a disposable synthetic entry when opted in |
| Published GitStore images | Pass locally against isolated `:latest` memdb stack: bootstrap, resource operations, clone/no-op sync/push/reclone; see image evidence below |
| External provider compatibility | Not run |
| Release provenance / publication | Not run; no binary published, no release credentials/workflow introduced |

The checked-in HTTP simulation is derived from the committed public contract at
`b6c866421dbcf43994565f847553c1f6aec83994`, with source hashes recorded under
`crates/gs/tests/fixtures/upstream/`. Synthetic transport/error probes do not establish
new public fields or server guarantees. No owning-team mock was retrieved: no owning
workspace is bound and the local collection run was unsigned-in. Nothing was pushed
to a cloud workspace.

## Important implementation evidence

- Local commands use isolated temporary config in tests. Atomic-write failure and
  concurrent switches preserve valid state; flags/environment/context source reporting
  and proxy-prefix isolation are exercised. Explicit token output is tested separately
  from redacted status/diagnostics and unsafe-file rejection.
- GraphQL tests preserve partial response data/errors/extensions while returning a
  nonzero category. Queries have bounded transient retries; mutations are not retried.
  Malformed/oversized response, 401/403, rate limit, timeout and lost-response scenarios
  exercise the actual process/HTTP client. Invalid later author input submits nothing;
  a remote batch failure leaves accepted/unknown/not-submitted outcomes explicit.
- Authoring covers exact Unicode/blank-line/trailing-newline body content, typed
  nested inputs, duplicate/unknown/controller-owned fields, namespace conflict and
  document-count bounds. JSON inspection is not advertised as hydrated export.
- Git tests run the CLI with no Git executable on PATH, create repositories through
  libgit2, and serve controlled upload-pack/receive-pack responses. They cover clone
  naming/tracking/empty destination, clean/no-op/fast-forward/behind/divergent history,
  subdirectories, detached and saved refs, ambiguous/mismatched targets, concurrent
  advancement, per-ref rejection, redirect/timeout/auth failures and hostile ambient
  configuration. Lost push response can follow a committed fixture ref; the client
  retains the revision and reports unknown submission without retrying.
- A small reqwest subtransport supplies HTTP I/O to libgit2's maintained smart protocol.
  This fixes the same-host redirect exception in libgit2's built-in policy and enforces
  absolute deadlines. Libgit2 still owns Git negotiation, packs, objects and ref updates;
  no Git executable or custom send-pack implementation is used. Outgoing packs spool
  to an owner-only temporary file; incoming responses stream to the library. Progress
  redacts credentials even when split across callback chunks.
- Linux Secret Service access refuses locked/ambiguous entries without invoking an
  unlock prompt. macOS disables Keychain UI interaction. These code paths still need
  native OS-store smoke and supported-host evidence; file input is an explicit option,
  never an automatic fallback.

## Success criteria and release boundaries

| Criterion | Result / limit |
| --- | --- |
| SC-001: local usability/isolation | Pass for tested local/file-input cases; native-store behavior not claimed from mocks |
| SC-002: remote public-operation behavior | Controlled scenarios and isolated published-image acceptance pass; external providers remain unverified |
| SC-003: deterministic failures/no secret leakage | Pass for exercised local/controlled invalid-input, selection, auth, partial-error and gate cases |
| SC-004: clone/sync safety | Pass in disposable library-backed Smart HTTP fixtures, without Git executable |
| SC-005: truthful outcomes | Pass in exercised API/Git cases; waits remain unsupported before writes |
| SC-006: checks/documentation/provenance | Development checks and scope documentation pass; release provenance remains open |

The full milestone still needs discovery/advertised auth flows (DEP-001/006), safe
updates (DEP-009), Namespace/Repository authoring and export fidelity (DEP-005),
Collection mutations (DEP-002), watch conformance (DEP-017), correlated waits (DEP-004)
and verified streaming claims (DEP-016), as applicable. File lookup remains
`file(namespace,name)` until the upstream File lifecycle specification ships the
normalized `by` contract. No new File list/write/lifecycle operation is inferred.

Before publication, DEP-013/ADR 0003 requires exact digest-bound producer/workflow/
source verification and tampering/wrong-identity/missing-evidence/offline-trust cases.
An ordinary local build does not close that gate. Linux/native-store/real HTTPS and
live-provider support must be validated before advertising those release guarantees.

Use `scripts/test-live.sh --help` for explicit opt-in fixture-only checks. It requires
an existing disposable `gs-m1-*` namespace and protected test credentials. It is not
part of default CI and never creates a commit or makes a live Git push.


## Published-image integration evidence

On 2026-10-29, `scripts/test-integration.sh` passed against the three published
`:latest` images, all labeled with source revision
`b1b26caa2ec7b0267ba3e74f50f592229b7e9592`:

| Image (`ghcr.io/gitstore-dev/`) | Resolved digest |
| --- | --- |
| `api` | `sha256:d333cd40d1dc2c240311381395f51562985a0530e199687707d4c74f94be9323` |
| `git-service` | `sha256:9dacabc1516ff747d5aba7b5fb352226c438fc385f6c264fb381081563d26a0e` |
| `controller-manager` | `sha256:678ccfc05a6bd4c678d67c343b7ee909ebaadc6d8d57fa85566b0778e22d23cf` |

Docker Desktop on the macOS arm64 host ran the isolated Linux containers. Bootstrap
used generated credentials, image-native key generation/controller enrollment, and
public GraphQL Namespace/Repository creation. Actual CLI resource checks and the
ignored image test passed, including a library-seeded Git repository, default/override
remote names, branch tracking, no-op sync, accepted push, and reclone verification.
Compose removed all project containers, volumes and its network after the run.

[Run instructions and coverage](../tests/integration/README.md) distinguish this local
image evidence from the Ubuntu GitHub-hosted workflow, which has not run in this session.
The image source revision differs from the M0 source baseline; this records exercised
compatibility and does not rebaseline the public contract or close release gates.


The published-image suite now reports six independent Git scenarios and 30 named
command cases rather than one combined Git test and an aggregated live script.
[The coverage table](../tests/integration/README.md) records which other CLI commands
run against images and which remain covered by controlled/local/native-store suites.
Each Git case owns a distinct server repository; command cases isolate configuration
and authored resources. A failure does not stop remaining cases or the other suite.

The expanded 36-case suite passed locally against the same published image digests
on 2026-10-29; project containers, volumes and network were removed afterward.


Automated-review regressions cover malformed GraphQL envelopes with empty errors
and absent data, valid null-data/error-only responses, unsupported versus genuine
post-replacement directory-sync errors, and final Git progress flushing. The progress
checks include a real CLI clone receiving a sideband message ending with a credential
prefix, while retaining complete-credential redaction and escape sanitization.
The local suite passes 46 Rust tests and 38 controlled CLI HTTP requests plus the
saved collection's two assertions. Native-store and image checks remain opt-in.
