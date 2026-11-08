# Explicit static-user auth checks

Derived from [the pinned GraphQL contract](../../specs/gitstore-alpha9-auth.openapi.json).
Run only against the owned numeric-loopback simulation with
`ownedDisposableFixture=true`. Credentials are synthetic. Session variables are
in-memory run state; never export environments/reports containing real tokens.
Login/refresh omit Authorization; logout sends the current Bearer. Checks cover
rotation and rejection of the consumed token rather than assuming HTTP 200 is
success. No OIDC/device/discovery contract is inferred. These controls do not
establish live provider compatibility or native credential-store persistence.
