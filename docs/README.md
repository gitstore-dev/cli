# GitStore CLI documentation

Use `gs` to work with a GitStore installation or run a local GitStore deployment.
This repository currently provides a source preview; published CLI/daemon binaries
are not available yet. See [building from source](contributing/building.md).

| I want to… | Guide |
| --- | --- |
| Select a remote installation and supply credentials | [Configuration](configuration.md) |
| Inspect resources, submit GraphQL, clone or sync a repository | [Remote commands](remote-cli.md) |
| Start, inspect or stop a local deployment | [Local deployments](local-runtime.md) |
| Configure local selection, credentials or offline images | [Local runtime configuration](local-runtime-configuration.md) |
| Build, test or change the implementation | [Contributing](contributing/README.md) |

Remote commands work without `gitstored`. Local execution currently provides the
core API/Git/controller profile with a default in-memory catalogue or explicit
retained Scylla catalogue. Deployment logs and registered standalone migration
are available in matching source builds. Native execution has been exercised on
Linux arm64; broader host support is still being validated. Optional
OIDC/Admin/Storefront components remain unavailable with named prerequisites.
