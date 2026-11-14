# CLI publication verification

This policy gates artifacts published by `gitstore-dev/cli`. It does not gate
upstream service images. `gs` and `gitstored` do not contain or invoke GitHub CLI;
the verification script is maintainer tooling outside the installed package.

## Required identity

Promotion must bind the final artifact SHA-256 to all of these verified claims:

| Claim | Required value |
| --- | --- |
| Producer | `https://github.com/gitstore-dev/cli` |
| Stable repository ID | `1431005655`, read from the repository API on 2026-11-05 |
| Signer workflow | `gitstore-dev/cli/.github/workflows/release.yml` |
| OIDC issuer | `https://token.actions.githubusercontent.com` |
| Source ref | The reviewed exact `refs/tags/vMAJOR.MINOR.PATCH[-PRERELEASE]` |
| Source revision | The reviewed full 40-character Git commit |
| Predicate | `https://slsa.dev/provenance/v1` |
| Runner | GitHub hosted; self-hosted signing is rejected |

The workflow path is the trusted release policy identity. Alpha.5 has own signed
positive and negative controls in the
[current qualification record](../../docs/contributing/validation/release-qualification.md).
Unsigned development binaries and candidate packages cannot pass this gate.
External experiment controls grant no GitStore publication authority.

[`release_verification.py`](release_verification.py) snapshots bounded ordinary
artifact/bundle/root files before invoking the maintained verifier. It requires
all policy flags and checks the certificate's stable repository ID after successful
cryptographic verification. It ignores workflow-controlled repository assertions
in the provenance predicate. Missing evidence, signature failure, digest mismatch,
wrong workflow/source/issuer or another repository ID fail promotion.

## Offline verification

Future releases publish one `release.sigstore.jsonl` multi-subject bundle. Each
subject is independently verified against that same bundle. The producer ledger
selects `attestationLayout: shared-v1`; legacy schema-v1 ledgers without that field
select the original per-subject layout used by alpha.5/alpha.6. Layout is never
inferred from available filenames. No verifier is required by installers.

Acquire the artifact and its attestation bundle, and refresh trust roots while
online using reviewed GitHub CLI. Import trust roots through the maintainer's
trusted channel; an arbitrary root shipped beside an untrusted artifact is not
an authority. Preserve hashes and acquisition/source identity for the root file.
Offline verification cannot discover trust-key revocation or later repository
renames; the pinned stable repository ID prevents accepting a new repository that
reuses the same name. Review identity changes explicitly.

```sh
python3 runtime/packaging/release_verification.py \
  --artifact /absolute/final-artifact.tar.gz \
  --bundle /absolute/downloaded-attestation.jsonl \
  --trusted-root /absolute/reviewed-trusted-root.jsonl \
  --source-ref refs/tags/v0.1.0 \
  --source-digest FULL_REVIEWED_COMMIT \
  --expected-sha256 EXPECTED_ARTIFACT_SHA256
```

The verifier uses isolated temporary GitHub configuration and no personal
credentials. Verification never executes or extracts the subject artifact.
Its successful output is a verification receipt, not a publishing operation.

## Trusted generation

Build, package and sign before attesting final bytes. The trusted release
job must use reviewed commit-pinned `actions/attest`, scoped `contents: read`,
`id-token: write` and `attestations: write`. Current action guidance also requires
`artifact-metadata: write`; grant it only to that trusted job. Registry write is
unnecessary for the current file-only package. PR, fork and candidate validation
jobs remain read-only and must not promote their outputs as trusted releases.
Record the immutable source/ref, workflow, artifact digest and verifier outcome
before promotion. A source tag alone or successful signature from another
repository is insufficient. SBOM attestations, if added, bind the same final digest;
provenance is not a vulnerability or higher-SLSA-level claim.

The reviewed `actions/attest` v4 reference on 2026-11-05 resolves to
`1e69f48acb82d1966a394da916b4c1698aa569d6`. Local experiments do not authorize publication. Actual hosted signing and
producer verification plus required maintainer approval gate publication.
This repository currently publishes no OCI image, so OCI signing/verification is
not added speculatively.

## Experiment evidence

[EXP-003C](../../tests/packaging/cli-provenance/README.md) uses the installed GitHub CLI
with pinned official signed controls, without extracting or executing them. The
three local policy tests and ten real verification controls pass. A valid signed
external control verifies; wrong producer/workflow/ref/revision/issuer, modified
artifact/signature, missing evidence and an unsigned actual `gs` are rejected.
These results establish verifier behavior, not a signed GitStore release.

Sources: [GitHub offline verification](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/verify-attestations-offline),
[verifier policy flags](https://cli.github.com/manual/gh_attestation_verify), and
[reviewed attestation action](https://github.com/actions/attest/tree/1e69f48acb82d1966a394da916b4c1698aa569d6).

Reference-host receipts include 100-sample performance records for the exact
candidate executable hashes and byte sizes. `validate_receipts` rejects missing,
wrong-pair, incomplete or nonfinite metrics, target misses, and native measurement
owners left running. macOS records local command costs and retains control-only
scope; Linux additionally measures successful current-generation inspection and
separate offline runtime costs. This is maintainer-owned execution evidence, not
an attestation or a substitute for the other required host/channel checks.

## Hosted download names

The GitHub transport adapter discovers drafts through the authenticated release
listing and pins their existing release ID for reads and uploads. Metadata updates
explicitly retain the reviewed tag. GitHub normalizes Debian filename tildes to
periods; the adapter maps only the selected version's two Debian names and their
bundles back to the signed logical names. It rejects collisions and retains the
complete asset-set, digest, source, receipt and signature checks. Signed subjects
and package bytes are never rewritten to accommodate a transport filename.
