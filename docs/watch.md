# Typed resource watches

The explicit `gitstore-alpha9-2026-11-05` profile supports typed Namespace,
Repository, Product, CategoryTaxonomy (`category`) and File streams over the
configured Admin endpoint, preserving proxy prefixes. These client paths have
controlled test coverage; shipped-version/host/provider qualification remains
separate. Collection watch is unavailable (DEP-002).

Pinned alpha.9 memdb/static-users conformance passed through
`/gitstore/admin/graphql`, including all five bootstrap kinds and File admission
over public Git. After remote logout an existing watch remained open at the
200 ms observation point; reconnecting with that revoked session failed. Immediate
termination of every already-open socket is not promised. See
[the exact evidence](contributing/validation/release-alpha1.md).

```sh
gs watch product --timeout 1h
gs watch product example --selector '{"matchLabels":{"app":"sample"}}' --timeout 1h
gs watch namespace --max-events 10
```

The default is future changes. The client requests the selected kind's verified
bootstrap high-water BOOKMARK, then emits events immediately; it does not perform
an object snapshot or claim an atomic list/watch snapshot. A name filter requires
a namespace except for Namespace itself; otherwise namespace selection follows
the current context/`--namespace`. Without a namespace, supported kinds use the
public fleet-wide stream and the server's authorization. Namespace is cluster
scoped and rejects explicit `--namespace`.

`--selector` accepts JSON `LabelSelectorInput` with string `matchLabels` and
`matchExpressions` using `IN`, `NOT_IN`, `EXISTS`, `DOES_NOT_EXIST`. Expressions
are canonicalized for checkpoint binding. Unsupported fields/operators, invalid
value arity and input limits fail before connection. Name filtering is local;
kind/namespace/labels use the exact public typed subscription.

An ID read does not establish complete ID-watch semantics: typed deletion
tombstones omit UID. `watch --id` fails explicitly (DEP-018), rather than assigning
a name-only deletion to an unverified incarnation. Use a name/label stream when
that selection matches your intended behavior.

## Output, resumption and gaps

Output is NDJSON (`-o json`, also the default). Each event includes type, kind,
name/namespace, the opaque **journal** resourceVersion, typed object or null, and
a bound checkpoint. Object metadata.resourceVersion is a different value and
must not be used as the watch cursor. BOOKMARK/DELETED have no object payload.
Controller status remains observed data, not readiness correlation for a prior
submission. Duplicate deliveries are allowed; there is no exactly-once guarantee.

Checkpoints are private mode-0600 files under
`$XDG_STATE_HOME/gitstore/watch`, defaulting to `$HOME/.local/state/gitstore/watch`.
Use `--checkpoint-file /absolute/private/watch.json` to select another path; its
parent must be owned mode 0700. Resumption binds application version, contract
profile, installation/issuer/account/provider identity, kind, namespace, name and
canonical selector. It rejects mismatches before connecting. Concurrent observers
of the same selection need distinct checkpoint files.

```sh
gs watch product --checkpoint-file /absolute/private/watch.json --max-events 10
gs watch product --checkpoint-file /absolute/private/watch.json --resource-version JOURNAL_CURSOR --timeout 1h
```

Copy the exact cursor from that checkpoint/its emitted record. An unbound raw
server cursor cannot prove selection identity and is rejected. A matching private
checkpoint can be copied for explicit resumption. Ordinary cursor contents stay
opaque; known per-kind bootstrap sentinels are handled separately.

The client persists a position **after** fully emitting the record. A crash may
therefore repeat output; it must not silently skip a record that was only partly
written. Reconnect uses that position with bounded backoff. Losing a connection
before the first durable position fails with `WATCH_GAP`. `WATCH_EXPIRED` stops
and requires an explicit fresh read/list and a new watch; the client never hides
expiry by starting a fresh stream automatically. Starting without a resume cursor
is an explicit new future-change stream, not history recovery.

## Bounds and credentials

The normal command timeout is 30 seconds; use `--timeout` to select a longer finite
watch window. Reconnect has at most five retries within that deadline. Read I/O is
bounded to 15 seconds, writes to 2 seconds. There is one event in progress and no
accumulating client event queue. A consumer must accept a complete output record
within 2 seconds; otherwise `WATCH_BACKPRESSURE` stops the stream without advancing
past partial output. Interrupting the process closes its connection and releases
its checkpoint lock; the last completed record remains the resume position.

`--max-events` stops successfully after that many delivered data events (duplicates
count; bookmarks do not). Frames/messages default to 1 MiB, configurable through
`--max-frame-bytes` between 1 KiB and 16 MiB. Read/write buffers are 16/256 KiB;
selectors are at most 16 KiB with 64 label/expression entries, and credentials and
cursors at most 64 KiB. Oversized/malformed frames and wrong typed identity,
namespace or selected labels fail explicitly. No truncation is used.

Credentials use the selected account's normal protected source. `connection_init`
carries Authorization; URLs/checkpoints never contain a token. Reconnect can read
an updated explicit file/OS credential for the same identity; credential stdin is
consumed once. Authentication denial/expiry stops rather than refreshing or
discovering a provider implicitly. Redirects are not followed. No daemon, runtime
assets or Git executable participates.

These are prerelease behaviors. Upstream is early alpha with incompatible schema
changes; there are no upgrade compatibility, data preservation or recovery
guarantees. Keep independent backups and avoid irreplaceable data.
