# Local Configuration and Credential Contract

## Bootstrap without login discovery

This increment consumes existing credentials; it does not offer login/import or
context-create commands. Users may create the following TOML config manually or
through their existing configuration/secret-management tooling. `--config <path>`
or `GITSTORE_CONFIG` overrides the default OS user-config path (`gitstore/config.toml`
under XDG config on Linux, `~/Library/Application Support/gitstore/config.toml` on
macOS). Relative explicit paths resolve from the invocation directory.

```toml
version = 1
current_context = "development"

[contexts.development]
endpoint = "https://store.example.test/team/graphql"
contract_profile = "m0-2026-10-27"
account = "operator"
namespace = "catalogue"
git_endpoint = "https://git.example.test/team"

[contexts.development.accounts.operator]
issuer = "configured-static-provider"
credential_source = "os-store"

[contexts.development.git_auth]
mechanism = "basic"
account = "operator"
issuer = "configured-static-provider"
```

The example endpoints are placeholders, not discovered or contacted during setup.
GraphQL endpoint is the full URL; append no guessed `/graphql`. Git root includes
any public reverse-proxy prefix; append encoded namespace/repository only. TLS is
required; `allow_loopback_http = true` is permitted only with a loopback address.
Unknown keys/version, embedded URL secrets and empty required values are errors.

## Selection

`context list` lists saved names and redacted identity summaries. `context show`
reports effective selection and field sources. `context use <name>` validates and
atomically persists the saved active context. `auth switch <account>` atomically
updates the selected account within that context; switching installations requires
explicit context selection. Neither command contacts a server.

`config validate` checks file shape, references, URL policy and conflicts without
reading/printing secret material or probing a server. It reports missing credential
configuration as such, rather than claiming authenticated status. `auth status`
checks local secret presence and reports `verified=false`; it does not make a remote
verification call while DEP-001 remains open. `auth logout` removes only the selected
CLI-owned OS-store credential and reports remote invalidation unsupported. It rejects
per-invocation credential overrides rather than deleting caller-managed external files;
unrelated GraphQL/Git identities remain untouched.

Selection flags/env are the M0 names. `--config`/`GITSTORE_CONFIG` select the file;
`--context`/`GITSTORE_CONTEXT`, `--endpoint`/`GITSTORE_ENDPOINT`,
`--account`/`GITSTORE_ACCOUNT`, `--namespace`/`GITSTORE_NAMESPACE`,
`--repository`/`GITSTORE_REPOSITORY`, `--ref`/`GITSTORE_REF` retain M0 precedence.
Deployment is outside the remote increment. Empty explicit values fail.

An endpoint override may use only a saved credential matching its normalized
installation/issuer/account or an explicitly supplied protected credential for this
invocation. It must not retain the old context's Git endpoint, compatibility profile
or credentials automatically. Require a matching saved installation for built-in
resource/Git commands; direct GraphQL may use the explicit protected credential.
No alias, hostname-only matching or proxy-prefix stripping.

## Protected credentials

Default store service is `dev.gitstore.gs`; entry identity is the canonical compact
JSON array specified in the data model. Provision existing entries through native
OS secret-store tooling or an existing secret manager. Do not document commands
that embed a secret as an argument. Default-store lookup never falls back silently.

`--credential-file <path>` explicitly supplies a raw GraphQL token from a protected
file; `--credential-stdin` reads it from stdin. These are mutually exclusive and
never persist the supplied value. Reject conflicting configured/explicit input
choices, unsafe files or additional stdin consumers before making requests.
`auth token` reads only the selected GraphQL credential and emits raw text; reject
`--output` for that command. Local status may report presence/source only.

Git material is separate. Config may explicitly opt into `credential_file` under
`git_auth`; its absence selects the OS store. This is an opt-in protected-file
source, not an automatic fallback. File shape is either `{ "username": "...",
"password": "..." }` for Basic or `{ "token": "..." }` for Bearer. No values are
stored in the TOML example, URL, arguments, logs or persistent Git configuration.
Supply this material directly through library credential/header APIs. No browser,
terminal prompt or external Git credential helper is allowed as a fallback.

## Atomic persistence

Create the config directory user-only where new; use a same-directory restricted
temporary file, flush, atomic rename and directory sync where supported. Preserve
unrelated saved contexts and unknown future versions by rejecting unsupported
versions before editing. Serialize mutation under an exclusive config lock and
reread under lock so concurrent context/account switches do not lose updates.
Tests use temporary files and injected storage failures, not the real user config.
