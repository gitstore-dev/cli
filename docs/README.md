# GitStore CLI documentation

Use `gs` to work with a GitStore installation or run a local GitStore deployment.
Install the matching alpha.6 CLI/daemon pair using
[Homebrew, APT, DNF or npm](installation.md), or follow
[building from source](contributing/building.md) for contributor builds.

| I want to… | Guide |
| --- | --- |
| Install the GitHub-hosted packages | [Installation and qualified hosts](installation.md) |
| Select a remote installation and supply credentials | [Configuration](configuration.md) |
| Inspect resources, submit GraphQL, clone or sync a repository | [Remote commands](remote-cli.md) |
| Observe typed future changes and resume a journal | [Typed watches](watch.md) |
| Start, inspect or stop a local deployment | [Local deployments](local-runtime.md) |
| Configure local selection, credentials or offline images | [Local runtime configuration](local-runtime-configuration.md) |
| Build, test or change the implementation | [Contributing](contributing/README.md) |

Remote commands work without `gitstored`. Local execution currently provides the
core API/Git/controller profile with a default in-memory catalogue or explicit
retained Scylla catalogue. Deployment logs and registered standalone migration
are available in matching binaries. Alpha.6 native core acceptance covers Ubuntu
24.04 and Fedora 44 on x86_64 and arm64; macOS supports remote/control commands.
The optional OIDC graph has separate development evidence; this release's host
qualification covers the core profile. Admin/Storefront remain gated.
