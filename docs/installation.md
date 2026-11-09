# GitHub-hosted installation

**Release preparation is in progress. No binary/package release is qualified or
published yet.** The commands below describe the selected alpha.1 distribution
paths once its reviewed assets exist. Consult the
[release evidence](contributing/validation/release-alpha1.md) for actual results.

GitStore and this CLI are early alpha. Upstream schemas will continue changing
incompatibly. Upgrade compatibility, data preservation and recovery are not
guaranteed. Use, upgrade, migration or removal may cause data loss, including
configuration, credentials and stored data. Keep independent backups and do not
use prereleases for irreplaceable data. Ownership/retention checks are engineering
safeguards, not preservation or recovery promises.

## What installation provides

Every package installs both `gs` and the matching `gitstored` as real sibling
executables in a private directory, with public command links, native library
closure, notices, command reference and shell completions. No separately installed
Git executable, container engine or verifier is required by the product.

Help and remote commands run without a daemon or image downloads. On a qualified
Linux user session, the first local command, such as `gs deployment list`, creates
private per-user state and launches the matching ordinary-user manager. Concurrent
first use converges on one manager. Installation does not initialize another
user's home, create credentials, enable linger or download/start service images.
Starting workloads still requires the existing asset/credential consent.

An explicit `--daemon-socket` is connect-only. A running manager with another full
version causes an actionable error; installation never kills/replaces that manager
or its workloads automatically. Stop workloads and the manager explicitly before
replacement. Optional user units can be stopped using `systemctl --user stop
gitstored.service` if you explicitly started that unit. Directly launched managers
must be stopped by their invoking user after verifying the exact owned process;
do not terminate unrelated processes by a broad name match.

## Selected hosts

The required qualification matrix is Linux x86_64/arm64 on Ubuntu 24.04 and Fedora
44, and macOS 15 arm64/x86_64 for remote/control commands. Linux archives require
glibc 2.39 or newer; native workload execution additionally needs kernel 6.8+,
systemd user sessions, delegated cgroup-v2 cpu/memory/pids controllers, rootless
namespaces and TUN. A package install alone cannot establish those prerequisites.
See [local runtime setup](local-runtime.md#before-you-start).

macOS packages do not provide a local workload backend. Local execution returns
an unavailable-host error; remote operations and explicit control connections
remain separate. Nix and Windows distribution are deferred. These are selected
test targets, not a claim that all rows already passed acceptance.

## Homebrew

Start with a working Homebrew installation and its platform prerequisites. On
Linux, [Homebrew requires a system C compiler and development tools](https://docs.brew.sh/Homebrew-on-Linux#requirements);
its installer checks those even though this formula copies prebuilt GitStore
binaries and never compiles `gs` or `gitstored`. The archive, APT, DNF and npm
paths do not require a compiler.

After promotion creates the matching generated formula:

```sh
brew tap gitstore-dev/cli https://github.com/gitstore-dev/cli.git
```

On Homebrew 7, review the generated formula and authorize that formula:

```sh
brew trust --formula gitstore-dev/cli/gitstore
```

Then install it:

```sh
brew install gitstore-dev/cli/gitstore
```

The explicit Git URL tap uses this repository, with exact versioned GitHub asset
URLs/checksums. No Homebrew registry account is required. Installation selects
prebuilt binaries and does not compile or run `brew services start`.

To replace/remove the code:

```sh
brew update
brew upgrade gitstore-dev/cli/gitstore
brew uninstall gitstore-dev/cli/gitstore
```

## APT

Download the exact release `.deb` from the GitHub release page. For example,
`gitstore_0.1.0~alpha.1-1_arm64.deb` (`amd64` for x86_64), then:

```sh
sudo apt install ./gitstore_0.1.0~alpha.1-1_arm64.deb
```

APT resolves declared system dependencies. This is a local package installation,
not an APT repository subscription. Install a later exact downloaded `.deb` to
replace it; remove code using `sudo apt remove gitstore`. The real executable pair
is under `/usr/libexec/gitstore/bin`, with public links under `/usr/bin`.

## DNF

Use the exact GitHub Release URL or a downloaded RPM. For Fedora arm64:

```sh
sudo dnf install https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.1/gitstore-0.1.0-0.alpha.1.1.aarch64.rpm
```

Use `x86_64` for that architecture. DNF resolves declared dependencies; no RPM
repository subscription or generic RHEL compatibility is promised. Install the
next exact RPM to replace it; remove code with `sudo dnf remove gitstore`.

## npm

Choose exactly one OS/CPU tarball; no npm registry account is needed. For macOS
arm64 with a user-writable npm prefix:

```sh
npm install --global --ignore-scripts https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.1/gitstore-npm-0.1.0-alpha.1-darwin-arm64.tgz
```

Other tuples are `darwin-x64`, `linux-arm64` and `linux-x64`. The scoped package
name is `@gitstore/cli-OS-CPU`. Its bin maps point directly to both real native
executables; there is no Node wrapper, registry dependency or lifecycle download
script. OS/cpu metadata rejects an incompatible selection. Node/npm are needed
to manage this channel, not to execute the installed binaries.

Install the next exact URL to replace the package. For this example, remove code
using `npm uninstall --global @gitstore/cli-darwin-arm64`. Non-default prefixes
are supported by layout design; clean lifecycle qualification remains recorded
separately in the release evidence.

## Verification and removal

Release assets will include SHA-256 checksums and standard signed GitHub Actions
attestation bundles. Verification is available to compatible tools and is
**optional for consumers**; installation does not enforce it or install a verifier.
Maintainers must verify producer/source/digest identity before promotion under
[the release policy](../runtime/packaging/release-policy.md). A checksum is not a
substitute for signed provenance, and provenance is not a vulnerability claim.

Removing/replacing code does not intentionally initialize or erase per-user
deployment directories, but no data-preservation or recovery guarantee applies.
Do not infer that package-manager removal stops an independently running manager
or that rollback can read a newer schema. Review the exact transition's release
notes and make independent backups before use, replacement or removal.
