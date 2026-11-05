# Run GitStore locally

The local core profile runs the API, Git service and controller. Its default
catalogue is in memory. Select Scylla explicitly to retain catalogue data across
restarts. Repository data and deployment credentials survive ordinary stops in
both modes. Remote GraphQL/Git commands are independent of local execution.

The current managed profile pins GitStore alpha.8. Alpha.9 changes API endpoints,
listener configuration and stored-data layout; it has not been validated by this
runtime profile. Existing deployments retain their pinned images and data format.

## Before you start

Local execution is a source preview, exercised on a Linux arm64 development host.
Use matching native `gs` and `gitstored` builds installed in the same trusted
directory; see [building from source](contributing/building.md). A control-only
daemon build cannot start workloads. Broader Linux host support and native macOS
execution have not been established.

The Linux host needs user namespaces, cgroup v2 with delegated CPU/memory/PID
controllers, a working systemd user session and bus, accessible TUN, and the pidfd
and openat2 kernel interfaces. Native library dependencies must be available.
The CLI reports missing capabilities; it does not invoke sudo or change host
security settings. Docker and Compose are not required.

The default credential backend uses your OS secret store. Headless hosts without
a usable store need an explicit [protected-directory configuration](local-runtime-configuration.md#credentials).
Service images are verified before execution. First-use downloads need interactive
consent or `--allow-provision`; [offline inputs](local-runtime-configuration.md#offline-images)
can authorize already available image data.

## Start a deployment

Select an explicit deployment name:

```sh
gs serve --deployment dev --allow-provision
```

This starts the per-user daemon if needed, prepares the selected images, starts
the services and waits for current-generation readiness. It prints progress to
stderr and the requested result to stdout. Foreground mode stays attached after
readiness. Ctrl-C requests cancellation while startup is active, or an owned stop
of that start's generation after it has completed. Cleanup can remain uncertain;
inspect the reported identity rather than assuming rollback.

To return after durable acceptance:

```sh
gs serve --deployment dev --allow-provision --detach --idempotency-key dev-start-1 -o json
```

Save the returned `operationId` and key. Detached success means accepted; check
readiness using that operation ID:

```sh
gs deployment inspect dev --operation OPERATION_ID --wait ready --timeout 120s -o json
```

Replace `OPERATION_ID` with the returned ID. A timeout ends observation and leaves
the deployment running. Repeating a submission after a lost response must use
the same key and parameters; a new key does not recover an unknown outcome.
Different parameters with the original key produce `IDEMPOTENCY_CONFLICT`.

## Inspect services and endpoints

```sh
gs deployment list
gs deployment inspect dev -o json
gs deployment inspect dev --operation OPERATION_ID -o json
```

A ready deployment exposes `endpoints.api` and `endpoints.git` as high loopback
HTTP ports. Use the reported URLs; ports are selected separately for each
deployment. Controller and internal RPC ports are not published.

Readiness describes the current generation. A successful historical start result
can remain successful after a service fails. Health checks invalidate current
readiness when a process exits or a service stops responding. A temporarily
stalled service can regain readiness without being replaced.

After observing an actual owned workload or network-worker exit, the current
manager can stop the whole deployment and start a replacement. It waits for
observed cleanup and released writers first. Each stop/start has its own operation
ID; the original successful start result stays unchanged. Replacement uses existing
verified assets with downloads disabled. Inspect the deployment again for its
current operation, generation and endpoints.

One supervision chain permits at most four replacements, delayed by 250, 500,
1,000 and 2,000 milliseconds after successful cleanup. The normal stop/start
deadlines still apply. A failed stop, failed replacement, uncertain ownership,
new operator intent or manager loss ends retries. Recovered deployments require
explicit owned stop/start. A new explicit start begins a new supervision chain.

Replacement restarts all selected services. Memdb catalogue state resets; named
repository and Scylla data remain retained. This behavior has native source-preview
evidence on the named Linux arm64 fixture, not other hosts or published releases.

Lists return up to 100 deployments per page. Use `--limit` and the returned
`nextCursor` with `--cursor`. If a cursor expires or the visible state changes,
start the listing again. JSON and YAML are available for inspection; Markdown is
not supported for local commands.

## Stop and restart

```sh
gs deployment stop dev --idempotency-key dev-stop-1
```

Stop waits for observed workload termination and writer release. Repository
volumes and credentials remain. Stopping the manager process alone does not stop
a deployment. Manager exit leaves recorded ownership available for recovery; it
is not evidence that a repository writer has stopped.

If a stop times out, ownership remains held or uncertain. Replay the same key to
inspect that attempt; use a new stop key for a new attempt once the reason for the
timeout is resolved. Inspect the deployment and its previous operation first.
The stop budget is 30 seconds. If state storage becomes unwritable, the operation
can remain pending until its outcome can be recorded. Restore disk headroom;
repeating the same key recovers that attempt without repeating completed effects.

Start the same deployment again with a new start key:

```sh
gs serve --deployment dev --detach --idempotency-key dev-start-2
```

Verified cached images can be reused without new download consent. Existing
credentials and repository storage are reused. **The memdb catalogue resets when
the API restarts.** Retained Git objects do not restore catalogue identities or
admission state. Use the explicit Scylla selection below for a retained catalogue.

## Retain the catalogue with Scylla

Matching Linux source builds can run the registered Scylla profile:

```sh
gs serve --deployment persistent --datastore scylladb --allow-provision --detach --idempotency-key persistent-start-1
gs deployment inspect persistent --operation OPERATION_ID --wait ready --timeout 180s -o json
```

Scylla needs additional disk and memory: its image expands to roughly 636 MiB,
its service uses a 1.5 GiB memory setting, and startup creates private execution
copies for the service and helpers. Allow several GiB of free disk space beyond
the retained datastore and repository volumes. Cache limits do not bound the
total footprint; automatic collection of retired execution copies is pending.
The native execution evidence is Linux arm64; amd64 metadata alone does not
establish equivalent runtime support.

Fresh datastore startup creates the registered keyspace and runs the compatible
image-native schema helper before API startup. Retained startup checks the
existing schema and runs the API with automatic migration disabled. Stop retains
both volumes. Restart with the same deployment name and `--datastore scylladb`;
selecting memdb does not convert or restore a retained catalogue.

To run the registered schema migration separately, stop the deployment first and
have a backup available:

```sh
gs deployment stop persistent --idempotency-key before-migration-1
gs migrate --deployment persistent --target gitstore-06152d-schema-012 --backup-acknowledged --idempotency-key migration-1 --timeout 180s -o json
```

`--backup-acknowledged` records your acknowledgment; it does not create a backup.
This preview has no user-facing backup or restore command. Migration requires an
existing keyspace and the registered API/Scylla images in verified cache or
[offline inputs](local-runtime-configuration.md#offline-images). It cannot download
missing images, run arbitrary scripts, downgrade the datastore or start the API.
A successful result includes `migrationCompleted`, `targetVersion` and
`targetDigest`, after schema verification and observed cleanup. A failed or
uncertain migration blocks dependent startup; inspect its operation before retrying.

A timeout or disconnection leaves accepted migration running. Replay the same
key and parameters to recover its identity. Queued migration intent can be
cancelled before execution starts; running migration cancellation returns
`UNSAFE_CANCELLATION`. No running schema helper is killed to satisfy cancellation.

## Manager loss and recovery

After manager loss, reconnect with a matching daemon and inspect the deployment.
The replacement manager invalidates old readiness. It can stop a fully recorded
core graph, recover a created container after a lost creation reply when its exact
identity can be verified, and resume a partial stop when committed termination
receipts and all remaining resource identities can be verified.

Incomplete startup records, lost native replies or missing/replaced resource
identities can still leave ownership uncertain. Keep the state, volumes and
credentials. A different key, deleting a lock or removing the state directory is
not a safe recovery method for a surviving writer. There is no general repair or
force-takeover command in this preview.

A separate stop can reconcile an interrupted migration when the original native,
cgroup, volume and network identities remain verifiable. It waits for a surviving
schema helper to finish naturally, within the stop budget. Cleanup success does
not turn the original failed migration into a successful migration. Missing
cgroup receipts or an unresolved helper leave ownership uncertain.

## Troubleshooting

| Result | Action |
| --- | --- |
| `PROVISIONING_REQUIRED` | Authorize the selected downloads, or configure verified offline inputs. |
| `UNSUPPORTED_HOST` / `UNSUPPORTED_CAPABILITY` | Check native build capabilities and the host prerequisites above. |
| `DEPLOYMENT_CONFLICT` | Inspect the active mutation before submitting another start/stop. |
| `IDEMPOTENCY_CONFLICT` | Recover the original parameters; the key already identifies different intent. |
| `DEADLINE_EXCEEDED` | Inspect the known operation; the timeout did not cancel detached work. |
| `OWNERSHIP_UNCERTAIN` | Preserve state and data; resolve the unavailable resource proof before retrying cleanup. |
| `RESOURCE_LIMIT` | Wait for work to complete or restore disk headroom. Do not delete active history or ownership records. |

If the manager cannot persist a health observation, inspection reports failed
readiness and uncertain recovery and withholds endpoints. Historical operation
results remain unchanged. Ready waits cannot succeed using the old cached row.

## Read deployment logs

Follow retained and new output from a deployment, optionally selecting a component:

```sh
gs deployment logs dev --component api
gs deployment logs dev --component api -o json
```

Text is the default. JSON emits one event per line, including its subscription
identity, sequence and opaque `payload.cursor`. Resume after a retained record:

```sh
gs deployment logs dev --component api --cursor '<payload.cursor>' -o json
```

Ctrl-C closes this observation and leaves the deployment running. Foreground
`serve` sends logs to stderr while its requested result stays on stdout; its
Ctrl-C behavior still requests owned cleanup. Table and YAML are not log formats.
The command timeout bounds setup and individual protocol requests; quiet periods
in an established observation do not become a new workload operation.

Logs are redacted before storage and retained for at most seven days, with
64 MiB per-deployment and 1 GiB global ceilings. Storage pressure may expire
history earlier. `CURSOR_EXPIRED` means the selected record is no longer retained;
start a new observation without its cursor. `STREAM_OVERFLOW` ends only the slow
subscription; reconnect using a retained cursor. A full worker budget reports
`RESOURCE_LIMIT`; retry later. Neither log expiry nor observation failure stops
workloads or removes operation history. Logging is available from matching Linux
source builds that advertise `stream.read`.

## Networking and unavailable features

The current backend uses libslirp for rootless outbound connectivity and loopback
API/Git forwarding. It does not load CNI configurations or execute CNI plugins.
CNI integration would need a separate adapter and its own rootless, cleanup and
isolation validation; selecting libslirp does not provide CNI compatibility.

OIDC, Admin UI and Storefront remain unavailable in this preview. OIDC still needs
managed deployment lifecycle, API issuer integration and recovery. Admin UI needs
a published reference image and verified authentication/startup/readiness mapping.
Storefront needs a verified reference runtime, publication reads and trusted target
selection; alpha.9's dedicated API boundary intentionally returns unavailable.
Component selection reports
the missing prerequisite before provisioning or credential creation.
