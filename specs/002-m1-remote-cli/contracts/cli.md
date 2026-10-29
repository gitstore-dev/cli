# M1 CLI Contract

M0 [CLI behavior](../../../docs/contracts/cli-behavior.md) supplies exit codes,
redaction, GraphQL grammar and output rules. These commands define the development implementation;
release evidence is recorded separately in `docs/m1-status.md`. The feature deliberately restricts some M1 options.

## Included command grammar

```text
gs [selectors] context list
gs [selectors] context show
gs [selectors] context use <name>
gs [selectors] config validate
gs [selectors] auth switch <account>
gs [selectors] auth status
gs [selectors] auth logout
gs [selectors] auth token
gs [selectors] api graphql [-f key=value ...] [-F key=value ...] [--input <file|->]
gs [selectors] read <kind> <name>
gs [selectors] read <kind> --id <id>
gs [selectors] list <kind> [--first <1..100>] [--after <cursor>]
gs [selectors] create -f <path|-> [-f <path> ...]
gs [selectors] delete <kind> <name>
gs [selectors] delete <kind> --id <id>
gs [selectors] repository clone <repository> [directory] [--remote <name>]
gs [selectors] repository sync [repository] [--remote <name>] [--ref <branch>]
gs --help
gs --version
```

Selectors may appear before or after subcommands without changing precedence.
Global inputs include config/context/endpoint/account/namespace, credential-file or
credential-stdin, `--non-interactive`, `--timeout <duration>`, and applicable output.
Repository/ref selectors apply to Git target selection. Contradictory explicit
positional/flag identities fail rather than silently taking one. `--timeout` defaults
to 30 seconds and applies to the complete remote command, including retries and Git
library transport work. Duration must be positive and representable.

Kinds and operation availability follow the spec scope table: File read has no ID
selector or list; Collection is read/list only. Category's CLI name is `category`,
its manifest kind is `CategoryTaxonomy`. Create accepts only Product/CategoryTaxonomy.
Repeated `-f` is the multi-document interface; one Markdown document per input file.

## Validation order

1. Parse command, format and bounded input; reject conflicting stdin consumers,
   selectors, unsupported kinds/options and known gated waits.
2. Resolve/validate configuration, endpoint/profile and credential identity.
3. Validate a whole author batch or Git target/worktree/ref before any write.
4. Send only the required public requests through the HTTP/Git library transports.
5. Emit faithful requested results and independent diagnostics with stable exits.

Excluded operations return exit 6 and their dependency reference; malformed grammar
or contradictory selectors return exit 2. A valid but missing object returns exit 4;
missing/denied credentials return exit 3. Unknown command spelling is usage, not a
fabricated capability. No preflight error causes writes or credential output.

## GraphQL and resource outputs

Direct GraphQL emits the complete response object on stdout as JSON even with
operation errors; nonempty `errors` means nonzero exit. Preserve data/errors/extensions.
For missing/invalid response shape, report a safe transport/protocol diagnostic.
`--input` is mutually exclusive with all -f/-F; duplicate keys and malformed envelopes
fail before transmission. Selected multi-operation requests require operationName.
Use an AST to determine query vs mutation/subscription. Subscription operations are
unsupported in this HTTP-only increment (DEP-017), before network submission.

Read/list default to tables; JSON inspection contains returned selected resource
fields and list pageInfo, not invented hydration. Query selections include identity,
metadata and supported human-summary/body fields; null/omitted data is not fabricated.
Resource YAML/Markdown and authoring export remain unsupported (DEP-005). Local
status/validation and Git summaries accept table/JSON/YAML; GraphQL accepts JSON only;
`auth token` accepts raw output only. Create/delete summaries accept table/JSON; their
resource YAML representation remains gated in this increment.

Machine-mode errors use JSON stderr with M0 fields, irrespective of summary format.
No terminal styling when redirected or when NO_COLOR is set. Never log credentials,
request headers or input excerpts containing protected material. Server error text
must be sanitized before diagnostics; the deliberate direct GraphQL response remains
faithful requested data rather than a diagnostic logger.

## Mutation outcomes and retry

Every author document has a result entry, including unsubmitted remainder after a
stopping failure. A returned create object is evidence of that API's admission result,
not controller readiness. Deletes expose server `outcome`, including already-terminating;
no client cleanup. Preserve known revision/identity; lost responses are uncertain.
Do not retry mutations automatically. Direct queries/reads allow at most two additional
transient attempts within the command deadline, respecting Retry-After when it fits.
No retry of invalid/auth/not-found/conflict responses and no transport/credential fallback.

## Git behavior

Clone validates repository/namespace against the explicit Git endpoint/profile,
uses the Git library clone/remote APIs to select the remote name, and checks resulting
remote/tracking configuration. Default name is `gitstore`; no extra origin alias.
A nonempty destination fails without overwrite. Empty repository success does not
invent a branch. Clone is read-only on the server and has no admission outcome.

Sync resolves worktree from subdirectories, validates both fetch and push destinations,
selects one eligible remote/ref or requires explicit selection, and rejects dirty
worktrees. A name `gitstore` supplies no trust or priority. Fetch only the selected
remote branch into its remote-tracking ref, never a local branch. Equal commit is
no-op; remote ancestor of local permits ordinary push; local behind or divergent is
conflict. Concurrent advancement is rejected by normal push, never force-retried.
No commit/reset/merge/rebase or context-default target fallback.

Git operations use the maintained library adapter in-process, never C Git or another
Git executable. The libgit2 smart-HTTP adapter supplies Basic/Bearer credentials only in memory
through reqwest, with no redirects and an absolute deadline. Never persist secrets in URLs/configuration or hand
them to ambient helpers. Disable external credential helper/filter/hook execution and
untrusted inherited auth/header settings, reject redirects and preserve TLS verification.
Clone checkout must not invoke external filters. Verify deadlines/cancellation on the
selected network backend and map unsupported repository formats/features explicitly.

Library progress callbacks relay received side-band/progress to stderr; structured
results alone go to stdout. A push reports exact commit and pending/unverified
admission/readiness. Check per-ref server status as well as transport success, including
rejection of concurrent ref advancement. No end-to-end incremental server streaming
claim. Known unsupported `--wait=admitted|ready` fails before any Git write. Run the
built CLI's Git scenarios without a Git executable available to prove this boundary.
