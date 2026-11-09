#!/usr/bin/env python3
"""Verify pinned libslirp source despite Gitiles' changing archive timestamps."""
import argparse
import copy
import hashlib
import gzip
import io
import json
import posixpath
from pathlib import Path, PurePosixPath
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

MAX_ARCHIVE = 1024 * 1024
MAX_SOURCE = 4 * 1024 * 1024


def download(url):
    if not url.startswith('https://'):
        raise ValueError('Source download requires HTTPS')
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=30) as source:
                if not source.url.startswith('https://'):
                    raise ValueError('Source redirect left HTTPS')
                return source.read(MAX_ARCHIVE + 1)
        except urllib.error.HTTPError as error:
            retryable = error.code in (429, 500, 502, 503, 504)
            error.close()
            if not retryable or attempt == 2:
                raise
            time.sleep(2**attempt)


def normalized_archive(raw):
    """Preserve all source bytes/identities; normalize only archive timestamps."""
    with gzip.GzipFile(fileobj=io.BytesIO(raw)) as compressed:
        unpacked = compressed.read(MAX_SOURCE + 1)
    if len(unpacked) > MAX_SOURCE:
        raise ValueError('Uncompressed archive exceeds bounds')
    output = io.BytesIO()
    with tarfile.open(fileobj=io.BytesIO(unpacked), mode='r:') as source:
        members = source.getmembers()
        if len(members) > 1024 or sum(m.size for m in members) > MAX_SOURCE:
            raise ValueError('Source archive exceeds bounds')
        seen = set()
        with tarfile.open(fileobj=output, mode='w', format=tarfile.PAX_FORMAT) as target:
            for member in sorted(members, key=lambda m: m.name):
                name = PurePosixPath(member.name)
                if (name.is_absolute() or '..' in name.parts or member.name in seen
                        or not (member.isfile() or member.isdir() or member.issym())
                        or set(member.pax_headers) - {'mtime'}):
                    raise ValueError('Unexpected source archive entry')
                seen.add(member.name)
                if member.issym():
                    link = PurePosixPath(member.linkname)
                    resolved = posixpath.normpath(str(name.parent / link))
                    if link.is_absolute() or resolved == '..' or resolved.startswith('../'):
                        raise ValueError('Unexpected source link')
                stable = copy.copy(member)
                stable.mtime = 0
                stable.pax_headers = {}
                target.addfile(stable, source.extractfile(member) if member.isfile() else None)
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    lock = json.loads(Path(__file__).with_name('source-lock.json').read_text())
    if args.input:
        with args.input.open('rb') as source:
            raw = source.read(MAX_ARCHIVE + 1)
    else:
        raw = download(lock['url'])
    if len(raw) > MAX_ARCHIVE:
        raise SystemExit('Source download exceeds bounds')
    normalized = normalized_archive(raw)
    if hashlib.sha256(normalized).hexdigest() != lock['normalizedTarSha256']:
        raise SystemExit('Refusing changed libslirp source')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=args.output.parent, delete=False) as output:
        temporary = Path(output.name)
        try:
            output.write(normalized)
            output.close()
            temporary.replace(args.output)
        finally:
            temporary.unlink(missing_ok=True)
    print('Verified exact pinned libslirp source; archive timestamps normalized')


if __name__ == '__main__':
    main()
