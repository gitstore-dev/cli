# Local issuer document consumers

Derived from [the pinned consumer contract](../../specs/oidc-discovery.openapi.json).
Both requests use an already allocated loopback issuer origin. Discovery must name
that exact issuer (including its trailing slash) and the fixed JWKS path. Response
URLs are checked as data; they never select a subsequent request. JWKS readiness
checks bounded public RSA/RS256 signing-key envelopes, including absence of private
fields. It does not verify a token signature or establish browser login support.

Run `postman collection lint postman/collections/oidc-discovery` and the
maintainer [controlled runner](../../../runtime/experiments/oidc-client/run-discovery.py)
against `postman/mocks/oidc-discovery`. All artifacts remain local.
