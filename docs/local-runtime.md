# Local runtime development status

M2 is in development. Control-only builds provide owner-authenticated inspection
and read-only assessment. Fully linked native Linux builds now run the registered
core memdb profile through `gs serve`, with explicit provisioning consent, current
readiness and owned stop/interrupt cleanup. The selected Linux arm64 fixture has
real CLI evidence; broad host support, full manager recovery, logs, migrations and
publication remain gated. See [the evidence record](m2-status.md). Remote catalogue
and Git commands remain independent of this daemon and runtime downloads.

## Build and start control

Use the pinned Rust toolchain and CMake >=3.28/C17 compiler. Native builds fetch
Jansson/SQLite source archives with the hashes in
[the dependency lock](../runtime/dependencies.lock.json); an explicit
`GITSTORED_SOURCE_CACHE` can supply those same archives offline. Build tools and
source archives are development inputs, not bundled runtime executables.

```sh
cargo build --workspace --locked
cmake -S daemon -B build/daemon -DGITSTORED_TESTING=ON
cmake --build build/daemon
```

Create a private, absolute runtime directory without symlink path components, then
start the control process as the invoking user:

```sh
mkdir -m 700 "$XDG_RUNTIME_DIR/gitstore"
build/daemon/gitstored --runtime-dir "$XDG_RUNTIME_DIR/gitstore"
```

Existing directories must be owned by the invoking UID with mode 0700. The socket
is `gitstored.sock`, mode 0600. A lifetime lock excludes a second manager. An
explicit `--daemon-socket /absolute/path/gitstored.sock` or
`GITSTORE_DAEMON_SOCKET` selects another owned fixture. The default is
`$XDG_RUNTIME_DIR/gitstore/gitstored.sock`. The standard Linux serve path can start
a matching installed `gitstored` sibling, using a private readiness channel and an
authenticated version handshake. Other commands and explicit socket selections are
connect-only. State is `$XDG_STATE_HOME/gitstore` or `$HOME/.local/state/gitstore`.
See [candidate user-service packaging](../runtime/packaging/README.md); building
does not enable a service or install privileged session settings.
macOS supports these portable control checks; native workload execution is unverified.

## Read-only assessment

```sh
target/debug/gs api config validate --profile core --profile-version 1 -o json
target/debug/gs controller config validate --profile core --profile-version 1 -o yaml
```

The immutable `core` / `1` metadata maps the recorded alpha.8 server revision and
Linux amd64/arm64 image digests. This is a metadata registration, not a runnable
profile claim. API/Git have mutual prerequisites; controller adds separate signing
record preparation, enrollment and launch stages. Optional OIDC/Admin/Storefront
mappings are unavailable. `scylladb` maps to upstream `scylla`, but its pinned
runtime/migration profile is not registered yet.

Results contain the effective non-secret plan, configuration source roles, image
sources/digests, host requirements and exact unavailable/unperformed checks.
Image sizes and cache verification are unperformed. API/controller configuration
planning permits additive overlays; Git permits one file. Native TOML/environment
and RBAC validation has not run. The removed-key checks operate on internal
normalized metadata, not arbitrary user-provided service TOML files. There is no
caller-controlled service configuration path on the wire. No raw configuration
values or credentials are returned.

Table, JSON and YAML are supported. An unmet plan is printed to stdout, with a
safe diagnostic on stderr and exit 8. Markdown is rejected. `--non-interactive`
never prompts. Local timeouts accept 1 ms through one hour and bound observation,
not workload lifetime. Validation remains read-only even if the protocol request
has `allowProvision: true`.

## Separate local selection

Selection precedence is flags, `GITSTORE_DEPLOYMENT` / `GITSTORE_PROFILE` /
`GITSTORE_PROFILE_VERSION`, optional local selection configuration, then the single
registered defaults (`core`, `1`, `memdb`). Remote context/account/endpoint never
select a deployment. Local commands reject remote credential file/stdin flags.

The optional selection file is `$XDG_CONFIG_HOME/gitstore/local.toml`. Without
XDG_CONFIG_HOME it is under `~/Library/Application Support/gitstore` on macOS,
or `~/.config/gitstore` on Linux. It must be an owned regular 0600 file in an owned
0700 directory; symlink components and hard links are rejected. The CLI does not
create or change it during assessment. It accepts only this versioned structure:

```toml
version = 1
[selection]
profile = "core"
profile_version = "1"
datastore = "memdb"
# deployment = "my-local-deployment"
```

Service configuration, passwords, tokens, executable paths and registry credentials
are not accepted in this file. Unknown/duplicate fields fail before connecting.

## Remaining command gates

`serve`, verified core component `serve`, detached acceptance, foreground current
readiness and owned stop/interrupt cleanup are implemented for fully linked native
Linux builds. Select an explicit deployment or configure local selection; no default
deployment is fabricated. Missing assets need displayed interactive consent,
`--allow-provision`, a cache candidate or explicit offline input. Every candidate
is verified before execution. Optional components, logs and `migrate` retain named
gates. Local operation waits remain separate from remote correlated waits (DEP-004).

No `gh`, Git executable, container engine, image/network/VM helper bundle or runtime
library loader has been added. Jansson/SQLite are ordinary native code dependencies.
Later runtime implementation will prefer maintained library APIs; compatible
remaining host dependencies or verified consented downloads must be assessed
before use. The <=5 MiB core executable target remains unmeasured for the completed
runtime. CLI-published artifact attestations are separate from upstream image
source/platform/digest validation.

## Deployment and operation observation

To enable persistent control-record queries in this development build, select a
separate private state directory explicitly:

```sh
build/daemon/gitstored --runtime-dir "$XDG_RUNTIME_DIR/gitstore" --state-dir /absolute/owned/state
```

The directory must be owned, 0700 and free of symlink components. `state.db` and
its lifetime state lock are mode 0600. A second manager cannot open this state
through another runtime directory. The build refuses incompatible/corrupt schema
rather than silently upgrading or downgrading it. Installed platform state-path
selection is part of the unfinished manager packaging work.

With state configured, the daemon also advertises `deployment.read` and
`operation.read`. Without it, these capabilities are unavailable. Examples:

```sh
target/debug/gs deployment list --limit 100 -o json
target/debug/gs deployment inspect my-deployment -o yaml
target/debug/gs deployment inspect my-deployment --operation my-operation --wait terminal --timeout 60s -o json
```

List/inspect support table/JSON/YAML. Inspect requires an explicit deployment ID;
there is no remote-context inference. An operation selector must belong to that
deployment. Inspection with an operation returns separate `deployment` and
`operation` records; it is not an atomic snapshot across observations. After a wait,
the CLI refreshes the deployment record. Failed/cancelled terminal waits emit the
observed records and exit nonzero. Standalone inspection can successfully report
a failed operation without claiming that it succeeded.

Pages cap at 100. Opaque cursors are bound to the store, visible-record generation
and last position, retained for five minutes, and capped at 4096 stored cursors.
A foreign, expired or stale cursor produces `CURSOR_EXPIRED` (exit 8); restart the
listing. Any visible deployment/component/resource change invalidates prior
cursors. This is not a stable snapshot guarantee. A missing operation/deployment,
including a record that has expired from retention, produces `NOT_FOUND` (exit 4).
Accepted-history retention/eviction remains unfinished; the protocol's seven-day
minimum and 100,000-operation/256 MiB bounds are not replaced by cursor retention.

`--wait accepted|terminal|ready` uses local operation observation. The default wait
timeout is 60 seconds, while other exchanges default to 30 seconds. Ready applies
only to start operations and the current observed generation/last operation.
Failed/cancelled starts never satisfy readiness. Deadline expiry (exit 7) retains
known operation/deployment identity and does not cancel work. The server polls
wait conditions independently of other connections. Unknown IDs and operation
mismatches never select or cancel another deployment.

Public operation outcomes currently expose the stable non-secret identity/state
projection and safe error code/retryability with withheld raw backend messages.
Other backend result details are not exposed yet. Control-only tests use seeded
records; they do not establish native execution. Fully linked Linux builds run
the registered real core subset, with its separate evidence and remaining gates
recorded in [the implementation status](m2-status.md).


The explicit-state manager now reads an optional owner-only `settings.json` from
its state directory at startup. Schema version 1 selects `bootstrapCredentialSource`
(`os-store` by default, or explicit `protected-directory` with a private absolute directory path)
and optional `offlineLayouts` paths for registered core components. Unknown fields,
unsafe file/parent ownership, modes or links fail before state DB/credential effects.
Offline input authorizes only its verified bytes and never fallback downloads.
Settings are a stable startup snapshot and are not lifecycle request parameters.
The real public core path is verified within its recorded Linux fixture scope. Under memdb,
catalogue identities and controller checkpoints are ephemeral; repository objects
and private key material are retained. Retained Git objects alone do not restore
the catalogue automatically. A new API instance needs a newly assigned controller
UID while reusing the same existing private signing key.

The explicit directory fallback derives separate protected files from the full
installation/issuer/deployment/purpose identity. It never shares one global file
between deployments. Files remain add-only and owner-only; ordinary restart
reuses the existing deployment material. The pinned directory must retain its
identity. Older unreleased prototype state schemas are refused after adding
reserved configuration resources; use fresh test state instead of modifying old
fixture databases.


After manager loss, old readiness becomes pending/uncertain. A replacement
manager can stop the recorded core resources when their filesystem, process,
namespace and cgroup identities can be verified. Stop retains volume contents and
existing credentials. Missing or substituted recovery roots are refused without
creation or repair; incomplete receipts remain uncertain. Reconnecting with the
same idempotency key returns the original operation rather than repeating its
bootstrap. Partial-stage reconciliation and automatic workload restart remain
unfinished.

Successful running core deployments retain exact process watches and perform
read-only health checks through the shared eight-worker budget. API, Git and
controller checks rotate at five-second intervals; health proof expires after
30 seconds without refresh. A process exit or failed health check invalidates
readiness while preserving the completed start operation. A temporarily stalled
service can regain readiness from successful checks without changing generation,
process identity or credentials. Stops wait for an in-flight bounded observation
worker; observations never enroll clients, generate keys or provision images.
These behaviors have scoped Linux arm64 development evidence, including a stalled
live API and manager loss. They do not establish broader release support.
