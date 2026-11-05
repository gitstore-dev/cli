# Work with a remote GitStore

[Configure your installation and credentials](configuration.md), then run `gs`
against its public GraphQL and Git endpoints. A local daemon, runtime download,
browser login and separately installed Git executable are not required.

## Inspect resources

```sh
gs context show -o json
gs list repository --first 25
gs read product summer-shirt -o json
```

Read/list are available for Namespace, Repository, Product, CategoryTaxonomy
(`category`) and Collection. File metadata can be read by namespace and name;
File listing is not available.

List returns one page of 1–100 objects with an opaque continuation cursor. Use
JSON for structured resource inspection. Table is the default; resource YAML,
Markdown and lossless authoring export are not available yet.

## Supply credentials for one command

If credentials are not in your configured OS secret store:

```sh
gs --credential-file /protected/token read product summer-shirt -o json
```

The file must be owned, regular, non-symlink and mode 0600 or stricter, containing
a raw single-line token. This override is not saved. See
[configuration](configuration.md) for stdin, OS-store and separate Git credentials.

## Submit GraphQL

```sh
gs --credential-file /protected/token api graphql -F query=@query.graphql
```

`-f key=value` supplies a string; `-F` recognizes booleans, null, safe integers and
`@file` text. Use `--input` for a complete JSON request envelope. Direct GraphQL
outputs JSON and preserves `data`, `errors` and `extensions`. HTTP 200 with GraphQL
errors still exits nonzero. Subscriptions are not available.

## Create and delete resources

Product and CategoryTaxonomy creation accept Markdown documents:

```sh
gs --credential-file /protected/token create -f product.md -f category.md
```

The CLI validates the whole batch first, preserves document bodies and submits in
order. If a submission fails, later documents remain unsubmitted; inspect the
reported per-document outcomes before retrying. Batches accept at most 100 documents
and 16 MiB total input.

Namespace, Repository, Product and CategoryTaxonomy deletion resolve the selected
resource before requesting deletion. A successful deletion request starts
termination; it does not prove that controller cleanup has finished.

Namespace/Repository creation, safe updates and Collection mutations are not
available as high-level commands in this preview.

## Clone and sync repositories

```sh
gs repository clone catalog
# Or choose the saved remote name:
gs repository clone catalog --remote origin
```

Clone defaults to a remote named `gitstore` and tracks the checked-out branch when
one exists. The destination defaults to the repository name.

Make commits using your Git-compatible editor/client, then submit them:

```sh
gs repository sync --remote gitstore --ref main
```

Sync requires a clean worktree, an existing remote branch and fast-forward history.
It does not commit, merge, rebase, reset or force-push. A remote name alone does not
identify the installation: selection uses the configured public URLs. Ambiguous
targets need an explicit repository or remote.

Git credentials are separate from GraphQL credentials. Configure the supported
Basic username/password or Bearer mechanism in [configuration](configuration.md).
The CLI does not use external credential helpers, filters or inherited auth/TLS
settings; TLS verification stays enabled.

## Outcomes and limits

Progress and diagnostics use stderr; requested results use stdout. Local selection,
auth and Git summaries also support YAML. `auth token` deliberately prints a raw
token and accepts no output-format option.

Remote requests default to a 30-second deadline; use `--timeout 5s` to change it.
Transient reads retry at most twice within that deadline. Mutations and pushes are
not blindly retried. A lost response retains known identity and reports uncertain
submission. Accepted transport does not prove server admission or readiness.

Browser login, refresh, remote credential invalidation, discovery, watches,
correlated remote waits and hydrated exports are not available yet. `auth status`
reports local credential presence; logout reports when remote invalidation is
unsupported. Use the installation's supported account tooling to obtain existing
credentials before configuring this source preview.
