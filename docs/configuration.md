# CLI configuration

Start from [examples/config.toml](../examples/config.toml), replace the example endpoints
with public installation endpoints and save it under the OS user configuration directory:
`~/Library/Application Support/gitstore/config.toml` on macOS or
`${XDG_CONFIG_HOME:-$HOME/.config}/gitstore/config.toml` on Linux. Use `--config` or
`GITSTORE_CONFIG` for an explicit path. No context-create or login workflow is required.

Run `gs context list`, `gs context use development`, `gs context show` and
`gs config validate`. Flags override environment, then saved context, then defaults.
Empty values and ambiguous cross-installation selection fail. GraphQL and Git URLs
are configured independently, retaining proxy prefixes. HTTPS is required, except
explicit `allow_loopback_http = true` for a loopback development endpoint.

Selection changes use a locked atomic replacement and sync the saved file. Directory
sync is best effort only when the filesystem explicitly reports it unsupported. A
real directory-sync failure after replacement returns exit 1 (`CONFIG_DURABILITY`,
stage `persistence`) and states that the selection changed but crash durability is
unconfirmed; inspect `gs context show` before retrying.

Existing GraphQL credentials come from the OS store by default. Service is
`dev.gitstore.gs`; user key is compact JSON `[endpoint,issuer,account,"graphql"]`
with the normalized full GraphQL endpoint. Provision the entry using native OS
secret-store UI or your secret manager; never put secrets in shell arguments.
Linux uses the session Secret Service; macOS uses the user Keychain. An unavailable
store is an error, not an implicit plaintext fallback.

For per-invocation input use `--credential-stdin` or `--credential-file /protected/path`.
A file must be a regular non-symlink file owned by the user, mode 0600 or stricter,
containing a raw single-line token. Ordinary remote commands never persist this override. Stdin can
have only one consumer, so it cannot simultaneously supply credentials and a request.
`auth status` reports local presence only. `auth token` deliberately prints the token
and accepts no output option. `auth logout` deletes only the selected OS-store entry
and reports remote invalidation unsupported; caller-managed files are not deleted.

Git material has a separate key and audience. Its `git_auth` config selects Basic
(actual username/password) or Bearer. Optional `credential_file` is an explicit
protected-file source containing JSON username/password or token; otherwise the
OS store is used. A GraphQL token is never substituted as a Basic password.
See [the config contract](../specs/002-m1-remote-cli/contracts/config.md) for exact
identity fields. No daemon, browser login or Git executable is required.

For GitStore alpha.9, explicitly select
`contract_profile = "gitstore-alpha9-2026-11-05"` and configure the Admin endpoint
including `/admin/graphql`. The legacy `m0-2026-10-27` profile keeps its original
File selector. The alpha.9 profile supports File name or ID reads, full core
resource JSON/YAML reads/lists and exact Namespace/Repository creation inputs.
File IDs need no namespace. There is no File list/write mapping. These client
additions have controlled validation; shipped-version acceptance is still open.
Selecting the remote contract does not change local runtime image pins.

Use `gs read product NAME --authoring -o markdown` for a writable Markdown
projection. Product/Category bodies preserve Unicode and line endings. Export
omits controller status and server metadata; resolved reference identity fields
are read-only. Unknown/unrepresentable author fields fail explicitly, including
nonempty Namespace/Repository body. Extended Namespace push-policy groups and
Repository pushPolicy are not accepted create inputs; a read containing those
groups cannot silently become a lossless authoring export. Plain Markdown
presentation without `--authoring` remains unsupported.

`gs api-resources` and `gs explain product.spec.title` use local built-in
alpha.9 knowledge and require no configuration, credentials, network or daemon.
Use their explicit `--profile m0-2026-10-27` option to inspect legacy availability.
Their output includes the source revision, profile, field types, writable fields
and implemented operations. Selected context configuration is not consulted.
Arbitrary CRDs, server discovery and author updates remain gated. Typed name/label
watches are available in this explicit profile; see [watch behavior](watch.md).

For the shipped static-user provider, configure the selected account explicitly:

```toml
[contexts.development.accounts.operator.auth]
version = 1
provider = "static-users"
username = "operator"
```

The context must also select the alpha.9 profile. This mode extends the GraphQL
credential identity with provider, version and username; it does not reuse an
unqualified legacy entry or Git/registry credential. The OS store remains the
default. An unavailable or locked store fails without prompting or fallback.
Session changes hold a private per-identity lock beneath
`${XDG_STATE_HOME:-$HOME/.local/state}/gitstore/auth`. Concurrent operations on
the same identity fail explicitly; unrelated accounts use separate locks. These
locks coordinate CLI processes and do not provide transactional remote rotation.

```sh
gs auth login --username operator --password-file /absolute/private/password
gs auth refresh
gs auth logout --remote
```

Login also accepts redirected `--password-stdin`. It never prompts or opens a
browser and accepts no password argument. Competing stdin consumers fail before
network writes. With these three explicit session commands,
`--credential-file /absolute/private/session` selects a protected atomic session
destination/source; login creates it, refresh replaces it, and confirmed remote
logout removes it. Its parent must be private and user-owned. This is an explicit
fallback, never an automatic one. Default local logout still removes only the OS
entry and does not invalidate the remote session or delete caller-managed files.

Refresh consumes the old session token. A lost response or persistence failure can
leave rotation uncertain; do not retry blindly. Review the error and log in again.
Remote logout removes local material only after server confirmation. Auth results
contain no issued tokens; only explicit `auth token` prints a secret. Arbitrary
OIDC/device flows and discovery remain unavailable.
