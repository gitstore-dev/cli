#!/usr/bin/env python3
"""Execute selected unsigned review channels in explicitly disposable accounts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import stat
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'runtime/packaging'),str(ROOT/'tests/packaging')]
from review_replacements import arguments
from versions import encode
from inventory import snapshot_file


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--review',type=Path,required=True)
    p.add_argument('--parent',type=Path,required=True)
    p.add_argument('--channel',nargs='+',choices=('archive','npm','homebrew','deb','rpm'),required=True)
    p.add_argument('--brew-source',type=Path)
    p.add_argument('--result',type=Path)
    args=p.parse_args()
    root=args.review
    if not root.is_absolute() or root.is_symlink() or not args.parent.is_absolute() or args.parent.is_symlink():
        p.error('select ordinary absolute review/private experiment paths')
    os_name='darwin' if sys.platform=='darwin' else 'linux'
    arch={'x86_64':'x64','arm64':'arm64','aarch64':'arm64'}.get(platform.machine())
    if arch is None:p.error('select a supported native architecture')
    system=any(channel in ('deb','rpm') for channel in args.channel)
    if system and (os.geteuid()!=0 or os.environ.get('GITHUB_ACTIONS')!='true' or
            os.environ.get('RUNNER_ENVIRONMENT')!='github-hosted' or
            os.environ.get('GITHUB_REPOSITORY_ID')!='1431005655'):
        p.error('system review requires explicit disposable hosted infrastructure root')
    if not system and os.geteuid()==0:p.error('select the non-root package test account')
    info=args.parent.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid!=(11000 if system else os.geteuid()) or info.st_mode&0o077:
        p.error('select the dedicated account private experiment parent')
    record=json.loads((root/'review.json').read_text());encode(record['version'])
    assets=root/(os_name+'-'+arch)/'complete-candidate/assets'
    for channel in args.channel:
        suffix={'archive':'.tar.gz','homebrew':'.tar.gz','npm':'.tgz','deb':'.deb','rpm':'.rpm'}[channel]
        selected=list(assets.glob('*'+suffix))
        if channel in ('archive','homebrew'):
            selected=[assets/f"gitstore-{record['version']}-{os_name}-{arch}.tar.gz"]
        if len(selected)!=1:raise ValueError('select exactly one native package per review channel')
        artifact=selected[0]
        snapshot_file(root,artifact.relative_to(root).as_posix())
        replacement=arguments(root,record,os_name,arch,channel)
        command=[sys.executable,str(ROOT/'tests/packaging/run.py'),'--channel',channel,
            '--version',record['version'],'--fixture-parent',str(args.parent)]
        if channel in ('deb','rpm'):
            if (os.geteuid()!=0 or os.environ.get('GITHUB_ACTIONS')!='true' or
                    os.environ.get('RUNNER_ENVIRONMENT')!='github-hosted' or
                    os.environ.get('GITHUB_REPOSITORY_ID')!='1431005655'):
                raise ValueError('system review requires explicit disposable hosted infrastructure root')
            release=platform.freedesktop_os_release()
            if (release['ID'],release['VERSION_ID'])!={'deb':('ubuntu','24.04'),'rpm':('fedora','44')}[channel]:
                raise ValueError('system review requires the actual selected distribution')
            staging=args.parent.parent/('review-original.'+channel)
            shutil.copyfile(artifact,staging);staging.chmod(0o400);artifact=staging
            if replacement:
                staging=args.parent.parent/('review-next.'+channel)
                shutil.copyfile(replacement[1],staging);staging.chmod(0o400)
                replacement[1]=str(staging)
            fixture=dict(schemaVersion=1,disposableUid=11000,disposableGid=11000,
                dedicatedTestAccount=True,disposableHost=True,hostname=socket.gethostname(),
                osId=release['ID'],osVersion=release['VERSION_ID'],arch=arch,fixtureParent=str(args.parent))
            manifest=args.parent.parent/('review-system-'+channel+'.json')
            with manifest.open('x') as output:json.dump(fixture,output)
            manifest.chmod(0o600)
            command+=['--system-fixture',str(manifest)]
        elif os.geteuid()==0:
            raise ValueError('archive/npm/Homebrew product commands require the selected non-root account')
        if channel=='homebrew':
            if args.brew_source is None:p.error('select the reviewed private Homebrew checkout')
            formula=root/'gitstore.rb'
            command+=['--formula',str(formula),'--formula-sha256',hashlib.sha256(formula.read_bytes()).hexdigest(),
                '--brew-source',str(args.brew_source),'--brew-revision','bf464f57275cae0be106b43d190c98564ac59b0e']
        command+=['--artifact',str(artifact),'--sha256',hashlib.sha256(artifact.read_bytes()).hexdigest(),*replacement]
        subprocess.run(command,cwd=ROOT,check=True)
    if args.result:
        if args.channel not in (['deb'],['rpm']):p.error('bounded system receipt copy requires one system channel')
        receipts=list(args.parent.glob('system-package-*/receipt.json'))
        if len(receipts)!=1:raise ValueError('expected one system lifecycle receipt')
        result=json.loads(receipts[0].read_text())
        result['candidateSourceRevision']=record['sourceRevision']
        if 'replacementSourceRevision' in record:result['replacementSourceRevision']=record['replacementSourceRevision']
        with args.result.open('x') as output:json.dump(result,output,indent=2)


if __name__=='__main__':main()
