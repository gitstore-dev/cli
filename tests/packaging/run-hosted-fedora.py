#!/usr/bin/env python3
"""Explicit unsigned Fedora workload review on an empty ephemeral GitHub Docker host."""
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

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'runtime/packaging'))
from inventory import validate_install
from system_packages import validate_rpm_metadata


def host_selection(environment,system,machine):
    arch={'x86_64':'x64','aarch64':'arm64'}.get(machine)
    if (system!='linux' or arch is None or environment.get('GITHUB_ACTIONS')!='true'
            or environment.get('RUNNER_ENVIRONMENT')!='github-hosted'
            or environment.get('GITHUB_REPOSITORY_ID')!='1431005655'
            or environment.get('RUNNER_ARCH')!={'x64':'X64','arm64':'ARM64'}[arch]
            or not re.fullmatch('[1-9][0-9]*',environment.get('GITHUB_RUN_ID',''))
            or not re.fullmatch('[a-f0-9]{40}',environment.get('GITHUB_SHA',''))):
        raise ValueError('Select an explicit same-repository ephemeral hosted Linux fixture')
    return arch


def ordinary(path,executable=False):
    info=path.lstat()
    if (not path.is_absolute() or not stat.S_ISREG(info.st_mode) or info.st_nlink!=1
            or info.st_uid not in (0,os.geteuid()) or info.st_mode&0o022
            or not 0<info.st_size<=512*1024*1024 or executable and not info.st_mode&0o100):
        raise ValueError('Select a bounded trusted ordinary public input')
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--unsupported-daemon',type=Path,required=True)
    parser.add_argument('--repository-probe',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--producer-run',type=int,required=True)
    args=parser.parse_args()
    arch=host_selection(os.environ,sys.platform,platform.machine())
    if args.producer_run<=0:
        parser.error('Select an explicit successful producer run')
    revision=os.environ['GITHUB_SHA'];identity=os.environ['GITHUB_RUN_ID']
    if subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()!=revision:
        parser.error('Select the actual committed workflow source')
    if not args.output.is_absolute() or args.output.exists() or args.output.is_symlink():
        parser.error('Select a new private hosted output directory')
    if not args.candidate.is_absolute() or args.candidate.is_symlink():
        parser.error('Select ordinary absolute downloaded candidate assets')
    manifests=list(args.candidate.glob('*.install.json'));packages=list(args.candidate.glob('*.rpm'))
    if len(manifests)!=1 or len(packages)!=1:
        parser.error('Select exactly one complete native inventory and RPM')
    inventory_path,package=manifests[0],packages[0]
    ordinary(inventory_path);package_digest=ordinary(package)
    helper_digests={name:ordinary(path,True) for name,path in (
        ('unsupportedDaemon',args.unsupported_daemon),('repositoryProbe',args.repository_probe))}
    inventory=json.loads(inventory_path.read_bytes());validate_install(inventory)
    if (inventory['os'],inventory['arch'])!=('linux',arch):
        parser.error('Candidate does not match the actual native hosted architecture')
    args.output.mkdir(mode=0o700)
    config=args.output/'docker-config';config.mkdir(mode=0o700)
    environment=dict(PATH='/usr/sbin:/usr/bin:/sbin:/bin',LANG='C',HOME=str(args.output),DOCKER_CONFIG=str(config))
    def call(*command,timeout=120,capture=False,check=True):
        return subprocess.run(list(map(str,command)),env=environment,cwd=ROOT,check=check,
            timeout=timeout,capture_output=capture,text=True)
    def docker(*arguments,**kwargs):
        return call('/usr/bin/docker','--host','unix:///var/run/docker.sock',*arguments,**kwargs)
    if docker('ps','--all','--quiet',capture=True).stdout.strip():
        parser.error('Refuse a Docker host with pre-existing containers')
    context=args.output/'context';context.mkdir(mode=0o700)
    (context/'Containerfile').write_bytes((ROOT/'tests/fixtures/runtime/fedora44-native.Containerfile').read_bytes())
    name=f'gs-fedora-{identity}-{arch}';image=f'gitstore-fedora-review:{identity}-{arch}'
    docker('build','--tag',image,'--file',context/'Containerfile',context,timeout=600)
    source=args.output/'public-inputs.tar'
    call('/usr/bin/git','archive','--format=tar','--output='+str(source),'HEAD',
        'tests/runtime','tests/packaging/prepare-fedora-fixture.sh','tests/packaging/check-linux-fixture.py',
        'tests/packaging/hosted-native-acceptance.py','tests/packaging/prepare-offline-cache.py',
        'tests/packaging/measure-native.py','tests/packaging/diagnose-native.py',
        'tests/packaging/test_native_autostart.py','runtime/packaging/command_measurements.py','runtime/profiles')
    created=False
    receipt=dict(schemaVersion=1,status='failed',scope='unsigned Fedora review; not trusted release qualification',
        sourceRevision=revision,candidateSourceRevision=inventory['sourceRevision'],producerRun=args.producer_run,arch=arch,
        packageSha256=package_digest,developerHelpers=helper_digests,publicInputsSha256=ordinary(source))
    try:
        docker('run','--detach','--name',name,'--hostname','gs-cli-alpha1-fedora44-native-20261109',
            '--label','gitstore.fixture='+identity,'--privileged','--cgroupns=private',
            '--cpus','2','--memory','4g','--tmpfs','/run','--tmpfs','/tmp','--tmpfs','/run/lock',image)
        created=True
        details=json.loads(docker('inspect',name,capture=True).stdout)[0]
        if (details['HostConfig']['CgroupnsMode']!='private' or details['HostConfig'].get('Binds')
                or details['HostConfig'].get('PortBindings')
                or any(m['Type']!='tmpfs' for m in details['Mounts'])):
            raise ValueError('Require private cgroups and no host filesystem bind mounts')
        if docker('exec',name,'readlink','/proc/1/ns/cgroup',capture=True).stdout.strip()==str(Path('/proc/self/ns/cgroup').readlink()):
            raise ValueError('Container must not share the runner cgroup namespace')
        receipt.update(imageId=details['Image'],privateCgroupNamespace=True,noHostBindMounts=True)
        # Wait for PID 1, not an arbitrary boot delay. /root avoids Fedora's
        # independently mounted /tmp, which is not the Docker cp root filesystem.
        docker('exec',name,'systemctl','is-system-running','--wait',check=False,timeout=60)
        code="import json,os,platform,pathlib; p=pathlib.Path('/sys/fs/selinux/enforce'); print(json.dumps(dict(distribution=platform.freedesktop_os_release()['ID'],version=platform.freedesktop_os_release()['VERSION_ID'],kernel=platform.release(),libc=os.confstr('CS_GNU_LIBC_VERSION'),selinuxEnforcing=(p.read_text().strip()=='1') if p.exists() else None)))"
        receipt['host']=json.loads(docker('exec',name,'python3','-c',code,capture=True).stdout)
        docker('cp',source,name+':/root/public-inputs.tar')
        docker('cp',ROOT/'tests/packaging/prepare-fedora-fixture.sh',name+':/root/prepare.sh')
        docker('exec','--env','GITSTORE_DISPOSABLE_FIXTURE=gs-cli-alpha1-fedora44-native-20261109',name,'sh','/root/prepare.sh')
        docker('exec',name,'install','-d','-m','700','-o','11000','-g','11000','/home/gitstorefixture/source','/home/gitstorefixture/helpers')
        docker('exec',name,'install','-m','400','-o','11000','-g','11000','/root/public-inputs.tar','/home/gitstorefixture/public-inputs.tar')
        docker('exec','--user','11000:11000',name,'tar','-xf','/home/gitstorefixture/public-inputs.tar','-C','/home/gitstorefixture/source')
        docker('cp',package,name+':/root/candidate.rpm');docker('cp',inventory_path,name+':/root/install-inventory.json')
        query=lambda *arguments:docker('exec',name,'rpm','-qp',*arguments,'/root/candidate.rpm',capture=True).stdout
        validate_rpm_metadata(query('--qf','%{NAME}\n%{VERSION}\n%{RELEASE}\n%{ARCH}').splitlines(),
            query('--scripts'),query('--triggers'),query('--qf','[%{FILENAMES}\t%{FILELINKTOS}\t%{FILEMODES:octal}\n]'),
            inventory['applicationVersion'],arch)
        docker('exec',name,'dnf','--assumeyes','install','/root/candidate.rpm',timeout=240)
        docker('exec',name,'rpm','-V','gitstore')
        actual=docker('exec',name,'sha256sum','/usr/libexec/gitstore/bin/gs','/usr/libexec/gitstore/bin/gitstored',capture=True).stdout
        if [line.split()[0] for line in actual.splitlines()]!=[inventory['executables'][key]['sha256'] for key in ('gs','gitstored')]:
            raise ValueError('Installed executable bytes differ from the selected complete candidate')
        for path,target in ((args.unsupported_daemon,'unsupported-daemon'),(args.repository_probe,'repository-probe')):
            docker('cp',path,name+':/root/'+target)
            docker('exec',name,'install','-m','500','-o','11000','-g','11000','/root/'+target,'/home/gitstorefixture/helpers/'+target)
        fixture=dict(schemaVersion=1,fixtureName=f'github-hosted-fedora44-{identity}-{arch}',disposableUid=11000,
            dedicatedTestAccount=True,fixtureParent='/home/gitstorefixture/experiments',gs='/usr/libexec/gitstore/bin/gs',
            daemon='/usr/libexec/gitstore/bin/gitstored',sourceRoot='/home/gitstorefixture/source',sourceRevision=revision,
            unsupportedDaemon='/home/gitstorefixture/helpers/unsupported-daemon',repositoryProbe='/home/gitstorefixture/helpers/repository-probe')
        code="import json,os,sys; p='/home/gitstorefixture/experiments/fedora.json'; fd=os.open(p,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600); os.fchown(fd,11000,11000); f=os.fdopen(fd,'w'); json.dump(json.loads(sys.argv[1]),f); f.close()"
        docker('exec',name,'python3','-c',code,json.dumps(fixture))
        docker('exec',name,'install','-m','400','-o','11000','-g','11000','/root/install-inventory.json','/home/gitstorefixture/experiments/install-inventory.json')
        def user(*arguments,timeout=120):
            return docker('exec','--user','11000:11000',name,'env','-i','PATH=/usr/bin:/bin','HOME=/home/gitstorefixture',
                *arguments,timeout=timeout)
        user('python3','/home/gitstorefixture/source/tests/packaging/check-linux-fixture.py','--fixture','/home/gitstorefixture/experiments/fedora.json')
        user('GITSTORE_RUNTIME_FIXTURE=/home/gitstorefixture/experiments/fedora.json','GITSTORE_TEST_INSTALLED_COMMANDS=1',
            'python3','/home/gitstorefixture/source/tests/packaging/test_native_autostart.py')
        receipt['installedConcurrentFirstUse']=True
        user('python3','/home/gitstorefixture/source/tests/runtime/run.py','--scenario','core-memdb',
            '--case','test_authorized_cold_start_requires_current_generation_health','--fixture','/home/gitstorefixture/experiments/fedora.json',timeout=300)
        user('python3','/home/gitstorefixture/source/tests/packaging/hosted-native-acceptance.py','--fixture-kind','hosted-fedora44',
            '--fixture','/home/gitstorefixture/experiments/fedora.json','--inventory','/home/gitstorefixture/experiments/install-inventory.json',
            '--unsupported-daemon','/home/gitstorefixture/helpers/unsupported-daemon','--repository-probe','/home/gitstorefixture/helpers/repository-probe',
            '--output','/home/gitstorefixture/experiments/native-acceptance.json',timeout=2800)
        docker('exec',name,'dnf','--assumeyes','remove','gitstore',timeout=180)
        docker('exec',name,'sh','-c','test ! -e /usr/bin/gs && test ! -e /usr/bin/gitstored && test ! -e /usr/libexec/gitstore && test ! -e /usr/lib/systemd/user/gitstored.service')
        receipt.update(status='passed',removed=True)
    finally:
        if created:
            # Copy only the established bounded summary, never scenario roots,
            # private databases, service logs or credential material.
            code="import os,stat; p='/home/gitstorefixture/experiments/native-acceptance.json'; s=os.lstat(p); assert stat.S_ISREG(s.st_mode) and s.st_uid==11000 and s.st_nlink==1 and not s.st_mode&0o077 and 0<s.st_size<=1048576"
            if docker('exec',name,'python3','-c',code,capture=True,check=False).returncode==0:
                docker('cp',name+':/home/gitstorefixture/experiments/native-acceptance.json',args.output/'native-acceptance.json')
            details=json.loads(docker('inspect',name,capture=True).stdout)[0]
            if details['Config']['Labels'].get('gitstore.fixture')!=identity:
                raise ValueError('Refuse cleanup of an unselected container')
            docker('rm','--force',name)
        (args.output/'package-controls.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(dict(status=receipt['status'],scope=receipt['scope'])))


if __name__=='__main__':
    main()
