"""Propose an immutable single-file tap update; never write protected main."""
import base64
import hashlib
import re
from urllib.parse import urlencode

PATH = 'Formula/gitstore.rb'


def propose(api, repository, tag, payload):
    if not re.fullmatch(r'v0\.1\.0-alpha\.[1-9][0-9]*', tag) or not 0 < len(payload) <= 65536:
        raise ValueError('select a bounded formula and exact alpha tag')
    text = payload.decode('utf-8')
    branch = 'codex/homebrew-' + tag
    message = 'chore: update Homebrew formula for ' + tag
    owner = repository.split('/')[0]

    def formula(ref):
        result = api('contents/' + PATH + '?' + urlencode({'ref': ref}), missing=True)
        if result is None:
            return None
        if result.get('type') != 'file' or result.get('encoding') != 'base64':
            raise ValueError('tap formula is not an ordinary file')
        return base64.b64decode(result['content'], validate=False)

    def ordinary_tree(tree_sha):
        tree = api('git/trees/' + tree_sha + '?recursive=1')
        entries = [entry for entry in tree['tree'] if entry['path'] == PATH]
        if tree.get('truncated') or len(entries) != 1 or entries[0]['type'] != 'blob' or entries[0]['mode'] != '100644':
            raise ValueError('tap formula mode/type changed')

    main = api('git/ref/heads/main')['object']['sha']
    current = formula(main)
    if current == payload:
        ordinary_tree(api('git/commits/' + main)['tree']['sha'])
        return dict(status='current')
    reference = api('git/ref/heads/' + branch, missing=True)
    if reference is None:
        tree = api('git/commits/' + main)['tree']['sha']
        tree = api('git/trees', method='POST', payload=dict(base_tree=tree,
            tree=[dict(path=PATH, mode='100644', type='blob', content=text)]))['sha']
        commit = api('git/commits', method='POST', payload=dict(message=message, tree=tree, parents=[main]))['sha']
        api('git/refs', method='POST', payload=dict(ref='refs/heads/' + branch, sha=commit))
        reference = api('git/ref/heads/' + branch)
    head = reference['object']['sha']
    commit = api('commits/' + head)
    files = commit.get('files', [])
    parents = commit.get('parents', [])
    if (commit['commit']['message'] != message or len(parents) != 1 or len(files) != 1
            or files[0].get('filename') != PATH or files[0].get('status') not in ('added', 'modified')
            or files[0].get('previous_filename') or formula(head) != payload):
        raise ValueError('tap branch changed; preserve it for maintainer review')
    ordinary_tree(commit['commit']['tree']['sha'])
    parent = parents[0]['sha']
    comparison = api('compare/' + parent + '...' + main)
    if comparison['status'] not in ('identical', 'ahead') or formula(parent) != current:
        raise ValueError('tap base changed; maintainer reconciliation required')
    query = urlencode(dict(state='all', head=owner + ':' + branch, per_page=100))
    pulls = api('pulls?' + query)
    if len(pulls) > 1:
        raise ValueError('ambiguous tap proposals')
    if not pulls:
        pulls = [api('pulls', method='POST', payload=dict(title=message, head=branch, base='main',
            maintainer_can_modify=False, body=(f'Install the verified formula from published release `{tag}`.\n\n'
                f'Formula SHA-256: `{hashlib.sha256(payload).hexdigest()}`.\n\n'
                'Before merging, confirm this is still the newest published prerelease. '
                'Merge through the normal review and merge queue. Publication is complete; '
                'Homebrew delivery remains pending until this formula is on main.')))]
    pull = pulls[0]
    if (pull['state'] != 'open' or pull['head']['sha'] != head or pull['head']['ref'] != branch
            or pull['head']['repo']['full_name'] != repository or pull['base']['ref'] != 'main'):
        raise ValueError('tap proposal closed or changed; maintainer review required')
    return dict(status='pending', pullRequest=pull['html_url'])
