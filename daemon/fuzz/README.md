# Local protocol fuzz smoke

Run `scripts/test-daemon-fuzz.sh` on a selected Linux development host with
Clang 18, CMake and the pinned dependency sources. It configures a dedicated
ignored `build/daemon-fuzz` tree and divides 60 seconds between the JSON/request
and incremental length-prefixed framer harnesses. `--seconds 2..3600` changes the
total execution budget; compilation is additional. No runtime/image/network
library is enabled or downloaded by this build.

The seed generator encodes all saved M0 messages plus duplicate/escaped keys,
UTF-8/NUL/surrogate/numeric/depth/string/collection limits, fragmented/truncated
headers and coalesced/over-limit frames. The command records seed=1, input limits,
5-second per-input deadlines, 1 GiB RSS and 64 MiB allocation caps. Sanitizers
instrument the C code and Jansson allocations. Each protocol input is also parsed
with deterministic allocation denial; the denial hook exists only in testing
builds. Generated mutations, findings and logs stay in the build tree.

This is a smoke test, not exhaustive parser or hostile-client evidence. Peer UID
checks are kernel/socket checks, not parser fuzzing. The separate
`kernel_peer_uid_boundary` test skips with exit 77 unless explicitly run as root
in a disposable Linux fixture. It creates an isolated abstract Unix listener and
creates the client socket *after* dropping its child to UID/GID 11001; the same
production credential predicate must reject it and accept an owner peer. It does
not run the daemon as root, grant product privileges or change host accounts.
Normal private-directory/socket checks remain in `server_security`.
