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
- Keep the repository Actions event policy **"Allow pull_request_target for release
  docs delivery"** (id 17922, `enforcement: active`). Release docs delivery
  (`docs-queue.yml`) uses `pull_request_target` to post the required
  `Docs release eligibility` status on every pull request, and GitHub's default
  policy blocks that event in public repositories from 2026-12-02. The policy
  allows `pull_request_target`, `workflow_run`, `merge_group` and
  `workflow_dispatch` for that one workflow path only. It is a repository setting,
  not a file; inspect it with
  `gh api repos/gitstore-dev/cli/actions/policies -H 'X-GitHub-Api-Version: 2026-03-10'`.
  It is an allowlist, so adding a trigger to `docs-queue.yml` also needs adding it
  to the policy. The workflow stays safe to allow because both jobs check out
  `main` only, run trusted scripts with `gh`/`node` argument arrays, pass event
  values through `env:`, and treat a fork PR that copies the proposal branch name as
  an ordinary PR. Deleting the policy restores the default (blocked) behavior.

No external package registry account is required. Homebrew uses the repository
as a Git URL tap; APT/DNF and npm install GitHub Release files. Nix is deferred.

## Release procedure

1. **Review and merge the Release Please proposal through the merge queue.**
   Required CI must pass. `version.txt`, Cargo workspace, the gs lock entry and
   release manifest must agree. The existing repository App creates the exact
   version tag and draft prerelease. Its tag push starts `release.yml`.
2. **Follow the single release pipeline.** It builds the four platform candidates
   once, freezes final bytes, then calls
   [the shared qualification workflow](../../.github/workflows/release-qualification.yml).
   All six hosts and 22 selections in [the platform matrix](../../packaging/platforms.json)
   must pass before receipt validation and signing. The default path passes
   artifacts within the same run: no qualification dispatch, run-ID copying or
   separate promotion dispatch is needed. All existing source, digest, native,
   replacement, performance and signing gates remain mandatory.
3. **Approve publication.** Review the exact tag/source, six-host results,
   inventory and producer verification in that run, then approve its waiting
   `release-publication` deployment. This is the pipeline's only manual approval
   after merging the release proposal. The job re-verifies the final bytes,
   uploads the subjects and shared attestation bundle, and publishes with
   `latest=false`. No workflow approves this environment on your behalf.
4. **Wait for Homebrew delivery.** Publication creates the immutable single-file
   formula PR with the scoped App. The separate **Homebrew delivery** job checks
   its exact commit, ordinary file mode and published formula digest, waits for
   required PR checks/reviews, then enqueues it with `expectedHeadOid`.
   GitHub's merge queue runs the normal merge-group checks before merging.
   The job reports **ready** only after observing the formula on main.
   Repository-wide auto-merge need not be enabled: the job uses a queue-only
   mutation and never pushes main or uses an administrator bypass.
5. **Verify delivery and record acceptance.** Check public asset names/digests,
   the shared `release.sigstore.jsonl` bundle, early-alpha notice, formula and
   [installation instructions](../installation.md). After Homebrew is ready,
   run `brew update` before `brew upgrade gitstore-dev/cli/gitstore`.
   Update the compact qualification record and its machine-readable evidence;
   claim acceptance only for checks that actually passed.

**Published** and **Homebrew ready** are distinct pipeline milestones. A failed
formula check, queue removal or delivery timeout does not undo a published release.
Fix the delivery problem and rerun only that failed job. Its App token is minted
separately; no App key or signing permission reaches qualification hosts. It
refuses edited/foreign/closed PRs and removes a queued formula if a newer published
release supersedes it. Uncertain queue submissions require inspection; retries
observe existing queue/merge state rather than blindly repeating mutations.

The website updater is dispatched after publication and remains a separate,
reviewed documentation PR. A dispatch failure leaves publication successful and
reports its manual retry. Neither Homebrew nor documentation failures authorize
rebuilding, retagging or replacing already published bytes.

## Recovery and explicit build-only review

For a failed automatic pipeline, inspect its failed job and rerun failed jobs
after resolving the cause. Successful build/qualification jobs and their frozen
artifacts are reused. Candidate/aggregate names and collision checks remain
unchanged; host receipts use `release-receipt-*` so their collector cannot also
download the build's `qualification-controls-*` artifacts in the same run.
If artifacts expire or source must change, prepare a new reviewed alpha rather
than weakening validation or moving the tag.

Manual dispatch defaults to `phase=pipeline`, which starts the full pipeline for
an exact reviewed tag. Explicit `phase=build` still creates read-only candidates
without starting qualification, signing or requesting publication approval.
Legacy split-run recovery remains available when deliberately selected:

```sh
gh workflow run release.yml --repo gitstore-dev/cli --ref "$RELEASE_TAG" -f phase=build
# After that exact-source build succeeds:
gh workflow run runtime.yml --repo gitstore-dev/cli --ref "$RELEASE_TAG" \
  -f candidate_run="$BUILD_RUN_ID" -f candidate_tag="$RELEASE_TAG"
# After all six hosts and the aggregate receipt check succeed:
gh workflow run release.yml --repo gitstore-dev/cli --ref "$RELEASE_TAG" \
  -f phase=promote -f candidate_run="$BUILD_RUN_ID" \
  -f qualification_run="$QUALIFICATION_RUN_ID"
```

External inputs still require completed successful same-repository/source/tag
runs. Only the current tagged `release.yml` pipeline may use its own in-progress
run's artifacts, and job dependencies require completed assembly and all six-host
receipts before signing. The signer identity stays
`gitstore-dev/cli/.github/workflows/release.yml`.

Already existing tags keep their original workflow definitions; these changes
apply to future reviewed tags, beginning with alpha.8. Do not dispatch an old tag
expecting it to acquire main's new pipeline.

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
- Future candidates select `publicationLayout: packages-v1`: paired archives,
  npm/DEB/RPM packages, source archives, Linux relink kits and formula remain
  downloads. The eight loose executables and four loose install manifests stay
  in the private candidate directory and qualification ledger. The public
  inventory's `installationInventories` map embeds those four manifests; resolve
  a payload's `installInventory` key there, rather than as a download URL. Each
  paired archive contains the same `share/gitstore/install.json` and exact
  qualified executable pair. Qualification receipts still bind every input and
  final package hash, across all six hosts and 22 selections.
  With four source archives and two relink kits this gives 21 signed subjects
  plus one shared bundle (22 uploaded assets, 24 entries with GitHub archives).
  Legacy ledgers without this selector retain their full public input set.
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
  --bundle /absolute/release.sigstore.jsonl
```

## Failure and recovery

| Failure | Action |
| --- | --- |
| Build or qualification incomplete | Keep the draft unpublished. Preserve failed-run evidence and repair the cause. Never substitute older or partial receipts. |
| Source or fixture code changes | Use a new reviewed commit/version proposal; rebuild and requalify. Do not move an existing version tag. |
| Upload interrupted | Retry the same reviewed candidate/tag and successful qualification. Matching uploads are reused; conflicting bytes fail. Never use `--clobber`. |
| Publication gate or provenance check fails | Resolve the configuration/evidence problem. Do not bypass the gate or publish through the release editor. |
| Formula PR pending | The delivery job waits for required checks/reviews and queues the exact head. Resolve failing checks, queue removal or timeout, then rerun delivery. Human review requirements still apply. |
| Tap proposal fails after publication | Keep published assets intact. Retry the same verified candidate to resume a matching branch/PR. Edited branches, changed bases and closed PRs require maintainer reconciliation; never force-update them. |

The publisher checks the exact complete asset set and early-alpha notice before
and after promotion. A published release missing its notice requires explicit
correction; retries do not silently rewrite published release notes.
Draft discovery uses the authenticated release listing and pins the existing
release ID. GitHub's release-by-tag endpoint returns published releases only.

## Unsigned development package review

For development review, dispatch `runtime.yml` with successful same-source
`review_cli_run`/`review_linux_run` inputs; add `review_upgrades=true` for replacement
checks. Separate native review uses `hosted_native_run` or `fedora_native_run`.
These artifacts and next-alpha test controls are unqualified, never published,
and cannot replace tag-bound release receipts. See [fixture instructions](../../tests/packaging/README.md).
