#!/usr/bin/env python3
"""Read pinned public source/image metadata; never execute or change upstream code."""
import hashlib
import json
from pathlib import Path
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
REVISION = '06152d193e854bc643fa884daa8b9f30b0d75d0f'
ACCEPT = ', '.join(['application/vnd.oci.image.index.v1+json', 'application/vnd.docker.distribution.manifest.list.v2+json', 'application/vnd.oci.image.manifest.v1+json', 'application/vnd.docker.distribution.manifest.v2+json'])

class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != 'https':
            raise ValueError('Refusing non-HTTPS metadata redirect')
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if urllib.parse.urlsplit(req.full_url).netloc != urllib.parse.urlsplit(newurl).netloc:
            redirected.remove_header('Authorization')
        return redirected

OPENER = urllib.request.build_opener(SafeRedirect())

def fetch(url, headers=None):
    request = urllib.request.Request(url, headers=headers or {})
    with OPENER.open(request, timeout=20) as response:
        body = response.read(8 * 1024 * 1024 + 1)
        if len(body) > 8 * 1024 * 1024:
            raise ValueError('Metadata response exceeds bound')
        return body, response.headers

def source(path, needles):
    url = f'https://raw.githubusercontent.com/gitstore-dev/GitStore/{REVISION}/{path}'
    data, _ = fetch(url)
    text = data.decode('utf-8')
    matches = [{'line': n, 'text': line.strip()} for n, line in enumerate(text.splitlines(), 1)
               if any(needle in line for needle in needles)]
    return {'path': path, 'url': url, 'sha256': hashlib.sha256(data).hexdigest(), 'matches': matches}

def image(name):
    repo = 'gitstore-dev/' + name
    query = urllib.parse.urlencode({'service': 'ghcr.io', 'scope': 'repository:' + repo + ':pull'})
    data, _ = fetch('https://ghcr.io/token?' + query)
    token = json.loads(data)['token']
    headers = {'Authorization': 'Bearer ' + token, 'Accept': ACCEPT}
    attempts = []
    for tag in ['v0.1.0-alpha.8', '0.1.0-alpha.8']:
        try:
            data, response_headers = fetch(f'https://ghcr.io/v2/{repo}/manifests/{tag}', headers)
        except urllib.error.HTTPError as error:
            if error.code not in (404, 401, 403):
                raise
            attempts.append({'tag': tag, 'httpStatus': error.code})
            continue
        digest = 'sha256:' + hashlib.sha256(data).hexdigest()
        advertised = response_headers.get('Docker-Content-Digest')
        if advertised and advertised != digest:
            raise ValueError('Registry digest mismatch')
        manifest = json.loads(data)
        platforms = []
        for child in manifest.get('manifests', []):
            platform = child.get('platform', {})
            if platform.get('os') != 'linux' or platform.get('architecture') not in ('amd64', 'arm64'):
                continue
            child_data, _ = fetch(f'https://ghcr.io/v2/{repo}/manifests/{child["digest"]}', headers)
            if 'sha256:' + hashlib.sha256(child_data).hexdigest() != child['digest']:
                raise ValueError('Platform manifest digest mismatch')
            config_digest = json.loads(child_data)['config']['digest']
            config_data, _ = fetch(f'https://ghcr.io/v2/{repo}/blobs/{config_digest}', headers)
            if 'sha256:' + hashlib.sha256(config_data).hexdigest() != config_digest:
                raise ValueError('Image config digest mismatch')
            config = json.loads(config_data).get('config', {})
            labels = config.get('Labels') or {}
            platforms.append({'platform': platform, 'digest': child['digest'], 'configDigest': config_digest,
                              'sourceRevisionLabel': labels.get('org.opencontainers.image.revision'),
                              'entrypoint': config.get('Entrypoint'), 'cmd': config.get('Cmd')})
        return {'repository': 'ghcr.io/' + repo, 'tag': tag, 'digest': digest,
                'platforms': platforms, 'attempts': attempts, 'evidence': 'registry metadata only; not executed or attested'}
    return {'repository': 'ghcr.io/' + repo, 'attempts': attempts, 'status': 'release tag unavailable; no mutable-tag substitution'}

def main():
    evidence = {'schemaVersion': 1, 'recorded': '2026-11-03', 'serverRevision': REVISION,
                'evidenceLevel': 'committed source and public registry metadata; no runtime execution',
                'sources': [
                    source('gitstore-api/cmd/gitctl/main.go', ['case "', 'flags.', 'Migrate', 'migrateSchema', 'Usage:']),
                    source('gitstore-api/cmd/gitctl/enroll_serviceaccount.go', ['flags.', 'enroll', 'stdin', 'private-key-path', 'identity-output', 'replace-existing', 'generateSigning']),
                    source('compose.yml', ['image:', 'healthcheck:', 'depends_on:', 'test:']),
                    source('compose.local.yml', ['command:', 'image:', 'depends_on:', 'enroll-serviceaccount', 'generate-signing-key', 'serviceaccount-enrollment']),
                    source('docs/configuration.md', ['--config-file', 'auto_migrate', 'migrate --', 'keyspace first', 'signing_key', 'controller.serviceaccount.uid', 'observability', 'profiling.uri'])],
                'images': [image(name) for name in ['api', 'git-service', 'controller-manager']],
                'optionalMappings': {'oidc': 'not verified', 'admin': 'not verified', 'storefront': 'not verified'},
                'limits': ['Source handlers, not help text, determine callable mappings.', 'Image revision labels are claims, not build provenance.', 'Registry metadata is not Linux/runtime compatibility evidence.', 'Upstream images do not require attestations.']}
    path = ROOT / 'runtime/profiles/upstream-evidence.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2) + '\n')
    print('Recorded pinned upstream source and image metadata; no upstream writes or workload execution')

if __name__ == '__main__':
    main()
