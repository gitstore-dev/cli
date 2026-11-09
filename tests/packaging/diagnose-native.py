#!/usr/bin/env python3
"""Bounded safe metadata from an explicitly owned failed native test fixture.

Never emits intent, result bodies, raw error messages, descriptors, service logs,
configuration, environment, credentials, database files or workload data.
"""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import sqlite3
import stat
import subprocess
import tempfile


def audit_blobs(cache):
    """Only fixed cache paths, bounded public image bytes and scalar results."""
    rows=[]
    try:
        with ExitStack() as cleanup:
            flags=os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC
            root_fd=os.open(cache,flags|os.O_DIRECTORY)
            cleanup.callback(os.close,root_fd)
            blobs_fd=os.open('blobs',flags|os.O_DIRECTORY,dir_fd=root_fd)
            cleanup.callback(os.close,blobs_fd)
            with os.fdopen(os.open('manifest.json',flags,dir_fd=root_fd),'rb') as source:
                info=os.fstat(source.fileno())
                if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.geteuid() or info.st_nlink!=1:
                    return rows
                data=source.read(1024*1024+1)
            if len(data)>1024*1024:
                return rows
            layers=json.loads(data).get('layers',[])
            if not isinstance(layers,list) or len(layers)>128:
                return rows
            remaining=128*1024*1024
            for index,descriptor in enumerate(layers):
                expected=descriptor.get('size')
                digest=descriptor.get('digest','')
                if not isinstance(expected,int) or not 0<expected<=remaining or not re.fullmatch('sha256:[a-f0-9]{64}',digest):
                    break
                remaining-=expected
                with os.fdopen(os.open(f'layer-{index}',flags,dir_fd=blobs_fd),'rb') as source:
                    info=os.fstat(source.fileno())
                    policy=stat.S_ISREG(info.st_mode) and info.st_uid in (0,os.geteuid()) and info.st_nlink==1 and not info.st_mode&0o022
                    matches=False
                    if policy and info.st_size==expected:
                        value=hashlib.sha256()
                        for _ in range((expected+65535)//65536):
                            value.update(source.read(65536))
                        matches='sha256:'+value.hexdigest()==digest
                    rows.append(dict(index=index,policy=policy,mode=stat.S_IMODE(info.st_mode),links=info.st_nlink,
                        sizeMatches=info.st_size==expected,digestMatches=matches))
    except (OSError,ValueError,TypeError,AttributeError):
        pass
    return rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fixture',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--provision-probe',type=Path,help='Explicit developer-only offline materialization probe')
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
    if args.provision_probe:
        probe=args.provision_probe.lstat()
        if not args.provision_probe.is_absolute() or not stat.S_ISREG(probe.st_mode) or probe.st_uid!=os.geteuid() or probe.st_mode&0o022 or not probe.st_mode&0o100:
            p.error('select an owned ordinary developer probe')
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
            materialization=[]
            if args.provision_probe:
                for key,descriptor in db.execute("SELECT asset_key,descriptor FROM assets WHERE state='active' LIMIT 8"):
                    if not re.fullmatch('[a-f0-9]{64}',key) or len(descriptor)>65536:
                        continue
                    images=json.loads(descriptor).get('images',[])
                    component=images[0].get('component') if len(images)==1 and isinstance(images[0],dict) else None
                    if component not in ('api','git','controller'):
                        continue
                    cache=root/'state/assets'/key
                    cached=cache.lstat()
                    if not stat.S_ISDIR(cached.st_mode) or cached.st_uid!=os.geteuid() or stat.S_IMODE(cached.st_mode)!=0o700:
                        continue
                    blob_checks=audit_blobs(cache)
                    for mode in ('materialize','materialize-sdk'):
                        target=Path(tempfile.mkdtemp(prefix='image-materialization-probe-',dir=parent))
                        result=subprocess.run([str(args.provision_probe),component,str(target),'180000',str(cache),mode],
                            env=dict(PATH='/nonexistent',LANG='C',HOME=str(target)),capture_output=True,text=True,timeout=190)
                        value=json.loads(result.stdout) if 0<len(result.stdout)<=4096 else {}
                        phases=[]
                        for line in result.stderr.splitlines():
                            match=re.fullmatch(r'GSTD_IMAGE_DIAGNOSTIC phase=(source-root|separate-roots|manifest-read|manifest-verify|config-copy|config-verify|layer-copy|unpack) status=([0-7])',line)
                            if match and len(phases)<16:
                                phases.append(dict(phase=match[1],status=int(match[2])))
                        # Only fixed numeric SDK/provisioning outcomes; no raw child diagnostics.
                        materialization.append(dict(component=component,mode=mode,target=target.name,exit=result.returncode,
                            status=value.get('status') if isinstance(value.get('status'),int) else None,
                            phases=phases,blobChecks=blob_checks,stderrCharacters=len(result.stderr)))
        record['fixtures'].append(dict(name=root.name,operations=operations,assets=assets,resources=owned,
            workloadMilestoneCounts=milestones,materializationProbes=materialization))
    fd=os.open(args.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,'w') as stream:
        json.dump(record,stream,indent=2);stream.write('\n')
    print(json.dumps(record))


if __name__=='__main__':
    main()
