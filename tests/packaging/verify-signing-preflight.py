#!/usr/bin/env python3
"""Verify our signed text fixture and controls; never qualify a release package."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'runtime/packaging'))
from release_verification import REPOSITORY, REPOSITORY_ID, repository_identity, snapshot, verify

WORKFLOW=REPOSITORY+'/.github/workflows/runtime.yml'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('artifact','bundle','trusted-root','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--source-ref',required=True)
    parser.add_argument('--source-digest',required=True)
    args=parser.parse_args()
    if (not args.source_ref.startswith('refs/heads/') or '\n' in args.source_ref
            or not re.fullmatch('[a-f0-9]{40}',args.source_digest)):
        parser.error('Select the actual reviewed branch and source revision, never a release tag')
    if not args.output.is_absolute() or args.output.exists():
        parser.error('Select a new ordinary result path')
    gh=shutil.which('gh')
    if not gh:
        parser.error('Select a fixture with the maintainer GitHub CLI')
    with tempfile.TemporaryDirectory(prefix='gitstore-signing-preflight-') as temporary:
        root=Path(temporary)
        artifact=root/'fixture';digest=snapshot(args.artifact,artifact,65536)
        if not artifact.read_bytes().startswith(b'UNQUALIFIED_RELEASE_PREFLIGHT\n'):
            parser.error('Only the generated signing-test text is permitted')
        bundle=root/'bundle.jsonl';snapshot(args.bundle,bundle,4*1024*1024)
        trust=root/'trusted-root.jsonl';snapshot(args.trusted_root,trust,4*1024*1024)
        environment=dict(PATH='/usr/bin:/bin',HOME=str(root),GH_CONFIG_DIR=str(root/'gh'),
            GH_HOST='github.com',GH_TOKEN='offline-verification-placeholder',GH_PROMPT_DISABLED='1',
            LANG='C',NO_COLOR='1')
        command=[gh,'attestation','verify',str(artifact),'--repo',REPOSITORY,
            '--signer-workflow',WORKFLOW,'--cert-identity',f'https://github.com/{WORKFLOW}@{args.source_ref}',
            '--cert-oidc-issuer','https://token.actions.githubusercontent.com','--source-ref',args.source_ref,
            '--source-digest',args.source_digest,'--deny-self-hosted-runners',
            '--predicate-type','https://slsa.dev/provenance/v1','--bundle',str(bundle),
            '--custom-trusted-root',str(trust),'--format','json']
        def run(arguments):
            return subprocess.run(arguments,env=environment,stdin=subprocess.DEVNULL,
                capture_output=True,timeout=60,check=False)
        positive=run(command)
        if positive.returncode or len(positive.stdout)>16*1024*1024 or not repository_identity(json.loads(positive.stdout)):
            raise ValueError('Own cryptographic signing or immutable repository identity verification failed')
        altered=root/'altered';altered.write_bytes(artifact.read_bytes()+b'altered\n')
        controls=[]
        for name,flag,value in (
                ('altered-artifact',None,str(altered)),
                ('wrong-workflow','--signer-workflow',REPOSITORY+'/.github/workflows/release.yml'),
                ('wrong-source','--source-digest','0'*40),
                ('missing-bundle','--bundle',str(root/'absent.jsonl'))):
            negative=list(command)
            if flag is None: negative[3]=value
            else: negative[negative.index(flag)+1]=value
            if run(negative).returncode==0:
                raise ValueError('A signing negative control was accepted: '+name)
            controls.append(dict(case=name,status='rejected'))
        try:
            verify(artifact,bundle,trust,'refs/tags/v0.1.0-alpha.1',args.source_digest,digest)
        except ValueError as error:
            if str(error)!='cryptographic provenance or producer policy verification failed':
                raise
        else:
            raise ValueError('The release producer gate must reject this branch test fixture')
        result=dict(schemaVersion=1,status='passed',scope='signed branch test fixture only; not release qualification',
            repository=REPOSITORY,repositoryId=REPOSITORY_ID,workflow=WORKFLOW,sourceRef=args.source_ref,
            sourceDigest=args.source_digest,sha256=digest,cryptographicPositive=True,
            controls=controls,releaseProducerPolicyRejected=True)
        fd=os.open(args.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        with os.fdopen(fd,'w') as output:
            json.dump(result,output,indent=2);output.write('\n')
        print(json.dumps(result))


if __name__=='__main__':
    main()
