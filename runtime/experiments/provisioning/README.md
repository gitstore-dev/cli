# Private core bootstrap probes

These developer fixtures exercise actual published core images through the C
libcrun/libslirp adapters. They do not enable public deployment methods or prove a
clean installation. Use a disposable non-root Linux user with the recorded user
session, delegated cgroups, TUN and kernel prerequisites. The VM harness itself is
development tooling, not a product Docker/Apple-container prerequisite.

Build with `GITSTORED_TESTING`, `GITSTORED_LIBCRUN`, `GITSTORED_SLIRP` and
`GITSTORED_BOOTSTRAP` enabled. The image SDK can be built in a separate tree.
Declare OpenSSL, libxcrypt and libcurl as code dependencies; no corresponding
command-line executables are invoked. The probe uses an installed single-link
private copy of the built `gs` broker and an explicitly selected synthetic
protected-file credential source. It never consults personal credentials.

First acquire the registered API/Git/controller manifests using `cache_probe`,
then use `provision_probe COMPONENT TARGET 60000 CACHE_ROOT materialize` to create
each private `COMPONENT-execution/rootfs` under an owned asset fixture directory.
Only successful manifest-bound private materializations are execution inputs.

From the checkout on the selected Linux fixture:

```sh
python3 runtime/experiments/provisioning/test_core.py \
  build/daemon-native/core_bootstrap_probe /absolute/installed/gs \
  /absolute/verified-materializations --fixture-parent /absolute/private/experiments
```

For ASan/UBSan, select the instrumented probe and add `--sanitized`. The integration
probe disables exit leak reporting because its retained fixture ownership handles
are deliberately left to process exit; the independent behavior suite runs with
leak detection enabled. UBSan remains fatal. Published service payloads are the
original image executables; C adapters and pinned native libraries are instrumented.

The runner retains its exact private fixture on success or failure. It prints only
stage/identity/status records. Do not print credential/config file contents. Normal
success stops controller/API/Git in reverse order, collects actual init exits and
performs non-forced native deletion before stopping the packet worker. Repository
data and ownership records remain retained. It does not claim all lifecycle or
descendant-recovery invariants from init termination alone.

If a probe fails with running resources, inspect its ledger before taking action.
For the fully launched retained fixture, the test-only probe's final `cleanup`
argument reopens the recorded network identity and verifies each native boot,
PID/start time, bundle and root before signals and non-forced deletion. It refuses
missing or uncertain evidence. It is not a generic PID killer, data remover or
substitute for the pending public recovery dispatcher.

[bootstrap-evidence.json](bootstrap-evidence.json) records the actual runs, including
the first deadline failure and its subsequent verified stop. Controlled HTTP cases
are reproducible through `test_bootstrap_http.py`; add-only file/TOML checks through
`test_bootstrap_files.py`. They establish separate evidence from real-image runs.


The current probe additionally captures each live native cgroup2 directory and
`cgroup.events` inode with boot/PID/start and exact scope matching. It verifies live
membership after START, holds those descriptors across TERM and requires subtree
empty or verified node deactivation after collecting init exit, before deletion.
Actual Git's inherited FD3 matches the writer lease; closing the parent's copy
still denies a competing acquisition. After stop, reacquisition preserves the
recorded volume data inode. libcrun deletion success alone is not a termination
receipt. This parent-held proof does not yet implement crash-recovery reopening.

The parent deployment actor now has a separate opt-in integration:

```sh
python3 runtime/experiments/provisioning/test_deployment.py \
  build/daemon-native/deployment_probe /absolute/installed/gs \
  /absolute/verified-core-cache-parent --fixture-parent /absolute/disposable-parent
```

The selected cache parent contains `api/cache/KEY`, `git/cache/KEY` and
`controller/cache/KEY`. The runner copies/hash-checks OCI blobs into new offline
layouts; the managed SDK verifies them again. It installs a single-link copy of
our credential broker, uses explicit per-deployment protected-directory settings,
retains all synthetic state, and drives the production asynchronous actors through
real readiness and reverse stop. It does not test public Unix dispatch or CLI UX.
`deployment_asset_probe ROOT API_OCI_LAYOUT` separately tests cold import, warm
reuse without consent and a deliberately tampered private cache. It leaves that
cache invalid by design; do not adopt it as a later trusted fixture.

`test_dispatch.py DAEMON GS OFFLINE_LAYOUT_PARENT --fixture-parent PARENT` drives
real public Unix start/replay/inspect/stop plus repeated stop and warm restart.
It installs private copies of our two binaries and uses no third-party executable.
The layout parent supplies `offline-api`, `offline-git`, `offline-controller` from
a selected earlier experiment. Every fixture is retained; failed observations
request only that fixture's owned stop while its manager remains reachable.
This proves the wire dispatcher independently of pending CLI serve UX.
