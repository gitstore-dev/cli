# Remote CLI development implementation

Build with `cargo build --locked -p gs`; the executable is `target/debug/gs`.
See [configuration](configuration.md) for existing-credential bootstrap. No daemon,
runtime download, browser login or Git executable is required.

Implemented command subsets:

- Context list/show/use, configuration validation, local auth status/logout/switch
  and deliberate raw token output.
- Direct GraphQL using literal `-f`, typed/file-backed `-F`, or a complete JSON
  envelope through `--input`. GraphQL responses retain data/errors/extensions;
  HTTP 200 with errors still exits nonzero. Subscriptions are gated.
- Read/list Namespace, Repository, Product, CategoryTaxonomy (`category`) and
  Collection; File metadata read retains `file(namespace,name)` pending the
  upstream File lifecycle specification. There is no File list command.
- Product/CategoryTaxonomy create from one Markdown document per repeated `-f`.
  Validate the whole batch before writing, preserve raw body bytes/newlines,
  submit in order, stop after failure and report every unsubmitted remainder.
- Namespace/Repository/Product/CategoryTaxonomy delete by resolved opaque ID.
  Deletion starts termination; it does not prove controller cleanup completed.
- Repository clone with default remote `gitstore` or `--remote <name>`, plus
  submission-only sync using libgit2. Sync preserves local branch/worktree and
  accepts only an existing remote branch, clean worktree and fast-forward history.

```sh
gs context show -o json
gs --credential-file /protected/token read product summer-shirt -o json
gs --credential-file /protected/token list repository --first 25
gs --credential-file /protected/token api graphql -F query=@query.graphql
gs --credential-file /protected/token create -f product.md -f category.md
gs repository clone catalog --remote gitstore
# Make a commit with a Git-compatible editor/client, then submit it:
gs repository sync --remote gitstore --ref main
```

Git uses separately configured credentials with the proven Basic or Bearer mechanism;
a GraphQL session token is not a Basic password. Git target resolution checks public
URLs and installation prefixes, including push URLs. A remote name supplies no trust
or ambiguity priority. External credential helpers, filters and inherited auth/TLS
configuration are not used. TLS verification stays enabled. Unsupported local URL
rewrites/extensions fail explicitly rather than changing the network target.

Table is the default for resource/local/Git commands; inspection JSON is available.
Local/Git summaries also support YAML. Resource YAML/Markdown and authoring export
remain gated by DEP-005; JSON inspection is not advertised as lossless export.
Direct GraphQL is JSON-only; `auth token` is raw-only. Progress/diagnostics go to
stderr; machine-mode diagnostics are JSON. Known waits fail before submission.

Default remote deadline is 30 seconds, adjustable with `--timeout 5s`. List commands
return one page (1–100 objects), with opaque continuation cursors. Input, expanded
batch content and HTTP responses are bounded; author batches accept at most 100
documents and 16 MiB total input. Transient reads retry at most twice within the
same deadline; mutations and pushes are never blindly retried. Lost responses retain
known identity and report uncertainty; transport acceptance never proves readiness.

Login/refresh/remote invalidation, discovery, Namespace/Repository authoring, safe
updates, watches, Collection mutations, verified waits and hydrated exports retain
the [M0 dependency gates](contracts/dependencies.md). File selector normalization is
a future upstream contract change, not permission to invent lifecycle operations.

See [M1 evidence and limitations](m1-status.md) for actual validation. This is a
source implementation, not a published or provenance-verified product release.
