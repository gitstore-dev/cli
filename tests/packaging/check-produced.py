#!/usr/bin/env python3
"""Exercise complete locally produced packages without publishing or qualifying a host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'runtime/packaging'))
from command_measurements import measure


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    if os.geteuid()==0 or not args.candidate.is_absolute() or not args.output.is_absolute() or args.output.exists():
        parser.error('select an existing local candidate and new absolute non-root experiment directory')
    args.output.mkdir(mode=0o700)
    if sys.platform == 'linux':
        finalization = json.loads((args.candidate/'native/finalization.json').read_text())
        relink = json.loads((args.candidate/'relink/relink.json').read_text())
        binary = args.candidate/'prefix/bin/gitstored'
        actual = hashlib.sha256(binary.read_bytes()).hexdigest()
        if (finalization['operation'] != 'strip-debug' or finalization['sha256'] != actual or
                relink['originalSha256'] != actual or relink['finalization'] != finalization):
            raise ValueError('production bytes and relink transformation evidence disagree')
        sections = subprocess.check_output(['readelf', '--sections', str(binary)])
        if b'.debug_' in sections or b'.symtab' not in sections:
            raise ValueError('production ELF must omit debug data and retain ordinary symbols')
        native = args.candidate/'native/gitstored'
        if hashlib.sha256(native.read_bytes()).hexdigest() != actual:
            raise ValueError('installed daemon differs from finalized native ELF')
    subjects=json.loads((args.candidate/'assets/subjects.json').read_text())
    version=(ROOT/'version.txt').read_text().strip()
    tested=[]
    for channel in ('archive','npm'):
        selected=[entry for entry in subjects if entry['kind']==channel]
        if len(selected)!=1:
            raise ValueError('candidate must contain one archive and one platform npm package')
        name=selected[0]['filename']
        if Path(name).name!=name:
            raise ValueError('candidate package name must be an ordinary basename')
        artifact=args.candidate/'assets'/name
        digest=hashlib.sha256(artifact.read_bytes()).hexdigest()
        subprocess.run([sys.executable,str(ROOT/'tests/packaging/run.py'),'--channel',channel,
            '--artifact',str(artifact),'--sha256',digest,'--version',version,
            '--fixture-parent',str(args.output)],check=True)
        tested.append(dict(channel=channel,filename=name,sha256=digest,bytes=artifact.stat().st_size))
    record=measure(args.candidate/'prefix/bin',args.output/'command-costs',100)
    record['qualification']='controlled production package lifecycle and warm command measurement only; no release qualification'
    record['packages']=tested
    if sys.platform == 'linux':
        record['finalization'] = finalization
    record['sourceInputs']={name:dict(sha256=hashlib.sha256((args.candidate/'prefix/bin'/name).read_bytes()).hexdigest(),
        bytes=(args.candidate/'prefix/bin'/name).stat().st_size) for name in ('gs','gitstored')}
    with (args.output/'measurement.json').open('x') as stream:
        json.dump(record,stream,indent=2)
        stream.write('\n')


if __name__=='__main__':
    main()
