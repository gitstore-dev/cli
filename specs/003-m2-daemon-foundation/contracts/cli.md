# M2 CLI Contract

**Status**: Design contract. Read-only component assessment and state-backed
list/inspect/wait have development implementations; lifecycle and standalone logs
remain planned. See [actual evidence](../../../docs/contributing/validation/local-runtime.md). Preserve M0
command families; these refinements must be reconciled with CLI behavior/command
matrix when implemented. Remote contexts/accounts are not deployment selection.

## Command mapping

| Command | Parameters / planned options | Local operation |
| --- | --- | --- |
| `gs serve` | `--deployment <id>`, `--profile <id>`, `--profile-version <version>`, `--datastore memdb\|scylladb`, repeatable `--add <component>`, `--detach`, `--allow-provision`, `--idempotency-key <key>` | validate then deployment.start; foreground waits for generation-ready and observes logs |
| `gs api\|git\|controller\|oidc\|admin\|storefront serve` | Same selection/provisioning/lifecycle options, constrained component selection | Same deployment owner and readiness dependency graph; no unmanaged process tree |
| `gs <component> config validate` | Same registered profile/deployment selection; no provisioning | deployment.validate for selected component/dependency plan; unknown/unperformed native checks explicit |
| `gs deployment list` | `--limit 1..100`, `--cursor <opaque>` | deployment.list |
| `gs deployment inspect <id>` | Optional `--operation <id>`; optional `--wait=accepted\|terminal\|ready` with operation selector | deployment.inspect plus operation.inspect/wait; selected operation must belong to deployment |
| `gs deployment stop <id>` | `--idempotency-key <key>`, `--detach` | deployment.stop; default waits for terminal outcome and observed termination |
| `gs deployment logs <id>` | Optional `--component <enum>`, `--cursor <opaque>` | stream.subscribe(kind=logs), stream.unsubscribe on exit |
| `gs migrate` | Explicit deployment/profile; `--target <registered-version>`, `--backup-acknowledged`, `--idempotency-key <key>`, `--detach` | migration.start; default waits for terminal outcome |

Do not expose a new registry credential command until its syntax is contracted.
Private-image support uses separate configured registry credential references via
the credential broker. Protected-file fallback remains explicit. Offline bundle
location and component configuration are owner-managed daemon/profile settings;
no new caller-controlled path field is added to protocol 1.0.

## Selection and validation

Flags override documented local environment, selected local configuration, then
registered defaults. Use separate `GITSTORE_DEPLOYMENT`, `GITSTORE_PROFILE` and
`GITSTORE_PROFILE_VERSION`; never infer deployment from `--context`. Absent explicit
selection is allowed only for one unambiguous local choice. Unknown component,
invalid format, conflicting stdin or missing consent fails before mutation.

`--add` accepts only registered optional components. Component serve expands only
necessary profile dependencies and shares the same deployment/storage locks.
Different intent against an existing incompatible deployment fails explicitly;
there is no implicit destructive upgrade. A profile version selects immutable
server config/image/readiness mappings, never arbitrary runtime flags.

## Results, waits and interrupt behavior

- Local status, plan and operation commands support table/JSON/YAML; Markdown is
  unsupported. Logs support text (default) or JSON events; reject table/YAML logs.
- Requested results use stdout; progress/logs during foreground serve use stderr.
  Standalone `deployment logs` puts requested log records on stdout.
- Detached start returns durable operation/deployment identity and current readiness;
  success means accepted, not ready. Default foreground start waits for readiness
  and continues observing until interruption/stop; it does not exit merely because
  container processes appeared. Non-interactive foreground execution never prompts.
- `--timeout` bounds each acceptance/observation exchange per protocol, not the
  lifetime of detached work. Clamp/reject values outside local 1..3,600,000 ms
  limits; existing remote deadline behavior is unchanged.
- Generate a key before mutation and report it with known identity. On reconnect
  preserve the same key; expose explicit key for reruns after lost acknowledgment.
  Do not silently issue a new key after uncertain submission. Changed intent/key
  conflicts remain machine-readable.
- Ctrl-C during foreground serve requests durable cancellation, then observes
  owned cleanup within the stop bound. State may remain uncertain; print identity
  and truthful recovery guidance. Ctrl-C during logs only unsubscribes.
- Local waits use the local protocol. Existing remote `--wait` remains gated by
  DEP-004; local implementation must not accidentally enable remote correlation.
- Errors retain M0 code/stage/retryable/category and known operation/deployment
  identity. `NO_COLOR` and redirected output remain unstyled.

## Provisioning consent

Validation is read-only regarding workloads/assets/credentials even if the request
contains allowProvision. Interactive start shows source, assets and sizes when
known, obtains consent, then submits the same immutable profile/asset plan.
Non-interactive start needs `--allow-provision` or verified existing/offline assets.
A changed source/profile manifest requires revalidation, not reused broad consent.
Help, version and remote commands never start runtime downloads or query registries.

## Missing runtime dependencies

Preflight reports selected library capability, compatible host resolution or missing
dependency, expected download/source/size and compatibility diagnostics. Reuse does
not mean arbitrary PATH/loader selection. Existing `--allow-provision` consent covers
only the displayed immutable dependency/profile plan, including libraries and any
justified executable fallback. No separate automatic package-manager installation
or privilege escalation is performed. Incompatible versions, denied downloads and
missing offline assets produce actionable errors without breaking remote commands.


Interactive first-use provisioning displays the selected registered assets, origins,
immutable digests, platform and transfer/disk bounds on stderr before one prompt:
`Provision these assets? [y/N]`. The default is no. No other browser, credential or
selection prompt is permitted in automation; `--allow-provision` supplies explicit
non-interactive authorization for the immutable plan. The T045 implementation passes the real T031 PTY scenario. Native runtime and
host support remain bounded by the separately recorded evidence.

After a start reaches its immutable successful terminal result, foreground
interruption uses an observed stop bound to that start's operation identity. It
cannot cancel history or stop a later generation. Before terminal completion the
CLI submits an explicit cancellation; rejected unsafe stages and uncertain cleanup
retain identity and ownership and are reported without claiming rollback.

The source implementation of standalone log observation follows until Ctrl-C.
Its timeout bounds subscribe/unsubscribe and transport setup, not total follow
duration. JSON is NDJSON with the complete event envelope. A pending subscribe
can be interrupted by closing its observation connection without guessing an
unacknowledged subscription identity. Foreground logs use stderr when the
negotiated daemon supports `stream.read`; lifecycle cancellation/owned stop
semantics remain independent of the log subscription.
