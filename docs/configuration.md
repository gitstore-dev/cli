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
containing a raw single-line token. The CLI never persists this override. Stdin can
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
