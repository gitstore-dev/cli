"""Release-docs queue policy with synthetic server state; no credentials/network."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('queue_docs', ROOT/'scripts/queue-docs.py')
queue = importlib.util.module_from_spec(spec)
spec.loader.exec_module(queue)
HEAD = 'a' * 40
SOURCE = 'b' * 40
BASELINE = 'c' * 40
GROUP = 'd' * 40


class Client:
    def __init__(self):
        self.pull = dict(number=55, node_id='PR_fixture', state='open', merged=False, draft=False,
            changed_files=3, user=dict(login='release-bot[bot]'),
            head=dict(sha=HEAD, ref=queue.BRANCH, repo=dict(id=queue.REPOSITORY_ID)),
            base=dict(ref='main', repo=dict(id=queue.REPOSITORY_ID)))
        self.files = [dict(filename=name, status='modified') for name in
            ['site/release.json', 'site/release-evidence.json', 'site/reference/gs.md']]
        self.tree = dict(truncated=False, tree=[dict(path=f['filename'], type='blob', mode='100644') for f in self.files])
        self.snapshot = dict(id='PR_fixture', state='OPEN', merged=False, headRefOid=HEAD,
            headRefName=queue.BRANCH, baseRefName='main', isDraft=False,
            reviewDecision=None, mergeQueueEntry=None)
        self.contexts = ['Website checks', queue.CONTEXT, 'rust', 'native']
        self.required = [dict(name=name, bucket='pass') for name in self.contexts]
        self.legacy = None
        self.evidence = dict(status='valid', head=HEAD, sourceCommit=SOURCE,
            baselineSourceCommit=BASELINE, evidenceStatus='unchanged')
        self.assessments = []
        self.reviews = []
        self.permission = 'write'
        self.mutations = []
        self.statuses = []
        self.race = False
        self.assessment_error = False
        self.compare = 'ahead'
        self.before_enqueue = None

    def api(self, path):
        if path == 'pulls/55':
            return copy.deepcopy(self.pull)
        if path == 'pulls/55/files?per_page=100':
            return copy.deepcopy(self.files)
        if path == f'git/trees/{HEAD}?recursive=1':
            return copy.deepcopy(self.tree)
        if path == 'rules/branches/main':
            return [dict(type='required_status_checks', parameters=dict(required_status_checks=[dict(context=n) for n in self.contexts]))]
        if path == 'pulls/55/reviews?per_page=100&page=1':
            return copy.deepcopy(self.reviews)
        if path.startswith('collaborators/'):
            return dict(permission=self.permission)
        if path.startswith('pulls?state=open&head='):
            return [dict(number=55)]
        if path == f'compare/{HEAD}...{GROUP}':
            return dict(status=self.compare)
        raise AssertionError(path)

    def graphql(self, query, variables):
        if query == queue.QUERY:
            return dict(repository=dict(pullRequest=copy.deepcopy(self.snapshot), ref=dict(branchProtectionRule=self.legacy)))
        self.mutations.append((query, variables))
        if query == queue.ENQUEUE:
            if self.before_enqueue:
                self.before_enqueue()
            if self.race:
                raise RuntimeError('GitHub expectedHeadOid rejected the raced push')
            self.snapshot['mergeQueueEntry'] = dict(id='queued')
            return dict(enqueuePullRequest=dict(mergeQueueEntry=dict(id='queued')))
        if query == queue.DEQUEUE:
            self.snapshot['mergeQueueEntry'] = None
            return dict(dequeuePullRequest=dict(clientMutationId=None))
        raise AssertionError(query)

    def assess(self, number, head):
        if self.assessment_error:
            raise ValueError('Proposed status file says unchanged but trusted recomputation differs')
        value = self.assessments.pop(0) if self.assessments else self.evidence
        return copy.deepcopy(value)

    def checks(self, number):
        return copy.deepcopy(self.required)

    def status(self, head, state, description):
        self.statuses.append((head, state, description))


def approval(**updates):
    value = dict(id=1, state='APPROVED', commit_id=HEAD, submitted_at='2026-11-15T00:00:00Z',
        user=dict(login='maintainer', type='User'), author_association='COLLABORATOR')
    value.update(updates)
    return value


class DocsQueue(unittest.TestCase):
    def test_exact_validated_head_queues_without_optional_checks_blocking(self):
        c = Client(); c.required.append(dict(name='optional integration', bucket='fail'))
        self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'queued')
        self.assertEqual(c.mutations, [(queue.ENQUEUE, dict(id='PR_fixture', head=HEAD))])

    def test_all_required_contexts_must_register_including_new_rules_and_legacy(self):
        for mode in ['missing', 'new-rule', 'legacy', 'pending', 'unknown']:
            c = Client()
            if mode == 'missing': c.required = []
            elif mode == 'new-rule': c.contexts.append('newly required')
            elif mode == 'legacy': c.legacy = dict(requiresStatusChecks=True, requiredStatusCheckContexts=['legacy required'])
            elif mode == 'pending': c.required[0]['bucket'] = 'pending'
            else: c.required[0]['bucket'] = 'mystery'
            with self.subTest(mode=mode):
                self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'waiting-checks')
                self.assertFalse(c.mutations)

    def test_failed_required_checks_never_enqueue(self):
        for bucket in ['fail', 'cancel']:
            c = Client(); c.required[0]['bucket'] = bucket
            self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'failed-checks')
            self.assertFalse(c.mutations)

    def test_current_head_human_maintainer_approval_is_required_for_changed_capabilities(self):
        for mode in ['missing', 'old-head', 'bot', 'self', 'outside', 'read-only', 'dismissed', 'changes-requested']:
            c = Client(); c.evidence['evidenceStatus'] = 'review-required'; c.reviews = [approval()]
            if mode == 'missing': c.reviews = []
            elif mode == 'old-head': c.reviews[0]['commit_id'] = 'e' * 40
            elif mode == 'bot': c.reviews[0]['user']['type'] = 'Bot'
            elif mode == 'self': c.pull['user']['login'] = 'maintainer'
            elif mode == 'outside': c.reviews[0]['author_association'] = 'NONE'; c.permission = 'read'
            elif mode == 'read-only': c.permission = 'read'
            elif mode == 'dismissed': c.reviews[0]['state'] = 'DISMISSED'
            else: c.reviews.append(approval(id=2, state='CHANGES_REQUESTED', submitted_at='2026-11-15T01:00:00Z'))
            with self.subTest(mode=mode):
                self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'waiting-review')
                self.assertFalse(c.mutations)
        c = Client(); c.evidence['evidenceStatus'] = 'review-required'; c.reviews = [approval()]
        self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'queued')

    def test_normal_review_requirements_and_human_change_requests_remain_gates(self):
        for decision in ['REVIEW_REQUIRED', 'CHANGES_REQUESTED']:
            c = Client(); c.snapshot['reviewDecision'] = decision
            self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'waiting-review')
            self.assertFalse(c.mutations)
        c = Client(); c.reviews = [approval(state='CHANGES_REQUESTED')]
        self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'waiting-review')

    def test_untrusted_evidence_claim_invalid_inputs_and_head_races_fail_closed(self):
        for mode in ['assessment', 'head', 'repo', 'branch', 'base', 'file', 'mode', 'truncated', 'incomplete', 'evidence-schema']:
            c = Client()
            if mode == 'assessment': c.assessment_error = True
            elif mode == 'head': c.snapshot['headRefOid'] = 'f' * 40
            elif mode == 'repo': c.pull['head']['repo']['id'] = 1
            elif mode == 'branch': c.pull['head']['ref'] = 'untrusted'
            elif mode == 'base': c.pull['base']['ref'] = 'other'
            elif mode == 'file': c.files[0]['filename'] = 'scripts/unsafe.py'
            elif mode == 'mode': c.tree['tree'][0]['mode'] = '120000'
            elif mode == 'truncated': c.tree['truncated'] = True
            elif mode == 'incomplete': c.pull['changed_files'] = 4
            else: del c.evidence['baselineSourceCommit']
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                queue.deliver(c, 55, HEAD)
            self.assertFalse(c.mutations)

    def test_queued_and_merged_are_idempotent_but_revoked_reviews_dequeue(self):
        c = Client(); c.snapshot['mergeQueueEntry'] = dict(id='queued')
        self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'queued'); self.assertFalse(c.mutations)
        c = Client(); c.pull['merged'] = True; c.pull['state'] = 'closed'
        self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'merged'); self.assertFalse(c.mutations)
        for invalid in ['review', 'evidence', 'superseded']:
            c = Client(); c.snapshot['mergeQueueEntry'] = dict(id='queued')
            if invalid == 'review': c.evidence['evidenceStatus'] = 'review-required'
            elif invalid == 'evidence': c.assessment_error = True
            else: c.evidence['status'] = 'superseded'
            if invalid == 'evidence':
                with self.assertRaises(ValueError): queue.deliver(c, 55, HEAD)
            else: queue.deliver(c, 55, HEAD)
            self.assertEqual(c.mutations, [(queue.DEQUEUE, dict(id='PR_fixture'))])

    def test_fresh_assessment_and_atomic_expected_head_prevent_stale_enqueue(self):
        c = Client(); c.assessments = [c.evidence, c.evidence, dict(status='superseded', head=HEAD)]
        self.assertEqual(queue.deliver(c, 55, HEAD)['status'], 'superseded'); self.assertFalse(c.mutations)
        c = Client(); c.race = True
        with self.assertRaises(RuntimeError): queue.deliver(c, 55, HEAD)
        self.assertEqual(c.mutations, [(queue.ENQUEUE, dict(id='PR_fixture', head=HEAD))])

    def test_required_status_is_pending_for_review_and_failure_for_untrusted_evidence(self):
        c = Client(); c.evidence['evidenceStatus'] = 'review-required'
        self.assertEqual(queue.publish_eligibility(c, 55, HEAD)['status'], 'waiting-review')
        self.assertEqual([s[1] for s in c.statuses], ['pending', 'pending'])
        c.reviews = [approval()]
        self.assertEqual(queue.publish_eligibility(c, 55, HEAD)['status'], 'eligible')
        self.assertEqual(c.statuses[-1][1], 'success')
        c.assessment_error = True
        with self.assertRaises(ValueError): queue.publish_eligibility(c, 55, HEAD)
        self.assertEqual(c.statuses[-1][1], 'failure')
        self.assertFalse(c.mutations)

    def test_nonrolling_pr_gets_success_without_app_or_proposal_code(self):
        c = Client(); c.pull['head']['ref'] = 'normal-fix'; c.assessment_error = True
        self.assertEqual(queue.publish_eligibility(c, 55, HEAD)['status'], 'not-applicable')
        self.assertEqual(c.statuses[-1][1], 'success'); self.assertFalse(c.mutations)

    def test_merge_group_rechecks_included_docs_and_ignores_nonmembers(self):
        c = Client(); self.assertEqual(queue.merge_group(c, GROUP)['status'], 'eligible')
        c.evidence['evidenceStatus'] = 'review-required'
        with self.assertRaises(ValueError): queue.merge_group(c, GROUP)
        c.compare = 'diverged'
        self.assertEqual(queue.merge_group(c, GROUP)['status'], 'eligible')
        self.assertFalse(c.mutations)

    def test_signals_resolve_server_head_and_ignore_foreign_or_unrelated_runs(self):
        c = Client(); event = dict(repository=dict(id=queue.REPOSITORY_ID), workflow_run=dict(
            status='completed', repository=dict(id=queue.REPOSITORY_ID), head_repository=dict(id=queue.REPOSITORY_ID), head_branch=queue.BRANCH))
        self.assertEqual(queue.resolve_event(c, event, 'workflow_run'), dict(number=55, head=HEAD, rolling=True))
        event['workflow_run']['head_repository']['id'] = 1
        self.assertIsNone(queue.resolve_event(c, event, 'workflow_run'))
        self.assertEqual(queue.resolve_event(c, event, 'workflow_dispatch', 55)['head'], HEAD)
        event['repository']['id'] = 1
        with self.assertRaises(ValueError): queue.resolve_event(c, event, 'workflow_dispatch', 55)

    def test_app_token_only_reaches_queue_mutations_and_never_arguments(self):
        env = dict(GH_TOKEN='synthetic-read-token', GITSTORE_QUEUE_TOKEN='synthetic-app-token')
        with patch.dict(os.environ, env), patch.object(queue.subprocess, 'run',
                return_value=subprocess.CompletedProcess([], 0, '{"data":{"ok":true}}', '')) as run:
            queue.GitHub().graphql(queue.ENQUEUE, dict(id='PR', head=HEAD))
            args, kwargs = run.call_args
            self.assertNotIn('synthetic-app-token', str(args))
            self.assertEqual(kwargs['env']['GH_TOKEN'], 'synthetic-app-token')
            self.assertEqual(json.loads(kwargs['input'])['variables']['head'], HEAD)
            queue.GitHub().graphql(queue.QUERY, dict(number=55))
            self.assertEqual(run.call_args.kwargs['env']['GH_TOKEN'], 'synthetic-read-token')
            self.assertNotIn('GITSTORE_QUEUE_TOKEN', run.call_args.kwargs['env'])
            queue.GitHub().status(HEAD, 'pending', 'fixture')
            self.assertNotIn('GITSTORE_QUEUE_TOKEN', run.call_args.kwargs['env'])
        with patch.dict(os.environ, dict(GH_TOKEN='read-only'), clear=True), patch.object(queue.subprocess, 'run') as run:
            with self.assertRaises(ValueError): queue.GitHub().graphql(queue.ENQUEUE, dict(id='PR', head=HEAD))
            run.assert_not_called()

    def test_invalid_check_response_is_not_success_and_missing_policy_fails_closed(self):
        with patch.object(queue.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'API failed')):
            with self.assertRaises(RuntimeError): queue.GitHub().checks(55)
        c = Client(); c.contexts.remove(queue.CONTEXT)
        with self.assertRaises(ValueError): queue.deliver(c, 55, HEAD)
        self.assertFalse(c.mutations)


if __name__ == '__main__':
    unittest.main()
