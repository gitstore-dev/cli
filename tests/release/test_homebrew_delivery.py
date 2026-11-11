"""GitHub API contract/state fixtures; never touch a live branch or release."""
import base64
import copy
from pathlib import Path
import sys
import unittest
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'runtime/packaging'))
from homebrew_delivery import propose


class API:
    def __init__(self):
        self.main = 'a' * 40
        self.parent = self.main
        self.head = 'b' * 40
        self.branch = None
        self.payload = None
        self.main_payload = None
        self.parent_payload = None
        self.files = [dict(filename='Formula/gitstore.rb', status='added')]
        self.message = 'chore: update Homebrew formula for v0.1.0-alpha.5'
        self.pulls = []
        self.writes = []
        self.fail = None
        self.comparison = 'identical'
        self.mode = '100644'
        self.main_mode = '100644'

    def __call__(self, endpoint, *, method='GET', payload=None, missing=False):
        path = urlsplit(endpoint).path
        if method != 'GET':
            self.writes.append((path, copy.deepcopy(payload)))
            if self.fail == path:
                raise RuntimeError('uncertain API submission')
        if path == 'git/ref/heads/main':return dict(object=dict(sha=self.main))
        if path.startswith('git/ref/heads/codex/'):
            return dict(object=dict(sha=self.head)) if self.branch else None
        if path == 'contents/Formula/gitstore.rb':
            ref = parse_qs(urlsplit(endpoint).query)['ref'][0]
            data = self.payload if ref == self.head else self.main_payload if ref == self.main else self.parent_payload
            return None if data is None else dict(type='file', encoding='base64', content=base64.b64encode(data).decode())
        if path.startswith('git/commits/'):
            return dict(tree=dict(sha='c' * 40))
        if path.startswith('git/trees/'):
            return dict(truncated=False, tree=[dict(path='Formula/gitstore.rb', type='blob', mode=self.main_mode if path.startswith('git/trees/'+'c'*40) else self.mode)])
        if path == 'git/trees':
            self.payload = payload['tree'][0]['content'].encode()
            return dict(sha='d' * 40)
        if path == 'git/commits':
            self.parent = payload['parents'][0]
            self.parent_payload = self.main_payload
            return dict(sha=self.head)
        if path == 'git/refs':
            self.branch = payload['ref'].removeprefix('refs/heads/')
            return dict(object=dict(sha=self.head))
        if path.startswith('commits/'):
            return dict(commit=dict(message=self.message, tree=dict(sha='d'*40)), parents=[dict(sha=self.parent)], files=self.files)
        if path.startswith('compare/'):return dict(status=self.comparison)
        if path == 'pulls':
            if method == 'POST':
                self.pulls = [dict(state='open', head=dict(sha=self.head, ref=self.branch,
                    repo=dict(full_name='gitstore-dev/cli')), base=dict(ref='main'),
                    html_url='https://github.com/gitstore-dev/cli/pull/123')]
            if method == 'POST':return self.pulls[0]
            base = parse_qs(urlsplit(endpoint).query).get('base', [None])[0]
            return [p for p in self.pulls if base is None or p['base']['ref'] == base]
        raise AssertionError((method, endpoint))


class HomebrewDelivery(unittest.TestCase):
    def setUp(self):
        self.api = API()
        self.payload = b'public signed formula fixture\n'

    def propose(self):return propose(self.api, 'gitstore-dev/cli', 'v0.1.0-alpha.5', self.payload)

    def test_creates_single_file_branch_and_pr_without_writing_main(self):
        result = self.propose()
        self.assertEqual(result['status'], 'pending')
        self.assertEqual([path for path, _ in self.api.writes], ['git/trees','git/commits','git/refs','pulls'])
        tree = self.api.writes[0][1]['tree']
        self.assertEqual(tree, [dict(path='Formula/gitstore.rb', mode='100644', type='blob', content=self.payload.decode())])
        self.assertEqual(self.api.writes[2][1]['ref'], 'refs/heads/codex/homebrew-v0.1.0-alpha.5')
        self.assertEqual(self.api.writes[3][1]['base'], 'main')
        self.assertFalse(self.api.writes[3][1]['maintainer_can_modify'])
        writes = list(self.api.writes)
        self.assertEqual(self.propose(), result)
        self.assertEqual(self.api.writes, writes)

    def test_recovers_after_uncertain_branch_or_pr_submission_without_force_updates(self):
        for point in ['git/refs', 'pulls']:
            with self.subTest(point=point):
                self.api = API();self.api.fail = point
                with self.assertRaises(RuntimeError):self.propose()
                # A lost response can have committed its mutation remotely.
                if point == 'git/refs':self.api.branch = 'codex/homebrew-v0.1.0-alpha.5'
                else:
                    self.api.fail = None
                    self.api('pulls', method='POST', payload={})
                self.api.fail = None
                count = len(self.api.writes)
                self.assertEqual(self.propose()['status'], 'pending')
                self.assertEqual([p for p, _ in self.api.writes[count:]], ['pulls'] if point == 'git/refs' else [])

    def test_matching_main_formula_needs_no_branch_or_pr(self):
        self.api.main_payload = self.payload
        self.assertEqual(self.propose(), dict(status='current'))
        self.assertFalse(self.api.writes)

    def test_matching_main_content_cannot_hide_a_symlink_or_executable_mode(self):
        self.api.main_payload = self.payload
        for mode in ['120000', '100755']:
            with self.subTest(mode=mode):
                self.api.main_mode = mode
                with self.assertRaises(ValueError):self.propose()
        self.assertFalse(self.api.writes)

    def test_refuses_modified_branch_extra_files_changed_base_and_closed_pr(self):
        for change in ['payload','extra-file','message','base','diverged','closed','head','mode','pr-base']:
            with self.subTest(change=change):
                self.api = API();self.propose();self.api.writes.clear()
                if change == 'payload':self.api.payload = b'edited'
                elif change == 'extra-file':self.api.files.append(dict(filename='other', status='added'))
                elif change == 'message':self.api.message = 'maintainer edit'
                elif change == 'base':self.api.main='e'*40;self.api.main_payload=b'maintainer edit';self.api.comparison='ahead'
                elif change == 'diverged':self.api.comparison='diverged'
                elif change == 'pr-base':self.api.pulls[0]['base']['ref']='maintainer-branch'
                elif change == 'mode':self.api.mode='100755'
                elif change == 'closed':self.api.pulls[0]['state']='closed'
                else:self.api.pulls[0]['head']['sha']='f'*40
                with self.assertRaises(ValueError):self.propose()
                self.assertFalse(self.api.writes)

    def test_main_can_advance_without_changing_the_formula(self):
        self.propose();self.api.main = 'e'*40;self.api.comparison = 'ahead'
        self.assertEqual(self.propose()['status'], 'pending')

    def test_rejects_invalid_tag_and_oversized_payload_before_api(self):
        for tag, payload in [('main',self.payload),('v0.1.0-alpha.5',b'x'*65537)]:
            with self.subTest(tag=tag),self.assertRaises(ValueError):
                propose(self.api,'gitstore-dev/cli',tag,payload)
        self.assertFalse(self.api.writes)


if __name__=='__main__':unittest.main()
