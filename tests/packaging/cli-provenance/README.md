# EXP-003C: CLI provenance verification

This is maintainer development tooling. It is not packaged with `gs`/`gitstored`
and places no attestation requirement on GitStore's upstream service images.
The [release policy](../../../runtime/packaging/release-policy.md) defines the producer,
workflow, stable repository ID, source/ref and final digest requirements.

The real controls use official GitHub CLI test data from commit
`4f2cfe92e3499f8581bf39db77e0a1431fa7b55c` (v2.103.2), under
`pkg/cmd/attestation/test/data/`. Download the three named files into an explicitly
selected development directory. Do not execute or extract the archive; it is only
a signature subject. Binary fixture inputs stay outside Git.

| Input | SHA-256 |
| --- | --- |
| `sigstore-js-2.1.0.tgz` | `f0c641fce0310cb43208f4272dc1423153630e44ba8cdc0b5ce8466f724e0eed` |
| `sigstore-js-2.1.0-bundle.json` | `cfadddd579b9fb45947530cd0e860986ec610a28f7fa6de3ef31cc051f07f761` |
| `trusted_root.json` | `455b3fe53e2678889f093d66ebbfda5f12ebb9d682b146c3535fa04e97689787` |

The fixture trust root is a test input, never the production release root. The
driver supplies refused-loopback HTTP/HTTPS proxies, temporary HOME/gh config and
a synthetic login placeholder; no personal token or credential store is used.
The positive control still verifies offline with its exact producer, workflow,
source/ref, issuer and hosted-runner policy. The driver then requires rejection
of each negative case. No test result can publish an artifact or change the
production verifier's fixed GitStore producer policy.

```sh
python3 tests/packaging/cli-provenance/test_policy.py
python3 tests/packaging/cli-provenance/run.py \
  --inputs /absolute/pinned-fixture-directory \
  --cli-artifact /absolute/own/unpublished/gs \
  --output /absolute/new-evidence.json
```

The local policy tests isolate argument/snapshot/identity handling with a mock
verifier; they are not cryptographic evidence. The ten real controls use GitHub
CLI 2.103.2. [The saved result](evidence.json) records exact fixture and actual
unsigned CLI hashes. Only the external signed control is accepted. GitStore has
no published release in the repository API at this checkpoint; a real trusted
own-release positive and hosted attestation generation remain unverified.

Primary inputs: [official integration tests](https://github.com/cli/cli/blob/4f2cfe92e3499f8581bf39db77e0a1431fa7b55c/pkg/cmd/attestation/verify/verify_integration_test.go)
and [pinned test data](https://github.com/cli/cli/tree/4f2cfe92e3499f8581bf39db77e0a1431fa7b55c/pkg/cmd/attestation/test/data).
