#!/usr/bin/env python3
"""Deliver one verified formula through required checks and the normal merge queue."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

REPOSITORY = 'gitstore-dev/cli'
REPOSITORY_ID = 1431005655
FORMULA = 'Formula/gitstore.rb'
QUERY = '''query($number:Int!) {
  repository(owner:"gitstore-dev",name:"cli") { pullRequest(number:$number) {
    id state headRefOid headRefName baseRefName isDraft merged mergeStateStatus reviewDecision
    mergeQueueEntry { id }
  } }
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
            env={**os.environ, 'GH_HOST': 'github.com', 'GH_PROMPT_DISABLED': '1'})

    def api(self, path):
        reply = self.command(['api', f'repos/{REPOSITORY}/{path}'])
        if reply.returncode:
            raise RuntimeError('Cannot read Homebrew delivery state from GitHub; inspect before retry')
        return json.loads(reply.stdout)

    def graphql(self, query, variables):
        environment = {**os.environ, 'GH_HOST': 'github.com', 'GH_PROMPT_DISABLED': '1'}
        if query in (ENQUEUE, DEQUEUE):
            token = os.environ.get('GITSTORE_QUEUE_TOKEN')
            if not token:
                raise ValueError('The scoped App token is required for queue mutations')
            environment['GH_TOKEN'] = token
        reply = subprocess.run(['gh', 'api', 'graphql', '--input', '-'],
            input=json.dumps(dict(query=query, variables=variables)), capture_output=True,
            text=True, timeout=120,
            env=environment)
        if reply.returncode:
            raise RuntimeError('Merge queue request failed; inspect its state before retry')
        value = json.loads(reply.stdout)
        if value.get('errors'):
            raise RuntimeError('GitHub rejected the merge queue request; no protection is bypassed')
        return value['data']

    def checks(self, number):
        reply = self.command(['pr', 'checks', str(number), '--repo', REPOSITORY,
                              '--required', '--json', 'name,bucket'])
        if reply.returncode not in (0, 1, 8):
            raise RuntimeError('Cannot read required formula checks')
        if not reply.stdout.strip() and 'no checks' in reply.stderr.lower():
            return []  # CI registration can lag behind PR creation.
        try:
            checks = json.loads(reply.stdout)
        except ValueError as error:
            raise RuntimeError('Cannot read required formula checks') from error
        if not isinstance(checks, list):
            raise ValueError('Malformed required checks')
        return checks


def formula(client, ref):
    value = client.api(f'contents/{FORMULA}?ref={ref}')
    if value.get('type') != 'file' or value.get('encoding') != 'base64':
        raise ValueError('Formula is not an ordinary file')
    payload = base64.b64decode(value['content'])
    if not 0 < len(payload) <= 65536:
        raise ValueError('Formula exceeds its input bounds')
    return payload


def newest_release(client, tag, digest):
    releases = client.api('releases?per_page=100')
    if len(releases) >= 100:
        raise ValueError('Release listing is incomplete; maintainer review required')
    published = [r for r in releases if not r['draft'] and r.get('published_at')]
    def order(release):
        match = re.fullmatch(r'v(\d+)\.(\d+)\.(\d+)(?:-alpha\.([1-9]\d*))?', release['tag_name'])
        if not match:
            raise ValueError('Unrecognized published version; do not guess release order')
        return (*map(int, match.groups()[:3]), match[4] is None, int(match[4] or 0))
    selected = next((r for r in published if r['tag_name'] == tag), None)
    if selected is None:
        raise ValueError('Selected release is not published')
    assets = [a for a in client.api(f"releases/{selected['id']}/assets?per_page=100") if a['name'] == 'gitstore.rb']
    if (len(assets) != 1 or assets[0].get('digest') != 'sha256:' + digest
            or assets[0].get('browser_download_url') != f'https://github.com/{REPOSITORY}/releases/download/{tag}/gitstore.rb'):
        raise ValueError('Published formula differs from the verified publication result')
    return max(published, key=order)['tag_name']


def validate_proposal(client, tag, number, head, digest):
    pull = client.api(f'pulls/{number}')
    if (pull['number'] != number or pull['head']['sha'] != head
            or pull['head']['ref'] != 'codex/homebrew-' + tag
            or pull['head']['repo']['id'] != REPOSITORY_ID
            or pull['base']['repo']['id'] != REPOSITORY_ID or pull['base']['ref'] != 'main'
            or pull.get('draft')):
        raise ValueError('Formula PR identity changed; preserve it for maintainer review')
    files = client.api(f'pulls/{number}/files?per_page=2')
    if (len(files) != 1 or files[0]['filename'] != FORMULA
            or files[0]['status'] not in ('added', 'modified') or files[0].get('previous_filename')):
        raise ValueError('Only the published single-file formula PR can be queued')
    tree = client.api(f'git/trees/{head}?recursive=1')
    entries = [entry for entry in tree['tree'] if entry['path'] == FORMULA]
    if (tree.get('truncated') or len(entries) != 1 or entries[0]['type'] != 'blob'
            or entries[0]['mode'] != '100644'
            or hashlib.sha256(formula(client, head)).hexdigest() != digest):
        raise ValueError('Formula bytes or mode differ from the verified publication result')
    return pull['node_id']


def deliver(client, tag, number, head, digest, *, timeout=3300, sleep=time.sleep, clock=time.monotonic):
    if (not re.fullmatch(r'v0\.1\.0-alpha\.[1-9]\d*', tag)
            or type(number) is not int or number <= 0
            or not re.fullmatch('[a-f0-9]{40}', head) or not re.fullmatch('[a-f0-9]{64}', digest)):
        raise ValueError('Select an exact published alpha, PR number, commit and formula digest')
    node = validate_proposal(client, tag, number, head, digest)
    deadline = clock() + timeout
    enqueued = False
    previous = None
    while clock() < deadline:
        pull = client.graphql(QUERY, dict(number=number))['repository']['pullRequest']
        if (not pull or pull['id'] != node or pull['headRefOid'] != head or pull['baseRefName'] != 'main'
                or pull['headRefName'] != 'codex/homebrew-' + tag or pull['isDraft']):
            raise ValueError('Formula PR changed; no queue mutation is allowed')
        latest = newest_release(client, tag, digest)
        if latest != tag:
            if pull['mergeQueueEntry']:
                client.graphql(DEQUEUE, dict(id=node))
            return dict(status='superseded', tag=tag, latest=latest)
        if pull['merged']:
            if hashlib.sha256(formula(client, 'main')).hexdigest() != digest:
                raise ValueError('Merged formula is not current on main; maintainer review required')
            return dict(status='ready', tag=tag, pullRequest=f'https://github.com/{REPOSITORY}/pull/{number}')
        if pull['state'] != 'OPEN':
            raise ValueError('Formula PR closed without merging; do not recreate it automatically')
        if pull['mergeQueueEntry']:
            enqueued = True
            state = 'queued; waiting for merge-group checks'
        elif enqueued:
            raise ValueError('Formula PR left the merge queue; inspect failed checks before retry')
        else:
            checks = client.checks(number)
            if any(c['bucket'] in ('fail', 'cancel') for c in checks):
                raise ValueError('Required formula checks failed; publication remains successful')
            if (checks and all(c['bucket'] in ('pass', 'skipping') for c in checks)
                    and pull['mergeStateStatus'] in ('CLEAN', 'UNSTABLE')
                    and pull.get('reviewDecision') not in ('REVIEW_REQUIRED', 'CHANGES_REQUESTED')):
                # Queue-only mutation: never merge directly or use an admin bypass.
                # GitHub atomically rejects a concurrent push via expectedHeadOid.
                client.graphql(ENQUEUE, dict(id=node, head=head))
                enqueued = True
                state = 'queued; waiting for merge-group checks'
            else:
                state = 'waiting for required PR checks/reviews'
        if state != previous:
            print(state, flush=True)
            previous = state
        sleep(20)
    raise TimeoutError('Homebrew delivery timed out; inspect the PR/queue and rerun this job, not publication')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--pr', type=int, required=True)
    parser.add_argument('--head', required=True)
    parser.add_argument('--formula-sha256', required=True)
    args = parser.parse_args()
    if not os.environ.get('GH_TOKEN') or not os.environ.get('GITSTORE_QUEUE_TOKEN'):
        parser.error('the read-only GitHub token and repository-scoped queue App token are required')
    result = deliver(GitHub(), args.tag, args.pr, args.head, args.formula_sha256)
    print(json.dumps(result))
    if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
        with Path(summary).open('a') as stream:
            stream.write(f"**Homebrew delivery: {result['status']}** for {args.tag}.\n\n")
            if result['status'] == 'ready':
                stream.write('The formula merged through the normal queue. Run `brew update` then `brew upgrade gitstore-dev/cli/gitstore`.\n')
            else:
                stream.write(f"A newer published release ({result['latest']}) superseded this formula; no downgrade was queued.\n")


if __name__ == '__main__':
    main()
