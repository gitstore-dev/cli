# Fixed private health consumers

Generated from the role mappings in [the consumer specification](../../specs/oidc-health.openapi.json).
These five GETs assert only verified success predicates. The identity UI and Hydra
share a path but have different body contracts; `x-consumer-roles` preserves that
distinction. Other roles deliberately have no JSON assertions.

Run only against owned controlled listeners or the corresponding owned native
namespace. Provider admin ports must never be forwarded publicly. Override the
five base variables to select already allocated private endpoints. The C worker
accepts fixed roles and ports, never these collection URLs.

```sh
postman collection lint postman/collections/oidc-health
postman collection run postman/collections/oidc-health --report-events=false
```

Passing a collection against controlled responses verifies the saved consumer
requests and assertions. It does not establish native readiness or public lifecycle
support. C regression tests separately exercise the actual worker and its failures.
