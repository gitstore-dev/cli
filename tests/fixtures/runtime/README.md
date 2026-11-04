# Public metadata fixture

`api-arm64-manifest.json` is the raw public GitStore API alpha.8 Linux arm64 OCI
manifest at
`ghcr.io/gitstore-dev/api@sha256:3c61f77edcbe719069d9173f1bad270cc46a81e97e756844cea3db8dcfb31172`.
It matches the compiled core profile and upstream evidence revision
`06152d193e854bc643fa884daa8b9f30b0d75d0f`. Its exact 1,435 bytes have that SHA256;
do not pretty-print/rewrite the fixture. This records public descriptor metadata,
not downloaded image layers, image execution, release support or an attestation.

The developer Go probe's `manifest-evidence` command recorded it through maintained
go-containerregistry APIs with explicit anonymous auth, no proxy/helper discovery,
digest verification and exclusive creation of a new evidence file. The fixture
lets offline C tests check real registered host/cache manifest-byte observations,
tamper/mode/link/FD bounds and rejection of self-asserted metadata. Tests copy it
into owned temporary roots and never inspect user config/credentials/deployments.
