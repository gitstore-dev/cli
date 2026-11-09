#!/usr/bin/env python3
"""Bounded safe metadata from an explicitly owned failed native test fixture.

Never emits intent, result bodies, raw error messages, descriptors, service logs,
configuration, environment, credentials, database files or workload data.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import sqlite3
import stat


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fixture',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    args=p.parse_args()
    info=args.fixture.lstat()
    if os.geteuid()==0 or info.st_uid!=os.geteuid() or not stat.S_ISREG(info.st_mode) or info.st_mode&0o077:
        p.error('select only a dedicated owned private native fixture')
    settings=json.loads(args.fixture.read_text())
    if settings['dedicatedTestAccount'] is not True or settings['disposableUid']!=os.geteuid():
        p.error('fixture must select this dedicated account')
    parent=Path(settings['fixtureParent'])
    if parent.is_symlink() or parent.stat().st_uid!=os.geteuid() or parent.stat().st_mode&0o077:
        p.error('select an owned private parent')
    record=dict(scope='unsigned native fixture diagnostic enums/counts only; never release qualification',
        host=platform.platform(),uid=os.geteuid(),sigchld=('default' if signal.getsignal(signal.SIGCHLD)==signal.SIG_DFL else
            'ignored' if signal.getsignal(signal.SIGCHLD)==signal.SIG_IGN else 'handler'),
        limits={name:resource.getrlimit(kind) for name,kind in [('as',resource.RLIMIT_AS),('fsize',resource.RLIMIT_FSIZE),('core',resource.RLIMIT_CORE),('nofile',resource.RLIMIT_NOFILE)]},fixtures=[])
    def atom(value):
        return value if isinstance(value,str) and re.fullmatch('[A-Za-z0-9_.-]{1,80}',value) else 'omitted'
    roots=list(parent.glob('gs-public-runtime-*'))
    if len(roots)>32:
        p.error('select a bounded dedicated batch')
    for root in roots:
        path=root/'state/state.db'
        if root.is_symlink() or root.stat().st_uid!=os.geteuid() or not path.is_file() or path.is_symlink():
            continue
        with sqlite3.connect(path.as_uri()+'?mode=ro',uri=True) as db:
            operations=[]
            for state,error in db.execute('SELECT state,error FROM operations LIMIT 32'):
                value=json.loads(error) if error else {}
                operations.append(dict(state=atom(state),code=atom(value.get('code')),stage=atom(value.get('stage'))))
            assets=[dict(state=atom(state),count=count) for state,count in db.execute('SELECT state,count(*) FROM assets GROUP BY state')]
            owned=[dict(type=atom(kind),stage=atom(stage),ownership=atom(ownership),count=count) for kind,stage,ownership,count in db.execute(
                'SELECT resource_type,lifecycle_stage,ownership_state,count(*) FROM resource_ownership GROUP BY resource_type,lifecycle_stage,ownership_state')]
            milestones={key:0 for key in ('execution','population','native')}
            for (identity,) in db.execute("SELECT backend_identity FROM resource_ownership WHERE resource_type='workload' LIMIT 32"):
                if not isinstance(identity,str) or len(identity)>65536:
                    continue
                value=json.loads(identity)
                if isinstance(value,dict):
                    for key in milestones:
                        milestones[key]+=isinstance(value.get(key),dict)
        record['fixtures'].append(dict(name=root.name,operations=operations,assets=assets,resources=owned,
            workloadMilestoneCounts=milestones))
    fd=os.open(args.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,'w') as stream:
        json.dump(record,stream,indent=2);stream.write('\n')
    print(json.dumps(record))


if __name__=='__main__':
    main()
