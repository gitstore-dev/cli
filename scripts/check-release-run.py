#!/usr/bin/env python3
"""Reject foreign, PR, failed or wrong-source artifacts before release download."""
import argparse
import json
import os
import re
import subprocess


def validate(run, workflow, revision, tag, *, current=None):
    same_run = current is not None
    if same_run and (
            workflow != 'release.yml'
            or current.get('GITHUB_ACTIONS') != 'true'
            or current.get('GITHUB_REPOSITORY_ID') != '1431005655'
            or str(run.get('id')) != current.get('GITHUB_RUN_ID')
            or current.get('GITHUB_SHA') != revision
            or current.get('GITHUB_REF') != 'refs/tags/' + tag
            or current.get('GITHUB_WORKFLOW_REF') != 'gitstore-dev/cli/.github/workflows/release.yml@refs/tags/' + tag):
        raise ValueError('current artifacts require the same trusted tagged release pipeline')
    if (not re.fullmatch(r'0\.1\.0-alpha\.[1-9][0-9]*', tag.removeprefix('v')) or not tag.startswith('v')
            or not re.fullmatch('[0-9a-f]{40}',revision)
            or run['repository']['id'] != 1431005655 or run['head_repository']['id'] != 1431005655
            or run['head_sha'] != revision or run['path'] != '.github/workflows/' + workflow
            or (same_run and (run['status'] != 'in_progress' or run['conclusion'] is not None))
            or (not same_run and (run['status'] != 'completed' or run['conclusion'] != 'success'))
            or run['event'] not in ('push', 'workflow_dispatch')
            or workflow == 'release.yml' and run['head_branch'] != tag
            or workflow == 'runtime.yml' and (run['event'] != 'workflow_dispatch' or run['head_branch'] not in ('main', tag))):
        raise ValueError('run is not an approved successful same-repository/source release input')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--run-id', type=int)
    selection.add_argument('--current', action='store_true',
                           help='Current tagged release pipeline only; its jobs enforce needs dependencies')
    parser.add_argument('--workflow', choices=['release.yml', 'runtime.yml'], required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--tag', required=True)
    args = parser.parse_args()
    run_id = int(os.environ.get('GITHUB_RUN_ID', '0')) if args.current else args.run_id
    if run_id <= 0:
        parser.error('select an explicit positive run ID')
    reply = subprocess.check_output(['gh', 'api', f'repos/gitstore-dev/cli/actions/runs/{run_id}'])
    validate(json.loads(reply), args.workflow, args.revision, args.tag,
             current=os.environ if args.current else None)
    print('Approved exact source/workflow input run:', run_id)


if __name__ == '__main__':
    main()
