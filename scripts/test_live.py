"""Opt-in live checks against caller-provisioned disposable resources only."""
import argparse, json, os, pathlib, subprocess, tempfile, uuid
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--allow-live',action='store_true',required=True)
p.add_argument('--config',type=pathlib.Path,required=True)
p.add_argument('--credential-file',type=pathlib.Path,required=True)
p.add_argument('--namespace',required=True,help='A dedicated existing namespace named gs-m1-*')
p.add_argument('--repository',help='Optional existing fixture-owned repository for read/clone/no-op sync')
a=p.parse_args()
if not a.namespace.startswith('gs-m1-'):p.error('Use an existing disposable gs-m1-* namespace')
root=pathlib.Path(__file__).resolve().parent.parent
binary=root/'target/debug/gs'
env={k:v for k,v in os.environ.items() if not k.startswith(('GITSTORE_','GIT_'))}
base=[str(binary),'--config',str(a.config.resolve()),'--namespace',a.namespace,'--credential-file',str(a.credential_file.resolve()),'--non-interactive','-o','json']
def run(args,cwd=None):
    r=subprocess.run([*base,*args],env=env,cwd=cwd,capture_output=True,text=True,timeout=40)
    if r.returncode: raise RuntimeError(f'{args[0]} failed with exit {r.returncode}; inspect the disposable installation before retrying')
    return json.loads(r.stdout)
run(['read','namespace',a.namespace])
with tempfile.TemporaryDirectory(prefix='gs-live-') as temp:
    temp=pathlib.Path(temp)
    for kind,cli_kind in [('Product','product'),('CategoryTaxonomy','category')]:
        name='gs-m1-'+uuid.uuid4().hex[:12]
        doc=temp/(name+'.md');doc.write_text('---\n'+json.dumps({'apiVersion':'catalog.gitstore.dev/v1beta1','kind':kind,'metadata':{'name':name,'namespace':a.namespace},'spec':{'title':'Disposable CLI acceptance fixture'}},ensure_ascii=False)+'\n---\nCafé\n\n')
        # On uncertain create failure, retain the exact fixture name in the error;
        # do not blindly retry or delete an object whose identity was not verified.
        try: created=run(['create','-f',str(doc)])
        except RuntimeError as error: raise RuntimeError(f'{error}; fixture name {name}') from error
        item=created['items'][0];identity=item['resource']['id']
        try:
            observed=run(['read',cli_kind,'--id',identity]);assert observed['metadata']['name']==name
            assert observed['body']=='Café\n\n'
            run(['list',cli_kind,'--first','1'])
        finally:
            # Only the exact server-returned fixture identity is eligible for cleanup.
            result=run(['delete',cli_kind,'--id',identity]);assert result['outcome'] in ['TERMINATION_STARTED','ALREADY_TERMINATING']
    if a.repository:
        run(['read','repository',a.repository])
        dest=temp/'clone'
        run(['repository','clone',a.repository,str(dest)])
        # No commit is created; this checks the no-new-submission path only.
        result=run(['repository','sync',a.repository],cwd=dest)
        assert result['submission']=='not-submitted'
print('PASS: opted-in live inspection, fixture create/read/list/delete and optional read/clone/no-op sync. No readiness or full-M1 release claim.')
