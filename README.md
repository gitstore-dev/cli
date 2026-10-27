# GitStore CLI

The command line interface and container runtime daemon for
[GitStore](https://github.com/gitstore-dev/GitStore).

**Status: product design.** This repository currently contains requirements and
documentation. The commands below describe the intended interface; no CLI or
daemon release is available from this repository yet.

## Components

| Component | Language | Purpose |
| --- | --- | --- |
| `gs` | Rust | Manage GitStore through its public GraphQL API, Git workflows, and the local daemon socket. |
| `gitstored` | C | Run GitStore service profiles: images, containers, networks, volumes, health, logs, and recovery. |

`gitstored` is a container runtime daemon with responsibilities comparable to
`dockerd`, limited to GitStore profiles. On Linux, `gitstored` uses **libcrun as a
library dependency**, calling its API rather than invoking a crun executable.
Static versus dynamic linking remains a packaging decision. It is separate from
the existing GitStore Git service, which owns server repositories.

The Admin UI is optional. CLI operations use the same server authentication,
authorization, admission, and resource contracts.

## Intended first use

The goal is to install GitStore and run:

```sh
gs serve
```

GitStore should start its per-user daemon, prepare the default profile, provision
verified runtime assets and images, generate local credentials, enroll the
controller, and report ready endpoints. Normal operation must not require `sudo`.
The default local profile uses an ephemeral in-memory datastore; repository data
uses retained volumes. Persistent datastore profiles are configured separately.

Docker, Compose, make, a compiler, and separately installed crun/CNI plugins are
not prerequisites for this intended path. Runtime components are shipped or
provisioned by GitStore. Keep the shipped binaries small: download large or
optional assets, such as service images and VM kernels, on first use of the
selected profile rather than embedding them in the executables. Cache verified,
versioned assets for subsequent runs. Interactive setup explains required
downloads; automation must explicitly authorize provisioning and never prompt.
Downloads and host capabilities are still required; offline assets must be
supported. This follows the first-use kernel provisioning model shown by Apple's
`container`.

This installation experience is not implemented yet. To run GitStore today, see
the existing [quickstart](https://github.com/gitstore-dev/quickstart).

## Platform direction

| Platform | Intended local execution |
| --- | --- |
| Linux amd64 and arm64 | Embedded libcrun with bundled rootless networking and user-owned volumes. |
| Apple silicon on supported macOS | Evaluate Apple's [container](https://github.com/apple/container) backend and packaging. |
| Other supported macOS configurations | A managed Linux VM with the Linux backend, if validated; crun cannot execute Linux containers directly on macOS. |
| Windows | Deferred; a future Linux-container backend requires WSL2 or another Linux VM. |

Apple currently documents Apple silicon and macOS 26 support, and its standard
installer requests administrator permission. Packaging and installation-time
permissions must be resolved before claiming a seamless macOS experience.
Linux rootless execution also depends on host namespace, UID/GID mapping, and
cgroup support. Unsupported hosts must receive actionable diagnostics rather
than silent privilege escalation.

Remote GraphQL/Git operations do not require a local container daemon.

## Intended commands

```sh
# Authenticate to the selected installation.
gs auth login
gs auth status

# Work with Markdown documents containing YAML frontmatter.
gs create -f product.md
gs read product summer-shirt -o yaml
gs list product -o table
gs read product summer-shirt -o markdown
gs update -f product.md
gs watch product

# Synchronise the current repository through Git.
gs repository sync

# An explicit repository name overrides inference.
gs repository sync catalog

# Execute GraphQL directly using the selected credentials.
gs api graphql -F query=@query.graphql

# Operate local service profiles.
gs serve --datastore scylladb --detach
gs deployment list
gs deployment inspect <deployment-id>
gs deployment logs <deployment-id>
gs deployment stop <deployment-id>
```

Resource verbs use GraphQL; repository synchronisation uses Git. GraphQL writes
to Git-backed resources still use server-managed Git persistence. A successful
push does not imply successful admission or completed reconciliation.

The repository argument is optional. Infer it from the current Git repository
and its configured remote; require an explicit target when inference is missing
or ambiguous.

## Output formats

- `json`: structured results for automation.
- `yaml`: structured resource export, preserving Markdown in top-level `body`.
- `table`: concise resource summaries.
- `markdown`: resource body rendered in the terminal; plain Markdown when redirected.

Select a format with `-o` or `--output`. Rendered Markdown is a presentation, not
a complete resource export. Authoring export must preserve YAML frontmatter and
the raw Markdown body separately. Progress and diagnostics use stderr.

## Design and tracking

The [M0 contract baseline](docs/contracts/README.md) records the command matrix,
upstream prerequisites, shared client behavior, daemon protocol and acceptance review.
It is a design baseline; it does not provide runnable CLI or daemon binaries.

Runtime decisions:

- [Daemon protocol](docs/ADRs/0001-daemon-protocol.md)
- [Runtime ownership and recovery](docs/ADRs/0002-runtime-ownership.md)
- [Runtime assets and packaging](docs/ADRs/0003-runtime-assets-packaging.md)
- [Rootless networking and volumes](docs/ADRs/0004-rootless-network-volumes.md)
- [Platform backends and validation](docs/ADRs/0005-platform-backends.md)

The [product requirements](docs/prd.md) define the command matrix, transport
rules, authentication, runtime architecture, embedding tradeoffs, storage,
delivery milestones, and release acceptance criteria.

- [Issue 1: hydrated export](https://github.com/gitstore-dev/cli/issues/1)
- [Issue 2: CLI-first management](https://github.com/gitstore-dev/cli/issues/2)
- [Issue 3: authentication discovery and lifecycle](https://github.com/gitstore-dev/cli/issues/3)
- [Issue 4: commands and service lifecycle](https://github.com/gitstore-dev/cli/issues/4)

## License

This repository is [MIT licensed](LICENSE). Bundled runtime dependencies retain
their own licenses and distribution requirements.
