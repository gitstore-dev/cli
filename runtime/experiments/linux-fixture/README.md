# Disposable Linux development fixture

The user authorized either installed container service on 2026-11-03. The current
fixture is `gitstore-m2-linux-a1a13c`, an Apple-backed Ubuntu 24.04 arm64 VM used
only for development tests. [Evidence](evidence.json) records exact source/image
identities. It has no host home mount, no SSH-agent environment/socket and no
published ports. It uses a separate UID/GID 11000 and private `/home/gitstorefixture`.
No user config, credential store, existing deployment or source checkout is mounted.

Apple's service was initially stopped/unregistered and was started for this work.
The official Ubuntu platform image was pulled, then a named owned container
installed systemd and native build packages. Original Ubuntu lacks `/sbin/init`.
Apple `commit` and original exported-image import failed on the journal's binary
POSIX ACL PAX headers. `normalize-export.py` removes only those two headers from
`var/log/journal` in this disposable fixture export, without extracting anything.
The fixture producer under `../image-library/go-probe/cmd/fixture-image` uses
maintained Go container-image APIs to create a local OCI archive. The normalized
archive imports successfully; no `--force`, publication or upstream image change
is involved. This is a development-only compatibility workaround.

Apple `machine` mode also automatically exposes an SSH-agent socket even with
home mounting disabled and SSH_AUTH_SOCK removed from the caller environment.
That owned machine was stopped/deleted before running tests or querying the agent.
The replacement uses an ordinary named VM-backed container with no `--ssh` or
volume options. Separate direct checks confirm both absent home and absent agent.
Its explicit full-guest Linux capabilities/unmasked proc/sys allow a Linux init;
these are isolated VM fixture settings, not a product privilege/security fallback.
Normal runtime tests and builds invoke UID 11000. Guest-root package preparation
never changes host accounts, kernel controls or deployment data.

Only whitelisted daemon sources, trusted profile metadata and the digest-verified
Jansson/SQLite archives were streamed to the private source directory. GCC 13.3.0,
CMake 3.28.3 and OpenSSL 3.0.13 build the daemon there; all 14 current C tests pass.
The glibc build caught a missing X/Open feature declaration in the socket fixture,
which was fixed in repository source. This is Linux arm64 **control** evidence.
These control checks do not establish GitStore image, Scylla or hosted/amd64
support. Subsequent [libcrun](../libcrun.md) and [network](../network-library.md)
probes have separate evidence, including the failing network sanitizer gate.

The prepared builder `gitstore-m2-init-builder-a1a13c` is stopped; the ordinary
fixture remains available for ongoing tests. Cleanup must target these exact
owned names/images only, never global prune or user workloads. The service was
started for this session; shutdown/cleanup is part of finishing the experiments.
