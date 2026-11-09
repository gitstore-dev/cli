"""Named client conformance against the runner's disposable pinned alpha.9 stack."""
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import time
import unittest
import uuid

from test_image_commands import Commands


class Alpha9(Commands):
    def setUp(self):
        super().setUp()
        self.directory.chmod(0o700)
        with self.config.open('a') as stream:
            stream.write('\n[contexts.integration.accounts.admin.auth]\nversion=1\nprovider="static-users"\nusername="admin"\n')

    def session(self):
        session = self.directory / 'session'
        self.invoke(['auth', 'login', '--username', 'admin', '--password-file', os.environ['GITSTORE_IMAGE_PASSWORD']], credential=session)
        self.assertEqual(session.stat().st_mode & 0o777, 0o600)
        return session

    def test_login_refresh_consumed_token_and_confirmed_remote_logout(self):
        session = self.session()
        old = self.directory / 'old-session'
        old.touch(mode=0o600)
        old.write_bytes(session.read_bytes())
        self.invoke(['auth', 'refresh'], credential=session)
        self.assertNotEqual(session.read_bytes(), old.read_bytes())
        self.invoke(['auth', 'refresh'], credential=old, code=3)
        self.invoke(['auth', 'logout', '--remote'], credential=session)
        self.assertFalse(session.exists())
        # Never rotate/revoke the runner's bootstrap credential shared by suites.

    def create(self, kind, name):
        namespace = kind != 'Namespace'
        metadata = dict(name=name)
        if namespace:
            metadata['namespace'] = 'gs-m1-ci'
        spec = {'Namespace': dict(title='CLI alpha.9 fixture', tier='ORGANIZATION'),
                'Repository': dict(defaultBranch='main', visibility='PRIVATE', storageClass='standard'),
                'Product': dict(title='Café ü'), 'CategoryTaxonomy': dict(title='Café ü')}[kind]
        document = self.directory / (name + '.md')
        document.write_text('---\n' + json.dumps(dict(
            apiVersion='gitstore.dev/v1beta1' if kind in ('Namespace', 'Repository') else 'catalog.gitstore.dev/v1beta1',
            kind=kind, metadata=metadata, spec=spec), ensure_ascii=False) + '\n---\n' +
            ('' if kind in ('Namespace', 'Repository') else 'Café\n\n'))
        return self.invoke(['create', '-f', str(document)])['items'][0]['resource']

    def test_scoped_create_full_projection_and_product_markdown_fidelity(self):
        for kind, command in [('Namespace', 'namespace'), ('Repository', 'repository'), ('Product', 'product'), ('CategoryTaxonomy', 'category')]:
            with self.subTest(kind=kind):
                name = 'alpha9-' + uuid.uuid4().hex[:12]
                created = self.create(kind, name)
                try:
                    resource = self.invoke(['read', command, '--id', created['id']])
                    self.assertEqual(resource['metadata']['name'], name)
                    self.assertIn('status', resource)
                    self.assertIn('spec', resource)
                    if kind in ('Product', 'CategoryTaxonomy'):
                        markdown = self.invoke(['read', command, name, '--authoring', '-o', 'markdown'], raw=True)
                        self.assertTrue(markdown.endswith('Café\n\n'))
                        self.assertNotIn('observedGeneration:', markdown)
                finally:
                    self.wait_resource_ready(command, created['id'])
                    self.invoke(['delete', command, '--id', created['id']])

    def watch(self, kind, name=None, credential=None):
        checkpoint = self.directory / ('checkpoint-' + kind + '.json')
        command = [str(Path(__file__).resolve().parents[1] / 'target/debug/gs'),
            '--config', str(self.config), '--credential-file', str(credential or self.credential),
            '--non-interactive', '--timeout', '20s', '-o', 'json', 'watch', kind,
            '--checkpoint-file', str(checkpoint), '--max-events', '1']
        if name:
            command.append(name)
        process = subprocess.Popen(command, env=self.env, cwd=self.directory,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop, process)
        return process, checkpoint

    def test_revoked_session_cannot_reconnect_and_durable_checkpoint_is_retained(self):
        session = self.session()
        revoked = self.directory / 'revoked-session'
        revoked.touch(mode=0o600)
        revoked.write_bytes(session.read_bytes())
        process, checkpoint = self.watch('namespace', credential=session)
        self.assertEqual(self.line(process)['type'], 'BOOKMARK')
        deadline = time.monotonic() + 3
        while not checkpoint.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(checkpoint.exists())
        before = checkpoint.read_bytes()
        self.invoke(['auth', 'logout', '--remote'], credential=session)
        # Alpha.9 authenticates at connection initialization. Record the observed
        # existing-connection outcome separately from reconnect rejection.
        time.sleep(0.2)
        print('OBSERVED: after static-user logout, existing watch ' +
              ('remained open at 200ms' if process.poll() is None else 'closed'), flush=True)
        self.stop(process)
        retry, _ = self.watch('namespace', credential=revoked)
        retry.wait(timeout=5)
        self.assertEqual(retry.returncode, 3, retry.stderr.read().decode())
        self.assertEqual(before, checkpoint.read_bytes())

    def stop(self, process):
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        process.stdout.close()
        process.stderr.close()

    def line(self, process):
        with selectors.DefaultSelector() as poll:
            poll.register(process.stdout, selectors.EVENT_READ)
            if not poll.select(15):
                self.fail('watch produced no bounded event/bookmark')
            line = process.stdout.readline()
        if not line:
            self.fail('watch closed: ' + process.stderr.read().decode())
        return json.loads(line)

    def test_all_five_typed_bootstraps_and_four_authoring_events(self):
        for kind, manifest in [('namespace', 'Namespace'), ('repository', 'Repository'),
                               ('product', 'Product'), ('category', 'CategoryTaxonomy'), ('file', None)]:
            with self.subTest(kind=kind):
                name = 'watch-' + uuid.uuid4().hex[:12]
                process, checkpoint = self.watch(kind, name if manifest else None)
                bookmark = self.line(process)
                self.assertEqual(bookmark['type'], 'BOOKMARK')
                self.assertTrue(bookmark['resourceVersion'])
                if manifest:
                    created = self.create(manifest, name)
                    try:
                        event = self.line(process)
                        self.assertEqual(event['type'], 'ADDED')
                        self.assertEqual(event['name'], name)
                        self.assertEqual(event['object']['id'], created['id'])
                        process.wait(timeout=5)
                        self.assertEqual(process.returncode, 0, process.stderr.read().decode())
                    finally:
                        self.wait_resource_ready(kind, created['id'])
                        self.invoke(['delete', kind, '--id', created['id']])
                # stdout is emitted before durable checkpoint replacement. Wait
                # for that boundary before deliberately interrupting the stream.
                deadline = time.monotonic() + 3
                while not checkpoint.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.stop(process)
                self.assertTrue(checkpoint.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
