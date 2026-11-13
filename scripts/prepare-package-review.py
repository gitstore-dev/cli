#!/usr/bin/env python3
"""Download same-source unsigned CI candidates and render a test-only real-hash formula.

Never emits release qualification, attestations or publication inputs. Product
commands run only in the separately selected disposable package-test jobs.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime/packaging'))
from homebrew import render
from inventory import validate_install
from versions import validate_alpha_proposal


LINUX_PRODUCERS = ('runtime-ci.yml', 'runtime.yml')


def selected_run(identity,workflow):
    if identity<=0:
        raise ValueError('select explicit positive CI run IDs')
    workflows = (workflow,) if isinstance(workflow, str) else workflow
    value=json.loads(subprocess.check_output(['gh','api',f'repos/gitstore-dev/cli/actions/runs/{identity}']))
    if (value['repository']['id']!=1431005655 or value['head_repository']['id']!=1431005655
            or value['path'] not in {'.github/workflows/'+name for name in workflows} or value['status']!='completed'
            or value['conclusion']!='success' or value['event'] not in ('push','pull_request','merge_group')
            or not re.fullmatch('[0-9a-f]{40}',value['head_sha'])):
        raise ValueError('select successful same-repository development CI, never a foreign run')
    return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cli-run',type=int,required=True)
    p.add_argument('--linux-run',type=int,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--replacement-cli-run',type=int)
    p.add_argument('--replacement-linux-run',type=int)
    args=p.parse_args()
    if bool(args.replacement_cli_run)!=bool(args.replacement_linux_run):
        p.error('select both replacement producer runs or neither')
    cli=selected_run(args.cli_run,'cli.yml')
    linux=selected_run(args.linux_run,LINUX_PRODUCERS)
    if cli['head_sha']!=linux['head_sha']:
        raise ValueError('all candidate platforms must come from the same source revision')
    if not args.output.is_absolute() or args.output.exists() or os.geteuid()==0:
        p.error('select a new absolute non-root review directory')
    args.output.mkdir(mode=0o700)
    rows=[]
    versions=set()
    revisions=set()
    for os_name,arch,label,run in [('darwin','arm64','macos-15',args.cli_run),
            ('darwin','x64','macos-15-intel',args.cli_run),('linux','arm64','linux-arm64',args.linux_run),
            ('linux','x64','linux-amd64',args.linux_run)]:
        root=args.output/(os_name+'-'+arch)
        subprocess.run(['gh','run','download',str(run),'--repo','gitstore-dev/cli',
            '--name','unpublished-production-candidate-'+label,'--dir',str(root)],check=True,timeout=300)
        assets=root/'complete-candidate/assets'
        inventories=list(assets.glob('*.install.json'))
        if len(inventories)!=1:
            raise ValueError('select a complete one-platform production candidate')
        inventory=json.loads(inventories[0].read_text())
        validate_install(inventory)
        if (inventory['os'],inventory['arch'])!=(os_name,arch):
            raise ValueError('artifact does not select the expected actual platform')
        version=inventory['applicationVersion']
        versions.add(version);revisions.add(inventory['sourceRevision'])
        name=f'gitstore-{version}-{os_name}-{arch}.tar.gz'
        path=assets/name
        with tarfile.open(path,'r:gz') as archive:
            installed=json.load(archive.extractfile('share/gitstore/install.json'))
            if installed!=inventory:
                raise ValueError('archive and loose inventory differ')
        rows.append(dict(os=os_name,arch=arch,url=f'https://github.com/gitstore-dev/cli/releases/download/v{version}/{name}',
            sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    if len(versions)!=1 or len(revisions)!=1:
        raise ValueError('all four installed inventories must agree on version and actual checkout revision')
    version=versions.pop()
    # PR merge checkout revisions differ from run.head_sha. Retain both rather
    # than pretending a branch-head artifact was built from a different tree.
    value=dict(qualification='UNSIGNED DEVELOPMENT PACKAGE REVIEW ONLY; NEVER RELEASE QUALIFICATION',
        branchRevision=cli['head_sha'],sourceRevision=revisions.pop(),version=version,
        cliRun=args.cli_run,linuxRun=args.linux_run,archives=rows)
    (args.output/'review.json').write_text(json.dumps(value,indent=2)+'\n')
    (args.output/'gitstore.rb').write_text(render(version,rows))
    if args.replacement_cli_run:
        next_cli=selected_run(args.replacement_cli_run,'cli.yml')
        next_linux=selected_run(args.replacement_linux_run,LINUX_PRODUCERS)
        if next_cli['head_sha']!=next_linux['head_sha']:
            raise ValueError('replacement producers must have the same branch source')
        next_rows=[];next_versions=set();next_sources=set()
        for os_name,arch,label,run in [('darwin','arm64','macos-15',args.replacement_cli_run),
                ('darwin','x64','macos-15-intel',args.replacement_cli_run),
                ('linux','arm64','linux-arm64',args.replacement_linux_run),
                ('linux','x64','linux-amd64',args.replacement_linux_run)]:
            root=args.output/'replacements'/(os_name+'-'+arch)
            subprocess.run(['gh','run','download',str(run),'--repo','gitstore-dev/cli',
                '--name','unqualified-replacement-'+label,'--dir',str(root)],check=True,timeout=300)
            result=json.loads((root/'result.json').read_text())
            next_version=result['replacementVersion'];next_versions.add(next_version)
            next_sources.add(result['sourceRevision'])
            validate_alpha_proposal(version,next_version)
            if (result['status']!='passed' or result['sourceVersion']!=version or
                    result['qualification']!='UNQUALIFIED replacement test only; never release qualification'):
                raise ValueError('select successful, explicitly unqualified replacement controls')
            name=f'UNQUALIFIED-ONLY-{next_version}-{os_name}-{arch}.tar.gz'
            archive=root/'packages'/name
            with tarfile.open(archive,'r:gz') as source:
                metadata=json.load(source.extractfile('share/gitstore/install.json'))
            validate_install(metadata)
            if ((metadata['os'],metadata['arch'],metadata['applicationVersion'],metadata['sourceRevision'])!=
                    (os_name,arch,next_version,result['sourceRevision'])):
                raise ValueError('replacement archive identity differs from its producer')
            next_rows.append(dict(os=os_name,arch=arch,
                url=f'https://github.com/gitstore-dev/cli/releases/download/v{next_version}/gitstore-{next_version}-{os_name}-{arch}.tar.gz',
                sha256=hashlib.sha256(archive.read_bytes()).hexdigest()))
        if len(next_versions)!=1 or len(next_sources)!=1:
            raise ValueError('all replacement archives must have the same actual source/version')
        next_version=next_versions.pop()
        value.update(replacementVersion=next_version,replacementSourceRevision=next_sources.pop(),
            replacementCliRun=args.replacement_cli_run,replacementLinuxRun=args.replacement_linux_run,
            replacementArchives=next_rows)
        (args.output/'gitstore-next.rb').write_text(render(next_version,next_rows))
        (args.output/'review.json').write_text(json.dumps(value,indent=2)+'\n')
    print(json.dumps(value))


if __name__=='__main__':
    main()
