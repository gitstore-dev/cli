# CLI documentation operations

The website is a static Astro/Starlight build at `https://cli.gitstore.dev`.
Website jobs do not build, execute, publish, or qualify the CLI. A failed build
leaves the last successful Pages deployment serving traffic.

## Development and checks

Use Node 24.16.0 and pnpm 12.7.0. From the repository root:

```sh
export ASTRO_TELEMETRY_DISABLED=1
pnpm --dir site install --frozen-lockfile
pnpm --dir site check
pnpm --dir site build
pnpm --dir site exec playwright install chromium webkit
pnpm --dir site test:browser
```

The build imports the explicit guide allowlist from canonical `docs/` files.
`content-audit.json` binds their SHA-256 hashes to the selected release: changed
guides or a new release fail the build until reviewed. After checking release
support, update the audit version and hashes (`shasum -a 256 docs/installation.md`,
and each allowlisted guide). Do not mechanically accept hashes without reviewing
the content. Generated content is disposable build output. `site/release.json`
and `site/reference/gs.md` are the reviewed release inputs.

## Release updates

The publisher sends `repository_dispatch` with type `release-docs-published`
after publication. This explicit event works when publication used
`GITHUB_TOKEN`; a release event alone would not reliably start the updater.
Failure to dispatch is a maintainer warning, not a failed CLI publication.

For bootstrap or recovery, run **Update release documentation** on **main** in
Actions. Leave the tag blank to select the newest published semantic version,
including prereleases, or enter an exact published tag. Draft releases are never
selected. The equivalent authenticated local command is:

```sh
node site/scripts/update-release.mjs
# or: node site/scripts/update-release.mjs --tag v0.1.0-alpha.7
```

The updater validates release/tag/source identity, inventory, asset digests and
the archive's bundled reference without executing downloaded programs. Malformed
inputs leave the selected release unchanged. The repository-scoped App uses the
existing `RELEASE_PLEASE_APP_CLIENT_ID` variable and
`RELEASE_PLEASE_APP_PRIVATE_KEY` secret. Its token is limited to this repository's
contents and pull requests.

One serialized workflow updates `codex/docs-release-update` and one rolling PR.
The pending proposal's version participates in downgrade protection. Duplicate
and stale events cannot select an earlier version. Review the capability notices,
installation filenames, guide changes, and website checks before merging. The
workflow never merges its own proposal. Closing the PR without merging leaves
the currently deployed release unchanged.

Duplicate and stale events leave an existing PR and its branch untouched. A new
release appends to the existing proposal, preserving reviewer guide and audit
commits, and merges main without rewriting history. Merge conflicts or concurrent
reviewer pushes stop the updater for manual resolution; it never force-pushes.

`Website checks` is the stable required status. It runs for all PR target
branches and merge-queue entries; unrelated changes report success. PR build
jobs have read-only access and no deployment secrets. Only trusted main pushes
or manual dispatches on main may deploy, through the `github-pages` environment.

## First rollout

These are administrative steps, not capabilities established by a local build:

1. Merge the reviewed website implementation after normal repository checks.
2. In repository **Settings → Pages**, select **GitHub Actions** as the source.
   Configure the `github-pages` environment to permit deployments from main
   only. Keep normal branch protection and review requirements enabled.
3. In the GitStore organization settings, verify ownership of `cli.gitstore.dev`
   if it is not already covered by a verified parent domain. GitHub supplies the
   exact `_github-pages-challenge-…` TXT name/value; ask the DNS administrator to
   install that record and complete verification before removing it.
4. Set the Pages custom domain to `cli.gitstore.dev` in repository settings.
   A committed `CNAME` file alone does not configure Actions-based Pages.
5. Ask the DNS administrator to add **`cli CNAME gitstore-dev.github.io.`**.
   Do not include a repository suffix. Leave the apex `gitstore.dev` and all
   unrelated records unchanged.
6. Run **CLI website** on main (or let its main push finish). Wait for GitHub's
   DNS check and TLS certificate issuance, then enable **Enforce HTTPS**.
7. Verify the live homepage, installation, guides, a command page, search and
   an unknown URL. Confirm the certificate hostname, HTTP-to-HTTPS redirect,
   canonical URLs, social image, sitemap and robots policy. Confirm there is
   no `/cli` path prefix.

Example administrative commands, when using a suitably authorized GitHub CLI:

```sh
gh api --method POST repos/gitstore-dev/cli/pages -f build_type=workflow
gh api --method PUT repos/gitstore-dev/cli/pages -f cname=cli.gitstore.dev
# After certificate issuance:
gh api --method PUT repos/gitstore-dev/cli/pages -F https_enforced=true
gh api repos/gitstore-dev/cli/pages
curl --fail --head https://cli.gitstore.dev/
curl --fail https://cli.gitstore.dev/robots.txt
curl --fail https://cli.gitstore.dev/sitemap-index.xml
```

If Pages already exists, update its source instead of repeating the POST. Never
place credentials in source files, logs, or command arguments. Domain activation
requires the DNS administrator; repository administration alone does not prove
DNS access or establish a working HTTPS domain.

## Rollback

Each successful main build retains its Pages artifact for 30 days. Record the
previous successful workflow run and deployment URL before launch. For a
deployment-only failure, rerun the failed deployment job. To restore a previous
known-good release promptly, rerun its deployment job while its artifact remains
available. Confirm the result in Pages deployment history and over live HTTPS.

The durable rollback is a reviewed revert of the website change on main, followed
by the normal website workflow. Do not force-push main, alter CLI tags, republish
release assets, or change the DNS target to roll back content. Expired artifacts
can be regenerated from the known-good commit through a reviewed revert. Never
delete the last successful deployment merely because the next build failed.

References: [custom domains](https://docs.github.com/en/pages/configuring-a-custom-domain-for-your-github-pages-site/managing-a-custom-domain-for-your-github-pages-site),
[Pages workflows](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages),
[GitHub token event behavior](https://docs.github.com/en/actions/concepts/security/github_token).
