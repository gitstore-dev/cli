"""Queue-only delivery, with synthetic GitHub state and no real credentials/network."""
import base64
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('queue_homebrew', ROOT/'scripts/queue-homebrew.py')
queue = importlib.util.module_from_spec(spec)
spec.loader.exec_module(queue)
TAG = 'v0.1.0-alpha.8'
HEAD = 'a'*40
PAYLOAD = b'verified formula\n'
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()


class Client:
    def __init__(self):
        self.pull = dict(number=1,node_id='PR_fixture',draft=False,
            head=dict(sha=HEAD,ref='codex/homebrew-'+TAG,repo=dict(id=1431005655)),
            base=dict(ref='main',repo=dict(id=1431005655)))
        self.files = [dict(filename=queue.FORMULA,status='modified')]
        self.tree = dict(truncated=False,tree=[dict(path=queue.FORMULA,type='blob',mode='100644')])
        self.payload = PAYLOAD
        self.main = PAYLOAD
        self.releases = [dict(id=123,tag_name=TAG,draft=False,published_at='2026-11-14T22:00:00Z')]
        self.digest = DIGEST
        self.snapshot = dict(id='PR_fixture',state='OPEN',headRefOid=HEAD,headRefName='codex/homebrew-'+TAG,
            baseRefName='main',isDraft=False,merged=False,mergeStateStatus='CLEAN',mergeQueueEntry=None)
        self.required = [dict(name='required',bucket='pass')]
        self.mutations = []
        self.elapsed = 0
        self.ticks = []
        self.race = False
        self.fail_enqueue = False

    def api(self, path):
        if path == 'pulls/1':return self.pull
        if path == 'pulls/1/files?per_page=2':return self.files
        if path == f'git/trees/{HEAD}?recursive=1':return self.tree
        if path.startswith('contents/'):
            return dict(type='file',encoding='base64',content=base64.b64encode(self.main if path.endswith('ref=main') else self.payload).decode())
        if path == 'releases?per_page=100':return self.releases
        if path == 'releases/123/assets?per_page=100':
            return [dict(name='gitstore.rb',digest='sha256:'+self.digest,
                         browser_download_url=f'https://github.com/gitstore-dev/cli/releases/download/{TAG}/gitstore.rb')]
        raise AssertionError(path)

    def graphql(self, query, variables):
        if query == queue.QUERY:return dict(repository=dict(pullRequest=copy.deepcopy(self.snapshot)))
        self.mutations.append((query,variables))
        if query == queue.ENQUEUE:
            if self.race or self.fail_enqueue:raise RuntimeError('expected head mismatch or uncertain response')
            self.snapshot['mergeQueueEntry'] = dict(id='queue-fixture')
            return dict(enqueuePullRequest=dict(mergeQueueEntry=dict(id='queue-fixture')))
        if query == queue.DEQUEUE:
            self.snapshot['mergeQueueEntry'] = None
            return dict(dequeuePullRequest=dict(clientMutationId=None))
        raise AssertionError(query)

    def checks(self, number):return self.required
    def clock(self):return self.elapsed
    def sleep(self, seconds):
        self.elapsed += seconds
        if self.ticks:self.ticks.pop(0)()
        else:self.snapshot.update(merged=True,state='MERGED',mergeQueueEntry=None)


class HomebrewQueue(unittest.TestCase):
    def deliver(self,c,**kwargs):
        return queue.deliver(c,TAG,1,HEAD,DIGEST,sleep=c.sleep,clock=c.clock,**kwargs)

    def test_waits_for_checks_enqueues_exact_head_and_observes_main(self):
        c=Client();c.required[0]['bucket']='pending';c.snapshot['mergeStateStatus']='BLOCKED'
        def pass_checks():
            self.assertFalse(c.mutations)
            c.required[0]['bucket']='pass';c.snapshot['mergeStateStatus']='CLEAN'
        c.ticks=[pass_checks]
        self.assertEqual(self.deliver(c)['status'],'ready')
        self.assertEqual(c.mutations,[(queue.ENQUEUE,dict(id='PR_fixture',head=HEAD))])

    def test_existing_queue_and_merged_delivery_are_idempotent(self):
        for merged in [False,True]:
            c=Client();c.snapshot.update(merged=merged,state='MERGED' if merged else 'OPEN',
                                         mergeQueueEntry=None if merged else dict(id='existing'))
            self.assertEqual(self.deliver(c)['status'],'ready')
            self.assertFalse(c.mutations)

    def test_failed_checks_and_changed_pr_never_queue(self):
        for change in ['check','head','file','mode','payload','repo','base','closed','draft']:
            c=Client()
            if change=='check':c.required[0]['bucket']='fail'
            elif change=='head':c.snapshot['headRefOid']='b'*40
            elif change=='file':c.files.append(dict(filename='scripts/unsafe.py',status='added'))
            elif change=='mode':c.tree['tree'][0]['mode']='120000'
            elif change=='payload':c.payload=b'edited'
            elif change=='repo':c.pull['head']['repo']['id']=1
            elif change=='base':c.pull['base']['ref']='other'
            elif change=='closed':c.snapshot['state']='CLOSED'
            else:c.snapshot['isDraft']=True
            with self.subTest(change=change),self.assertRaises(ValueError):self.deliver(c)
            self.assertFalse(c.mutations)

    def test_newer_release_never_queues_downgrade_and_dequeues_stale_entry(self):
        for queued in [False,True]:
            c=Client();c.releases.append(dict(id=124,tag_name='v0.1.0-alpha.9',draft=False,published_at='later'))
            if queued:c.snapshot['mergeQueueEntry']=dict(id='existing')
            self.assertEqual(self.deliver(c)['status'],'superseded')
            self.assertEqual(c.mutations,[(queue.DEQUEUE,dict(id='PR_fixture'))] if queued else [])

    def test_missing_or_tampered_publication_and_missing_checks_fail_closed(self):
        for change in ['draft','digest','no-checks']:
            c=Client()
            if change=='draft':c.releases[0]['draft']=True
            elif change=='digest':c.digest='b'*64
            else:c.required=[];c.sleep=lambda seconds:setattr(c,'elapsed',c.elapsed+seconds)
            with self.subTest(change=change),self.assertRaises((ValueError,TimeoutError)):
                self.deliver(c,timeout=40)
            self.assertFalse(c.mutations)

    def test_queue_removal_or_raced_push_requires_inspection_instead_of_repeated_mutation(self):
        c=Client();c.race=True
        with self.assertRaises(RuntimeError):self.deliver(c)
        self.assertEqual(len(c.mutations),1)
        c=Client();c.ticks=[lambda: c.snapshot.update(mergeQueueEntry=None)]
        with self.assertRaises(ValueError):self.deliver(c)
        self.assertEqual(len(c.mutations),1)

    def test_auth_stays_in_environment_and_mutations_are_structured(self):
        with patch.dict(os.environ,GH_TOKEN='synthetic-read-token',GITSTORE_QUEUE_TOKEN='synthetic-app-token'),patch.object(queue.subprocess,'run',
                return_value=subprocess.CompletedProcess([],0,'{"data":{"ok":true}}','')) as run:
            queue.GitHub().graphql(queue.ENQUEUE,dict(id='PR',head=HEAD))
            args,kwargs=run.call_args
            self.assertNotIn('synthetic-app-token',str(args))
            self.assertEqual(kwargs['env']['GH_TOKEN'],'synthetic-app-token')
            self.assertEqual(json.loads(kwargs['input'])['variables'],dict(id='PR',head=HEAD))
            self.assertEqual(os.environ['GH_TOKEN'],'synthetic-read-token')
            queue.GitHub().graphql(queue.QUERY,dict(number=1))
            self.assertEqual(run.call_args.kwargs['env']['GH_TOKEN'],'synthetic-read-token')

    def test_read_token_cannot_be_used_as_fallback_to_modify_the_queue(self):
        with patch.dict(os.environ,dict(GH_TOKEN='synthetic-read-token'),clear=True),patch.object(queue.subprocess,'run') as run:
            with self.assertRaises(ValueError):queue.GitHub().graphql(queue.ENQUEUE,dict(id='PR',head=HEAD))
            run.assert_not_called()

    def test_unknown_pr_checks_response_is_not_treated_as_success(self):
        with patch.object(queue.subprocess,'run',return_value=subprocess.CompletedProcess([],1,'','API failure')):
            with self.assertRaises(RuntimeError):queue.GitHub().checks(1)


if __name__=='__main__':unittest.main()
