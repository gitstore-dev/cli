# M0 Review and Validation Guide

M0 is a contract package. These procedures validate documentation completeness and
consistency; no `gs` or `gitstored` executable, running deployment, Docker, or compiler
is required. Do not run the PRD's intended commands as if they were implemented.

## Prerequisites and setup

Use a checkout with Git, a shell, and access to the PRD, constitution and published M0
documents. For upstream evidence, use read-only source access to the recorded revision
or a checkout with its exact provenance. Do not change or start upstream services.

From the repository root, identify the feature and review context:

```sh
git branch --show-current
bash .specify/scripts/bash/check-prerequisites.sh --json --paths-only
cat .specify/memory/constitution.md
cat specs/001-m0-contracts/spec.md
cat specs/001-m0-contracts/plan.md
```

Expected: actual branch `main`, feature directory `specs/001-m0-contracts`, constitution
v1.0.0, and M0 scope. Reading paths does not imply that the final M0 deliverables exist.

## 1. Validate publication and complete coverage

After M0 implementation, list the published documents:

```sh
rg --files docs/contracts docs/ADRs
cat docs/contracts/README.md
cat docs/contracts/command-matrix.md
cat docs/contracts/dependencies.md
cat docs/contracts/acceptance.md
```

Expected: every artifact in [artifacts.md](contracts/artifacts.md) is present. Compare
the PRD command families and every resource/operation pair against the matrix. Count
the expected operations directly from the PRD and the matching rows; record both counts
and omissions. Do not use a guessed fixed row count or treat a whole kind as one operation.

Every row has the [data-model fields](data-model.md), evidence or a prerequisite, owner,
milestone and status. Every blocked operation has a resolution gate; deferred entries
have scope reasons. Acceptance maps all 14 FRs and all four SCs with actual review results.
Expected: zero missing operations, orphan references, unowned gaps or false support claims.

## 2. Check evidence and shared client behavior

For each source-backed capability, inspect the referenced public schema and implementation
at its recorded revision; distinguish committed and modified source. Confirm Collection
read/list does not imply writes/watch, and File read/watch does not imply list/upload.
Check auth flow-specific capabilities, category kind mapping and controller-only fields.
An issue or internal method alone is insufficient evidence for a public operation.

Review the cases in [cli.md](contracts/cli.md), including partial success, GraphQL errors
on HTTP 200, pending admission, stale readiness and wait timeout. Expected: deterministic
selection/routing, stable error categories, preserved operation identity, no inferred
refresh semantics and no promises of snapshot consistency without a server contract.
Inspect examples of Unicode, multiline body and final-newline variants for export rules.
This is design review; executable round-trip and failure tests remain M1 gates.

## 3. Walk through daemon lifecycle

Use the published protocol and ownership ADR; record preconditions, messages, durable
state, ownership, user-visible result, failure/recovery, reviewer/date and outcome.

| Walkthrough | Required observation |
| --- | --- |
| Start | Peer/profile/host/provisioning checks precede side effects; accepted intent is durable; readiness differs from process existence. |
| Repeated start | Same key/parameters returns original operation; changed parameters conflict; distinct keys cannot duplicate Git writers. |
| Disconnect | Detached operation survives and can be inspected; foreground cancellation is explicit. |
| Stop | Prior writer termination is observed before lock release; retained volumes survive cleanup and retries. |
| Migration | Exclusive execution, compatibility/backup preconditions and inspectable failure; dependent start blocked, no downgrade. |
| Restart | Existing workloads/volumes are discovered before replacement; uncertain writer ownership fails closed. |

Expected: all six walkthroughs resolve ownership, completion and failure behavior. Verify
numeric resource bounds, version mismatch, unauthorized peer, malformed frames and event
overflow using examples. These are paper walkthroughs, not runtime fault-injection results.

## 4. Inspect contribution policy without adding lint CI

```sh
cat .github/workflows/pr-title-lint.yml
cat CONTRIBUTING.md
rg --files .github/workflows
git diff --check
```

Expected: only PR-title validation, accepted Conventional Commit types with optional scope,
metadata-only execution, read-only permissions, no contributor checkout, and no new
documentation/workflow or application jobs. Whitespace checks are a local review aid.
GitHub-hosted behavior requires an actual PR after the workflow lands on the default branch;
do not label local inspection as a hosted pass.

## Completion and limits

Record findings in `docs/contracts/acceptance.md`. M0 passes only when inventory and
decision coverage are complete and all M0 review findings are resolved. Named upstream
prerequisites and M2/M3 experiments may remain open with owners and gates. This does not
establish CLI implementation, runtime/platform support, performance, or release readiness.

During planning, final `docs/contracts/` and ADR files are intentionally absent; review
this guide for runnable steps and expected outputs, then execute it during M0 implementation.
