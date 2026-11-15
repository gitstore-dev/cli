# Release documentation delivery

Publication dispatches `release-docs-published` after the CLI release succeeds.
`Update release documentation` can also be run manually from main, with an
optional published tag. Prereleases participate in normal SemVer selection.
A tag alone does not publish or deploy documentation.

The updater validates published inventory, assets and checksums, extracts bundled
help without executing the CLI, and updates one rolling PR on
`codex/docs-release-update`. It renders marked release-dependent blocks in the
canonical Markdown sources. Historical qualification notes and authored prose
remain outside those blocks. Missing assets, changed immutable releases,
conflicting reviewer edits and nonordinary input files stop preparation.

`content-audit.json` records the last reviewed capability baseline, normalized
prose hashes, notices and source fingerprint. `release-evidence.json` compares
that trusted baseline with the selected release. Own package version fields and
explicit delivery-only inputs are normalized or excluded; production code,
dependencies, profiles, public contracts and qualification/build policy are
included. Identical parser help alone never carries a capability review.

When those inputs are unchanged, the existing audit carries forward. When they
change, the bot proposes a fresh audit and readable evidence. A human maintainer
must review the evidence and approve the **current PR head**. Edit the canonical
guides or notices first when behavior changed. Approval is a GitHub review, not
an editable JSON approval flag. Bots, the PR author, nonmaintainers and approvals
of earlier heads do not satisfy this gate. Dismissal or changes requested revoke
eligibility; the trusted workflow removes a queued ineligible proposal.

`Release docs delivery` runs trusted main policy only. It independently verifies
published metadata/reference, generated content, proposed audit and evidence,
and posts `Docs release eligibility` on the exact head. Once that status and all
normal required checks/reviews pass, the repository App requests normal merge
queue entry. It does not approve or merge directly. Merge-group validation
rechecks eligibility and applicable reviews. The review relay has no secrets.
Unrelated PRs receive success for this check.

## One-time activation

Merge the implementation before adding `Docs release eligibility` as a required
status for main, with GitHub Actions integration ID **15368**. Requiring it before
the trusted workflow exists on main would block its own introduction. Keep all
existing required checks and merge queue rules. Run `Release docs delivery`
manually for existing open PRs to seed the new status. Re-run `Update release
documentation` to upgrade an existing legacy rolling proposal. A failed website
build does not affect CLI publication or the last successful Pages deployment.

## Recovery and verification

Use manual `Update release documentation` on main after a failed publication
dispatch; duplicate events preserve the existing PR and its review commits.
Conflicts require a normal merge resolution, never a reset or force push.
Use manual `Release docs delivery` with a PR number to reassess after a rules or
permission change. Inspect its summary for waiting review/checks or invalid data.

Local checks:

```sh
pnpm --dir site check
ASTRO_TELEMETRY_DISABLED=1 pnpm --dir site build
pnpm --dir site test:browser
python3 -m unittest discover -s tests/release -p 'test_docs_queue.py'
```
