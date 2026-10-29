# Agent Instructions

## Communication

- Be direct and concise. Lead with the answer; omit flattery, praise and filler.
- Evaluate proposed solutions on their merits. Point out concrete gaps, edge cases
  and simpler alternatives; do not default to agreement or invent objections.
- State disagreements with reasoning and reconsider when new evidence warrants it.
- Distinguish planned behavior, implemented behavior and verified release support.

## Repository and sources of truth

This repository defines the GitStore CLI (`gs`) and its future local runtime daemon
(`gitstored`). M0 contracts are documented. M1 has an executable development subset and implementation tasks. Check
[the evidence record](docs/m1-status.md), working tree and task state before claiming
completion, live compatibility or release support.

Read the relevant sources before changing behavior:

- [Constitution](.specify/memory/constitution.md): project principles and quality gates.
- [PRD](docs/prd.md): product scope and milestone exit criteria.
- [M0 contracts](docs/contracts/README.md): evidence and contract index.
- [Command matrix](docs/contracts/command-matrix.md),
  [CLI behavior](docs/contracts/cli-behavior.md) and
  [dependency register](docs/contracts/dependencies.md): operation boundaries and gates.
- [M1 specification](specs/002-m1-remote-cli/spec.md),
  [plan](specs/002-m1-remote-cli/plan.md),
  [tasks](specs/002-m1-remote-cli/tasks.md) and
  [validation guide](specs/002-m1-remote-cli/quickstart.md): current implementation scope.
- [Contributing](CONTRIBUTING.md): contribution and CI policy.

Explicit user instructions take precedence. Surface conflicts between project
artifacts rather than silently changing scope. Preserve unrelated working-tree edits.

## Architecture

- Implement `gs` in Rust and `gitstored` in C. The M1 plan selects a Cargo workspace
  with one package under `crates/gs`; those paths are planned until created.
- Resource operations use public GraphQL; repository clone/sync use public Git;
  local lifecycle operations use the daemon interface. Never fall back between
  write paths or call internal RPC, datastores or server repository files.
- Implement Git operations through a maintained library, never by wrapping C Git
  or another Git executable. The M1 plan selects Rust `git2` bindings to libgit2
  for clone/fetch/push; re-evaluate Gitoxide when its required push support exists.
  End users must not need a separately installed Git executable.
- Remote CLI commands must work without a daemon or runtime-asset downloads.
- The future Linux daemon embeds libcrun as a library and runs as the invoking
  user. Do not introduce a generic container engine or runtime work into M1.

## M1 scope and contract discipline

- Implement tasks in dependency order and mark only completed work checked.
  Keep specifications, contracts and implementation status consistent.
- The unblocked increment includes local context/configuration and credential
  operations, explicit GraphQL, core resource inspection, supported Product/Category
  creation and deletes, clone, and submission-only sync. Consult the specification
  for exact command subsets and output restrictions.
- Login/discovery, remote credential invalidation, Namespace/Repository authoring,
  safe author updates, watches, correlated waits and hydrated exports retain their
  named gates. A `supported-contract` row does not close its listed dependencies.
- Missing capabilities fail explicitly before side effects. Do not implement
  speculative endpoints or treat an arbitrary GraphQL request as proof that a
  corresponding high-level command is supported.
- Use committed upstream source evidence with its recorded revision. The M0/M1
  baseline is GitStore commit `b6c866421dbcf43994565f847553c1f6aec83994`.
  Do not use unrelated upstream working-tree changes as contract evidence or edit
  an upstream checkout as part of CLI work without authorization.
- Configure GraphQL and Git public endpoints independently; preserve proxy prefixes.
  The verified repository list operation is `repositories`. File currently reads
  through `file(namespace, name)`; adopt a normalized `by` selector only after the
  upstream File lifecycle specification ships and its public mapping is verified.
- Clone defaults to a remote named `gitstore`, permits `--remote <name>`, and tracks
  the checked-out branch when present. Infer sync targets by URL and installation,
  never remote name; ambiguous targets require explicit selection.
- Sync must not commit, reset, merge, rebase or force-push. Preserve local history
  and worktree; distinguish transport acceptance, admission and readiness.

## Credentials and automation

- Isolate credentials by installation, issuer, account and purpose. Git credentials
  and registry credentials are separate from GraphQL identities.
- Use the OS credential store by default; protected-file fallback is explicit.
  Never expose secrets in arguments, logs, diagnostics, fixtures or saved remote URLs.
- Only explicit `auth token` prints a token. Local auth status is not server-verified
  identity; local logout must report when remote invalidation is unsupported.
- Non-interactive execution never prompts or opens a browser. Reject ambiguous
  selection, unsupported formats and conflicting stdin consumers before writes.
- Keep requested results on stdout and diagnostics/progress on stderr. Follow M0
  machine-readable errors, exit categories, input bounds and no-color rules.
- Do not retry mutations blindly. Preserve partial results and known revision
  identity; report uncertain submissions honestly rather than claiming rollback.

## Verification and delivery

During documentation-only work, check local links, task/contract consistency and
`git diff --check`. Do not add application CI before executable targets exist or
claim that intended `gs` commands have been run.

Once the Rust workspace exists, run the relevant behavior checks and these commands
using the toolchain pinned by the implementation plan:

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo build --workspace --locked
```

- Add build/format/lint/behavior CI with executable targets. Preserve the existing
  metadata-only PR-title workflow; Markdown/workflow-manifest lint remains deferred.
- Use isolated temporary configuration, synthetic credentials and disposable Git
  repositories. Never let tests consume the user's real config or credentials.
- Follow the M1 validation guide for controlled HTTP/Git scenarios. Separate local,
  simulated, native credential-store and live evidence; none substitutes for another.
- Do not claim host/provider support or full M1 completion before its gates pass.
  Binary publication requires DEP-013 provenance evidence and the policy in
  [ADR 0003](docs/ADRs/0003-runtime-assets-packaging.md).
- PR titles follow Conventional Commits with the types listed in CONTRIBUTING.md.
  Report what changed, what was checked and any remaining material limitation.

<API_TASKS>
Before responding to any non-trivial API engineering task — designing, implementing, mocking, testing, monitoring, documenting, or deploying an API or Postman Flow — read `postman/skills/api-builder/SKILL.md` and follow it. It is the default entry point for API work in this repository. Pure questions and trivial one-line edits do not need it.
</API_TASKS>
