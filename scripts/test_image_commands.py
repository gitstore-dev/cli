"""Individually reported gs command checks for the disposable image installation."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parent.parent


class Commands(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gs-command-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.config = self.directory / 'config.toml'
        self.config.write_text(Path(os.environ['GITSTORE_IMAGE_CONFIG']).read_text())
        self.credential = Path(os.environ['GITSTORE_IMAGE_CREDENTIAL'])
        self.env = {'HOME': self.temp.name, 'PATH': '/nonexistent'}

    def invoke(self, args, code=0, credential=None, raw=False):
        command = [str(ROOT / 'target/debug/gs'), '--config', str(self.config),
                   '--credential-file', str(credential or self.credential), '--non-interactive']
        if not raw:
            command.extend(['-o', 'json'])
        result = subprocess.run([*command, *args], env=self.env, cwd=self.directory,
                                capture_output=True, text=True, timeout=30)
        if not raw:
            secret = self.credential.read_text().strip()
            self.assertTrue(secret not in result.stdout + result.stderr, 'Credential leaked in CLI output')
        self.assertEqual(result.returncode, code, f'{args[0]}: {result.stderr}')
        if raw:
            return result.stdout
        return json.loads(result.stdout) if result.stdout else None

    def graphql(self, query, variables=None):
        envelope = self.directory / 'graphql.json'
        envelope.write_text(json.dumps({'query': query, 'variables': variables or {}}))
        response = self.invoke(['api', 'graphql', '--input', str(envelope)])
        self.assertNotIn('errors', response)
        return response['data']


class LocalCommands(Commands):
    def test_context_list(self):
        contexts = self.invoke(['context', 'list'])
        self.assertTrue(any(item['name'] == 'integration' and item['selected'] for item in contexts))

    def test_context_show(self):
        context = self.invoke(['context', 'show'])
        self.assertEqual(context['namespace'], 'gs-m1-ci')
        self.assertEqual(context['account'], 'admin')

    def test_context_use(self):
        self.invoke(['context', 'use', 'integration'])
        self.assertEqual(self.invoke(['context', 'show'])['context'], 'integration')

    def test_config_validate(self):
        self.assertTrue(self.invoke(['config', 'validate'])['valid'])

    def test_auth_status_with_protected_file(self):
        status = self.invoke(['auth', 'status'])
        self.assertTrue(status['present'])
        self.assertFalse(status['verified'])

    def test_auth_token_explicit_raw_output(self):
        output = self.invoke(['auth', 'token'], raw=True)
        self.assertTrue(output.strip() == self.credential.read_text().strip(), 'Explicit token output differs')

    def test_auth_switch(self):
        with self.config.open('a') as config:
            config.write('\n[contexts.integration.accounts.secondary]\nissuer = "gitstore"\n')
        self.invoke(['auth', 'switch', 'secondary'])
        self.assertEqual(self.invoke(['context', 'show'])['account'], 'secondary')


class GraphqlCommands(Commands):
    def test_explicit_query_with_typed_variable(self):
        data = self.invoke(['api', 'graphql', '-f',
                            'query=query($name:String!){namespace(by:{name:$name}){metadata{name}}}',
                            '-F', 'name=gs-m1-ci'])['data']
        self.assertEqual(data['namespace']['metadata']['name'], 'gs-m1-ci')

    def test_invalid_credential_is_rejected(self):
        invalid = self.directory / 'invalid-token'
        invalid.touch(mode=0o600)
        invalid.write_text('synthetic-invalid-token')
        self.invoke(['api', 'graphql', '-f', 'query={namespaces(first:1){pageInfo{hasNextPage}}}'],
                    code=3, credential=invalid)


class InspectionCommands(Commands):
    def test_read_namespace(self):
        self.assertEqual(self.invoke(['read', 'namespace', 'gs-m1-ci'])['metadata']['name'], 'gs-m1-ci')

    def test_read_repository(self):
        self.assertEqual(self.invoke(['read', 'repository', 'catalog'])['spec']['defaultBranch'], 'main')

    def test_read_collection_missing_preserves_graphql_errors(self):
        # Current published API returns operation errors without a NOT_FOUND code.
        result = self.invoke(['read', 'collection', 'gs-m1-absent-' + uuid.uuid4().hex[:12]], code=8)
        self.assertTrue(result['errors'])

    def test_read_file_missing_is_not_found(self):
        self.invoke(['read', 'file', 'gs-m1-absent-' + uuid.uuid4().hex[:12]], code=4)

    def check_list(self, kind):
        page = self.invoke(['list', kind, '--first', '1'])
        self.assertIsInstance(page['items'], list)
        self.assertLessEqual(len(page['items']), 1)
        self.assertIsInstance(page['pageInfo']['hasNextPage'], bool)

    def test_list_namespace(self):
        self.check_list('namespace')

    def test_list_repository(self):
        self.check_list('repository')

    def test_list_product(self):
        self.check_list('product')

    def test_list_category(self):
        self.check_list('category')

    def test_list_collection(self):
        self.check_list('collection')


class AuthoringCommands(Commands):
    kind = None
    manifest_kind = None

    def setUp(self):
        super().setUp()
        self.name = 'gs-m1-' + uuid.uuid4().hex[:12]
        self.identity = None
        self.document = self.directory / 'resource.md'
        self.document.write_text('---\n' + json.dumps({
            'apiVersion': 'catalog.gitstore.dev/v1beta1', 'kind': self.manifest_kind,
            'metadata': {'name': self.name, 'namespace': 'gs-m1-ci'},
            'spec': {'title': 'Disposable command acceptance fixture'}}, ensure_ascii=False)
            + '\n---\nCafé\n\n', encoding='utf-8')
        self.addCleanup(self.remove_fixture)

    def create_fixture(self):
        result = self.invoke(['create', '-f', str(self.document)])
        item = result['items'][0]
        self.identity = item['resource']['id']
        self.assertEqual(item['submission'], 'accepted')
        return item['resource']

    def remove_fixture(self):
        if self.identity:
            self.invoke(['delete', self.kind, '--id', self.identity])

    def test_create(self):
        resource = self.create_fixture()
        self.assertEqual(resource['kind'], self.manifest_kind)
        self.assertEqual(resource['metadata']['name'], self.name)
        self.assertEqual(resource['body'], 'Café\n\n')

    def test_read_by_name_preserves_body(self):
        self.create_fixture()
        resource = self.invoke(['read', self.kind, self.name])
        self.assertEqual(resource['id'], self.identity)
        self.assertEqual(resource['body'], 'Café\n\n')

    def test_read_by_id(self):
        self.create_fixture()
        self.assertEqual(self.invoke(['read', self.kind, '--id', self.identity])['metadata']['name'], self.name)

    def test_delete_by_id(self):
        self.create_fixture()
        result = self.invoke(['delete', self.kind, '--id', self.identity])
        self.assertIn(result['outcome'], ['TERMINATION_STARTED', 'ALREADY_TERMINATING'])
        self.identity = None

    def test_delete_by_name(self):
        self.create_fixture()
        result = self.invoke(['delete', self.kind, self.name])
        self.assertIn(result['outcome'], ['TERMINATION_STARTED', 'ALREADY_TERMINATING'])
        self.identity = None


class ProductCommands(AuthoringCommands):
    kind = 'product'
    manifest_kind = 'Product'


class CategoryCommands(AuthoringCommands):
    kind = 'category'
    manifest_kind = 'CategoryTaxonomy'


class LifecycleCommands(Commands):
    def test_delete_namespace_by_id(self):
        name = 'gs-m1-' + uuid.uuid4().hex[:12]
        resource = self.graphql('mutation($input:CreateNamespaceInput!){createNamespace(input:$input){namespace{id}}}',
                                {'input': {'apiVersion': 'gitstore.dev/v1beta1', 'kind': 'Namespace',
                                           'metadata': {'name': name}, 'spec': {'title': 'Disposable', 'tier': 'ORGANIZATION'}}})
        identity = resource['createNamespace']['namespace']['id']
        result = self.invoke(['delete', 'namespace', '--id', identity])
        self.assertIn(result['outcome'], ['TERMINATION_STARTED', 'ALREADY_TERMINATING'])

    def test_delete_repository_by_id(self):
        name = 'gs-m1-' + uuid.uuid4().hex[:12]
        resource = self.graphql('mutation($input:CreateRepositoryInput!){createRepository(input:$input){repository{id}}}',
                                {'input': {'apiVersion': 'gitstore.dev/v1beta1', 'kind': 'Repository',
                                           'metadata': {'namespace': 'gs-m1-ci', 'name': name},
                                           'spec': {'defaultBranch': 'main', 'visibility': 'PRIVATE', 'storageClass': 'standard'}}})
        identity = resource['createRepository']['repository']['id']
        result = self.invoke(['delete', 'repository', '--id', identity])
        self.assertIn(result['outcome'], ['TERMINATION_STARTED', 'ALREADY_TERMINATING'])


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(loader.loadTestsFromTestCase(cls) for cls in (
        LocalCommands, GraphqlCommands, InspectionCommands, ProductCommands, CategoryCommands, LifecycleCommands))


if __name__ == '__main__':
    unittest.main(verbosity=2)
