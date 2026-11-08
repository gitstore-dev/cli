#!/usr/bin/env python3
"""Measure a real owned deployment and separate runtime costs on a selected fixture."""
import argparse
import json
import math
import os
from pathlib import Path
import platform
import stat
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'runtime/packaging'))
sys.path.insert(0,str(ROOT/'tests/runtime'))
from command_measurements import measure
from support import Fixture


def tree_cost(root,exclude=()):
    logical,allocated,files=0,0,0
    seen=set()
    if not root.exists():
        return dict(logicalBytes=0,allocatedBytes=0,regularFiles=0)
    for directory,children,names in os.walk(root,followlinks=False):
        base=Path(directory)
        children[:]=[name for name in children if not (base/name).is_symlink() and (base/name) not in exclude]
        for name in names:
            info=(base/name).lstat()
            if not stat.S_ISREG(info.st_mode):
                continue
            logical+=info.st_size
            files+=1
            identity=(info.st_dev,info.st_ino)
            if identity not in seen:
                seen.add(identity)
                allocated+=info.st_blocks*512
    return dict(logicalBytes=logical,allocatedBytes=allocated,regularFiles=files)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fixture',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--samples',default=100,type=int)
    args=p.parse_args()
    if sys.platform!='linux' or os.geteuid()==0 or not args.fixture.is_absolute() or not 10<=args.samples<=1000:
        p.error('select a dedicated non-root Linux fixture and 10–1000 samples')
    info=args.fixture.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid!=os.geteuid() or info.st_nlink!=1
            or info.st_mode&0o077 or not 0<info.st_size<=65536):
        p.error('select an owned private ordinary bounded fixture manifest')
    if not args.output.is_absolute() or args.output.exists() or args.output.is_symlink():
        p.error('select a new absolute private receipt path')
    settings=json.loads(args.fixture.read_text())
    if settings.get('dedicatedTestAccount') is not True or settings.get('disposableUid')!=os.geteuid():
        p.error('fixture must select this dedicated disposable account')
    os.environ['GITSTORE_RUNTIME_FIXTURE']=str(args.fixture)
    offline=bool(settings.get('offlineLayouts'))
    fixture=Fixture(offline='verified' if offline else None)
    record=dict(schemaVersion=1,status='running',host=platform.platform(),machine=platform.machine(),
        cpus=os.cpu_count(),samples=args.samples,fixture=str(fixture.root),
        sourceInputs={name:dict(sha256=fixture.digest(path),bytes=path.stat().st_size)
            for name,path in [('gs',fixture.gs),('gitstored',fixture.daemon)]},
        scope='owned three-service memdb deployment; process wall time includes CLI startup/negotiation; no user data')
    try:
        started=time.perf_counter()
        operation=fixture.start('performance')
        fixture.ready('performance',operation)
        record['coldOfflineReadySeconds' if offline else 'coldReadySecondsIncludingAssetPreparation']=round(time.perf_counter()-started,3)
        timings=[]
        for index in range(args.samples+1):
            started=time.perf_counter_ns()
            value=fixture.call('deployment','inspect','performance')
            elapsed=(time.perf_counter_ns()-started)/1_000_000
            deployment=value
            if deployment['readiness']!='ready' or deployment['observedGeneration']!=deployment['generation']:
                raise AssertionError('measurement cannot treat stale/unready state as successful inspection')
            if index:
                timings.append(elapsed)
        ordered=sorted(timings)
        record['daemonInspect']=dict(command='gs deployment inspect performance -o json through explicit owned socket',
            samples=args.samples,p95Milliseconds=round(ordered[math.ceil(args.samples*.95)-1],3),
            minimumMilliseconds=round(ordered[0],3),maximumMilliseconds=round(ordered[-1],3))
        record['commandLatency']=measure(fixture.root/'bin',fixture.root/'command-measurements',args.samples)
        assets=fixture.root/'state/assets'
        record['assetCost']=tree_cost(assets)
        record['stateCostExcludingAssets']=tree_cost(fixture.root/'state',exclude=(assets,))
        record['cacheCost']=tree_cost(fixture.root/'cache')
        record['assetLogicalBytes']=record['assetCost']['logicalBytes']
        record['stateLogicalBytes']=record['stateCostExcludingAssets']['logicalBytes']
        memory={}
        for line in Path('/proc',str(fixture.process.pid),'status').read_text().splitlines():
            if line.startswith(('VmRSS:','VmHWM:')):
                key,number,_=line.split()
                memory[key.removesuffix(':')+'Bytes']=int(number)*1024
        record['managerMemoryOnly']={**memory,'scope':'manager only; excludes service/worker peaks'}
        fixture.call('--timeout','90s','deployment','stop','performance','--idempotency-key','measurement-stop',timeout=100)
        fixture.deployments.discard('performance')
        record['status']='passed'
    except BaseException:
        record['status']='failed'
        raise
    finally:
        fixture.close()
        record['managerChildStopped']=fixture.process.poll() is not None
        fd=os.open(args.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        with os.fdopen(fd,'w') as stream:
            json.dump(record,stream,indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
    print(json.dumps(dict(receipt=str(args.output),status=record['status'],inspection=record.get('daemonInspect'))))


if __name__=='__main__':
    main()
