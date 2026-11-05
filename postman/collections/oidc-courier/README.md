# Disposable OIDC courier check

This collection verifies a synthetic native Kratos verification email reaches
Mailpit. It is a maintainer fixture, not a CLI sign-in flow or production identity
management interface. Run it only inside an explicitly owned disposable provider
namespace. The guarded URLs use its fixed numeric-loopback ports; admin endpoints
must not be forwarded to the host.

The environment must explicitly set `ownedDisposableOidcFixture=true`,
`identityAdminUrl=http://127.0.0.1:4434`,
`identityPublicUrl=http://127.0.0.1:24003`,
`mailboxUrl=http://127.0.0.1:24005`, a synthetic `fixtureEmail` under
`example.invalid`, and a `fixtureUsername` beginning `gitstore-oidc-fixture-`.
Use a fresh dedicated realm and empty mailbox. Both mutations are single-attempt;
failures stop the run. Four fixed read-only polls bound delivery observation.

With the maintainer-only Postman CLI and a protected environment file:

```sh
postman collection run /absolute/source/postman/collections/oidc-courier \
  -e /absolute/private/courier-environment.json --no-report-events \
  --ignore-redirects --delay-request 1000 --timeout 60000 \
  --timeout-request 5000 --timeout-script 5000 --bail
```

Keep runner output private. Do not export HTTP responses, email content or
verification links/tokens. Record only the exit code and assertion counts.
The source-derived contract is
[oidc-courier.openapi.json](../../specs/oidc-courier.openapi.json); native
[evidence](../../../runtime/experiments/oidc-client/courier-evidence.json) records
the tested provider versions, scope and cleanup.
