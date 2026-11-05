# Configure local deployments

Local selection is independent of remote contexts and accounts. Flags override
`GITSTORE_DEPLOYMENT`, `GITSTORE_PROFILE` and `GITSTORE_PROFILE_VERSION`, then the
local selection file, then the registered `core` / `1` / `memdb` defaults. A
deployment name must be selected explicitly or saved in that file.

## Save local defaults

The optional file is `$XDG_CONFIG_HOME/gitstore/local.toml`; without XDG_CONFIG_HOME,
it is `~/.config/gitstore/local.toml` on Linux or
`~/Library/Application Support/gitstore/local.toml` on macOS.

```toml
version = 1
[selection]
deployment = "dev"
profile = "core"
profile_version = "1"
datastore = "memdb"
```

Use an owned mode-0700 parent directory and a regular mode-0600 file. Symlinks,
hard links and unknown fields are rejected. This file contains selection only;
service configuration, passwords, tokens and executable paths are not accepted.
Set `datastore = "scylladb"` for the explicitly selected persistent profile;
memdb remains the default.

Check the selected plan before starting workloads:

```sh
gs api config validate --deployment dev -o json
gs controller config validate --deployment dev -o yaml
```

Validation reports host requirements, selected image identities and unavailable
checks. It does not start workloads, create credentials or provision images.

## Daemon paths

The default Linux socket is `$XDG_RUNTIME_DIR/gitstore/gitstored.sock` (normally
`/run/user/UID/gitstore/gitstored.sock`). Its directory is mode 0700 and its socket
is mode 0600. Use `--daemon-socket /absolute/path/gitstored.sock` or
`GITSTORE_DAEMON_SOCKET` to connect to a different owned manager. Explicit socket
selection does not auto-start a manager.

State defaults to `$XDG_STATE_HOME/gitstore`, or `~/.local/state/gitstore` on Linux.
Do not delete or relocate live state to change selection. It contains ownership
records, retained volumes and verified assets.

The daemon reads optional `settings.json` from its state directory at startup.
It must be an owned mode-0600 regular file in the private state directory, with
no symlink or hard-link substitution. Settings changes take effect on the next
manager startup; stopping the manager alone leaves workloads running.

## Optional systemd user service

Normal `gs serve` can start its matching per-user daemon. To have systemd manage
the daemon instead, build with your actual installation prefix, install both
executables there, then inspect the generated unit's `ExecStart`:

```sh
install -Dm644 build/daemon-release/packaging/gitstored.service "$HOME/.config/systemd/user/gitstored.service"
systemctl --user daemon-reload
systemctl --user enable --now gitstored.service
```

The [source build guide](contributing/building.md) uses `$HOME/.local` as the
prefix. The unit grants the daemon a delegated CPU/memory/PID cgroup subtree;
it does not install libraries, download images or start a deployment. Inspect the
manager using `systemctl --user status gitstored.service`. Stop workloads with
`gs deployment stop` before deliberately changing their manager installation.
`systemctl --user stop gitstored.service` stops the manager and preserves live
workloads/ownership records; it is not a deployment stop. Do not run a second
manager against the same state directory.

## Credentials

The default bootstrap source is the OS credential store:

```json
{
  "schemaVersion": 1,
  "bootstrapCredentialSource": { "kind": "os-store" }
}
```

For a headless host, explicitly choose a protected directory:

```json
{
  "schemaVersion": 1,
  "bootstrapCredentialSource": {
    "kind": "protected-directory",
    "path": "/absolute/private/gitstore-credentials"
  }
}
```

Create the directory as the invoking user with mode 0700. Each deployment gets
separate owner-only bootstrap material derived from its installation, issuer,
deployment and purpose. Existing material is reused on restart. Keep this directory
with retained deployments; do not substitute another directory at the same path.
The OS store never silently falls back to files.

Remote GraphQL and Git credentials are configured separately in
[CLI configuration](configuration.md). Their credential-file/stdin flags are not
accepted by local lifecycle commands. The headless file backend has native Linux
fixture validation; the default OS-store local runtime path still needs separate
native-store validation.

## Offline images

Add absolute paths to private OCI image-layout directories for the core components.
Include `scylladb` when selecting the persistent profile:

```json
{
  "schemaVersion": 1,
  "offlineLayouts": {
    "api": "/absolute/private/images/api",
    "git": "/absolute/private/images/git",
    "controller": "/absolute/private/images/controller",
    "scylladb": "/absolute/private/images/scylladb"
  }
}
```

This can be combined with `bootstrapCredentialSource` in the same settings object.
Each layout must contain the pinned image for the selected profile and platform.
Only selected components are acquired; configuring a Scylla layout does not
select that datastore for a memdb deployment. Offline input authorizes only those bytes. Truncated, altered or wrong-platform
images fail verification; the daemon does not silently fall back to a registry.
No offline bundle export/import command is provided yet.
