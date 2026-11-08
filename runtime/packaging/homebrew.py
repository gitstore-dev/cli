"""Render an explicit Git URL tap formula from the complete final archive set."""
import argparse
import json
from pathlib import Path
import re

from formats import write_new
from versions import encode

ROOT = Path(__file__).resolve().parents[2]
TUPLES = {('linux', 'x64'), ('linux', 'arm64'), ('darwin', 'x64'), ('darwin', 'arm64')}


def render(version, archives):
    tag = encode(version)['tag']
    rows = {(row['os'], row['arch']): row for row in archives}
    if len(archives) != 4 or set(rows) != TUPLES:
        raise ValueError('formula requires the complete four-platform archive set')
    substitutions = {'VERSION': version}
    for (os_name, arch), row in rows.items():
        name = f'gitstore-{version}-{os_name}-{arch}.tar.gz'
        expected_url = f'https://github.com/gitstore-dev/cli/releases/download/{tag}/{name}'
        if row['url'] != expected_url or not re.fullmatch('[0-9a-f]{64}', row['sha256']):
            raise ValueError('formula must select exact versioned CLI release assets')
        key = os_name.upper() + '_' + arch.upper()
        substitutions[key + '_URL'] = expected_url
        substitutions[key + '_SHA256'] = row['sha256']
    text = (ROOT / 'packaging/homebrew/gitstore.rb.in').read_text()
    for key, value in substitutions.items():
        text = text.replace('@' + key + '@', value)
    if re.search('@[A-Z0-9_]+@', text):
        raise ValueError('unexpanded formula placeholder')
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--archives', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    write_new(args.output, render(args.version, json.loads(args.archives.read_text())).encode())


if __name__ == '__main__':
    main()
