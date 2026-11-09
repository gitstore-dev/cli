# Preparing GitHub-hosted prereleases

GitStore and this CLI are early alpha. Upstream schemas continue changing
incompatibly. Upgrade compatibility, data preservation and recovery are not
guaranteed. Use, upgrade, migration or removal may cause data loss. Keep
independent backups; do not use prereleases for irreplaceable data.

This release implementation is under development. No complete qualified matrix
or own signed release has been produced. Do not publish an incomplete candidate.
See [the evidence record](validation/release-alpha1.md).

## Release Please setup

The [upstream alpha.9 workflow](https://github.com/gitstore-dev/GitStore/blob/68f8471edc947e417c98b6e8299a1367ab6b20da/.github/workflows/release-please.yml)
uses an App installation token. This repository follows that approach, with
commit-pinned actions and a main-only, non-cancelling serialized workflow.
Install the App on `gitstore-dev/cli` with Contents, Issues and Pull requests
write permissions. Configure `RELEASE_PLEASE_APP_CLIENT_ID` as a repository
variable and `RELEASE_PLEASE_APP_PRIVATE_KEY` as an Actions secret. Inherited
organization settings also work; effective repository organization-variable/secret
metadata lists them even when the repository-level settings lists are empty. The
App must separately include `cli` in its selected installation repositories. No npm,
Homebrew or other package registry account/token is required. Missing App
configuration skips proposals; it is not release acceptance evidence.

The workflow's ordinary GITHUB_TOKEN is read-only. The scoped App token creates
proposals and reviewed tags so their PR/tag events can trigger CI. Do not replace
it with GITHUB_TOKEN and assume those downstream runs happen. See
[the action's event guidance](https://github.com/googleapis/release-please-action/blob/5c625bfb5d1ff62eadeeb3772007f7f66fdcf071/README.md#other-actions-on-release-please-prs).
Do not expose release credentials in PR jobs.

`release-please-config.json` uses manifest/simple alpha mode, bounded history
starting at `b5c393e37b4787de0407ac3ccc1b058c067d69d8`, alpha.0 seed and the
one-time alpha.1 override. The generated alpha.1 proposal removes that override
before merge. Its manifest, version.txt and Cargo workspace must agree before
removal. A later proposal can then advance to alpha.2. CMake reads the full
application string from version.txt and derives its numeric project VERSION;
Cargo.toml and only the marked gs entry in Cargo.lock are updated by the engine.
Never replace transitive dependency versions with the application version.

Run offline history fixtures using the same engine as the pinned action:

```sh
npm install --prefix /absolute/disposable/tooling --ignore-scripts --no-audit --no-fund release-please@17.3.0
GITSTORE_RELEASE_PLEASE_MODULE=/absolute/disposable/tooling/node_modules/release-please python3 -m unittest discover -s tests/release
```

The fixture disables network requests and writes no GitHub resources. It proves
alpha.1 then alpha.2 and actual updater behavior; it does not prove configured App
permissions, hosted event delivery or an own signed positive. Those remain T038.

Release Please creates a **draft** prerelease and an explicit versioned tag after
the reviewed release proposal merges. A draft is not a qualified distribution.
Final promotion must wait for the complete package matrix, final-byte attestations
and producer verification. Public tag/release creation is a separate authorized
operation; none is performed by local fixture tests.

## Distribution and verification policy

Canonical and npm version: `0.1.0-alpha.1`; tag: `v0.1.0-alpha.1`.
Debian: `0.1.0~alpha.1-1`; RPM: Version `0.1.0`, Release `0.alpha.1.1`.
Package rebuild suffixes never alter application/socket identity. Every artifact
must contain both matching real executables. Strip/relocate/package before
calculating final digests or signing. Produce complete native/Rust notices and
any corresponding-source/relink assets before qualification.

Publish standard GitHub Actions attestation bundles for final subjects. Consumer
verification is optional; installers never require GitHub CLI or another verifier.
Producer promotion follows [the existing identity policy](../../runtime/packaging/release-policy.md).
Signed provenance binds build identity and bytes, not vulnerability freedom or
security certification. Never promote PR-produced artifacts using release credentials.

No external registry publication is in scope. Homebrew uses this repository's
explicit Git URL tap, APT/DNF install GitHub Release files, and npm installs a
platform tarball URL with both native binaries and no lifecycle download script.
Nix remains deferred. Linux/macOS platform qualification and release publication
are still open; consult the evidence record rather than inferring support from
package templates.

## Candidate preparation and recovery

`release.yml` builds only on an exact reviewed alpha tag. An ordinary tag push
prepares read-only candidates; it cannot promote a release. The four hosted build
rows use production facilities only and call `scripts/build-release-platform.py`.
Its per-tuple reviewed notices, closure and source/relink materials live under
`packaging/release-inputs/`; missing inputs fail before building. These inputs are populated and exercised by the recorded production candidates;
review them again when dependency or build inputs change. No placeholder manifest
is acceptable.

`scripts/assemble-release.py` requires all four build groups, rejects conflicting
assets and renders the formula from the four final archive digests. The local
`scripts/prepare-release.py` interface can also register explicit subjects and
advance immutable ledger files. The enforced sequence is:

```text
proposed → version-reviewed → built → packaged → validated
         → attested → producer-verified → observed published release
```

Version review binds the exact tag and full commit. Built requires every selected
platform/format; packaged freezes `release-inventory.json` and `SHA256SUMS`.
The inventory describes payloads; checksums include that inventory and every
payload, excluding themselves. The checksum file is itself a signed subject.
The mutable producer ledger is not a self-hashing signed inventory. Preserve its
packaged copy to resume; generated subjects refuse overwrite. If generation fails
before saving the packaged ledger, assemble a new private directory from the
same verified build groups rather than replacing existing frozen files.

Validation requires pass receipts for every selected OS/architecture/host/channel
in `packaging/platforms.json`. Each JSON receipt has `schemaVersion: 1`,
`result: "pass"`, exact `tag`/`sourceRevision`, `selection` as
`[os,arch,fixture,channel]` and `subjects` mapping every final filename to SHA-256.
Receipts are maintainer-owned execution evidence, not a cryptographic signature.
They must come from actual tests of those exact bytes. Structural fixture tests
do not supply qualification. The dedicated runtime workflow now produces the
`release-qualification` artifact only after all six selected clean reference hosts
pass. Each host also records 100 warm help/version samples; Linux measures 100
current-generation inspections and distinct runtime costs with offline assets.
Missed documented p95 targets fail before host receipts are emitted. Measurements
remain embedded in those maintainer-owned execution receipts.
Dispatch the exact reviewed candidate tag (or `main` while it still points
to the exact candidate source SHA) with
`candidate_run`, `candidate_tag` and `qualification_fixture` (an owned private
manifest at that path on each host). It rejects foreign/PR/wrong-source runs before
queuing dedicated hosts. Those hosts require the declared `gitstore-release-*`
labels; this workflow does not provision them or silently substitute hosted builds.

[`qualify-release.py`](../../scripts/qualify-release.py) requires an explicit
clean disposable host/account and actual OS match, complete next-alpha replacement
fixtures, the selected tooling revision, and Linux native/system fixture manifests.
See [fixture setup](../../tests/packaging/README.md). APT is tested on Ubuntu;
DNF on Fedora; archive/Homebrew/npm run on both selected Linux distributions and
macOS. Linux acceptance includes actual first-use/mismatched-manager and missing
library controls, offline/clean-install/recovery suites, and fd/memfd closure.
Tests run installed candidate bytes rather than rebuilding the product. All cases
must pass before per-host receipts are written; unavailable fixtures fail the job.
The final collector validates the full 22 host/channel selections before emitting
the promotion input. These new jobs have not been executed on the full reference
matrix; they do not establish release qualification by existing in source.

For promotion, dispatch `release.yml` **at the same tag**, select `promote`, and
supply the successful candidate build run and manually selected qualification
run IDs. Input validation rejects fork/PR artifacts, different workflows,
failed/in-progress runs and different source commits. It checks source identity
again after download and requires complete exact-byte receipts. Workflow
concurrency serializes the tag without cancelling an in-progress operation.

The hosted attestation job uses commit-pinned `actions/attest` in standard SLSA
provenance mode. It signs every final subject, including loose executable copies,
packages, inventories, checksums, formula and LGPL source/relink assets. Each asset
gets a downloadable `.sigstore.jsonl` bundle (a standard multi-subject bundle may
be repeated for its subjects). There is no registry upload. Trust roots are
obtained through the maintainer's trusted GitHub CLI channel, independently of
downloaded release assets. Producer verification checks bytes, repository ID,
workflow, hosted issuer, exact tag and full commit. The signing job has only read
access to contents plus OIDC/attestation/artifact-metadata writes.

Configure the `release-publication` environment with required reviewers before
enabling promotion. Only its publication job has Contents write permission.
`scripts/publish-release.py` runs producer verification again, requires an owned
private asset directory and a per-tag nonblocking lock, then inspects the existing
draft/tag commit. It never creates a tag/release or deletes/replaces an asset.
Matching uploads are reused; different bytes, names or source identities fail.
An interrupted upload leaves the draft incomplete. Retry the same reviewed
candidate/tag with matching receipts; do not use `--clobber` or create a fresh tag
to hide an incomplete candidate. Inspection must observe the exact complete set
before clearing draft, with prerelease true and latest false.
The publisher prepends the canonical early-alpha/upstream-schema/no-upgrade/
no-data-preservation/no-recovery/backup statement to the draft's existing release
notes and checks it again before and after promotion. Changelog preamble alone
would not put that notice in every GitHub release's generated notes. A published
release missing the notice requires explicit correction; retries do not silently
rewrite published notes.

The tap is updated only after observed publication. If branch protection prevents
the formula commit, the release stays published and the tap stays unchanged; retry
the same candidate after resolving that write policy. The script checks the most
recent published prerelease before updating, compares existing content and uses
the Contents API file SHA to reject concurrent conflicting edits. It never rolls
back a published release because a later tap update failed.

Optional consumer verification example (installation itself does not run this):

```sh
gh attestation verify /absolute/downloaded-package.tgz --repo gitstore-dev/cli \
  --bundle /absolute/downloaded-package.tgz.sigstore.jsonl
```

Maintainers use the stricter [producer policy](../../runtime/packaging/release-policy.md),
including all exact signed identity constraints. The branch-only App read-access
and own signing preflights pass, including negative controls. They do not establish
trusted tag-bound signing, actual release-PR/tag event delivery or full package
qualification. Those remain required before
calling alpha.1 ready. No public tag/release creation or publication was performed
while preparing this workflow.

## Unsigned development package review

Before a release tag exists, maintainers can dispatch `runtime.yml` with
`review_cli_run` and `review_linux_run` selecting successful same-source ordinary
CLI/runtime runs. This runs only ephemeral hosted review jobs; leave candidate,
native and denial fixture inputs empty. `prepare-package-review.py` checks both
run repository/workflow/source identities, verifies all four installed inventories
agree on actual checkout revision/version, and renders a private formula from the
four actual archive digests. It retains branch-head and PR-merge checkout identities
separately. No synthetic unused-platform digests are accepted by this path.

Ubuntu 24.04 and macOS 15 jobs on both architectures install/remove their exact
complete candidate through an isolated Homebrew checkout/prefix. Separate pinned
Fedora 44 job containers test Homebrew as fresh UID 11000, and Ubuntu jobs also
exercise actual APT install/remove with non-root product commands. Receipts remain
scoped to these operations; install/remove does not establish replacement or native
workload acceptance. Use `review_fedora_only=true` to skip the Ubuntu/macOS Homebrew
jobs when their evidence already exists; Fedora Homebrew and APT checks still run.
Homebrew’s documented Linux compiler/tool prerequisites belong to the package
manager; the formula installs prebuilt GitStore binaries. Tooling is pinned
to Homebrew revision `bf464f57275cae0be106b43d190c98564ac59b0e`; it may acquire its
own Ruby/tool metadata in that private fixture. All product invocations retain the
empty-tool PATH boundary. The formula remains a review artifact, never a tap update.
These outputs are named unsigned development review, contain no release receipts
or attestations, and cannot replace the trusted qualification/publication path.

For controlled replacement, additionally set `review_replacement_cli_run` and
`review_replacement_linux_run` to successful producer runs containing separate
`unqualified-replacement-*` artifacts. The coordinator requires a complete actual
four-platform next-alpha set, retains its distinct source revision, and renders
the next formula using real hashes. The selected APT/DNF/Homebrew and Fedora
archive/npm cases then exercise replacement/removal. These incomplete-notice,
`UNQUALIFIED-ONLY` fixtures remain outside the release inventory and never satisfy
trusted tag-bound qualification on their own.

For a separate ephemeral Linux native smoke, dispatch `runtime.yml` with only
`hosted_native_run` selecting a successful unsigned runtime candidate run. It
validates the exact downloaded native pair, prepares its own non-root account,
runs prerequisites and one real core case, and retains bounded diagnostic enums/
counts on failure. It neither signs nor produces release qualification receipts.
Ordinary library/package builds do not assert native workload support; an explicit
native job failure stays a failure and cannot be used as release acceptance.
