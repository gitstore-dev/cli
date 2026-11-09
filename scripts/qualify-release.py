#!/usr/bin/env python3
"""Execute selected exact-byte package/native checks and emit host receipts.

Only for maintainer-selected clean, dedicated reference hosts. No unavailable
case is converted to a pass. Receipts are execution records, not attestations.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'runtime/packaging'))
sys.path.insert(0,str(ROOT/'tests/packaging'))
from support import owned_file
from archive_lifecycle import unpack
from release_candidate import qualification_rows, slot_filename, validate_bytes
from versions import order_key
from command_measurements import measure, validate_performance
sys.path.remove(str(ROOT/'tests/packaging'))


def selection(host, manifest):
    os_name = 'darwin' if sys.platform == 'darwin' else 'linux'
    arch = {'aarch64':'arm64','arm64':'arm64','x86_64':'x64'}.get(platform.machine())
    if (os.geteuid() == 0 or manifest.get('schemaVersion') != 1 or
            manifest.get('dedicatedTestAccount') is not True or manifest.get('cleanHost') is not True or
            manifest.get('disposableHost') is not True or manifest.get('disposableUid') != os.geteuid() or
            manifest.get('selection') != [os_name,arch,host]):
        raise ValueError('select an explicitly clean disposable host and dedicated non-root account')
    if os_name == 'linux':
        release = platform.freedesktop_os_release()
        if host != release['ID']+':'+release['VERSION_ID']:
            raise ValueError('selected reference OS differs from actual host')
    elif platform.mac_ver()[0].split('.')[0] != '15' or host != ('macos-15' if arch == 'arm64' else 'macos-15-intel'):
        raise ValueError('macOS qualification requires the actual selected macOS 15 reference host')
    channels = sorted(r[3] for r in qualification_rows() if r[:3] == (os_name,arch,host))
    if not channels:
        raise ValueError('host is outside the selected qualification matrix')
    return os_name, arch, channels


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--candidate',type=Path,required=True)
    p.add_argument('--fixture',type=Path,required=True)
    p.add_argument('--host',required=True)
    p.add_argument('--revision',required=True)
    p.add_argument('--tag',required=True)
    p.add_argument('--output',type=Path,required=True)
    args = p.parse_args()
    owned_file(args.fixture)
    if args.fixture.stat().st_mode & 0o077 or args.fixture.stat().st_size > 65536:
        raise ValueError('qualification manifest must be private and bounded')
    settings = json.loads(args.fixture.read_text())
    os_name,arch,channels = selection(args.host,settings)
    root = args.candidate
    if (not root.is_absolute() or root.is_symlink() or root.stat().st_uid != os.geteuid()
            or root.stat().st_mode & 0o077):
        p.error('select an owned private absolute candidate directory')
    value = json.loads(owned_file(root/'candidate.json').read_text())
    if value['state'] != 'packaged' or value['sourceRevision'] != args.revision or value['tag'] != args.tag:
        raise ValueError('candidate does not match this reviewed source/tag')
    if subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip() != args.revision:
        raise ValueError('test harness must use the exact candidate source revision')
    validate_bytes(value,root)
    parent = Path(settings['fixtureParent'])
    if not parent.is_absolute() or parent.is_symlink() or parent.stat().st_uid != os.geteuid() or parent.stat().st_mode & 0o077:
        raise ValueError('select an owned private experiment parent')
    if not args.output.is_absolute() or args.output.exists():
        p.error('select a new absolute receipt directory')
    # Keep AF_UNIX control and recovery relay paths below sun_path's limit.
    workspace = Path(tempfile.mkdtemp(prefix='q-',dir=parent)).resolve()
    workspace.chmod(0o700)
    (workspace/'home').mkdir(mode=0o700)
    environment = dict(PATH=os.environ.get('PATH','/usr/bin:/bin'),LANG='C',HOME=str(workspace/'home'))
    artifacts = {a['filename']:a for a in value['artifacts']}
    version = value['version']
    # Validate all selected replacement identities before the first install.
    replacements = settings['replacements']
    if set(replacements) != set(channels):
        raise ValueError('each selected channel requires an explicit next-alpha fixture')
    for replacement in replacements.values():
        path = owned_file(Path(replacement['path']))
        if hashlib.sha256(path.read_bytes()).hexdigest() != replacement['sha256']:
            raise ValueError('replacement fixture digest mismatch')
        if order_key(replacement['version']) <= order_key(version):
            raise ValueError('replacement must advance to a later alpha')
    if os_name == 'linux':
        if not shutil.which('patchelf'):
            raise ValueError('native loader negative control requires fixture patchelf')
        native = json.loads(owned_file(Path(settings['nativeFixture'])).read_text())
        if native.get('sourceRevision') != args.revision or native.get('disposableUid') != os.geteuid():
            raise ValueError('native fixture requires this exact reviewed source and account')
        system = json.loads(owned_file(Path(settings['systemFixture'])).read_text())
        if system.get('disposableUid') != os.geteuid() or system.get('fixtureParent') != str(parent):
            raise ValueError('privileged package checks must select this same disposable account/parent')
    executed = []
    def run(name, command, env=None):
        with (workspace/(name+'.log')).open('wb') as log:
            subprocess.run(command,cwd=ROOT,env=environment if env is None else env,stdin=subprocess.DEVNULL,
                stdout=log,stderr=subprocess.STDOUT,check=True,timeout=1800)
        executed.append(name)
    for channel in channels:
        name = slot_filename(version,'archive' if channel == 'homebrew' else channel,os_name,arch)
        replacement = replacements[channel]
        command = [sys.executable,str(ROOT/'tests/packaging/run.py'),'--channel',channel,
            '--artifact',str(root/name),'--sha256',artifacts[name]['sha256'],'--version',version,
            '--fixture-parent',str(parent),'--replacement',replacement['path'],
            '--replacement-sha256',replacement['sha256'],'--replacement-version',replacement['version']]
        if channel == 'homebrew':
            formula = slot_filename(version,'formula','any','any')
            command += ['--brew-source',settings['brewSource'],'--brew-revision',settings['brewRevision'],
                '--formula',str(root/formula),'--formula-sha256',artifacts[formula]['sha256'],
                '--replacement-formula',replacement['formula'],'--replacement-formula-sha256',replacement['formulaSha256']]
        if channel in ('deb','rpm'):
            command = ['sudo','--non-interactive',*command,'--system-fixture',settings['systemFixture']]
        run('package-'+channel,command)
    archive = slot_filename(version,'archive',os_name,arch)
    prefix = workspace/'installed'
    unpack(root/archive,artifacts[archive]['sha256'],version,prefix)
    if os_name == 'linux':
        native.update(gs=str(prefix/'bin/gs'),daemon=str(prefix/'bin/gitstored'),sourceRoot=str(ROOT))
        cold = workspace/'cold'
        cold.mkdir(mode=0o700)
        native['fixtureParent'] = str(cold)
        fixture = workspace/'native.json'
        fixture.write_text(json.dumps(native))
        fixture.chmod(0o600)
        env = dict(environment,GITSTORE_PACKAGE_PAIR=str(prefix/'bin'),GITSTORE_RUNTIME_FIXTURE=str(fixture),
            GITSTORE_MISMATCH_DAEMON=settings['mismatchDaemon'])
        run('native-prerequisites',[sys.executable,str(ROOT/'tests/packaging/check-linux-fixture.py'),
            '--fixture',str(fixture)],env)
        for test in ('test_native_autostart.py','test_native_failures.py'):
            run(test,[sys.executable,'-m','unittest','discover','-s','tests/packaging','-p',test,'-v'],env)
        run('native-cold',[sys.executable,str(ROOT/'tests/runtime/run.py'),'--scenario','core-memdb',
            '--case','test_authorized_cold_start_requires_current_generation_health','--fixture',str(fixture)])
        roots = list(cold.glob('gs-public-runtime-*'))
        records = list(cold.glob('runtime-run-*.json'))
        if len(roots)!=1 or len(records)!=1:
            raise ValueError('require one exact-pair cold start before preparing offline layouts')
        record = json.loads(records[0].read_text())
        if record['status']!='passed' or any(record['executables'][key]['sha256'] !=
                hashlib.sha256((prefix/'bin'/name).read_bytes()).hexdigest()
                for name,key in [('gs','gs'),('gitstored','daemon')]):
            raise ValueError('cold-start receipt must bind this exact installed pair')
        layouts = workspace/'offline'
        run('prepare-offline',[sys.executable,str(ROOT/'tests/packaging/prepare-offline-cache.py'),
            '--cache',str(roots[0]/'state/assets'),'--output',str(layouts)])
        batch = workspace/'a'
        batch.mkdir(mode=0o700)
        native.update(fixtureParent=str(batch),offlineLayouts=json.loads((layouts/'layouts.json').read_text()))
        fixture.write_text(json.dumps(native))
        for scenario in ('core-offline','clean-install','recovery'):
            run('native-'+scenario,[sys.executable,str(ROOT/'tests/runtime/run.py'),
                '--scenario',scenario,'--fixture',str(fixture)])
        for case in ('test_interrupted_offline_blob_never_activates',
                     'test_tampered_warm_blob_cannot_be_reused_for_execution'):
            run(case,[sys.executable,str(ROOT/'tests/runtime/run.py'),'--scenario','exp-003b',
                '--case',case,'--fixture',str(fixture)])
        for binary in ('gs','gitstored'):
            run('closure-'+binary,[sys.executable,str(ROOT/'runtime/packaging/closure.py'),
                '--binary',str(prefix/'bin'/binary)])
        costs = workspace/'native-costs.json'
        run('native-costs',[sys.executable,str(ROOT/'tests/packaging/measure-native.py'),
            '--fixture',str(fixture),'--output',str(costs)],env)
        performance = json.loads(costs.read_text())
    else:
        performance = measure(prefix/'bin',workspace/'command-costs',100)
        executed.append('command-costs')
        performance['sourceInputs']={name:dict(sha256=hashlib.sha256((prefix/'bin'/name).read_bytes()).hexdigest(),
            bytes=(prefix/'bin'/name).stat().st_size) for name in ('gs','gitstored')}
    validate_performance(performance,os_name == 'linux')
    # Emit nothing until every channel and required native case on this host passes.
    args.output.mkdir(mode=0o700)
    subjects = {a['filename']:a['sha256'] for a in value['artifacts']}
    for channel in channels:
        receipt = dict(schemaVersion=1,result='pass',tag=value['tag'],sourceRevision=args.revision,
            selection=[os_name,arch,args.host,channel],subjects=subjects,executed=executed,
            replacements=replacements,host=platform.platform(),performance=performance)
        target = args.output/(os_name+'-'+arch+'-'+args.host.replace(':','-')+'-'+channel+'.json')
        target.write_text(json.dumps(receipt,indent=2)+'\n')
        target.chmod(0o600)
    print(json.dumps(dict(receipts=str(args.output),executionLogs=str(workspace))))


if __name__ == '__main__':
    main()
