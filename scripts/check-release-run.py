#!/usr/bin/env python3
"""Reject foreign, PR, failed or wrong-source artifacts before release download."""
import argparse
import json
import re
import subprocess


def validate(run, workflow, revision, tag):
    if (not re.fullmatch(r'0\.1\.0-alpha\.[1-9][0-9]*', tag.removeprefix('v')) or not tag.startswith('v')
            or not re.fullmatch('[0-9a-f]{40}',revision)
            or run['repository']['id'] != 1431005655 or run['head_repository']['id'] != 1431005655
            or run['head_sha'] != revision or run['path'] != '.github/workflows/' + workflow
            or run['status'] != 'completed' or run['conclusion'] != 'success'
            or run['event'] not in ('push', 'workflow_dispatch')
            or workflow == 'release.yml' and run['head_branch'] != tag
            or workflow == 'runtime.yml' and (run['event'] != 'workflow_dispatch' or run['head_branch'] not in ('main', tag))):
        raise ValueError('run is not an approved successful same-repository/source release input')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-id', type=int, required=True)
    parser.add_argument('--workflow', choices=['release.yml', 'runtime.yml'], required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--tag', required=True)
    args = parser.parse_args()
    if args.run_id <= 0:
        parser.error('select an explicit positive run ID')
    reply = subprocess.check_output(['gh', 'api', f'repos/gitstore-dev/cli/actions/runs/{args.run_id}'])
    validate(json.loads(reply), args.workflow, args.revision, args.tag)
    print('Approved exact source/workflow input run:', args.run_id)


if __name__ == '__main__':
    main()
