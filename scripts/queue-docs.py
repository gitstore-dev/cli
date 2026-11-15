#!/usr/bin/env python3
"""Queue a validated release-docs proposal without executing or approving PR code."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

REPOSITORY = 'gitstore-dev/cli'
REPOSITORY_ID = 1431005655
BRANCH = 'codex/docs-release-update'
CONTEXT = 'Docs release eligibility'
ROOT = Path(__file__).resolve().parents[1]
ALLOWED = {
    'site/release.json', 'site/reference/gs.md', 'site/release-evidence.json',
    'site/content-audit.json', 'docs/README.md', 'docs/installation.md',
    'docs/configuration.md', 'docs/remote-cli.md', 'docs/watch.md',
    'docs/local-runtime.md', 'docs/local-runtime-configuration.md',
}
QUERY = '''query($number:Int!) {
  repository(owner:"gitstore-dev",name:"cli") {
    pullRequest(number:$number) {
      id state headRefOid headRefName baseRefName isDraft merged reviewDecision
      mergeQueueEntry { id }
    }
    ref(qualifiedName:"refs/heads/main") {
      branchProtectionRule { requiresStatusChecks requiredStatusCheckContexts }
    }
  }
}'''
ENQUEUE = '''mutation($id:ID!,$head:GitObjectID!) {
  enqueuePullRequest(input:{pullRequestId:$id,expectedHeadOid:$head}) {
    mergeQueueEntry { id }
  }
}'''
DEQUEUE = '''mutation($id:ID!) {
  dequeuePullRequest(input:{id:$id}) { clientMutationId }
}'''


class GitHub:
    def command(self, arguments):
        return subprocess.run(['gh', *arguments], capture_output=True, text=True, timeout=120,
            env={**{key: value for key, value in os.environ.items() if key != 'GITSTORE_QUEUE_TOKEN'},
                 'GH_HOST': 'github.com', 'GH_PROMPT_DISABLED': '1'})

    def api(self, path):
        reply = self.command(['api', f'repos/{REPOSITORY}/{path}'])
        if reply.returncode:
            raise RuntimeError('Cannot read documentation queue state; inspect before retry')
        return json.loads(reply.stdout)

    def graphql(self, query, variables):
        environment = {**{key: value for key, value in os.environ.items() if key != 'GITSTORE_QUEUE_TOKEN'},
                       'GH_HOST': 'github.com', 'GH_PROMPT_DISABLED': '1'}
        if query in (ENQUEUE, DEQUEUE):
            if not (token := os.environ.get('GITSTORE_QUEUE_TOKEN')):
                raise ValueError('Queue mutation requires the repository-scoped App token')
            environment['GH_TOKEN'] = token
        reply = subprocess.run(['gh', 'api', 'graphql', '--input', '-'],
            input=json.dumps(dict(query=query, variables=variables)), capture_output=True,
            text=True, timeout=120, env=environment)
        if reply.returncode:
            raise RuntimeError('Queue request failed; inspect current state before retry')
        value = json.loads(reply.stdout)
        if value.get('errors'):
            raise RuntimeError('GitHub rejected the queue request; protections were not bypassed')
        return value['data']

    def checks(self, number):
        reply = self.command(['pr', 'checks', str(number), '--repo', REPOSITORY,
                              '--required', '--json', 'name,bucket'])
        if reply.returncode not in (0, 1, 8):
            raise RuntimeError('Cannot read required documentation checks')
        if not reply.stdout.strip() and 'no checks' in reply.stderr.lower():
            return []
        try:
            checks = json.loads(reply.stdout)
        except ValueError as error:
            raise RuntimeError('Cannot read required documentation checks') from error
        if not isinstance(checks, list):
            raise ValueError('Malformed required checks')
        return checks

    def assess(self, number, head):
        # ROOT is the trusted main checkout. No script/module from the proposal
        # is checked out, imported, installed, or executed.
        reply = subprocess.run(['node', str(ROOT/'site/scripts/check-docs-proposal.mjs'),
            '--pr', str(number), '--head', head], cwd=ROOT,
            capture_output=True, text=True, timeout=600,
            env={key: value for key, value in os.environ.items() if key != 'GITSTORE_QUEUE_TOKEN'})
        if reply.returncode:
            raise ValueError('Trusted release-docs validation failed; maintainer review required')
        try:
            return json.loads(reply.stdout)
        except ValueError as error:
            raise ValueError('Malformed trusted release-docs assessment') from error

    def status(self, head, state, description):
        # Use GITHUB_TOKEN so this required context belongs to GitHub Actions.
        reply = subprocess.run(['gh', 'api', '--method', 'POST',
            f'repos/{REPOSITORY}/statuses/{head}', '--input', '-'],
            input=json.dumps(dict(state=state, context=CONTEXT, description=description)),
            capture_output=True, text=True, timeout=120,
            env={key: value for key, value in os.environ.items() if key != 'GITSTORE_QUEUE_TOKEN'})
        if reply.returncode:
            raise RuntimeError('Cannot publish required documentation eligibility status')


def identity(pull, number, head):
    if (pull.get('number') != number or pull.get('head', {}).get('sha') != head
            or pull['head'].get('ref') != BRANCH
            or pull['head'].get('repo', {}).get('id') != REPOSITORY_ID
            or pull.get('base', {}).get('repo', {}).get('id') != REPOSITORY_ID
            or pull['base'].get('ref') != 'main' or pull.get('draft')):
        raise ValueError('Documentation PR identity changed; no queue mutation is allowed')


def validate_files(client, pull, head):
    files = client.api(f"pulls/{pull['number']}/files?per_page=100")
    if (not isinstance(files, list) or len(files) != pull.get('changed_files')
            or not 0 < len(files) <= len(ALLOWED)):
        raise ValueError('Documentation changed-file inventory is incomplete')
    names = [f['filename'] for f in files]
    if (len(set(names)) != len(names) or not set(names) <= ALLOWED
            or not {'site/release.json', 'site/release-evidence.json'} <= set(names)
            or any(f.get('status') not in ('added', 'modified') or f.get('previous_filename') for f in files)):
        raise ValueError('PR changes exceed the release-documentation allowlist')
    tree = client.api(f'git/trees/{head}?recursive=1')
    entries = {entry['path']: entry for entry in tree.get('tree', [])}
    if tree.get('truncated') or any(entries.get(name, {}).get('type') != 'blob'
            or entries[name].get('mode') != '100644' for name in names):
        raise ValueError('Documentation inputs must be ordinary non-executable files')


def assessment(client, number, head):
    value = client.assess(number, head)
    if (not isinstance(value, dict) or value.get('status') not in ('valid', 'superseded')
            or value.get('head') != head):
        raise ValueError('Trusted assessment does not identify this exact PR head')
    if value['status'] == 'superseded':
        return value
    if (value.get('evidenceStatus') not in ('unchanged', 'review-required')
            or not re.fullmatch('[a-f0-9]{40}', value.get('sourceCommit', ''))
            or not re.fullmatch('[a-f0-9]{40}', value.get('baselineSourceCommit', ''))):
        raise ValueError('Trusted assessment lacks a verified capability baseline')
    return value


def required_contexts(client, repository):
    rules = client.api('rules/branches/main')
    if not isinstance(rules, list):
        raise ValueError('Cannot establish required status-check policy')
    contexts = set()
    for rule in rules:
        if rule.get('type') == 'required_status_checks':
            contexts.update(check['context'] for check in rule['parameters']['required_status_checks'])
    legacy = repository.get('ref', {}).get('branchProtectionRule')
    if legacy and legacy.get('requiresStatusChecks'):
        contexts.update(legacy['requiredStatusCheckContexts'])
    if not contexts or not {'Website checks', CONTEXT} <= contexts:
        raise ValueError('Required website check is not configured; do not guess queue readiness')
    return contexts


def human_review(client, pull, head, *, require_approval):
    reviews = []
    for page in range(1, 11):
        batch = client.api(f"pulls/{pull['number']}/reviews?per_page=100&page={page}")
        if not isinstance(batch, list):
            raise ValueError('Malformed review history')
        reviews.extend(batch)
        if len(batch) < 100:
            break
    else:
        raise ValueError('Review history exceeds its safe input bound')
    opinions = {}
    permissions = {}
    for review in sorted(reviews, key=lambda r: (r.get('submitted_at') or '', r.get('id', 0))):
        user = review.get('user') or {}
        if (user.get('type') != 'User' or not user.get('login')
                or user['login'] == pull.get('user', {}).get('login')):
            continue
        if user['login'] not in permissions:
            permission = client.api(f"collaborators/{user['login']}/permission")
            permissions[user['login']] = permission.get('permission') in ('admin', 'maintain', 'write')
        if not permissions[user['login']]:
            continue
        if review.get('state') in ('APPROVED', 'CHANGES_REQUESTED', 'DISMISSED'):
            opinions[user['login']] = review
    if any(r['state'] == 'CHANGES_REQUESTED' for r in opinions.values()):
        return False
    return not require_approval or any(r['state'] == 'APPROVED' and r.get('commit_id') == head for r in opinions.values())


def eligible(client, number, head):
    pull = client.api(f'pulls/{number}')
    identity(pull, number, head)
    if pull.get('merged') or pull.get('state') != 'open':
        return dict(status='merged' if pull.get('merged') else 'closed', head=head)
    validate_files(client, pull, head)
    trusted = assessment(client, number, head)
    if trusted['status'] == 'superseded':
        return dict(status='superseded', head=head)
    snapshot = client.graphql(QUERY, dict(number=number))['repository']['pullRequest']
    if (not snapshot or snapshot['id'] != pull['node_id'] or snapshot['headRefOid'] != head
            or snapshot['headRefName'] != BRANCH or snapshot['baseRefName'] != 'main'
            or snapshot['isDraft'] or snapshot['state'] != 'OPEN'):
        raise ValueError('Documentation PR changed during eligibility assessment')
    if (snapshot.get('reviewDecision') in ('REVIEW_REQUIRED', 'CHANGES_REQUESTED')
            or not human_review(client, pull, head, require_approval=trusted['evidenceStatus'] == 'review-required')):
        return dict(status='waiting-review', head=head)
    return dict(status='eligible', head=head)


def dequeue_if_current(client, number, head):
    try:
        identity(client.api(f'pulls/{number}'), number, head)
    except ValueError:
        return
    snapshot = client.graphql(QUERY, dict(number=number))['repository']['pullRequest']
    if (snapshot and snapshot['headRefOid'] == head and snapshot['headRefName'] == BRANCH
            and snapshot['baseRefName'] == 'main' and snapshot['mergeQueueEntry']):
        client.graphql(DEQUEUE, dict(id=snapshot['id']))


def publish_eligibility(client, number, head):
    pull = client.api(f'pulls/{number}')
    if pull.get('number') != number or pull.get('head', {}).get('sha') != head:
        raise ValueError('PR head changed before eligibility status publication')
    rolling = (pull['head'].get('ref') == BRANCH and pull['head'].get('repo', {}).get('id') == REPOSITORY_ID)
    if not rolling:
        client.status(head, 'success', 'Release-docs review gate does not apply to this PR')
        return dict(status='not-applicable', head=head)
    client.status(head, 'pending', 'Validating release docs and current-head human review')
    try:
        result = eligible(client, number, head)
    except (ValueError, RuntimeError):
        client.status(head, 'failure', 'Release-docs validation failed; inspect the queue workflow')
        raise
    if result['status'] == 'eligible':
        client.status(head, 'success', 'Verified release docs and required human review')
    elif result['status'] == 'waiting-review':
        client.status(head, 'pending', 'A human maintainer must review this exact docs head')
    else:
        client.status(head, 'failure', 'Release-docs proposal is superseded or no longer open')
    return result


def deliver(client, number, head):
    if type(number) is not int or number <= 0 or not re.fullmatch('[a-f0-9]{40}', head):
        raise ValueError('Select a PR number and exact server-observed head')
    pull = client.api(f'pulls/{number}')
    identity(pull, number, head)
    if pull.get('merged'):
        return dict(status='merged', head=head)
    if pull.get('state') != 'open':
        return dict(status='closed', head=head)
    try:
        gate = eligible(client, number, head)
    except (ValueError, RuntimeError):
        dequeue_if_current(client, number, head)
        raise
    if gate['status'] != 'eligible':
        dequeue_if_current(client, number, head)
        return gate
    trusted = assessment(client, number, head)
    repository = client.graphql(QUERY, dict(number=number))['repository']
    snapshot = repository['pullRequest']
    if (not snapshot or snapshot['id'] != pull['node_id'] or snapshot['headRefOid'] != head
            or snapshot['headRefName'] != BRANCH or snapshot['baseRefName'] != 'main'
            or snapshot['isDraft']):
        raise ValueError('Documentation PR changed during validation')
    if trusted['status'] == 'superseded':
        if snapshot['mergeQueueEntry']:
            client.graphql(DEQUEUE, dict(id=pull['node_id']))
        return dict(status='superseded', head=head)
    if snapshot['merged']:
        return dict(status='merged', head=head)
    if snapshot['state'] != 'OPEN':
        return dict(status='closed', head=head)
    if snapshot['mergeQueueEntry']:
        return dict(status='queued', head=head)
    contexts = required_contexts(client, repository)
    checks = client.checks(number)
    registered = {check.get('name') for check in checks}
    if not contexts <= registered:
        return dict(status='waiting-checks', head=head)
    relevant = [check for check in checks if check.get('name') in contexts]
    if any(check.get('bucket') in ('fail', 'cancel') for check in relevant):
        return dict(status='failed-checks', head=head)
    if not all(check.get('bucket') in ('pass', 'skipping') for check in relevant):
        return dict(status='waiting-checks', head=head)
    if snapshot.get('reviewDecision') in ('REVIEW_REQUIRED', 'CHANGES_REQUESTED'):
        return dict(status='waiting-review', head=head)
    if not human_review(client, pull, head, require_approval=trusted['evidenceStatus'] == 'review-required'):
        return dict(status='waiting-review', head=head)
    # Recompute evidence and re-read server identity immediately before mutation.
    # The App never approves, merges directly, or overrides branch protection.
    latest = assessment(client, number, head)
    if latest != trusted:
        return dict(status='superseded' if latest['status'] == 'superseded' else 'changed-baseline', head=head)
    if eligible(client, number, head)['status'] != 'eligible':
        return dict(status='waiting-review', head=head)
    identity(client.api(f'pulls/{number}'), number, head)
    client.graphql(ENQUEUE, dict(id=pull['node_id'], head=head))
    return dict(status='queued', head=head)


def resolve_event(client, event, event_name, manual_pr=None):
    if event.get('repository', {}).get('id') != REPOSITORY_ID:
        raise ValueError('Queue signals must belong to gitstore-dev/cli')
    if event_name == 'workflow_dispatch' and manual_pr:
        number = int(manual_pr)
    elif event_name == 'pull_request_target':
        number = event['pull_request']['number']
    elif event_name in ('workflow_run', 'workflow_dispatch'):
        if event_name == 'workflow_run':
            run = event.get('workflow_run', {})
            if (run.get('status') != 'completed' or run.get('repository', {}).get('id') != REPOSITORY_ID
                    or run.get('head_repository', {}).get('id') != REPOSITORY_ID
                    or run.get('head_branch') != BRANCH):
                return None
        pulls = client.api(f'pulls?state=open&head=gitstore-dev:{BRANCH}&base=main&per_page=2')
        if not isinstance(pulls, list) or len(pulls) > 1:
            raise ValueError('Ambiguous rolling release-docs proposal')
        if not pulls:
            return None
        number = pulls[0]['number']
    else:
        raise ValueError('Unsupported documentation queue event')
    if type(number) is not int or number <= 0:
        raise ValueError('Select a positive server PR number')
    pull = client.api(f'pulls/{number}')
    head = pull.get('head', {}).get('sha', '')
    if (pull.get('number') != number or pull.get('base', {}).get('repo', {}).get('id') != REPOSITORY_ID
            or pull['base'].get('ref') != 'main'):
        raise ValueError('Signal does not identify a PR against this repository main')
    if pull.get('state') != 'open' or not re.fullmatch('[a-f0-9]{40}', head):
        return None
    rolling = (pull['head'].get('ref') == BRANCH and pull['head'].get('repo', {}).get('id') == REPOSITORY_ID)
    return dict(number=number, head=head, rolling=rolling)


def merge_group(client, head):
    if not re.fullmatch('[a-f0-9]{40}', head):
        raise ValueError('Select an exact merge-group head')
    pulls = client.api(f'pulls?state=open&head=gitstore-dev:{BRANCH}&base=main&per_page=2')
    if not isinstance(pulls, list) or len(pulls) > 1:
        raise ValueError('Ambiguous rolling release-docs proposal')
    for pull in pulls:
        number = pull['number']
        current = client.api(f'pulls/{number}')
        proposal_head = current.get('head', {}).get('sha', '')
        identity(current, number, proposal_head)
        comparison = client.api(f'compare/{proposal_head}...{head}')
        if comparison.get('status') not in ('ahead', 'identical', 'behind', 'diverged'):
            raise ValueError('Cannot determine merge-group membership')
        if comparison['status'] in ('ahead', 'identical'):
            result = eligible(client, number, proposal_head)
            if result['status'] != 'eligible':
                raise ValueError('Merge group contains release docs without current eligibility')
    return dict(status='eligible', head=head)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pr', type=int)
    parser.add_argument('--head')
    parser.add_argument('--resolve-event', action='store_true')
    parser.add_argument('--eligibility', action='store_true')
    parser.add_argument('--merge-group')
    args = parser.parse_args()
    if not os.environ.get('GH_TOKEN'):
        parser.error('a read-only GitHub token is required')
    client = GitHub()
    if args.merge_group:
        print(json.dumps(merge_group(client, args.merge_group)))
        return
    if args.resolve_event:
        value = resolve_event(client, json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text()),
                              os.environ['GITHUB_EVENT_NAME'], args.pr)
        if value and (output := os.environ.get('GITHUB_OUTPUT')):
            with Path(output).open('a') as stream:
                stream.write(f"pr={value['number']}\nhead={value['head']}\nrolling={str(value['rolling']).lower()}\n")
        print(json.dumps(value))
        return
    if args.pr is None or not args.head:
        parser.error('--pr and --head are required')
    if args.eligibility:
        result = publish_eligibility(client, args.pr, args.head)
    elif not os.environ.get('GITSTORE_QUEUE_TOKEN'):
        parser.error('queueing requires --pr, --head and the repository-scoped queue App token')
    else:
        result = deliver(client, args.pr, args.head)
        # Status registration can lag the preceding eligibility write. Give
        # that propagation a bounded chance; later CI/review events retry real
        # pending work without a long-running privileged polling job.
        for _ in range(6):
            if result['status'] != 'waiting-checks':
                break
            time.sleep(10)
            result = deliver(client, args.pr, args.head)
    print(json.dumps(result))
    if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
        with Path(summary).open('a') as stream:
            stream.write(f"**Release documentation: {result['status']}** for PR #{args.pr} at `{args.head}`.\n")


if __name__ == '__main__':
    main()
