#!/usr/bin/env python3
"""Non-release replay of package/prerequisite failures; emits no release receipts."""
import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
ACCOUNT = '/home/gitstorefixture'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def reveal_captured_failures():
    original = subprocess.run
    def run(*args, **kwargs):
        try:
            result = original(*args, **kwargs)
        except subprocess.CalledProcessError as error:
            for payload in (error.stdout, error.stderr):
                if payload:
                    print(payload[-16384:].decode(errors='replace') if isinstance(payload, bytes) else payload[-16384:], file=sys.stderr, flush=True)
            raise
        if kwargs.get('capture_output') and (result.returncode or result.stderr):
            for payload in (result.stdout, result.stderr):
                if payload:
                    print(payload[-16384:].decode(errors='replace') if isinstance(payload, bytes) else payload[-16384:], file=sys.stderr, flush=True)
        return result
    subprocess.run = run


def inside(args):
    if os.geteuid() != 11000:
        raise ValueError('select the diagnostic fixture user')
    reveal_captured_failures()
    sys.path[:0] = [str(ROOT/'tests/packaging'), str(ROOT/'runtime/packaging')]
    if args.case == 'prerequisites':
        parent = Path(ACCOUNT)/'diagnostic'; parent.mkdir(mode=0o700)
        fixture = parent/'native.json'
        fixture.write_text(json.dumps(dict(fixtureName='nonrelease-diagnostic', disposableUid=11000,
            dedicatedTestAccount=True, fixtureParent=str(parent))))
        fixture.chmod(0o600)
        sys.argv = ['check-linux-fixture.py', '--fixture', str(fixture)]
        module('prerequisites', ROOT/'tests/packaging/check-linux-fixture.py').main()
        return
    import hashlib
    from inventory import validate_install
    arch = {'aarch64':'arm64', 'x86_64':'x64'}[platform.machine()]
    version = '0.1.0-alpha.3'; replacement = '0.1.0-alpha.4'
    original = args.inputs/'platform'
    controls = args.inputs/'replacement'
    manifest = json.loads((original/f'gitstore-{version}-linux-{arch}.install.json').read_text())
    validate_install(manifest)
    if manifest['sourceRevision'] != '6dffa2c08223209ebfd4c0bb01757e82b8751892':
        raise ValueError('diagnostic input is not the reviewed alpha.3 product source')
    result = json.loads((controls/'result.json').read_text())
    if (result['sourceRevision'],result['sourceVersion'],result['replacementVersion']) != (manifest['sourceRevision'],version,replacement):
        raise ValueError('diagnostic replacement input mismatch')
    for name in ('node','npm'):
        print('DIAGNOSTIC executable:', name, shutil.which(name), flush=True)
        subprocess.run([name, '--version'], check=True, timeout=20)
    parent = Path(ACCOUNT)/'diagnostic'; parent.mkdir(mode=0o700)
    artifact = original/f'gitstore-npm-{version}-linux-{arch}.tgz'
    next_artifact = controls/'packages'/f'UNQUALIFIED-ONLY-{replacement}-linux-{arch}.tgz'
    sys.argv = ['run.py','--channel','npm','--artifact',str(artifact),
        '--sha256',hashlib.sha256(artifact.read_bytes()).hexdigest(),'--version',version,
        '--fixture-parent',str(parent),'--replacement',str(next_artifact),
        '--replacement-sha256',hashlib.sha256(next_artifact.read_bytes()).hexdigest(),
        '--replacement-version',replacement]
    module('package_runner', ROOT/'tests/packaging/run.py').main()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--case', choices=['npm','prerequisites'], required=True)
    p.add_argument('--inputs', type=Path)
    p.add_argument('--inside', action='store_true')
    args = p.parse_args()
    if args.inside:
        return inside(args)
    outer = module('hosted', ROOT/'tests/packaging/run-hosted-qualification.py')
    arch = outer.host_selection(os.environ,sys.platform,platform.machine(),os.geteuid())
    if platform.freedesktop_os_release().get('VERSION_ID') != '24.04':
        raise ValueError('select the disposable hosted Ubuntu 24.04 infrastructure')
    print('NONRELEASE DIAGNOSTIC',json.dumps(dict(toolingRevision=os.environ['GITHUB_SHA'],
        productRevision='6dffa2c08223209ebfd4c0bb01757e82b8751892',arch=arch,case=args.case)),flush=True)
    parent = Path(os.environ['RUNNER_TEMP'])/'qualification-diagnostic'; parent.mkdir(mode=0o700)
    source = parent/'source'; source.mkdir(mode=0o755)
    payload = subprocess.check_output(['git','archive','HEAD'],cwd=ROOT)
    with tarfile.open(fileobj=io.BytesIO(payload)) as archive:
        archive.extractall(source,filter='data')
    if args.case == 'npm':
        subprocess.run(['sudo','useradd','--uid','11000','--user-group','--create-home','gitstorefixture'],check=True)
        subprocess.run(['sudo','chmod','700',ACCOUNT],check=True)
        for original,name in [(source,'source'),(args.inputs,'inputs')]:
            subprocess.run(['sudo','cp','-a',str(original),ACCOUNT+'/'+name],check=True)
            subprocess.run(['sudo','chown','-Rh','11000:11000',ACCOUNT+'/'+name],check=True)
        user=['sudo','-u','gitstorefixture','env','-i','PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin','HOME='+ACCOUNT,'LANG=C']
        outer.run_fixture_setup(user,[['python3','-m','venv',ACCOUNT+'/venv'],
            [ACCOUNT+'/venv/bin/pip','install','--require-hashes','--only-binary=:all:','-r',ACCOUNT+'/source/packaging/requirements.txt'],
            [ACCOUNT+'/venv/bin/python',ACCOUNT+'/source/tests/packaging/diagnose-linux-qualification.py','--inside','--case','npm','--inputs',ACCOUNT+'/inputs']],ACCOUNT,timeout=600)
        return
    config = parent/'docker-config'; config.mkdir(mode=0o700)
    environment=dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin',HOME=str(parent),DOCKER_CONFIG=str(config),LANG='C')
    def docker(*arguments,capture=False,check=True,timeout=600):
        return subprocess.run(['/usr/bin/docker','--host','unix:///var/run/docker.sock',*map(str,arguments)],
            env=environment,check=check,text=True,capture_output=capture,timeout=timeout)
    if docker('ps','--all','--quiet',capture=True).stdout.strip():
        raise ValueError('refuse a Docker host with pre-existing containers')
    context = parent/'context'; context.mkdir(mode=0o700)
    (context/'Containerfile').write_text((ROOT/'tests/fixtures/runtime/fedora44-native.Containerfile').read_text()+
        '\nRUN dnf -y install sudo git gcc make glibc-devel file curl bzip2 patch python3-pip python3-jsonschema /usr/bin/npm patchelf libseccomp libcap json-c libcurl openssl-libs libxcrypt && dnf clean all\n')
    name = 'gs-diagnostic-'+os.environ['GITHUB_RUN_ID']+'-'+arch
    docker('build','--file',context/'Containerfile','--tag',name,context)
    docker('run','--detach','--name',name,'--label','gitstore.diagnostic='+os.environ['GITHUB_RUN_ID'],
        '--hostname','gs-cli-alpha1-fedora44-native-20261109','--privileged','--cgroupns=private','--tmpfs','/run','--tmpfs','/tmp',name)
    try:
        state=json.loads(docker('inspect',name,capture=True).stdout)[0]
        if state['HostConfig']['Binds'] or state['HostConfig']['PortBindings'] or state['HostConfig']['CgroupnsMode']!='private':
            raise ValueError('diagnostic guest must not expose host filesystems or ports')
        boot=module('fedora_boot',ROOT/'tests/packaging/run-hosted-fedora.py')
        boot.wait_for_systemd(lambda:docker('exec',name,'systemctl','is-system-running',capture=True,check=False).stdout.strip())
        docker('cp',ROOT/'tests/packaging/prepare-fedora-fixture.sh',name+':/root/prepare.sh')
        docker('exec','--env','GITSTORE_DISPOSABLE_FIXTURE=gs-cli-alpha1-fedora44-native-20261109',name,'sh','/root/prepare.sh')
        docker('cp',source,name+':/opt/qualification-source')
        docker('exec','--workdir',ACCOUNT,'--user','11000:11000',name,'env','-i','PATH=/usr/bin:/bin:/usr/sbin:/sbin','HOME='+ACCOUNT,'LANG=C',
            'python3','/opt/qualification-source/tests/packaging/diagnose-linux-qualification.py','--inside','--case','prerequisites')
    finally:
        state=json.loads(docker('inspect',name,capture=True).stdout)[0]
        if state['Config']['Labels'].get('gitstore.diagnostic')!=os.environ['GITHUB_RUN_ID']:
            raise ValueError('refuse cleanup of an unrelated container')
        docker('rm','--force',name)


if __name__ == '__main__':
    main()
