#!/usr/bin/env python3
"""Render local field type evidence from immutable upstream source (read-only)."""
import argparse
import json
from pathlib import Path
import re
import subprocess

REVISION = '68f8471edc947e417c98b6e8299a1367ab6b20da'
FILES = ['schema','namespace','repository','product','category','collection','file']


def generate(upstream):
    types = {}
    inputs = {}
    for name in FILES:
        source = subprocess.check_output(['git','-C',str(upstream),'show',
            f'{REVISION}:shared/schemas/admin/{name}.graphqls'], text=True)
        source = re.sub(r'""".*?"""', '', source, flags=re.S)
        source = re.sub(r'#[^\n]*', '', source)
        for category, type_name, body in re.findall(r'\b(type|input)\s+(\w+)[^{]*\{([^}]+)\}', source):
            if type_name in ['Query','Mutation','Subscription']:
                continue
            fields = dict(re.findall(r'(?:^|\n)\s*(\w+)\s*:\s*([A-Za-z0-9_!\[\]]+)', body))
            (inputs if category == 'input' else types).setdefault(type_name, {}).update(fields)
    # Input types are retained only through the six selected resource envelopes.
    selected = {}
    def collect(name):
        if name in selected or name not in inputs:
            return
        selected[name] = inputs[name]
        for value in inputs[name].values():
            collect(value.strip('[]!'))
    for kind in ['Namespace','Repository','Product','Category']:
        collect('Create'+kind+'Input')
    return {'sourceRevision':REVISION, 'types':types, 'inputs':selected}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(generate(args.upstream), sort_keys=True, separators=(',',':')))
