# Image library comparison archive

The superseded Rust comparison executable, C caller and bridge runner are
[reproducible at their immutable source revision](https://github.com/gitstore-dev/cli/tree/8fb113e05255912c3e7cc10bceb2b85ade3ec8c7/runtime/experiments/image-library).
They have no current build, test or qualification consumers. The maintained
implementation and tests are in [daemon/image-backend](../../../daemon/image-backend/README.md).

The maintained synthetic registry and fixture-image producer moved to
[tests/runtime/image-library/go-probe](../../../tests/runtime/image-library/go-probe).
Its module pins and negative controls are retained unchanged. The actual SDK
runner is [test_sdk.py](../../../tests/runtime/image-library/test_sdk.py).

Historical measurements, failed attempts and selection rationale remain in
[the comparison record](../image-library.md). The source/license inventory and
digest-routing receipts here are historical evidence, not current qualification.
