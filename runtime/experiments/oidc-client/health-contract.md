# Fixed OIDC health consumer contract

This is an internal, private-worker contract. It does not expose an OIDC
profile, public endpoint, target capability or service status to callers.

| Fixed role | Request target inside the owned namespace | Ready predicate | Source/evidence |
| --- | --- | --- | --- |
| Hydra admin | `http://127.0.0.1:4445/health/ready` | HTTP 200; JSON `status` equals `ok` | Hydra `ee8c339ddada3a42529c0416897abc32bad03bbb`; existing `gstd_oidc_http` and controlled receipt |
| Kratos admin | `http://127.0.0.1:4434/admin/health/ready` | HTTP 200; body discarded | Kratos `36e624c716e45ab405599c71b42c384dd45bdfbb`, [serve evidence](serve-evidence.json) |
| Bridge | `http://127.0.0.1:<selected bridge port>/health` | HTTP 200; body discarded | pinned native combined-graph probe |
| Identity UI | `http://127.0.0.1:<selected UI port>/health/ready` | HTTP 200; body discarded | pinned native combined-graph probe |
| SMTP/Mailpit HTTP | `http://127.0.0.1:<selected mail port>/readyz` | HTTP 200; body discarded | pinned native combined-graph probe |

The worker accepts no caller URL, redirect, proxy setting or response schema.
It uses one GET with a 500 ms connection and 2 s total deadline. An unavailable
or non-200 non-Hydra role is pending; it is not interpreted as a shared provider
failure taxonomy. Hydra retains its existing JSON predicate and result semantics.
Non-Hydra reads discard at most 1 MiB (`GSTD_FRAME_MAX`); an oversized or
incomplete response cannot report ready. Health jobs inherit no configuration
or broker descriptor. Responses never enter IPC, records, logs or diagnostics.

The [OpenAPI consumer mapping](../../../postman/specs/oidc-health.openapi.json)
and [local collection](../../../postman/collections/oidc-health/README.md) encode
these success predicates. Hydra's exact JSON contract remains in the separate
[provider specification](../../../postman/specs/oidc-provider.openapi.json).
Bridge/UI/SMTP paths come from the pinned images in
[optional evidence](../../profiles/optional-evidence.json) and the actual
[five-service receipt](combined-graph-evidence.json); this does not claim their
failure status taxonomies are interchangeable.

Issuer readiness is a separate fixed read-only action. Its two GETs use the
selected public loopback port and paths `/.well-known/openid-configuration` and
`/.well-known/jwks.json`. Pinned Hydra revision
`ee8c339ddada3a42529c0416897abc32bad03bbb`, `oauth2/handler.go` and `jwk/handler.go`,
map the discovery issuer/JWKS fields and public RSA/RS256 signing keys. The consumer
requires exact configured issuer and JWKS URI, a nonempty bounded key array and
public signing-key fields; it rejects duplicate key IDs and private fields.
It checks the envelope, not a token signature. It never follows response URLs,
redirects or proxies. Each response has the existing 1 MiB/two-second bound;
the enclosing worker has a five-second deadline and no credential capability.
The final production sweep reads these documents while current native, namespace,
provider-writer and enrollment observations remain held. Active monitoring uses
this action for Hydra. [OpenAPI](../../../postman/specs/oidc-discovery.openapi.json)
and [collection](../../../postman/collections/oidc-discovery/README.md) encode the
consumer mapping. Hydra may initialize its own missing signing-key set on GET;
that provider behavior grants no client-enrollment retry authority to the daemon.
