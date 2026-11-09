#!/usr/bin/env python3
"""Read-only check of the already-required release publication approval gate."""
import json
import subprocess


def validate(value):
    if value.get('name') != 'release-publication':
        raise ValueError('select the release-publication environment')
    for rule in value.get('protection_rules',[]):
        if rule.get('type') != 'required_reviewers':
            continue
        reviewers = rule.get('reviewers',[])
        if reviewers and all(item.get('type') in ('User','Team') and
                type(item.get('reviewer',{}).get('id')) is int and item['reviewer']['id'] > 0
                for item in reviewers):
            return
    raise ValueError('configure required reviewers on release-publication before promotion')


def main():
    result = subprocess.run(['gh','api','repos/gitstore-dev/cli/environments/release-publication'],
        capture_output=True,text=True,timeout=30)
    if result.returncode:
        raise SystemExit('Cannot read release-publication; configure its required reviewers before promotion')
    validate(json.loads(result.stdout))
    print('Publication environment has a configured required-reviewer gate')


if __name__=='__main__':
    main()
