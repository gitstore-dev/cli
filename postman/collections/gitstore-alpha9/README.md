# Alpha.9 resource consumer checks

Generated from [the narrow consumer contract](../../specs/gitstore-alpha9.openapi.json),
then hardened with a fixture guard and response assertions. The pinned upstream
File schema uses `FileBy!`; the older contract profile retains flat arguments.
The collection also covers exact Namespace/Repository create inputs from
[the scoped authoring envelope](../../specs/gitstore-alpha9-authoring.openapi.json).

There is no default target or credential. Run only with the owned local Postman
simulation, `ownedDisposableFixture=true` and its numeric-loopback `baseUrl`
including `/gitstore`. `scripts/test_remote.py` exercises the actual CLI against
this simulation with both contract profiles, File name/ID and missing-object
handling, scoped create and rejection before effects. A mock pass does not establish live alpha.9 compatibility; the published
image integration target is the separate live check. Do not commit environments
or run reports.
