# GitStore CLI

`gs` manages [GitStore](https://github.com/gitstore-dev/GitStore) through its public
GraphQL and Git interfaces. Its companion daemon, `gitstored`, runs local GitStore
deployments on Linux.

Install the matching `gs` and `gitstored` pair using
[Homebrew, APT, DNF or npm](docs/installation.md). The
[alpha.5 prerelease](https://github.com/gitstore-dev/cli/releases/tag/v0.1.0-alpha.5)
is available for Linux and macOS; macOS supports remote/control commands only.
Upstream schemas change incompatibly. Upgrade compatibility, data preservation
and recovery are not guaranteed; keep independent backups. Contributor builds
are covered by [building from source](docs/contributing/building.md).

## Connect to GitStore

[Configure an installation and credentials](docs/configuration.md), then use:

```sh
gs context show
gs list repository --first 25
gs read product summer-shirt -o json
gs repository clone catalog
gs repository sync --remote gitstore --ref main
```

Remote commands work without a daemon, container engine or separately installed
Git executable. See [remote commands](docs/remote-cli.md) for supported operations,
credential selection and submission semantics.

## Run GitStore locally

With matching native Linux `gs` and `gitstored` builds installed together and the
[host prerequisites](docs/local-runtime.md#before-you-start) satisfied:

```sh
gs serve --deployment dev --allow-provision
```

The core profile starts API, Git and controller services. Foreground mode reports
readiness and stays attached; Ctrl-C requests owned cleanup. Use `--detach` to
return after acceptance, then inspect readiness explicitly.

```sh
gs deployment list
gs deployment inspect dev -o json
gs deployment stop dev
```

The default catalogue is in memory and resets when the API restarts; repository
data and credentials are retained. Select `--datastore scylladb` for a retained
catalogue. Matching Linux binaries provide deployment logs and registered
standalone migration; optional services and native macOS execution remain
unavailable. Alpha.5 native acceptance covers Ubuntu 24.04 and Fedora 44 on both
x64 and arm64. Read the
[local deployment guide](docs/local-runtime.md) for prerequisites, migration backup
acknowledgment, disk footprint and recovery limits.

## Documentation

Start with the [user guides](docs/README.md). Build instructions, tests, architecture,
contracts and validation records are indexed in
[contributor documentation](docs/contributing/README.md).

`gs` is written in Rust and `gitstored` in C. The Linux runtime integrates libcrun,
libslirp and maintained OCI image libraries. Third-party runtime/network/image
executables are not bundled. CNI configurations and plugins are not supported by
the current networking backend.

## License

[MIT](LICENSE). Dependencies retain their own licenses and distribution requirements.
