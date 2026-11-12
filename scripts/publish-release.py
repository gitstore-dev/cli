#!/usr/bin/env python3
"""Explicit maintainer operation: verify, resume a draft, promote, then update tap.

Requires a previously created reviewed tag/draft. Never creates tags or releases,
never replaces assets. Workflow concurrency must serialize this tag remotely.
"""
import argparse
import fcntl
import json
import os
import stat
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/packaging'))
from publication import publish
from homebrew_delivery import propose as propose_tap
from release_candidate import advance, safe_name
from release_verification import REPOSITORY


class GitHub:
    def __init__(self, tag):
        self.tag = tag

    def gh(self, *arguments, payload=None):
        reply = subprocess.run(['gh', *arguments], input=payload, capture_output=True, timeout=120)
        if reply.returncode:
            raise RuntimeError('GitHub release operation failed; draft/published state must be inspected before retry')
        return reply.stdout

    def api(self, endpoint, payload=None):
        arguments = ['api', f'repos/{REPOSITORY}/{endpoint}']
        if payload is not None:
            arguments += ['--method', 'PATCH', '--input', '-']
        return json.loads(self.gh(*arguments, payload=json.dumps(payload).encode() if payload is not None else None))

    def inspect(self):
        # GET releases/tags/{tag} only returns published releases. Resolve the
        # existing draft through the authenticated list, then pin its ID across
        # inspections; publication must never create or silently switch drafts.
        if not hasattr(self, 'release_id'):
            matches = []
            for page in range(1, 11):
                releases = self.api(f'releases?per_page=100&page={page}')
                if not isinstance(releases, list) or len(releases) > 100:
                    raise ValueError('invalid release listing')
                matches.extend(r for r in releases if r.get('tag_name') == self.tag)
                if len(releases) < 100:
                    break
            else:
                raise ValueError('release listing exceeds the bounded lookup; inspect before retry')
            if len(matches) != 1 or type(matches[0].get('id')) is not int or matches[0]['id'] <= 0:
                raise ValueError('select exactly one existing release for the reviewed tag')
            self.release_id = matches[0]['id']
        release = self.api(f'releases/{self.release_id}')
        revision = self.api('commits/' + self.tag)['sha']
        assets = {}
        for entry in release['assets']:
            name = safe_name(entry['name'])
            if name in assets or not 0 < entry['size'] <= 1024 * 1024 * 1024:
                raise ValueError('duplicate or oversized remote asset')
            assets[name] = self.gh('api', f'repos/{REPOSITORY}/releases/assets/{entry["id"]}',
                                   '--header', 'Accept: application/octet-stream')
            if len(assets[name]) != entry['size']:
                raise ValueError('remote asset length changed during inspection')
        return dict(tag=release['tag_name'], revision=revision, draft=release['draft'],
                    prerelease=release['prerelease'], assets=assets, body=release.get('body') or '')

    def ensure_notice(self, notice):
        release = self.api(f'releases/{self.release_id}')
        if not release['draft'] or release['tag_name'] != self.tag:
            raise ValueError('release changed before preparing its prerelease notice')
        body = release.get('body') or ''
        if not body.startswith(notice + '\n\n'):
            self.api(f'releases/{self.release_id}', dict(body=notice + '\n\n' + body))

    def upload(self, name, payload):
        with tempfile.TemporaryDirectory(prefix='gitstore-release-upload-') as directory:
            path = Path(directory) / safe_name(name)
            path.write_bytes(payload)
            self.gh('release', 'upload', self.tag, str(path), '--repo', REPOSITORY)

    def promote(self):
        self.api(f'releases/{self.release_id}', dict(draft=False, prerelease=True, make_latest='false'))

    def update_tap(self, tag, payload):
        releases = self.api('releases?per_page=100')
        current = next((r for r in releases if not r['draft'] and r['prerelease']), None)
        if current is None or current['tag_name'] != tag:
            raise ValueError('refusing to move the tap to an older prerelease')
        return propose_tap(self.tap_api, REPOSITORY, tag, payload)

    def tap_api(self, endpoint, *, method='GET', payload=None, missing=False):
        token = os.environ.get('GITSTORE_TAP_TOKEN')
        if not token:
            raise ValueError('select the repository-scoped App token for tap proposals')
        arguments = ['gh', 'api', f'repos/{REPOSITORY}/{endpoint}', '--method', method]
        if payload is not None:
            arguments += ['--input', '-']
        result = subprocess.run(arguments, input=json.dumps(payload).encode() if payload is not None else None,
            capture_output=True, timeout=120, env={**os.environ, 'GH_TOKEN': token, 'GH_HOST': 'github.com', 'GH_PROMPT_DISABLED': '1'})
        if result.returncode:
            if missing and b'(HTTP 404)' in result.stderr:
                return None
            raise RuntimeError('tap proposal failed; release remains published, inspect before retry')
        return json.loads(result.stdout)



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--asset-dir', required=True, type=Path)
    parser.add_argument('--trusted-root', required=True, type=Path)
    parser.add_argument('--update-tap', action='store_true')
    args = parser.parse_args()
    if args.update_tap and not os.environ.get('GITSTORE_TAP_TOKEN'):
        parser.error('tap proposals require the repository-scoped App token')
    if not args.asset_dir.is_absolute():
        parser.error('select an absolute private asset directory')
    info = args.asset_dir.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError('asset directory must be an owned private ordinary directory')
    value = json.loads(args.candidate.read_text())
    if value['state'] not in ('attested', 'producer-verified'):
        raise ValueError('select an attested ledger; producer verification runs again before publication')
    # Never trust a hand-edited producer-verified state or an earlier verifier
    # invocation. Recheck all current bytes, host receipts and signed identities.
    value['state'] = 'attested'
    value = advance(value, 'producer-verified', args.asset_dir, trusted_root=args.trusted_root)
    lock_path = args.asset_dir / (value['tag'] + '.publication.lock')
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_nlink != 1 or info.st_mode & 0o077:
            raise ValueError('unsafe publication lock')
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        print(json.dumps(publish(value, args.asset_dir, GitHub(value['tag']), update_tap=args.update_tap)))
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
