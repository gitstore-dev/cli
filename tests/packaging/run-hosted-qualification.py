#!/usr/bin/env python3
"""Run mandatory tag qualification on fresh hosted macOS/Ubuntu/Fedora fixtures."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'runtime/packaging'),str(ROOT/'tests/packaging')]
from inventory import snapshot_file
from archive_lifecycle import unpack
from release_candidate import validate_bytes,slot_filename


def host_selection(environment,system,machine,uid):
    arch={'x86_64':'x64','aarch64':'arm64','arm64':'arm64'}.get(machine)
    if (system not in ('linux','darwin') or arch is None or uid==0 or
            environment.get('GITHUB_ACTIONS')!='true' or environment.get('RUNNER_ENVIRONMENT')!='github-hosted'
            or environment.get('GITHUB_REPOSITORY_ID')!='1431005655'
            or environment.get('RUNNER_ARCH')!={'x64':'X64','arm64':'ARM64'}[arch]
            or not re.fullmatch('[1-9][0-9]*',environment.get('GITHUB_RUN_ID',''))
            or not re.fullmatch('[a-f0-9]{40}',environment.get('GITHUB_SHA',''))):
        raise ValueError('select a fresh same-repository GitHub-hosted qualification runner')
    return arch


def run_fixture_setup(user,commands,account,timeout=240):
    # Start outside the private runner checkout, switch UID through the user
    # prefix, then enter its private home. The runner cannot chdir there first.
    for command in commands:
        subprocess.run([*user,'sh','-c','cd "$1" && shift && exec "$@"',
            'gitstore-fixture',account,*command],cwd='/',check=True,timeout=timeout)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('candidate','controls','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--host',choices=('ubuntu:24.04','fedora:44','macos-15','macos-15-intel'),required=True)
    p.add_argument('--tag',required=True)
    args=p.parse_args()
    arch=host_selection(os.environ,sys.platform,platform.machine(),os.geteuid())
    revision=os.environ['GITHUB_SHA']
    if (not re.fullmatch(r'v0\.1\.0-alpha\.[1-9][0-9]*',args.tag)
            or os.environ['GITHUB_REF'] not in ('refs/heads/main','refs/tags/'+args.tag)
            or subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()!=revision):
        p.error('require the actual reviewed main/tag source')
    if any(not path.is_absolute() or path.is_symlink() for path in (args.candidate,args.controls,args.output)) or args.output.exists():
        p.error('select ordinary absolute inputs and a new output directory')
    value=json.loads((args.candidate/'candidate.json').read_text())
    if value['state']!='packaged' or value['sourceRevision']!=revision or value['tag']!=args.tag:
        p.error('require the exact packaged candidate source/tag')
    validate_bytes(value,args.candidate)
    if args.host.startswith('macos'):
        if sys.platform!='darwin' or platform.mac_ver()[0].split('.')[0]!='15' or args.host!=('macos-15' if arch=='arm64' else 'macos-15-intel'):
            p.error('select the actual macOS reference architecture/version')
        args.output.mkdir(mode=0o700)
        parent=Path(os.environ['HOME'])/('.gitstore-qualification-'+os.environ['GITHUB_RUN_ID'])
        if parent.exists():p.error('refuse an existing qualification experiment')
        subprocess.run([sys.executable,str(ROOT/'tests/packaging/prepare-qualification.py'),
            '--candidate',str(args.candidate),'--controls',str(args.controls),'--parent',str(parent),
            '--host',args.host,'--tag',args.tag,'--output',str(args.output/'receipts')],check=True,timeout=5400)
        return
    if sys.platform!='linux' or platform.freedesktop_os_release().get('ID')!='ubuntu' or platform.freedesktop_os_release().get('VERSION_ID')!='24.04':
        p.error('Linux qualification infrastructure requires ephemeral Ubuntu 24.04')
    args.output.mkdir(mode=0o700)
    public=args.output/'public';public.mkdir(mode=0o700)
    candidate=public/'candidate';candidate.mkdir(mode=0o700)
    # Only declared public subjects and the packaged ledger cross the account boundary.
    for name in {entry['filename'] for entry in value['artifacts']}|{'candidate.json'}:
        snapshot_file(args.candidate,name)
        shutil.copyfile(args.candidate/name,candidate/name)
    controls=public/'controls';controls.mkdir(mode=0o700)
    expected={'unqualified-replacement-'+os_name+'-'+target for os_name in ('linux','darwin') for target in ('x64','arm64')}
    expected|={'qualification-controls-linux-'+target for target in ('x64','arm64')}
    if {path.name for path in args.controls.iterdir()}!=expected:
        p.error('require the complete four-platform replacement and two native control groups')
    for group in sorted(expected):
        source=args.controls/group
        if source.is_symlink():p.error('control groups must not follow links')
        for path in source.rglob('*'):
            if path.is_symlink():p.error('control input must not follow links')
            if path.is_dir():continue
            relative=path.relative_to(args.controls)
            snapshot_file(source,path.relative_to(source).as_posix())
            target=controls/relative;target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(path,target)
    bundle=public/'source.bundle'
    subprocess.run(['git','bundle','create',str(bundle),'HEAD'],cwd=ROOT,check=True)
    # Git bundles contain committed public objects/refs, never checkout config,
    # credentials, hooks, environment or untracked build files.
    metadata={name:os.environ[name] for name in ('GITHUB_ACTIONS','RUNNER_ENVIRONMENT','GITHUB_REPOSITORY_ID',
        'GITHUB_RUN_ID','GITHUB_SHA','GITHUB_REF','RUNNER_ARCH')}
    account='/home/gitstorefixture'
    inner=[account+'/qualification-input/venv/bin/python',account+'/qualification-source/tests/packaging/prepare-qualification.py',
        '--candidate',account+'/qualification-input/candidate','--controls',account+'/qualification-input/controls',
        '--parent',account+'/experiments','--host',args.host,'--tag',args.tag,'--output',account+'/qualification-receipts']
    sudoers=('gitstorefixture ALL=(root) NOPASSWD: '+account+'/qualification-input/venv/bin/python '+
        account+'/qualification-source/tests/packaging/run.py *\n')
    sudoers_file=public/'sudoers';sudoers_file.write_text(sudoers)
    setup=[['git','clone','--no-checkout',account+'/qualification-input/source.bundle',account+'/qualification-source'],
        ['git','-C',account+'/qualification-source','checkout','--detach',revision],
        ['python3','-m','venv',account+'/qualification-input/venv'],
        [account+'/qualification-input/venv/bin/pip','install','--disable-pip-version-check','--no-cache-dir','--require-hashes','--only-binary=:all:','-r',account+'/qualification-source/packaging/requirements.txt']]
    if args.host=='ubuntu:24.04':
        prefix=args.output/'pair'
        archive=slot_filename(value['version'],'archive','linux',arch)
        digest=next(row['sha256'] for row in value['artifacts'] if row['filename']==archive)
        unpack(args.candidate/archive,digest,value['version'],prefix)
        inventory=prefix/'share/gitstore/install.json'
        shutil.copyfile(inventory,prefix/'bin/install-inventory.json')
        environment={**os.environ,'GITSTORE_DISPOSABLE_FIXTURE':'github-hosted-ubuntu24'}
        subprocess.run(['sudo','--preserve-env='+','.join(metadata)+',GITSTORE_DISPOSABLE_FIXTURE',
            'sh',str(ROOT/'tests/packaging/prepare-hosted-linux-fixture.sh'),str(prefix/'bin'),str(ROOT)],env=environment,check=True)
        subprocess.run(['sudo','cp','-a',str(public),account+'/qualification-input'],check=True)
        subprocess.run(['sudo','chown','-Rh','11000:11000',account+'/qualification-input'],check=True)
        subprocess.run(['sudo','install','-m','440',str(sudoers_file),'/etc/sudoers.d/gitstore-qualification'],check=True)
        user=['sudo','-u','gitstorefixture','env','-i','PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin','HOME='+account,'LANG=C']
        run_fixture_setup(user,setup,account)
        run_fixture_setup(user,[['env',*[name+'='+value for name,value in metadata.items()],*inner]],account,timeout=5400)
        subprocess.run(['sudo','cp','-a',account+'/qualification-receipts',str(args.output/'receipts')],check=True)
        subprocess.run(['sudo','chown','-Rh',str(os.geteuid())+':'+str(os.getegid()),str(args.output/'receipts')],check=True)
        return
    # Explicit Fedora fixture: empty ephemeral Docker host, private cgroup
    # namespace, no host mounts/ports, and no signing/App credentials.
    config=args.output/'docker-config';config.mkdir(mode=0o700)
    env=dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin',LANG='C',HOME=str(args.output),DOCKER_CONFIG=str(config))
    def docker(*arguments,capture=False,timeout=180,check=True):
        return subprocess.run(['/usr/bin/docker','--host','unix:///var/run/docker.sock',*map(str,arguments)],
            env=env,cwd=ROOT,check=check,text=True,capture_output=capture,timeout=timeout)
    if docker('ps','--all','--quiet',capture=True).stdout.strip():p.error('refuse a Docker host with pre-existing containers')
    context=args.output/'image-context';context.mkdir(mode=0o700)
    (context/'Containerfile').write_text((ROOT/'tests/fixtures/runtime/fedora44-native.Containerfile').read_text()+
        '\nRUN dnf -y install sudo git gcc make glibc-devel file curl bzip2 patch python3-pip python3-jsonschema /usr/bin/npm patchelf libseccomp libcap json-c libcurl openssl-libs libxcrypt && dnf clean all\n')
    image='gs-qualification:'+os.environ['GITHUB_RUN_ID']+'-'+arch
    name='gs-qualification-'+os.environ['GITHUB_RUN_ID']+'-'+arch
    docker('build','--file',context/'Containerfile','--tag',image,context,timeout=600)
    created=False
    try:
        docker('run','--detach','--name',name,'--hostname','gs-cli-alpha1-fedora44-native-20261109',
            '--label','gitstore.qualification='+os.environ['GITHUB_RUN_ID'],'--privileged','--cgroupns=private',
            '--tmpfs','/run','--tmpfs','/tmp',image)
        created=True
        state=json.loads(docker('inspect',name,capture=True).stdout)[0]
        if state['HostConfig']['Binds'] or state['HostConfig']['CgroupnsMode']!='private' or state['HostConfig']['PortBindings']:
            raise ValueError('Fedora qualification must not bind host filesystems or ports')
        spec=importlib.util.spec_from_file_location('fedora_boot',ROOT/'tests/packaging/run-hosted-fedora.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        actual_namespace=docker('exec',name,'readlink','/proc/self/ns/cgroup',capture=True).stdout.strip()
        if actual_namespace==os.readlink('/proc/self/ns/cgroup'):
            raise ValueError('Fedora qualification must have a separate cgroup namespace')
        module.wait_for_systemd(lambda:docker('exec',name,'systemctl','is-system-running',capture=True,check=False).stdout.strip())
        docker('cp',ROOT/'tests/packaging/prepare-fedora-fixture.sh',name+':/root/prepare.sh')
        docker('exec','--env','GITSTORE_DISPOSABLE_FIXTURE=gs-cli-alpha1-fedora44-native-20261109',name,'sh','/root/prepare.sh')
        docker('cp',str(public),name+':'+account+'/qualification-input')
        docker('exec',name,'chown','-Rh','11000:11000',account+'/qualification-input')
        docker('exec',name,'install','-m','440',account+'/qualification-input/sudoers','/etc/sudoers.d/gitstore-qualification')
        user=['exec','--workdir',account,'--user','11000:11000',name,'env','-i','PATH=/usr/bin:/bin:/usr/sbin:/sbin','HOME='+account,'LANG=C']
        for command in setup:docker(*user,*command,timeout=300)
        docker(*user,*[key+'='+value for key,value in metadata.items()],*inner,timeout=5400)
        docker('cp',name+':'+account+'/qualification-receipts',str(args.output/'receipts'))
    finally:
        if created:
            state=json.loads(docker('inspect',name,capture=True).stdout)[0]
            if state['Config']['Labels'].get('gitstore.qualification')!=os.environ['GITHUB_RUN_ID']:
                raise ValueError('refuse cleanup of an unrelated fixture')
            docker('rm','--force',name)


if __name__=='__main__':main()
