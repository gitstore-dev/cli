"""Run CLI acceptance checks against isolated published GitStore :latest images."""
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / 'tests/integration'
IMAGES = [f'ghcr.io/gitstore-dev/{name}:latest' for name in
          ('api', 'git-service', 'controller-manager')]


def main():
    with tempfile.TemporaryDirectory(prefix='gs-images-') as directory:
        state = Path(directory)
        state.chmod(0o700)
        (state / 'config').mkdir()
        password = secrets.token_urlsafe(32)
        sensitive = [password]

        def protected(path, value):
            path.touch(mode=0o600)
            path.write_text(value)

        protected(state / 'admin-password', password)
        config = (FIXTURE / 'gitstore.toml').read_text()
        for marker in ('__CI_JWT_SECRET__', '__CI_HMAC_SECRET__'):
            secret = secrets.token_hex(32)
            sensitive.append(secret)
            config = config.replace(marker, secret)
        protected(state / 'config/gitstore.toml', config)
        shutil.copyfile(FIXTURE / 'policy.yaml', state / 'config/policy.yaml')
        env = dict(os.environ, GITSTORE_CI_STATE=str(state))
        compose = ['docker', 'compose', '--ansi', 'never', '--project-name',
                   'gs-cli-ci-' + uuid.uuid4().hex[:12], '--file', str(FIXTURE / 'compose.yml')]

        def run(args, **kwargs):
            return subprocess.run(args, cwd=ROOT, env=env, check=True, timeout=300, **kwargs)

        def dc(*args, **kwargs):
            return run([*compose, *args], **kwargs)

        def redact(value):
            for secret in sensitive:
                value = value.replace(secret, '[REDACTED]')
            return value

        failed = False
        try:
            dc('config', '--quiet')
            dc('pull', '--policy', 'always')
            for image in IMAGES:
                # Record moving-tag provenance, without dumping container environment.
                run(['docker', 'image', 'inspect', '--format',
                     '{{json .RepoDigests}} revision={{index .Config.Labels "org.opencontainers.image.revision"}}', image])
            dc('up', '--detach', '--wait', '--wait-timeout', '180')

            def endpoint(port):
                address = dc('port', 'api', str(port), capture_output=True, text=True).stdout.strip()
                return 'http://' + address

            api, git = endpoint(4000) + '/graphql', endpoint(9000)
            token = None

            def graphql(query, variables):
                headers = {'Content-Type': 'application/json'}
                if token:
                    headers['Authorization'] = 'Bearer ' + token
                request = urllib.request.Request(api, json.dumps({'query': query, 'variables': variables}).encode(), headers)
                with urllib.request.urlopen(request, timeout=15) as response:
                    reply = json.load(response)
                if reply.get('errors'):
                    raise RuntimeError('GraphQL bootstrap: ' + redact(json.dumps(reply['errors'])))
                return reply['data']

            token = graphql('mutation($username:String!,$password:String!){login(input:{username:$username,password:$password}){token{accessToken}}}',
                            {'username': 'admin', 'password': password})['login']['token']['accessToken']
            sensitive.append(token)
            protected(state / 'token', token)
            namespace = 'gs-m1-ci'
            graphql('mutation($input:CreateNamespaceInput!){createNamespace(input:$input){namespace{id}}}',
                    {'input': {'apiVersion': 'gitstore.dev/v1beta1', 'kind': 'Namespace',
                               'metadata': {'name': namespace}, 'spec': {'title': 'CLI CI', 'tier': 'ORGANIZATION'}}})
            # This particular rejection occurs before admission; all other mutation
            # errors fail immediately, rather than risking duplicate submissions.
            deadline = time.monotonic() + 120
            while True:
                try:
                    graphql('mutation($input:CreateRepositoryInput!){createRepository(input:$input){repository{id}}}',
                            {'input': {'apiVersion': 'gitstore.dev/v1beta1', 'kind': 'Repository',
                                       'metadata': {'namespace': namespace, 'name': 'catalog'},
                                       'spec': {'defaultBranch': 'main', 'visibility': 'PRIVATE', 'storageClass': 'standard'}}})
                    break
                except RuntimeError as error:
                    if 'namespace system repository is unavailable' not in str(error) or time.monotonic() > deadline:
                        raise
                    time.sleep(1)
            deadline = time.monotonic() + 120
            while True:
                request = urllib.request.Request(git + '/gs-m1-ci/catalog.git/info/refs?service=git-upload-pack',
                                                 headers={'Authorization': 'Bearer ' + token})
                try:
                    with urllib.request.urlopen(request, timeout=10) as response:
                        if response.status == 200:
                            break
                except urllib.error.HTTPError as error:
                    if error.code not in (404, 409, 503):
                        raise
                if time.monotonic() > deadline:
                    raise RuntimeError('Repository storage did not become available')
                time.sleep(1)
            protected(state / 'git-credential.json', json.dumps({'username': 'admin', 'password': password}))
            protected(state / 'cli.toml', f'''version = 1
current_context = "integration"
[contexts.integration]
endpoint = "{api}"
allow_loopback_http = true
contract_profile = "m0-2026-10-27"
account = "admin"
namespace = "gs-m1-ci"
git_endpoint = "{git}"
[contexts.integration.accounts.admin]
issuer = "gitstore"
[contexts.integration.git_auth]
mechanism = "basic"
account = "admin"
issuer = "gitstore"
credential_file = {json.dumps(str(state / 'git-credential.json'))}
''')
            env.update(GITSTORE_IMAGE_CONFIG=str(state / 'cli.toml'),
                       GITSTORE_IMAGE_CREDENTIAL=str(state / 'token'),
                       GITSTORE_IMAGE_GIT_URL=git + '/gs-m1-ci/catalog.git')
            run(['cargo', 'test', '--locked', '-p', 'gs', '--test', 'images', '--', '--ignored', '--nocapture'])
            run([sys.executable, 'scripts/test_live.py', '--allow-live', '--config', str(state / 'cli.toml'),
                 '--credential-file', str(state / 'token'), '--namespace', namespace, '--repository', 'catalog'])
            print('PASS: CLI compatibility against published GitStore :latest images.', flush=True)
        except BaseException:
            failed = True
            # Capture before printing so fixture credentials can be redacted.
            for args in [('ps', '--all'), ('logs', '--no-color', '--tail', '100')]:
                try:
                    print(redact(dc(*args, capture_output=True, text=True).stdout), file=sys.stderr)
                except subprocess.SubprocessError:
                    print('Could not collect Compose diagnostics', file=sys.stderr)
            raise
        finally:
            try:
                dc('down', '--volumes', '--remove-orphans', '--timeout', '10')
            except subprocess.SubprocessError:
                if not failed:
                    raise
                print('Compose cleanup failed; inspect the project named above.', file=sys.stderr)


if __name__ == '__main__':
    main()
