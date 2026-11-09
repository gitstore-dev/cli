#!/usr/bin/env python3
"""Prepare exact-source controls on an explicitly isolated hosted qualification account."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'runtime/packaging'),str(ROOT/'tests/packaging')]
from inventory import validate_install, snapshot_file
from homebrew import render
from versions import validate_alpha_proposal
from archive_lifecycle import unpack
from release_candidate import validate_bytes, qualification_rows


def selected(environment, system, machine, uid):
    if (environment.get('GITHUB_ACTIONS')!='true' or environment.get('RUNNER_ENVIRONMENT')!='github-hosted'
            or environment.get('GITHUB_REPOSITORY_ID')!='1431005655' or uid==0
            or system=='linux' and uid!=11000 or system not in ('linux','darwin')
            or machine not in ('x86_64','aarch64','arm64')):
        raise ValueError('select the explicitly isolated hosted qualification account')
    return 'darwin' if system=='darwin' else 'linux', 'x64' if machine=='x86_64' else 'arm64'


def write(path,value):
    with path.open('x') as output:json.dump(value,output,indent=2)
    path.chmod(0o600)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('candidate','controls','parent','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--host',required=True);p.add_argument('--tag',required=True)
    args=p.parse_args()
    os_name,arch=selected(os.environ,sys.platform,platform.machine(),os.geteuid())
    if any(not path.is_absolute() or path.is_symlink() for path in (args.candidate,args.controls,args.parent,args.output)):
        p.error('select ordinary absolute hosted fixture paths')
    if args.output.exists():p.error('select a new hosted receipt directory')
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    if revision!=os.environ['GITHUB_SHA'] or os.environ['GITHUB_REF'] not in ('refs/heads/main','refs/tags/'+args.tag):
        p.error('require the actual reviewed source and main or exact tag')
    value=json.loads((args.candidate/'candidate.json').read_text())
    if value['state']!='packaged' or value['sourceRevision']!=revision or value['tag']!=args.tag:
        p.error('candidate must bind this exact source/tag')
    args.candidate.chmod(0o700);validate_bytes(value,args.candidate)
    channels=sorted(row[3] for row in qualification_rows() if row[:3]==(os_name,arch,args.host))
    if not channels:p.error('host is outside the required qualification matrix')
    if os_name=='linux':
        actual=platform.freedesktop_os_release()
        if args.host!=actual['ID']+':'+actual['VERSION_ID']:p.error('actual distribution differs from selection')
    elif platform.mac_ver()[0].split('.')[0]!='15':p.error('select actual macOS 15')
    args.parent.mkdir(mode=0o700,exist_ok=True)
    if args.parent.stat().st_uid!=os.geteuid() or args.parent.stat().st_mode&0o077:
        p.error('select an owned private experiment parent')
    versions=set();rows=[]
    for next_os,next_arch in [('linux','x64'),('linux','arm64'),('darwin','x64'),('darwin','arm64')]:
        root=args.controls/('unqualified-replacement-'+next_os+'-'+next_arch)
        result=json.loads((root/'result.json').read_text())
        version=result['replacementVersion'];versions.add(version)
        validate_alpha_proposal(value['version'],version)
        if (result['status']!='passed' or result['sourceVersion']!=value['version'] or result['sourceRevision']!=revision
                or result['qualification']!='UNQUALIFIED replacement test only; never release qualification'):
            raise ValueError('replacement controls must come from the same exact-source release build')
        filename=f'UNQUALIFIED-ONLY-{version}-{next_os}-{next_arch}.tar.gz'
        archive=root/'packages'/filename
        snapshot_file(archive.parent,archive.name)
        import tarfile
        with tarfile.open(archive,'r:gz') as source:metadata=json.load(source.extractfile('share/gitstore/install.json'))
        validate_install(metadata)
        if (metadata['os'],metadata['arch'],metadata['sourceRevision'],metadata['applicationVersion'])!=(next_os,next_arch,revision,version):
            raise ValueError('replacement archive does not match its exact-source control')
        rows.append(dict(os=next_os,arch=next_arch,
            url=f'https://github.com/gitstore-dev/cli/releases/download/v{version}/gitstore-{version}-{next_os}-{next_arch}.tar.gz',
            sha256=hashlib.sha256(archive.read_bytes()).hexdigest()))
    if len(versions)!=1:raise ValueError('replacement versions must agree on all four platforms')
    version=versions.pop();formula=args.parent/'gitstore-next.rb'
    with formula.open('x') as stream:stream.write(render(version,rows))
    formula.chmod(0o600)
    replacements={}
    for channel in channels:
        suffix={'archive':'tar.gz','homebrew':'tar.gz','npm':'tgz','deb':'deb','rpm':'rpm'}[channel]
        path=args.controls/('unqualified-replacement-'+os_name+'-'+arch)/'packages'/f'UNQUALIFIED-ONLY-{version}-{os_name}-{arch}.{suffix}'
        identity=snapshot_file(path.parent,path.name)
        replacements[channel]=dict(path=str(path),sha256=identity['sha256'],version=version)
        if channel=='homebrew':replacements[channel].update(formula=str(formula),formulaSha256=hashlib.sha256(formula.read_bytes()).hexdigest())
    brew=args.parent/'brew-source'
    environment=dict(PATH=os.environ.get('PATH','/usr/bin:/bin'),HOME=str(args.parent),LANG='C',
        GIT_CONFIG_GLOBAL='/dev/null',GIT_CONFIG_NOSYSTEM='1')
    for command in [['git','init',str(brew)],['git','-C',str(brew),'remote','add','origin','https://github.com/Homebrew/brew.git'],
            ['git','-C',str(brew),'fetch','--depth','1','origin','bf464f57275cae0be106b43d190c98564ac59b0e'],
            ['git','-C',str(brew),'checkout','--detach','FETCH_HEAD']]:
        subprocess.run(command,env=environment,check=True,timeout=180)
    settings=dict(schemaVersion=1,cleanHost=True,disposableHost=True,dedicatedTestAccount=True,disposableUid=os.geteuid(),
        selection=[os_name,arch,args.host],fixtureParent=str(args.parent),brewSource=str(brew),
        brewRevision='bf464f57275cae0be106b43d190c98564ac59b0e',replacements=replacements)
    if os_name=='linux':
        system=args.parent/'system.json'
        write(system,dict(schemaVersion=1,disposableUid=os.geteuid(),disposableGid=os.getegid(),dedicatedTestAccount=True,
            disposableHost=True,hostname=socket.gethostname(),osId=actual['ID'],osVersion=actual['VERSION_ID'],
            arch=arch,fixtureParent=str(args.parent)))
        helpers=args.controls/('qualification-controls-linux-'+arch)
        control=json.loads((helpers/'controls.json').read_text())
        if control['sourceRevision']!=revision:raise ValueError('native control source mismatch')
        paths={}
        for key in ('unsupportedDaemon','repositoryProbe'):
            row=control['files'][key];path=helpers/row['filename']
            if snapshot_file(helpers,row['filename'])['sha256']!=row['sha256']:raise ValueError('native helper byte mismatch')
            path.chmod(0o500);paths[key]=str(path)
        replacement_archive=Path(replacements['archive']['path']);next_pair=args.parent/'next-pair'
        unpack(replacement_archive,replacements['archive']['sha256'],version,next_pair)
        native=args.parent/'native.json'
        write(native,dict(schemaVersion=1,fixtureName='hosted-tag-qualification',dedicatedTestAccount=True,
            disposableUid=os.geteuid(),fixtureParent=str(args.parent),sourceRoot=str(ROOT),sourceRevision=revision,**paths))
        settings.update(systemFixture=str(system),nativeFixture=str(native),mismatchDaemon=str(next_pair/'bin/gitstored'))
    manifest=args.parent/'qualification.json';write(manifest,settings)
    subprocess.run([sys.executable,str(ROOT/'scripts/qualify-release.py'),'--candidate',str(args.candidate),
        '--fixture',str(manifest),'--host',args.host,'--revision',revision,'--tag',args.tag,
        '--output',str(args.output)],cwd=ROOT,env=environment,check=True,timeout=5400)


if __name__=='__main__':main()
