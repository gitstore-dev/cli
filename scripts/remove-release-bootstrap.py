#!/usr/bin/env python3
"""Remove the alpha.1 override in its generated release PR before it merges."""
import argparse
import json
from pathlib import Path
import tomllib


def remove(root):
    root = Path(root)
    config_path = root / 'release-please-config.json'
    config = json.loads(config_path.read_text())
    package = config['packages']['.']
    version = (root / 'version.txt').read_text().strip()
    manifest = json.loads((root / '.release-please-manifest.json').read_text())
    cargo = tomllib.loads((root / 'Cargo.toml').read_text())
    if package.get('release-as') is None:
        return False
    if (package['release-as'] != '0.1.0-alpha.1' or version != '0.1.0-alpha.1' or
            manifest.get('.') != version or cargo['workspace']['package']['version'] != version):
        raise ValueError('remove bootstrap only from the synchronized alpha.1 release PR')
    del package['release-as']
    config_path.write_text(json.dumps(config, indent=2) + '\n')
    return True


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    print('Bootstrap override removed' if remove(args.root) else 'Already removed')
