# gitstored Local Protocol 1.0

**Status**: Accepted M0 wire design, 2026-10-27. **Implementation**: absent.
**Owner**: CLI/daemon maintainers. **Evidence**: EVD-022; ADR-0001 and ADR-0002.
**Release gate**: DEP-012. Protocol acceptance resolves design DEP-008 only.

## Transport, authorization and framing

Use a Unix stream socket in a user-owned runtime directory, mode 0700 directory and
0600 socket, without following caller-controlled symlinks. Validate peer identity using
the supported OS peer-credential mechanism; only the daemon's effective user is admitted.
No TCP endpoint, catalogue-token substitute or root-equivalent shared socket group.
Perform peer authorization before accepting requests. An unauthorized connection may
receive a bounded error with requestId null, then is closed; it can never start work.

Each frame is four unsigned big-endian length bytes followed by exactly that many
UTF-8 JSON bytes. Prefix length excludes the four bytes. Valid payload size is 1 through
1,048,576 bytes; reject excess before allocation. Read/write partial frames correctly;
socket reads are not message boundaries. Zero length, truncation, invalid UTF-8, duplicate
keys, invalid JSON, excessive depth/collections or non-object top level cause a bounded
error and connection close. A completed JSON frame consumes no trailing out-of-frame bytes.

Objects contain unique string keys; no NaN, infinity or fractional numbers. Integers
range from −9,007,199,254,740,991 through +9,007,199,254,740,991; counts/sequences/timeouts
are nonnegative with narrower bounds below. Identifiers are 1–64 ASCII characters
`A-Z a-z 0-9 . _ : -`; deployment/profile names are selected registered identifiers,
not filesystem paths. Strings are UTF-8, at most 262,144 encoded bytes. Paths and binaries
are never accepted as free-form execution options. Optional fields are omitted, not null,
except an uncorrelatable error's requestId. Empty optional selectors/cursors are invalid.

## Envelope and version negotiation

Request fields: `version` = {major, minor}, `requestId`, `method`, `params` object,
optional `idempotencyKey`, optional `timeoutMs` (1–3,600,000). Unknown optional envelope
fields are ignored within a compatible minor version; unknown params are INVALID_ARGUMENT.
Missing or wrong-typed required fields fail. Method names and enum strings are case-sensitive.

First request is `system.negotiate`, with protocol envelope 1.0 and params
`{supported:[{major,minor}],requiredCapabilities:[string]}`. Supported list is 1–16 items;
capability names are unique, at most 64. The server selects the highest common minor
within major 1 and verifies each required capability. Its result contains selected
version, capabilities and numeric limits. No compatible major gives INCOMPATIBLE_VERSION;
missing required capability gives UNSUPPORTED_CAPABILITY. Neither may execute mutations.
After negotiation, requests must use the selected version. A version change needs a new
connection. `system.capabilities` can reread capabilities/limits; no mutation depends on
an absent capability. Minor changes are additive; semantic breaking changes need a major.

Response fields: `version`, `requestId`, and exactly one of `result` or `error`.
Unknown IDs or duplicate active request IDs are invalid; requestId identifies an exchange,
not durable work. Responses can arrive out of order; one response ends the exchange.
Requests are never batch JSON arrays. Fire-and-forget mutations are forbidden.

Error fields: `code`, `stage`, `retryable`, safe `message`, optional `operationId`,
`deploymentId`, `revision`; requestId is null only when correlation cannot be established.
Event fields: `version`, `subscriptionId`, `operationId` or `deploymentId`, `sequence`,
`type`, `payload`. Events do not carry requestId or satisfy a pending request. Each
subscription has strictly increasing nonnegative sequence and an opaque `cursor` in
payload where replay is available. Sequence is not a resourceVersion or object generation.

## Methods and typed parameters

All methods require authorized peer and negotiated version except negotiate itself.
Every method may return transport/argument/version/resource/deadline/internal errors.
Absent required capability is always unsupported before side effects. Read methods are
safe to repeat; durable mutations require idempotencyKey. No method accepts a command,
host path, OCI hooks, raw runtime flags, arbitrary mount or privileged mode.

| Method | Capability | Params (all fields listed; ? means optional) | Result / completion | Additional failure conditions |
| --- | --- | --- | --- | --- |
| system.negotiate | bootstrap | supported, requiredCapabilities as above | selectedVersion, capabilities, limits; connection established | incompatible version, missing required capability |
| system.capabilities | system.read | {} | selectedVersion, capabilities, limits | negotiation required |
| deployment.validate | deployment.validate | profileId, profileVersion, components? [registered enum], datastore? enum, deploymentId?, allowProvision? bool | valid bool, effective non-secret plan, requiredAssets, hostRequirements, diagnostics; no downloads/workloads | unknown profile, unsupported host/backend, config invalid |
| deployment.start | deployment.start | deploymentId, profileId, profileVersion, components? [enum], datastore? enum, allowProvision? bool=false | operation reference after durable intent; not readiness | existing incompatible deployment, unowned writer, provisioning required, unsupported host |
| deployment.inspect | deployment.read | deploymentId | Deployment record below | not found |
| deployment.list | deployment.read | limit? 1–100=100, cursor? opaque | deployments, nextCursor?; bounded page | cursor expired |
| deployment.stop | deployment.stop | deploymentId | durable operation; success after observed termination, volumes retained | unowned workloads, stop deadline, unknown target |
| operation.inspect | operation.read | operationId | Operation record below | not found or retention expired |
| operation.wait | operation.read | operationId, until enum accepted/terminal/ready | operation plus deployment when condition reached; otherwise error with identity | unsupported ready condition, operation failed, deadline exceeded |
| operation.cancel | operation.cancel | operationId | durable cancellation-request operation, target identity | unsafe/noncancellable stage; already-terminal returns no-op result |
| migration.start | migration.start | deploymentId, targetVersion, backupAcknowledged bool | durable exclusive migration operation; no implicit downgrade | migration conflict, compatibility/backup precondition, unsupported datastore |
| stream.subscribe | stream.read | deploymentId, kind enum logs/events, component? enum, cursor? opaque | subscriptionId, replayAvailable bool; then events | cursor expired, unknown component, subscription quota |
| stream.unsubscribe | stream.read | subscriptionId | closed bool; already-closed false | unknown connection subscription |

Profile datastores are `memdb` or `scylladb`; components are `api`, `git`, `controller`,
`oidc`, `admin`, `storefront`. A profile constrains allowed combinations and dependency
readiness graph. Server-resolved profile metadata, not caller-supplied paths, controls
images, volumes, mount policy and entrypoints. `targetVersion` identifies a registered
migration target, not an arbitrary script. `allowProvision` grants only the plan's verified
required assets; an unexpected plan/source change requires revalidation/consent.

profileId, profileVersion, deploymentId, operationId, subscriptionId and targetVersion
are identifier strings under the rule above. Components is a unique array of the listed
enum values, defaulting to the profile's declared components. Cursor is an opaque
nonempty UTF-8 string bounded by the string limit. Boolean fields accept only JSON
booleans. Limit and timeout fields accept only integers in their declared ranges.
Validate all params and normalize defaults before computing the semantic fingerprint.

Operation record: operationId, method, deploymentId when applicable, acceptedAt/updatedAt
(UTC RFC3339), state accepted/running/succeeded/failed/cancelled, recovery enum
normal/reconciling/uncertain, result? or error? only at terminal state, targetOperationId?
for cancellation. Deployment record: deploymentId, profileId/version, desiredState
running/stopped, component identities/states, readiness pending/ready/failed, retained
volume/network identities, writerOwnership held/released/uncertain, lastOperationId?,
generation, observedGeneration, recovery, observedAt. Redact configuration and backend diagnostics. Readiness belongs
to the intended deployment generation; it is independent of operation acceptance.

## Idempotency, deadlines, cancellation and recovery

Before acknowledgment, atomically persist operation ID, normalized intent, peer UID,
method, key and parameter fingerprint. Fingerprint is SHA-256 of canonical normalized
typed parameters, sorted UTF-8 object keys, explicit defaults, integer decimal values;
array order is significant. Exclude requestId, timeoutMs and negotiation version.
Same peer/method/key plus same parameters returns original operation, including after
reconnect; different parameters gives IDEMPOTENCY_CONFLICT. Lost acknowledgment must be
resolved by same-key replay/inspection, not a fresh untracked start. Store fingerprint
and operation identity rather than secret request payloads.

Retain all active operations and deployment start identity while deployment exists.
Retain terminal records/dedupe data for at least seven days; delete start identity only
after deployment removal and minimum terminal retention. Cap operation metadata at
256 MiB and 100,000 records per daemon, whichever first. Evict eligible expired terminal
records only; when no safe space remains, reject new mutation before side effects with
RESOURCE_LIMIT. Never evict active intent or lock identity to make room. Expired keys have
no replay guarantee; clients must inspect the deployment first. Unknown history plus
uncertain workload ownership fails closed, even if key differs.

States progress accepted → running → succeeded/failed/cancelled. Cancellation before
work starts may transition accepted → cancelled. Recovery does not manufacture a terminal
state; persist reconciling/uncertain while existing workloads and resources are inspected.
Acquire deployment/storage ownership before materializing a writer. Observe prior writer
termination, not just absence of an in-memory PID record, before replacement or lock release.
Exactly one writer per repository volume, including simultaneous distinct-key starts.

timeoutMs bounds the current observation/exchange, measured monotonically from receipt.
Default is 30 seconds for reads/acceptance and 60 seconds for wait. Timeout never silently
cancels accepted detached work. Return DEADLINE_EXCEEDED with known operation identity;
if a pre-acceptance response is lost, client uses the same idempotency key. A wait for
ready fails unsupported if not a deployment-start operation; terminal failed/cancelled
cannot yield successful ready. An old ready generation never satisfies a new start.

Client disconnect closes its subscriptions; detached operations continue. Foreground
CLI cancellation explicitly calls operation.cancel and then observes cleanup. Cancellation
is a separate idempotent operation, not guaranteed rollback. Unsafe migration stages
return UNSAFE_CANCELLATION; the target keeps its true state. A terminal target yields
a succeeded no-op cancellation with target state. Stop and cleanup retries operate only
on resources whose recorded owner matches the deployment. Preserve retained volumes.

Per-deployment mutation serialization applies to workload-changing start/stop/migrate,
not observation or cancellation signalling. Accept cancellation through a bounded control
path even while all runtime workers are occupied; it records intent and signals the
existing operation without acquiring a second writer or waiting behind that target's
exclusive execution lock. A conflicting start/stop/migrate returns DEPLOYMENT_CONFLICT
rather than waiting indefinitely behind an active mutation. Record a new deployment
generation before accepted start; ready waits require observedGeneration to equal that
generation. This avoids both cancellation deadlock and stale-ready success.

## Numeric resource policy

| Bound | Accepted value | Excess / expiry |
| --- | --- | --- |
| Frame / nesting | 1 MiB / 32 levels | Reject and close before unbounded allocation. |
| Single string / items per collection | 256 KiB / 4096 | Reject input; page/chunk output. |
| Connections / outstanding requests | 32 per daemon / 16 per connection | Reject connection or request. |
| Subscriptions | 8 per connection | RESOURCE_LIMIT. |
| Per-subscription event queue | 256 events or 4 MiB, whichever first | Overflow/gap event where possible, then close stream. |
| Global event queues | 64 MiB per daemon | Shed slow subscriptions with gap; never grow without bound. |
| Per-deployment logs | 64 MiB, maximum age 7 days | Evict oldest; expire cursors explicitly. |
| Global retained logs | 1 GiB | Evict oldest eligible logs; operation metadata retained separately. |
| Worker / per-deployment mutation | 8 workers / 1 mutation | Queue within cap; reject if full. |
| Pending worker jobs | 64 | RESOURCE_LIMIT before acceptance if intent cannot be scheduled durably. |
| Partial-frame / idle connection | 10 seconds / 5 minutes without frames | Close connection; accepted work survives. |
| Graceful stop / daemon shutdown | 30 seconds each | Record deadline, leave ownership held/uncertain until termination proven. |
| Operation metadata / retention | 256 MiB or 100,000 records / ≥7 days terminal | Safe eviction only; refuse new mutation if full. |

Idle timeout is refreshed by valid request/response/event traffic; client can query
capabilities as keepalive for silent streams. Limits are returned by negotiate/capabilities.
M0 accepts the planning values and adds global queue/log and operation-history caps to
prevent per-client bounds multiplying into unbounded daemon growth. These are policies,
not measured capacity. M2 must prove enforcement and revise them through protocol policy
review if unsuitable. No unlimited subscriber replay: expired cursor returns CURSOR_EXPIRED;
overflow returns STREAM_OVERFLOW and inspection/resubscription guidance.

## Error taxonomy

| Codes | CLI category | Stage / recovery |
| --- | --- | --- |
| INVALID_ARGUMENT, NEGOTIATION_REQUIRED, MALFORMED_FRAME | 2 | parse/validate; fix input, no automatic retry |
| UNAUTHORIZED_PEER | 3 | authorize; close, no side effects |
| NOT_FOUND | 4 | resolve; no blind mutation retry |
| IDEMPOTENCY_CONFLICT, DEPLOYMENT_CONFLICT, OWNERSHIP_UNCERTAIN, MIGRATION_CONFLICT | 5 | plan/recover; inspect ownership, never replace blindly |
| INCOMPATIBLE_VERSION, UNSUPPORTED_CAPABILITY, UNSUPPORTED_HOST, UNSAFE_CANCELLATION | 6 | negotiate/plan/cancel; explicit unsupported behavior |
| DEADLINE_EXCEEDED | 7 | wait/stop/transport; preserve known identity |
| RESOURCE_LIMIT, CURSOR_EXPIRED, STREAM_OVERFLOW, PROVISIONING_REQUIRED, MIGRATION_PRECONDITION, OPERATION_FAILED | 8 | validate/queue/stream/runtime; retryable only when safely observable/idempotent |
| INTERNAL | 1 | internal; safe message, operation inspection if known |

Each error carries its precise code/stage/retryability. A terminal operation's failure
does not erase recorded result, ownership or diagnostics; no automatic schema downgrade.
Malformed/oversized unauthenticated connections may be closed without response; there is
no obligation to parse an attacker-controlled request ID to return an error.

## Examples

The paired JSON objects below are documentation fixtures, not network test results.
Each request and response is separately framed in actual use. Version 1.0 is already
negotiated except the handshake example. Timestamp strings and IDs are illustrative.

### EX-001 Negotiate

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r1",
    "method": "system.negotiate",
    "params": {
      "supported": [
        {
          "major": 1,
          "minor": 0
        }
      ],
      "requiredCapabilities": [
        "deployment.start"
      ]
    }
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r1",
    "result": {
      "selectedVersion": {
        "major": 1,
        "minor": 0
      },
      "capabilities": [
        "system.read",
        "deployment.validate",
        "deployment.start",
        "deployment.read",
        "deployment.stop",
        "operation.read",
        "operation.cancel",
        "migration.start",
        "stream.read"
      ],
      "limits": {
        "frameBytes": 1048576,
        "nesting": 32,
        "stringBytes": 262144,
        "collectionItems": 4096,
        "connections": 32,
        "outstandingPerConnection": 16,
        "subscriptionsPerConnection": 8,
        "queuedEventsPerSubscription": 256,
        "queuedBytesPerSubscription": 4194304,
        "globalQueuedBytes": 67108864,
        "logsPerDeploymentBytes": 67108864,
        "logMaxAgeDays": 7,
        "globalLogBytes": 1073741824,
        "workers": 8,
        "mutationPerDeployment": 1,
        "pendingJobs": 64,
        "partialFrameMs": 10000,
        "idleMs": 300000,
        "stopMs": 30000,
        "shutdownMs": 30000,
        "operationMetadataBytes": 268435456,
        "operationRecords": 100000,
        "terminalRetentionDays": 7
      }
    }
  }
}
```

### EX-002 Validate without provisioning

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r2",
    "method": "deployment.validate",
    "params": {
      "profileId": "core",
      "profileVersion": "1"
    }
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r2",
    "result": {
      "valid": true,
      "effectivePlan": {
        "profileId": "core",
        "components": [
          "api",
          "git",
          "controller"
        ]
      },
      "requiredAssets": [],
      "hostRequirements": [
        "user-namespaces",
        "uid-mapping"
      ],
      "diagnostics": []
    }
  }
}
```

### EX-003 Start acceptance is not readiness

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r3",
    "method": "deployment.start",
    "params": {
      "deploymentId": "dev",
      "profileId": "core",
      "profileVersion": "1",
      "datastore": "memdb",
      "allowProvision": false
    },
    "idempotencyKey": "start-dev-1"
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r3",
    "result": {
      "operationId": "op-start",
      "method": "deployment.start",
      "deploymentId": "dev",
      "acceptedAt": "2026-10-27T12:00:00Z",
      "updatedAt": "2026-10-27T12:00:00Z",
      "state": "accepted",
      "recovery": "normal"
    }
  }
}
```

### EX-004 Same-key replay returns original operation

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r4",
    "method": "deployment.start",
    "params": {
      "deploymentId": "dev",
      "profileId": "core",
      "profileVersion": "1",
      "datastore": "memdb",
      "allowProvision": false
    },
    "idempotencyKey": "start-dev-1"
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r4",
    "result": {
      "operationId": "op-start",
      "method": "deployment.start",
      "deploymentId": "dev",
      "acceptedAt": "2026-10-27T12:00:00Z",
      "updatedAt": "2026-10-27T12:00:00Z",
      "state": "accepted",
      "recovery": "normal"
    }
  }
}
```

### EX-005 Changed payload conflicts

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r5",
    "method": "deployment.start",
    "params": {
      "deploymentId": "dev",
      "profileId": "core",
      "profileVersion": "1",
      "datastore": "scylladb",
      "allowProvision": false
    },
    "idempotencyKey": "start-dev-1"
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r5",
    "error": {
      "code": "IDEMPOTENCY_CONFLICT",
      "stage": "validate",
      "retryable": false,
      "message": "Key already identifies different parameters",
      "operationId": "op-start",
      "deploymentId": "dev"
    }
  }
}
```

### EX-006 Inspect running work

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r6",
    "method": "operation.inspect",
    "params": {
      "operationId": "op-start"
    }
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r6",
    "result": {
      "operationId": "op-start",
      "method": "deployment.start",
      "deploymentId": "dev",
      "acceptedAt": "2026-10-27T12:00:00Z",
      "updatedAt": "2026-10-27T12:00:00Z",
      "state": "running",
      "recovery": "normal"
    }
  }
}
```

### EX-007 Wait deadline preserves identity

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r7",
    "method": "operation.wait",
    "params": {
      "operationId": "op-start",
      "until": "ready"
    },
    "timeoutMs": 1000
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r7",
    "error": {
      "code": "DEADLINE_EXCEEDED",
      "stage": "wait",
      "retryable": false,
      "message": "Readiness not verified before deadline",
      "operationId": "op-start",
      "deploymentId": "dev"
    }
  }
}
```

### EX-008 Readiness failure is inspectable

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r8",
    "method": "operation.inspect",
    "params": {
      "operationId": "op-start"
    }
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r8",
    "result": {
      "operationId": "op-start",
      "method": "deployment.start",
      "deploymentId": "dev",
      "acceptedAt": "2026-10-27T12:00:00Z",
      "updatedAt": "2026-10-27T12:00:00Z",
      "state": "failed",
      "recovery": "normal",
      "error": {
        "code": "OPERATION_FAILED",
        "stage": "runtime",
        "retryable": false,
        "message": "API health check failed",
        "deploymentId": "dev"
      }
    }
  }
}
```

### EX-009 Stop acceptance

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r9",
    "method": "deployment.stop",
    "params": {
      "deploymentId": "dev"
    },
    "idempotencyKey": "stop-dev-1"
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r9",
    "result": {
      "operationId": "op-stop",
      "method": "deployment.stop",
      "deploymentId": "dev",
      "acceptedAt": "2026-10-27T12:00:00Z",
      "updatedAt": "2026-10-27T12:00:00Z",
      "state": "accepted",
      "recovery": "normal"
    }
  }
}
```

### EX-010 Stop completion retains volumes

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r10",
    "method": "operation.inspect",
    "params": {
      "operationId": "op-stop"
    }
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r10",
    "result": {
      "operationId": "op-stop",
      "method": "deployment.stop",
      "deploymentId": "dev",
      "acceptedAt": "2026-10-27T12:00:00Z",
      "updatedAt": "2026-10-27T12:00:00Z",
      "state": "succeeded",
      "recovery": "normal",
      "result": {
        "writerTerminated": true,
        "retainedVolumeIds": [
          "repo-dev"
        ]
      }
    }
  }
}
```

### EX-011 Migration is tracked

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r11",
    "method": "migration.start",
    "params": {
      "deploymentId": "dev",
      "targetVersion": "1",
      "backupAcknowledged": true
    },
    "idempotencyKey": "migrate-dev-1"
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r11",
    "result": {
      "operationId": "op-migrate",
      "method": "migration.start",
      "deploymentId": "dev",
      "acceptedAt": "2026-10-27T12:00:00Z",
      "updatedAt": "2026-10-27T12:00:00Z",
      "state": "accepted",
      "recovery": "normal"
    }
  }
}
```

### EX-012 Explicit cancellation request

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r12",
    "method": "operation.cancel",
    "params": {
      "operationId": "op-start"
    },
    "idempotencyKey": "cancel-dev-1"
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r12",
    "result": {
      "operationId": "op-cancel",
      "method": "operation.cancel",
      "deploymentId": "dev",
      "acceptedAt": "2026-10-27T12:00:00Z",
      "updatedAt": "2026-10-27T12:00:00Z",
      "state": "accepted",
      "recovery": "normal",
      "targetOperationId": "op-start"
    }
  }
}
```

### EX-013 Unsafe migration cancellation is refused

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r13",
    "method": "operation.cancel",
    "params": {
      "operationId": "op-migrate"
    },
    "idempotencyKey": "cancel-migrate-1"
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r13",
    "error": {
      "code": "UNSAFE_CANCELLATION",
      "stage": "cancel",
      "retryable": false,
      "message": "Migration has entered a noncancellable stage",
      "operationId": "op-migrate",
      "deploymentId": "dev"
    }
  }
}
```

### EX-014 Subscribe to bounded logs

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r14",
    "method": "stream.subscribe",
    "params": {
      "deploymentId": "dev",
      "kind": "logs",
      "component": "api"
    }
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r14",
    "result": {
      "subscriptionId": "sub-1",
      "replayAvailable": true
    }
  }
}
```

### EX-015 Event has separate subscription identity

```json
{
  "version": {
    "major": 1,
    "minor": 0
  },
  "subscriptionId": "sub-1",
  "deploymentId": "dev",
  "sequence": 1,
  "type": "log",
  "payload": {
    "cursor": "log:1",
    "text": "API starting"
  }
}
```

### EX-016 Version mismatch

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r16",
    "method": "system.negotiate",
    "params": {
      "supported": [
        {
          "major": 2,
          "minor": 0
        }
      ],
      "requiredCapabilities": []
    }
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r16",
    "error": {
      "code": "INCOMPATIBLE_VERSION",
      "stage": "negotiate",
      "retryable": false,
      "message": "No common protocol major"
    }
  }
}
```

### EX-017 Unknown capability

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r17",
    "method": "system.negotiate",
    "params": {
      "supported": [
        {
          "major": 1,
          "minor": 0
        }
      ],
      "requiredCapabilities": [
        "host.exec"
      ]
    }
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r17",
    "error": {
      "code": "UNSUPPORTED_CAPABILITY",
      "stage": "negotiate",
      "retryable": false,
      "message": "Required capability is unavailable"
    }
  }
}
```

### EX-018 Expired cursor

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r18",
    "method": "stream.subscribe",
    "params": {
      "deploymentId": "dev",
      "kind": "logs",
      "cursor": "log:expired"
    }
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r18",
    "error": {
      "code": "CURSOR_EXPIRED",
      "stage": "stream",
      "retryable": false,
      "message": "Requested log history is no longer retained",
      "deploymentId": "dev"
    }
  }
}
```

### EX-019 Overflow closes only the subscription

```json
{
  "version": {
    "major": 1,
    "minor": 0
  },
  "subscriptionId": "sub-1",
  "deploymentId": "dev",
  "sequence": 2,
  "type": "error",
  "payload": {
    "error": {
      "code": "STREAM_OVERFLOW",
      "stage": "stream",
      "retryable": false,
      "message": "Consumer queue exceeded limit",
      "deploymentId": "dev"
    },
    "gap": true
  }
}
```

### EX-020 Unauthorized peer

```json
{
  "version": {
    "major": 1,
    "minor": 0
  },
  "requestId": null,
  "error": {
    "code": "UNAUTHORIZED_PEER",
    "stage": "authorize",
    "retryable": false,
    "message": "Peer identity is not permitted"
  }
}
```

### EX-021 Invalid or oversized frame

```json
{
  "version": {
    "major": 1,
    "minor": 0
  },
  "requestId": null,
  "error": {
    "code": "MALFORMED_FRAME",
    "stage": "parse",
    "retryable": false,
    "message": "Frame is invalid or exceeds limit"
  }
}
```

### EX-022 Uncertain ownership after restart

```json
{
  "request": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r22",
    "method": "deployment.start",
    "params": {
      "deploymentId": "dev",
      "profileId": "core",
      "profileVersion": "1",
      "datastore": "memdb",
      "allowProvision": false
    },
    "idempotencyKey": "start-dev-2"
  },
  "response": {
    "version": {
      "major": 1,
      "minor": 0
    },
    "requestId": "r22",
    "error": {
      "code": "OWNERSHIP_UNCERTAIN",
      "stage": "recover",
      "retryable": false,
      "message": "Previous writer termination has not been established",
      "deploymentId": "dev"
    }
  }
}
```
