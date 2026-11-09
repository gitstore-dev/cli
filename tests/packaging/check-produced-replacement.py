#!/usr/bin/env python3
"""Check real produced packages against isolated, never-publish next-alpha fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'runtime/packaging'))
from inventory import validate_install
from versions import validate_alpha_proposal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if os.geteuid() == 0 or not args.candidate.is_absolute() or not args.output.is_absolute() or args.output.exists():
        parser.error('select a production candidate and new absolute non-root fixture directory')
    manifests = list((args.candidate/'assets').glob('*.install.json'))
    if len(manifests) != 1:
        parser.error('select one actual production install inventory')
    inventory = json.loads(manifests[0].read_text())
    validate_install(inventory)
    old = (ROOT/'version.txt').read_text().strip()
    revision = subprocess.check_output(['git','rev-parse','HEAD'], cwd=ROOT, text=True).strip()
    if inventory['applicationVersion'] != old or inventory['sourceRevision'] != revision:
        parser.error('candidate must match the exact current source and version')
    version = old.rsplit('.',1)[0]+'.'+str(int(old.rsplit('.',1)[1])+1)
    validate_alpha_proposal(old, version)
    args.output.mkdir(mode=0o700)
    def run(*arguments, env=None):
        subprocess.run(list(map(str,arguments)), cwd=ROOT, check=True, env=env)
    run(sys.executable,ROOT/'tests/packaging/build-version-fixture.py',
        '--native-build',args.candidate/'native','--output',args.output/'built','--version',version)
    channels = ['archive','npm']+(['deb','rpm'] if inventory['os']=='linux' else [])
    run(sys.executable,ROOT/'tests/packaging/prepare-smoke.py','--pair',args.output/'built/bin',
        '--output',args.output/'packages','--version',version,'--revision',revision,'--channels',*channels)
    subjects = json.loads((args.candidate/'assets/subjects.json').read_text())
    tested = []
    for channel,suffix in [('archive','tar.gz'),('npm','tgz')]:
        selected = [row for row in subjects if row['kind']==channel]
        if len(selected)!=1:
            raise ValueError('select one actual package per channel')
        original = args.candidate/'assets'/selected[0]['filename']
        replacement, = (args.output/'packages').glob('UNQUALIFIED-ONLY-*.'+suffix)
        run(sys.executable,ROOT/'tests/packaging/run.py','--channel',channel,
            '--artifact',original,'--sha256',hashlib.sha256(original.read_bytes()).hexdigest(),
            '--version',old,'--replacement',replacement,
            '--replacement-sha256',hashlib.sha256(replacement.read_bytes()).hexdigest(),
            '--replacement-version',version,'--fixture-parent',args.output)
        tested.append(channel)
    if inventory['os']=='linux':
        manifest = args.output/'native-failure-fixture.json'
        manifest.write_text(json.dumps(dict(disposableUid=os.geteuid(),dedicatedTestAccount=True,
            fixtureParent=str(args.output))))
        manifest.chmod(0o600)
        # Dedicated ephemeral CI account; all state and the mismatched manager
        # are private to this experiment, with no inherited HOME or credentials.
        home = args.output/'failure-home';home.mkdir(mode=0o700)
        env = dict(PATH='/usr/bin:/bin',LANG='C',HOME=str(home),TMPDIR=str(args.output),
            GITSTORE_PACKAGE_PAIR=str(args.candidate/'prefix/bin'),
            GITSTORE_RUNTIME_FIXTURE=str(manifest),GITSTORE_MISMATCH_DAEMON=str(args.output/'built/bin/gitstored'))
        import shutil
        if shutil.which('patchelf',path=env['PATH']) is None:
            raise ValueError('select fixture patchelf; loader negative must not skip')
        run(sys.executable,'-m','unittest','discover','-s','tests/packaging','-p','test_native_failures.py','-v',env=env)
        tested += ['missing-native-library','running-version-mismatch']
    receipt = dict(status='passed',qualification='UNQUALIFIED replacement test only; never release qualification',
        sourceRevision=revision,sourceVersion=old,replacementVersion=version,executed=tested,
        replacement=json.loads((args.output/'built/UNQUALIFIED.json').read_text()))
    (args.output/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')


if __name__=='__main__':
    main()
