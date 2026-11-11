# Releasing GitStore CLI

GitStore and this CLI are early alpha. Upstream schemas change incompatibly.
Upgrade compatibility, data preservation and recovery are not guaranteed. Keep
independent backups; do not use prereleases for irreplaceable data.

For candidate results and remaining gates, see [release qualification](validation/release-qualification.md).

## Repository setup

- Install the Release Please App on `gitstore-dev/cli` with Contents, Issues and
  Pull requests write permissions. Configure `RELEASE_PLEASE_APP_CLIENT_ID` and
  `RELEASE_PLEASE_APP_PRIVATE_KEY`; inherited organization settings are supported.
- Configure `release-publication` with required maintainer reviewers and a tag
  deployment policy for `v0.1.0-alpha.*`. Promotion refuses a missing reviewer rule.
- Keep App/signing credentials out of PR and native fixture jobs. Use the scoped
  App for release proposals/tags so their events trigger downstream workflows.

No external package registry account is required. Homebrew uses the repository
as a Git URL tap; APT/DNF and npm install GitHub Release files. Nix is deferred.

## Release procedure

1. **Review and merge the Release Please proposal through the merge queue.**
   Required CI must pass. `version.txt`, Cargo workspace, the gs lock entry and
   release manifest must agree. The first proposal removes the bootstrap override;
   subsequent proposals advance the alpha number. Merging creates the version tag
   and a draft prerelease; it does not publish packages.
2. **Wait for the tag's `release.yml` build to succeed.** All four platform rows
   and assembly must pass. Review the `release-candidate` artifact: frozen
   inventory/checksums, matching gs/gitstored pairs, package formats, notices and
   corresponding-source/relink materials. Never select PR-produced artifacts.
3. **Qualify the exact candidate.** Set `RELEASE_TAG` to the reviewed tag and
   `BUILD_RUN_ID` to its successful build run, then dispatch:

   ```sh
   gh workflow run runtime.yml --repo gitstore-dev/cli --ref "$RELEASE_TAG" \
     -f candidate_run="$BUILD_RUN_ID" -f candidate_tag="$RELEASE_TAG"
   ```

   All six hosts and 22 selections in [the platform matrix](../../packaging/platforms.json)
   must pass. The collector emits `release-qualification` only for a complete set
   of receipts binding the exact source, tag and final subject hashes. Ubuntu/macOS
   use isolated GitHub-hosted runners; Fedora uses pinned disposable fixtures.
   Linux product commands run as UID 11000. macOS qualification covers remote/control
   behavior; local workload execution remains unsupported.
4. **Review qualification and start promotion.** Set `QUALIFICATION_RUN_ID` to the
   successful qualification run. Check its installation/replacement/removal,
   native-runtime and performance results, then dispatch at the same tag:

   ```sh
   gh workflow run release.yml --repo gitstore-dev/cli --ref "$RELEASE_TAG" \
     -f phase=promote -f candidate_run="$BUILD_RUN_ID" \
     -f qualification_run="$QUALIFICATION_RUN_ID"
   ```

   Promotion rejects foreign, PR, failed or mismatched-source input runs. It signs
   every final subject and performs [producer verification](../../runtime/packaging/release-policy.md)
   before the publication job can request approval.
5. **Approve publication.** A required maintainer reviews the selected tag, source,
   runs and verified inventory, then approves the waiting `release-publication`
   deployment. The job re-verifies bytes, uploads matching assets/bundles, publishes
   the prerelease with `latest=false`, and opens a single-file Homebrew formula PR
   using the repository-scoped App. The publication result reports `tap.status`
   as `pending` with its PR URL, or `current` if the formula is already on main.
   Review that it still targets the newest published prerelease, then merge the
   formula PR through normal checks and the merge queue. Homebrew delivery is
   pending until that merge; the publisher never writes protected main directly.
6. **Verify delivery and record acceptance.** Check public asset names/digests,
   downloadable `.sigstore.jsonl` bundles, the mandatory early-alpha notice,
   formula and [installation instructions](../installation.md). Update the compact
   qualification record and its machine-readable evidence. Close acceptance tasks
   only when their checks have actually passed.

## Artifact and verification rules

- Package both real executables with matching application versions. Debian encodes
  `0.1.0-alpha.N` as `0.1.0~alpha.N-1`; RPM uses Version `0.1.0`, Release
  `0.alpha.N.1`. A package rebuild suffix does not change socket/application identity.
- Review [producer inputs](../../packaging/release-inputs/) when dependencies change.
  Strip, relocate and package before freezing hashes or signing. Include complete
  Rust/native notices and required source/relink assets.
- `release-inventory.json` describes payloads. `SHA256SUMS` covers payloads and that
  inventory, excluding itself. Both are signed subjects; the mutable candidate
  ledger is not part of its own hash inventory.
- [Python dependencies](../../packaging/requirements.txt) have exact versions and
  SHA-256 wheel hashes. Install with `--require-hashes --only-binary=:all:`; review
  wheel resolution on every selected platform when updating the lock. Install
  publication dependencies before supplying the Contents-write token.
- Standard GitHub Actions attestations bind producer identity and final bytes.
  They do not certify vulnerability freedom. Producer verification is mandatory;
  consumer verification is optional and is never required by an installer.

Optional consumer verification:

```sh
gh attestation verify /absolute/package.tgz --repo gitstore-dev/cli \
  --bundle /absolute/package.tgz.sigstore.jsonl
```

## Failure and recovery

| Failure | Action |
| --- | --- |
| Build or qualification incomplete | Keep the draft unpublished. Preserve failed-run evidence and repair the cause. Never substitute older or partial receipts. |
| Source or fixture code changes | Use a new reviewed commit/version proposal; rebuild and requalify. Do not move an existing version tag. |
| Upload interrupted | Retry the same reviewed candidate/tag and successful qualification. Matching uploads are reused; conflicting bytes fail. Never use `--clobber`. |
| Publication gate or provenance check fails | Resolve the configuration/evidence problem. Do not bypass the gate or publish through the release editor. |
| Formula PR pending | Confirm it still targets the newest published prerelease, then merge through normal checks and the merge queue. |
| Tap proposal fails after publication | Keep published assets intact. Retry the same verified candidate to resume a matching branch/PR. Edited branches, changed bases and closed PRs require maintainer reconciliation; never force-update them. |

The publisher checks the exact complete asset set and early-alpha notice before
and after promotion. A published release missing its notice requires explicit
correction; retries do not silently rewrite published release notes.

## Unsigned development package review

For development review, dispatch `runtime.yml` with successful same-source
`review_cli_run`/`review_linux_run` inputs; add `review_upgrades=true` for replacement
checks. Separate native review uses `hosted_native_run` or `fedora_native_run`.
These artifacts and next-alpha test controls are unqualified, never published,
and cannot replace tag-bound release receipts. See [fixture instructions](../../tests/packaging/README.md).
