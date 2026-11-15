# Install GitStore CLI

Install `gs` and its companion daemon, `gitstored`, together using one of the
package managers below. Both executables are included; you do not need to install
Git or a container engine separately.

## Choose your platform

| Platform | Architectures | What you can do |
| --- | --- | --- |
| macOS 15 or newer | Apple silicon, Intel | Connect to remote GitStore installations. Local deployments are unavailable. |
| Ubuntu 24.04 | x86_64, arm64 | Remote commands and local deployments. |
| Fedora 44 | x86_64, arm64 | Remote commands and local deployments. |

**On macOS, `gs deployment list`, `gs serve` and other local deployment commands
are not supported.** Installing `gitstored` does not provide a macOS execution
backend. Use [remote commands](remote-cli.md) with a configured GitStore server.
Local deployments require the [Linux host prerequisites](local-runtime.md#before-you-start).

GitStore and this CLI are early alpha. Upstream schemas change incompatibly.
Upgrade compatibility, data preservation and recovery are not guaranteed. Use,
upgrade, migration or removal may cause data loss. Keep independent backups; do
not use prereleases for irreplaceable data.

## Homebrew — macOS or Linux

<!-- release-docs:install-brew-target:start -->
The tap can lag a newly published release while its formula update is reviewed.
After adding the tap, run `brew info gitstore-dev/cli/gitstore` and confirm it
offers `0.1.0-alpha.7` before installing or upgrading. If it still offers an older
version, use the matching npm, APT or DNF package below, or wait for the formula update.
<!-- release-docs:install-brew-target:end -->

Add the tap:

```sh
brew tap gitstore-dev/cli https://github.com/gitstore-dev/cli.git
```

With Homebrew 7 or newer, review the formula and authorize it before installing:

```sh
brew trust --formula gitstore-dev/cli/gitstore
```

Install:

```sh
brew install gitstore-dev/cli/gitstore
```

On Linux, Homebrew requires a system C compiler for its own checks. This package
uses prebuilt binaries. APT and DNF are alternatives that do not require a compiler.

## APT — Ubuntu

<!-- release-docs:install-deb:start -->
Download the alpha.7 package for your architecture and install it:

```sh
# Choose amd64 (x86_64) or arm64 explicitly.
gitstore_arch=amd64
case "$gitstore_arch" in
  amd64) gitstore_url='https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore_0.1.0.alpha.7-1_amd64.deb' ;;
  arm64) gitstore_url='https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore_0.1.0.alpha.7-1_arm64.deb' ;;
  *) printf '%s\n' 'Choose amd64 or arm64' >&2; exit 1 ;;
esac
curl --fail --location --output gitstore.deb "$gitstore_url" &&
  sudo apt install ./gitstore.deb
```
<!-- release-docs:install-deb:end -->

Supported architecture names are `amd64` and `arm64`. APT installs the required
system dependencies. To install a later release, download its package and repeat
the installation command.

## DNF — Fedora

<!-- release-docs:install-rpm:start -->
Install alpha.7 directly from GitHub:

```sh
# Choose x86_64 or aarch64 (arm64) explicitly.
gitstore_arch=x86_64
case "$gitstore_arch" in
  x86_64) gitstore_url='https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-0.1.0-0.alpha.7.1.x86_64.rpm' ;;
  aarch64) gitstore_url='https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-0.1.0-0.alpha.7.1.aarch64.rpm' ;;
  *) printf '%s\n' 'Choose x86_64 or aarch64' >&2; exit 1 ;;
esac
sudo dnf install "$gitstore_url"
```
<!-- release-docs:install-rpm:end -->

Supported architecture names are `x86_64` and `aarch64`. DNF installs the required
system dependencies. Use the next release's URL to upgrade.

## npm — macOS or Linux

<!-- release-docs:install-npm:start -->
With Node.js and npm installed, select the package for your OS and processor:

```sh
# Choose darwin-arm64, darwin-x64, linux-arm64 or linux-x64 explicitly.
gitstore_platform=darwin-arm64
case "$gitstore_platform" in
  darwin-arm64) gitstore_url='https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-npm-0.1.0-alpha.7-darwin-arm64.tgz' ;;
  darwin-x64) gitstore_url='https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-npm-0.1.0-alpha.7-darwin-x64.tgz' ;;
  linux-arm64) gitstore_url='https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-npm-0.1.0-alpha.7-linux-arm64.tgz' ;;
  linux-x64) gitstore_url='https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-npm-0.1.0-alpha.7-linux-x64.tgz' ;;
  *) printf '%s\n' 'Choose a supported OS and processor' >&2; exit 1 ;;
esac
npm install --global --ignore-scripts --prefix "$HOME/.local" "$gitstore_url"
export PATH="$HOME/.local/bin:$PATH"
```
<!-- release-docs:install-npm:end -->

The supported combinations are `darwin-arm64`, `darwin-x64`, `linux-arm64` and
`linux-x64`. Add the `export PATH` line to your shell profile to keep the commands
available in future terminals. This prefix is owned by your user; do not use
`sudo` with these npm instructions.

Node.js is needed to manage this installation, not to run `gs` or `gitstored`.
Use the next release's URL with the same prefix to upgrade.

## Verify your installation

```sh
gs --version
gitstored --version
```

<!-- release-docs:install-version:start -->
Both commands should report `0.1.0-alpha.7`. These checks do not start services,
download images or require credentials.
<!-- release-docs:install-version:end -->

To connect to a remote GitStore installation, follow
[configuration and credentials](configuration.md), then use the
[remote command guide](remote-cli.md).

### First local use — Linux only

In a supported Linux user session:

```sh
gs deployment list
```

The first local command starts your per-user manager and creates private runtime
state. Installation itself does not start GitStore services or download images.
To start a deployment and authorize its image downloads:

```sh
gs serve --deployment dev --allow-provision
```

See [local deployments](local-runtime.md) for prerequisites, inspection, stopping
and recovery. An explicit `--daemon-socket` connects to an existing manager; it
does not start one.

### macOS reports `UNSUPPORTED_HOST` for a deployment command

Local deployment commands return `UNSUPPORTED_HOST` (exit code 6) because local
workloads require Linux. Use remote commands, or run the local runtime on a
supported Linux host. Setting `XDG_RUNTIME_DIR` does not enable macOS deployments.

If an older alpha.5 installation instead reports `NOT_FOUND: Select --daemon-socket
or set an absolute XDG_RUNTIME_DIR`, upgrade using the instructions below. Alpha.6
corrects that diagnostic; it does not add a macOS local-runtime backend.

To show a command's exit code in your shell:

```sh
gs deployment list
printf 'exit=%s\n' "$?"
```

## Upgrade or uninstall

Back up your data before upgrading or removing early-alpha software. On Linux,
stop deployments before replacement; see [stop and restart](local-runtime.md#stop-and-restart).
An already-running manager is not automatically replaced. If a command reports
`INCOMPATIBLE_VERSION`, stop the old manager before starting the newly installed
pair; an explicitly enabled user unit can be stopped with
`systemctl --user stop gitstored.service`.

| Installation | Upgrade | Uninstall |
| --- | --- | --- |
| Homebrew | `brew update` then `brew upgrade gitstore-dev/cli/gitstore` | `brew uninstall gitstore-dev/cli/gitstore` |
| APT | Install the next downloaded `.deb` | `sudo apt remove gitstore` |
| DNF | Install the next release's RPM URL | `sudo dnf remove gitstore` |
| npm | Install the next release's tarball using the same prefix | `npm uninstall --global --prefix "$HOME/.local" @gitstore/cli-OS-CPU` |

For npm removal, replace `OS-CPU` with your installation's combination, such as
`darwin-arm64`. Removing the package does not stop an independently running
manager. A downgrade may be unable to read data written by a newer version.

## Optional download verification

<!-- release-docs:install-verification:start -->
The [release page](https://github.com/gitstore-dev/cli/releases/tag/v0.1.0-alpha.7) provides `SHA256SUMS` checksums and one shared
`release.sigstore.jsonl` attestation bundle for the published packages and inventories.
With GitHub CLI installed, download a package and the bundle for its release, then verify:

```sh
gh attestation verify ./PACKAGE --repo gitstore-dev/cli \
  --bundle ./release.sigstore.jsonl
```
<!-- release-docs:install-verification:end -->

Replace `PACKAGE` with the downloaded filename. Older alpha.5/alpha.6 releases
instead use a per-file `PACKAGE.sigstore.jsonl` bundle. Verification is optional;
installation does not require or install a verifier. Debian download names use
`.` where the checksum inventory uses `~`; save the files under the inventory
names when checking `SHA256SUMS`.
