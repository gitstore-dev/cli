# M1 Validation Guide

**Status**: Development validation guide. The workspace and commands now exist;
[the evidence record](../../docs/m1-status.md) distinguishes actual local/controlled
checks from unverified native, live, hosted and release behavior.

## Prerequisites

Development requires the pinned Rust toolchain, libgit2/TLS build dependencies and
Postman CLI with local simulation support for controlled HTTP tests. No Docker,
GitStore daemon or production credentials are required for the offline/local suite.
Native credential-store tests need a disposable user-store entry on the tested host.
Use temporary config/state directories, synthetic secrets and disposable repositories.

## Build and local-only journey (US1)

Local validation (T001–T016):

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo build --workspace --locked
cargo run -p gs -- --help
cargo run -p gs -- --version
```

Create a temporary version-1 config from [the config contract](contracts/config.md)
using `.test` endpoints and synthetic OS-store entries or explicit protected files.
Run context list/show/use, auth switch/status/logout/token and config validate. Verify:

- Local commands issue zero network requests and require no daemon.
- Flags/environment/context precedence and source reporting match the contract.
- Invalid selection and injected persistence failure retain prior config.
- Same host with different proxy prefix does not reuse another installation's secret.
- Only `auth token` emits raw token text; output flags there fail before reading secrets.
- Local logout removes only the selected stored identity and reports remote invalidation
  unsupported; it never deletes an external credential file.
- Native-store unavailable/denied is distinguishable from absent; no fallback occurs.

Tests `local.rs` use a credential adapter; separately record a real OS-store smoke
check so adapter tests do not imply native backend validation.

## Controlled remote journey (US2)

For T017–T028, `scripts/test-remote.sh` starts the checked-in
Postman simulation, waits for its actual listening event, invokes the real `gs`
binary, asserts exit/stdout/stderr and recorded upstream requests, then stops only
that simulation process. Inspect `postman simulation run --help` at implementation
time for supported flags; do not invent a runnable simulation command in this plan.

Use the committed contract requests/fixtures for successful lookup/pages, create
and delete, HTTP 200 GraphQL errors/partial data, auth denial, missing object, rate
limit, timeout, malformed/oversized response and disconnected mutation response.
Verify exact operation names (`repositories`, not `repositorys`), selectors, input
projection, deadline/retry bounds, one-page output and absence of credential leakage.

Input cases cover literal `-f` versus typed/file `-F`, complete JSON input, duplicate
keys, unnamed multi-operation documents, unsupported subscriptions, multiple stdin
consumers and 16-MiB limits. Author cases cover 100/101 documents, invalid later input,
Unicode and all trailing-newline variants, output-only fields and conflicting namespaces.
Preflight failures must leave the simulation with zero mutation requests.

Gated commands/options (including YAML/Markdown resource export, author update,
Collection mutation, File list, watch, login and verified waits) must return the
expected unsupported category before a network write. Unknown spelling remains usage.

## Disposable Git journey (US3)

For T029–T035, tests create a bare origin plus independent working
copies, then expose a disposable public Smart HTTP fixture under a configured proxy
prefix. Filesystem-only fixtures prove ancestry/tracking behavior; Smart HTTP fixtures
also prove endpoint/auth/transport behavior. Record those evidence levels separately.

Run clone with default and overridden remote names and an empty repository; inspect
actual remote config and branch upstream. Reject nonempty destinations without changes.
Exercise sync from a subdirectory with no change, local ahead, behind, divergent,
dirty, detached/explicit ref, multiple remotes, pushurl rewrite, incorrect endpoint,
misleading remote name and concurrent remote advancement. Assert local branch and
worktree are unchanged after every rejection and only a normal fast-forward is pushed.

Test both supported auth mechanisms using synthetic credentials and ambient hostile
Git configuration. Drive the in-process library client with external helper/filter/hook
execution disabled. Secrets may not appear in saved URLs/config or diagnostic
traces; no browser/prompt/global-helper fallback. A GraphQL token as Basic password
must never be constructed by the client. Repeat CLI Git scenarios with no Git executable
available; create/inspect fixtures via the library and check that no helper or Git
process is spawned. Loss of a push response leaves known revision
and submission uncertainty, without claiming rollback or automatic retry.

## Live compatibility and release evidence

Only an explicitly configured disposable installation and test account may be used.
The opt-in live harness created in T037 confirms each included scenario and permission
boundary without touching unrelated resources. Existing Namespace/Repository resources
are prerequisites; login/create/export/wait release checks remain outside this subset.
A simulated server cannot establish live admission, real provider compatibility or
native credential-store support. Record live checks as not-run when access is absent.

Record compiler/git2/libgit2/TLS versions, host/architecture, profile/schema evidence, native-store
results, simulated request traces and live results in `docs/m1-status.md`. The initial
host matrix is Linux x86_64 and macOS arm64; other architectures remain unadvertised.

Do not publish binaries until DEP-013/ADR 0003 evidence verifies exact digest,
producer/workflow/source identity, tampering/wrong-identity/missing-evidence failures
and applicable offline trust behavior. No release workflow or credential is created
by this planning pass. Full PRD M1 exit criteria remain gated.
