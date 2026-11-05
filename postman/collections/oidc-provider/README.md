# OIDC provider consumer checks

The source is [the narrow consumer OpenAPI contract](../../specs/oidc-provider.openapi.json),
derived from Hydra 2.3.0 source revision
`ee8c339ddada3a42529c0416897abc32bad03bbb`. These requests do not enable an OIDC
profile or CLI login. The generated collection was hardened with assertions and
fixture guards; preserve those when regenerating it.

Run only against the explicitly owned disposable fixture. There is no default
target. Provide `ownedDisposableOidcFixture=true`, numeric-loopback `baseUrl`, a
`gitstore-oidc-fixture-*` client ID, the registered numeric-loopback `/callback`
URI, audience and synthetic `clientSecret` through a protected local environment
file. Do not put secrets in arguments or commit environments or run reports.

Creation mutates its fixture once. Hydra's successful response can contain the
supplied client secret and a registration access token. Never log/export that
response or copy it into operation history. Inspection checks public fields only;
it cannot establish ownership of a client secret after a lost creation reply.
Do not blindly retry, overwrite or rotate an uncertain registration.

`postman collection lint` passes all three requests. The Mac spec linter and
generator both exit with `SecItemCopyMatching failed -50`; the generator wrote
files before crashing. Independent structural validation against the official
OpenAPI 3.1 schema and component schema syntax passes. This is not a successful
Postman spec-lint run or a real Hydra lifecycle result.

Linux Postman 1.86.0 `spec lint --issueType syntax --no-report-events` passes with
no issues. Its normal governance lint requires cloud sign-in and was not run.
The actual saved collection also passes all three request/assertion blocks against
the contract-derived controlled mock; see the
[bounded worker receipt](../../../runtime/experiments/oidc-client/evidence.json).
Neither establishes a real Hydra provider or complete OIDC profile pass.
