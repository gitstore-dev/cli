# Research: M1 Remote CLI

**Date**: 2026-10-29. Decisions concern the unblocked increment, not full M1 release.

## R1 — Deliver restricted subsets, keep M0 gates

**Decision**: Include the specification's 30 records/subsets. Require existing
credentials and Namespace/Repository resources. Keep Namespace/Repository create,
login/refresh/remote revocation, safe author updates, watches and hydrated export excluded.

**Rationale**: Supported-contract rows may still name an unresolved option or release
prerequisite. Product create is usable with server-selected placement; sync is usable
without correlated waits or live-streaming claims. This does not resolve DEP-003/004/016.

**Alternatives considered**: Treating every supported-contract row as fully shippable
would erase gates. Including only dependency-free rows would discard useful local
status/logout and submission-only sync. Implementing upstream discovery is outside this request.

## R2 — One Rust package and a synchronous remote CLI

**Decision**: One Cargo workspace/package, Rust 2024 pinned initially to 1.96.0;
clap/serde/TOML/url with reqwest's blocking API and rustls. Commit exact dependency
resolution with the first executable target.

**Rationale**: The included commands need bounded requests, not watch or daemon event
loops. Reuse one HTTP client per invocation with redirects disabled, explicit deadline,
response bound and retry policy. The official [reqwest documentation](https://docs.rs/reqwest/latest/reqwest/)
describes both blocking support and proxy handling; [redirect policy](https://docs.rs/reqwest/latest/reqwest/redirect/struct.Policy.html)
allows redirects to be disabled. Disabling redirects prevents credential forwarding
and base-path changes rather than attempting heuristic cross-host trust decisions.

**Alternatives considered**: Async runtime for future watches adds no current benefit;
a custom HTTP/TLS client duplicates maintained infrastructure. Generated OpenAPI is
inappropriate for this GraphQL interface.

## R3 — Protected existing credentials and explicit bootstrap

**Decision**: Versioned user TOML config; atomic context/account selection. Credential
identity includes normalized installation base path, issuer and account. Read OS store
entries by default through a narrow adapter; explicit user-only file/stdin input is a
per-invocation override. Git secrets use a separate identity/purpose and mechanism.

**Rationale**: Login discovery remains blocked, but existing credentials can make the
unblocked client usable. [Keyring's current guidance](https://docs.rs/keyring/latest/keyring/)
recommends keyring-core plus specific stores for applications controlling backend
selection. Use native Apple Keychain and Linux Secret Service access, not an implicit
plaintext or transient kernel-keyring fallback. Headless systems can explicitly select
protected files. An unavailable store is an actionable error, not permission to downgrade.

**Alternatives considered**: Automatically importing environment tokens or storing
secrets in TOML risks leakage. An auth-import command or context-create family expands
the agreed command surface unnecessarily. Native OS tooling or a secret manager may
populate the documented OS entry; per-call protected input needs no import command.

## R4 — Strict frontmatter and operation parsing

**Decision**: Use serde-saphyr for YAML and [cynic-parser](https://docs.rs/cynic-parser/latest/cynic_parser/) for executable GraphQL syntax.
Read bounded UTF-8 bytes first; slice Markdown body without parse/render normalization.
Configure duplicate-key rejection, strict boolean handling and parser resource budgets;
disable includes, environment interpolation and custom tag evaluation.

**Rationale**: Typed YAML cannot silently drop unsupported author fields. The
[serde-saphyr documentation](https://docs.rs/serde-saphyr/latest/serde_saphyr/)
describes duplicate-key policy, strict booleans, parsing budgets and a Rust 1.89 minimum,
compatible with the chosen toolchain. GraphQL AST inspection distinguishes selected
queries from mutations and multi-operation documents without a fragile keyword test.
Input JSON uses duplicate-detecting deserialization, not serde_json map overwrite.

**Alternatives considered**: Regex GraphQL classification fails on comments/fragments
and operations. Splitting Markdown on arbitrary `---` corrupts valid body text. Reusing
an unmaintained YAML dependency without review is unnecessary.

## R5 — Maintained Git library, no executable wrapper

**Decision**: Use Rust `git2` bindings to libgit2 for repository discovery/status,
clone, fetch, ancestry checks, remote configuration and push. Pin the exact crate,
libgit2 and TLS dependency identities at implementation. Package native dependencies;
end users need no Git executable. Do not shell out to C Git, `gix`, or credential helpers.

**Rationale**: The user explicitly requires a library integration. Gitoxide's
[crate status](https://github.com/GitoxideLabs/gitoxide/blob/main/crate-status.md)
still lists push/send-pack as unfinished (checked 2026-10-29), so selecting it alone
would leave sync unimplemented. The maintained [git2 Rust bindings](https://github.com/rust-lang/git2-rs)
provide a library API to libgit2; [remote APIs](https://docs.rs/git2/latest/git2/struct.Remote.html)
cover fetch/push and [callbacks](https://docs.rs/git2/latest/git2/struct.RemoteCallbacks.html)
cover credentials, progress and per-ref push status. libgit2 is a C library, not the
C Git executable; this remains a Rust CLI with an explicit native dependency.

Implementation verification found that libgit2's built-in redirect policy permits
same-host redirects. Use a small reqwest subtransport for libgit2's smart-protocol API:
scoped Basic/Bearer authentication in memory, all redirects rejected, absolute request
deadlines, TLS verification retained, streamed responses and disk-spooled request packs.
Libgit2 continues to implement negotiation, packs and ref updates; no Git protocol
implementation is duplicated. Callback progress is redacted, including secrets spanning
callback boundaries. Per-ref rejection is checked separately from transport success.

**Alternatives considered**: Gitoxide is preferred for a future pure-Rust integration
when required push support is proven; adding it alongside libgit2 now duplicates the
Git stack. Wrapping C Git is disallowed. Implementing send-pack from scratch would
expand scope and replace maintained protocol infrastructure with custom code.

## R6 — Controlled HTTP evidence and no planning-time workspace mutation

**Decision**: Reuse committed upstream schemas and derive local runnable requests,
examples and a Postman simulation during implementation. Run actual `gs` requests
through that simulation for HTTP behavior/failures; use disposable Git repositories
and a public Smart HTTP fixture for the Git path. No production service is called.

**Rationale**: There is no existing API workspace/collection in this checkout.
Planning adds no runnable consumer, so initializing a workspace now would not validate
anything. Implementation tasks load Postman bootstrap/schema/mocking/simulation skills
before creating their artifacts, inspect the installed CLI, reuse any owning-team
contract if available, and avoid publishing unrelated cloud resources. Controlled
responses prove client handling, not server admission, live auth or release support.

**Alternatives considered**: Success-only stubs hide timeout/partial-error/isolation
failures. A second OpenAPI schema would duplicate the GraphQL source of truth.

## R7 — Development CI and publication gates

**Decision**: Add build/format/lint/behavior CI only with the Cargo targets. Keep
publication separate and disabled until the first M1 provenance gate is proven.

**Rationale**: The constitution requires meaningful executable checks; existing
metadata-only PR-title validation remains unchanged. DEP-013 and ADR 0003 require
trusted producer/workflow/source identity and exact digest evidence before publication.
An ordinary development build is not a signed release and need not invent a release
workflow now. Record release blockers rather than calling the full M1 exit criteria met.

**Alternatives considered**: Marking DEP-013 resolved from a checksum or successful
build is false evidence; adding release credentials to untrusted PR builds is unsafe.

## Evidence boundaries

Public operation evidence is read from upstream committed snapshot
`b6c866421dbcf43994565f847553c1f6aec83994` in the local GitStore repository, not its
modified working files. Exact schema/route findings are captured in
[contracts/upstream.md](contracts/upstream.md). External library links were checked
2026-10-29; implementation must recheck features/advisories before pinning.
No unresolved architectural choice requires user input. Live compatibility, OS-store
integration, supported-host and provenance validation remain implementation/release work.

## Implementation evidence

The generic Linux keyring adapter's locked-item search invokes Unlock; production
uses its maintained underlying `dbus-secret-service` APIs directly to refuse locked
matches and never display a prompt. macOS disables user interaction before Keychain
access. RustSec review prompted replacing the initial zbus stack; the resolved stack
has no reported vulnerabilities or maintenance warnings at the recorded audit time.
Actual checks and unverified environments are recorded in `docs/m1-status.md`.
