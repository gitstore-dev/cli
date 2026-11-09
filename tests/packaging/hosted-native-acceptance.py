#!/usr/bin/env python3
"""Opt-in disposable Ubuntu native acceptance; never a release qualification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'runtime/packaging'))
from command_measurements import validate_performance


def private_json(path,maximum=65536):
    info=path.lstat()
    if not path.is_absolute() or not stat.S_ISREG(info.st_mode) or info.st_uid!=os.geteuid() or info.st_mode&0o077 or info.st_nlink!=1 or not 0<info.st_size<=maximum:
        raise ValueError('Select an owned bounded private ordinary JSON file')
    return json.loads(path.read_bytes())


def write_new(path,value):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,'w') as output:
        json.dump(value,output,indent=2); output.write('\n')


def execute(*arguments,timeout=1800):
    subprocess.run([sys.executable,*map(str,arguments)],check=True,timeout=timeout,
        env=dict(PATH='/usr/bin:/bin',LANG='C'),umask=0o077)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fixture',required=True,type=Path)
    p.add_argument('--inventory',required=True,type=Path)
    p.add_argument('--unsupported-daemon',required=True,type=Path)
    p.add_argument('--repository-probe',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    args=p.parse_args()
    if sys.platform!='linux' or os.geteuid()!=11000:
        p.error('Select only the explicitly prepared disposable hosted UID 11000')
    settings=private_json(args.fixture)
    if settings.get('dedicatedTestAccount') is not True or settings.get('disposableUid')!=os.geteuid() or not settings.get('fixtureName','').startswith('github-hosted-ubuntu24-'):
        p.error('Select the dedicated ephemeral hosted Ubuntu fixture')
    parent=Path(settings['fixtureParent'])
    info=parent.lstat()
    if not parent.is_absolute() or not stat.S_ISDIR(info.st_mode) or info.st_uid!=os.geteuid() or stat.S_IMODE(info.st_mode)!=0o700:
        p.error('Select an owned private ordinary fixture parent')
    if not args.output.is_absolute() or args.output.parent!=parent or args.output.exists():
        p.error('Select a new receipt inside the owned fixture parent')
    inventory=private_json(args.inventory,4*1024*1024)
    if not re.fullmatch('[a-f0-9]{40}',inventory['sourceRevision']):
        p.error('Require the actual candidate source revision')
    inputs={}
    for name,key in [('gs','gs'),('gitstored','daemon'),('unsupportedDaemon','unsupported_daemon'),('repositoryProbe','repository_probe')]:
        path=Path(settings[key]) if key in settings else getattr(args,key)
        info=path.lstat()
        if not path.is_absolute() or not stat.S_ISREG(info.st_mode) or info.st_uid!=os.geteuid() or info.st_mode&0o022 or not info.st_mode&0o100:
            p.error('Select explicitly owned ordinary executables')
        with path.open('rb') as binary:
            digest=hashlib.file_digest(binary,'sha256').hexdigest()
        if name in ('gs','gitstored') and digest!=inventory['executables'][name]['sha256']:
            p.error('Candidate executable differs from its validated install inventory')
        inputs[name]=dict(sha256=digest,bytes=info.st_size)
    records=list(parent.glob('runtime-run-core-memdb-*.json'))
    roots=list(parent.glob('gs-public-runtime-*'))
    if len(records)!=1 or len(roots)!=1:
        p.error('Select one completed core smoke in a new owned hosted batch')
    info=roots[0].lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid!=os.geteuid() or stat.S_IMODE(info.st_mode)!=0o700:
        p.error('Select an ordinary owned core runtime root')
    core=private_json(records[0])
    if core['status']!='passed' or core['case']!='test_authorized_cold_start_requires_current_generation_health' or any(
            core['executables'][key]['sha256']!=inputs[name]['sha256'] for name,key in [('gs','gs'),('gitstored','daemon')]):
        p.error('Require successful cold-start evidence for this exact pair')
    # Keep the real AF_UNIX control and lost-ACK relay paths below sun_path.
    batch=Path(tempfile.mkdtemp(prefix='a-',dir=parent))
    if len(os.fsencode(batch/'gs-public-runtime-xxxxxxxx/runtime/gitstored.sock'))>=108:
        p.error('Select a shorter fixture parent for native Unix sockets')
    summary=dict(schemaVersion=1,status='running',scope='unsigned Ubuntu native acceptance; not package-channel or release qualification',
        host=platform.platform(),candidateSourceRevision=inventory['sourceRevision'],
        testSourceRevision=settings['sourceRevision'],applicationVersion=inventory['applicationVersion'],
        sourceInputs=inputs,scenarios=[])
    try:
        layouts=batch/'offline'
        execute(ROOT/'tests/packaging/prepare-offline-cache.py','--cache',roots[0]/'state/assets','--output',layouts,timeout=120)
        selected={**settings,'fixtureParent':str(batch),'offlineLayouts':private_json(layouts/'layouts.json'),
            'unsupportedDaemon':str(args.unsupported_daemon),'repositoryProbe':str(args.repository_probe),
            'purpose':'Explicit disposable hosted complete native acceptance; no release qualification'}
        fixture=batch/'fixture.json'; write_new(fixture,selected)
        for scenario,case in [('clean-install',None),('recovery',None),
                ('exp-003b','test_interrupted_offline_blob_never_activates'),
                ('exp-003b','test_tampered_warm_blob_cannot_be_reused_for_execution')]:
            command=[ROOT/'tests/runtime/run.py','--scenario',scenario,'--fixture',fixture]
            if case: command += ['--case',case]
            execute(*command)
        costs=batch/'measurements.json'
        execute(ROOT/'tests/packaging/measure-native.py','--fixture',fixture,'--output',costs,'--samples','100',timeout=600)
        measurements=private_json(costs)
        validate_performance(measurements,True)
        if any(measurements['sourceInputs'][name]['sha256']!=inputs[name]['sha256'] for name in ('gs','gitstored')):
            raise ValueError('Runtime measurements must bind this exact installed pair')
        summary['measurements']=measurements
        summary['status']='passed'
    finally:
        for record in sorted(batch.glob('runtime-run-*.json')):
            value=private_json(record)
            summary['scenarios'].append({key:value[key] for key in ('scenario','case','status','elapsedSeconds')})
            if any(value['executables'][key]['sha256']!=inputs[name]['sha256'] for name,key in [('gs','gs'),('gitstored','daemon')]):
                summary['status']='failed'
                summary['pairMismatch']=True
        if summary['status']!='passed': summary['status']='failed'
        write_new(args.output,summary)
    print(json.dumps(dict(status=summary['status'],receipt=str(args.output),scope=summary['scope'])))
    if summary['status']!='passed': raise SystemExit(1)


if __name__=='__main__':
    main()
