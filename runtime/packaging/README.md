# Candidate per-user installation

Install matching `gs` and `gitstored` ordinary executables in one trusted directory.
The manager resolves only this installed `gs` credential broker. It verifies the
original daemon file against its running image before using that sibling: pinned
libcrun deliberately re-executes the daemon from a sealed memory file. Do not use
`/proc/self/exe` as the installation directory after native-library startup.
No third-party helper executable is included. Code libraries and verified upstream
image data remain separate capabilities; candidate packaging is not publication.

The standard Linux serve path can start the installed per-user manager. It
uses `/run/user/UID/gitstore/gitstored.sock`, mode 0600, and owner-only application
directories. State is `$XDG_STATE_HOME/gitstore` or `$HOME/.local/state/gitstore`.
An explicit socket is connect-only. Startup pins the sibling executable, clears
the environment, requires a matching application version and authenticates the
ready socket's UID. It never uses sudo, PATH discovery or build tools.

`systemd/gitstored.service.in` is an optional user-service template configured by
CMake with the selected installation prefix. Inspect the generated unit before
installing it into the user's unit directory. The template is not enabled by the
build or by CLI startup. It requires native Linux, a functioning systemd user bus,
delegated cpu/memory/pids cgroup-v2 controllers and usable rootless namespaces/TUN.
The service preserves manager-owned lifetime locks and uses mode 0700 state/runtime
directories. Stopping a manager is not a deployment stop: live/uncertain resources
remain journaled; recovered readiness must be re-observed before replacement.
Use the deployment stop operation for observed reverse cleanup and retained data.

Publication, cross-host acceptance and the per-core stripped size budget are later
gates. These files establish installation behavior, not release support.

The current Linux arm64 candidate uses Cargo's size release profile, one codegen
unit, LTO and stripped symbols. `gs` keeps Rustls with the maintained Ring provider
instead of AWS-LC; no TLS verification is disabled. The native daemon uses
`MinSizeRel`, section garbage collection, its ordinary linked image code library,
libcrun and libslirp, and `GITSTORED_TESTING=OFF`. Optional code integrations remain
explicit build inputs. No helper is copied into the product prefix.

`candidate.py --prefix /absolute/owned/prefix --output /absolute/new.tar.gz` is
Linux developer packaging tooling. It records exact own ELF hashes/byte sizes,
budget misses and the selected host's unique `ldd` shared-library closure. It
creates only `bin/gs`, `bin/gitstored` and `share/gitstore/candidate.json`; it never
publishes, signs, installs a service or packages workload images/developer probes.
Inputs and output must be explicitly selected; existing output files are refused.

The [measured candidate](linux-arm64-candidate.json) is unpublished. Its compressed
package is 5,185,243 bytes. Stripped cores are 5,525,032 bytes (`gs`) and 5,658,000
bytes (`gitstored`): **both miss the 5,242,880-byte target**. Host libraries are
reused rather than included; their unique traced closure totals 25,611,792 bytes.
This is a fixture-specific disk inventory, not total OS cost or runtime memory.
`gs`'s prior size-s/AWS-LC candidate measured 6,114,824 bytes. The daemon size-s
candidate measured 5,657,952 bytes; size-z did not improve its final footprint.
These measurements do not close the later size/performance/publication gates.

Actual release CLI TLS acceptance passed with a generated private test CA and a
separate valid server certificate, and rejected the same endpoint when only an
unrelated CA was trusted (`runtime-run-cli-tls-zdss9az5.json` under the selected
Linux fixture parent). The original Go default CA-as-leaf fixture was rejected;
it was corrected instead of changing product verification. Native OpenSSL policy
checks pass after assigning the generated leaf's issuer key identifier
(`runtime-run-exp-003b-sdk-_a1ihv82.json`). These synthetic TLS checks are separate
from the real native core/API/Git scenarios.

Candidate acceptance uses an explicitly configured synthetic protected-file
credential source on the dedicated headless fixture; this is separate from later
native OS credential-store support evidence. Cold interactive registry startup
passed in 136.208 seconds (`runtime-run-clean-install-q0y6y2qq.json`). Automated
denial/offline readiness, warm reuse, concurrent isolation, foreground cleanup/
detached reconnect and missing-runtime/optional-map rejection also passed on the
same candidate. Full Linux Rust checks passed after refreshing stale copied test
artifacts; host fmt/Clippy/workspace checks also passed.

The additional public interface check creates a disposable Namespace/Repository
through raw GraphQL, seeds only its initial Git branch using a developer libgit2
helper, then proves normal `gs` clone/fast-forward sync/reclone without history
rewrites or a Git executable. Source identities are recorded in
[public-api-evidence.json](public-api-evidence.json). The helper is outside the
package. These checks do not ungate high-level Namespace/Repository authoring.

Memdb catalogue identities/checkpoints reset when the API restarts. An earlier
stronger public-Git-after-restart assertion failed as expected under that design
(`runtime-run-clean-install-61ytwmca.json`). Retained Git objects do not silently
restore admission/identity. The corrected check explicitly expects NOT_FOUND for
the old catalogue identity and checks its known retained commit/tree/blob only as
read-only fixture instrumentation after observed stop, under the writer lease.
Persistent catalogue continuity still needs the persistent datastore profile.

The final combined public GraphQL/Git + memdb reset + retained-object check passed
in 83.245 seconds (`runtime-run-clean-install-v4i_ohvt.json`, retained root
`gs-public-runtime-77es6c_3`). Offline first readiness measured 42.353 seconds.
At the sampled second readiness, the manager's RSS/high-water RSS was 13,920 kB;
this excludes workload and provisioning-worker peaks. Logical asset files total
258,759,008 bytes; total logical state is 776,611,941 bytes after two starts. The
latter includes retained private execution copies; bounded transient cleanup is
still T055 work, not a hidden package cost or a proven live-memory bound.

Each cold profile verifies 84,691,615 payload bytes across exactly three assets.
For the registry case this is SDK-reported image payload transfer, excluding
HTTP/TLS framing; the offline case imports those bytes without registry traffic.
No optional component image was downloaded.
