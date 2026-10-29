"""Exercise the built gs process through the local Postman simulation; synthetic data only."""
import json, os, pathlib, selectors, shutil, subprocess, tempfile, time, urllib.request
ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = ROOT / 'target/debug/gs'
proc = subprocess.Popen([shutil.which('postman') or 'postman', 'simulation', 'run',
    str(ROOT / 'postman/simulations/m1.sim.yaml'), '--port', 'auto', '--output', 'ndjson', '--no-history'],
    cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    selector = selectors.DefaultSelector(); selector.register(proc.stdout, selectors.EVENT_READ)
    deadline = time.monotonic()+60; address = None
    while time.monotonic()<deadline:
        if proc.poll() is not None:
            raise RuntimeError('Simulation failed: '+proc.stderr.read())
        if not selector.select(1): continue
        line = proc.stdout.readline()
        try: event = json.loads(line)
        except json.JSONDecodeError: continue
        if event.get('event') == 'listening': address = event['url']+'/gitstore'; break
    if not address: raise RuntimeError('Simulation did not become ready within 60 seconds')
    with tempfile.TemporaryDirectory(prefix='gs-simulation-') as temp:
        temp = pathlib.Path(temp); token = temp/'credential'; token.write_text('synthetic-simulation-token\n'); token.chmod(0o600)
        cfg=temp/'config.toml'
        def configure(suffix=''):
            cfg.write_text(f'''version = 1
current_context = "test"
[contexts.test]
endpoint = "{address}/graphql{suffix}"
allow_loopback_http = true
contract_profile = "m0-2026-10-27"
account = "test"
namespace = "shop"
[contexts.test.accounts.test]
issuer = "controlled-fixture"
''')
        def invoke(args, code=0, suffix=''):
            configure(suffix)
            env={k:v for k,v in os.environ.items() if not k.startswith(('GITSTORE_','GIT_'))}
            env.update(HOME=str(temp),PATH='/nonexistent')
            out=subprocess.run([str(BINARY),'--config',str(cfg),'--credential-file',str(token),'-o','json',*args],env=env,cwd=temp,capture_output=True,text=True,timeout=10)
            assert out.returncode==code,(args,out.returncode,out.stderr)
            assert 'synthetic-simulation-token' not in out.stdout+out.stderr
            return json.loads(out.stdout) if out.stdout else None
        v=invoke(['api','graphql','-f','query=query Echo($n:Int){echo}', '-F','n=3']); assert v['data']['echo']['n']==3
        v=invoke(['api','graphql','-f','query={echo}'],3,'/partial'); assert v['data']['example'] is None and v['errors'] and v['extensions']['trace']=='fixture'
        invoke(['api','graphql','-f','query={echo}'],3,'/unauthorized')
        invoke(['api','graphql','-f','query={echo}'],3,'/denied')
        invoke(['api','graphql','-f','query={echo}'],8,'/rate')
        invoke(['api','graphql','-f','query={echo}'],8,'/malformed')
        invoke(['api','graphql','-f','query={echo}'],8,'/oversized')
        invoke(['--timeout','100ms','api','graphql','-f','query={echo}'],7,'/timeout')
        invoke(['api','graphql','-f','query=mutation { fixture }'],8,'/disconnect')
        for kind in ['namespace','repository','product','category','collection','file']:
            value=invoke(['read',kind,'fixture']); assert value['metadata']['name']=='fixture'
        for kind in ['namespace','repository','product','category','collection']:
            value=invoke(['list',kind]); assert value['items']==[] and not value['pageInfo']['hasNextPage']
        for kind in ['Product','CategoryTaxonomy']:
            document=temp/(kind+'.md'); document.write_text(f'''---
apiVersion: catalog.gitstore.dev/v1beta1
kind: {kind}
metadata:
  name: {kind.lower()}
  namespace: shop
spec:
  title: Unicode café
---
Body ü\n\n''')
            result=invoke(['create','-f',str(document)]); assert result['items'][0]['submission']=='accepted';assert result['items'][0]['resource']['body']=='Body ü\n\n'
            kind_name='product' if kind=='Product' else 'category'
            assert invoke(['read',kind_name,kind.lower()])['body']=='Body ü\n\n'
            assert invoke(['delete',kind_name,kind.lower()])['outcome']=='TERMINATION_STARTED'
        for kind in ['namespace','repository']:
            assert invoke(['delete',kind,'fixture'])['outcome']=='TERMINATION_STARTED'
        good=temp/'batch-good.md'; bad_remote=temp/'batch-fail.md'; unsubmitted=temp/'batch-last.md'
        source=(temp/'Product.md').read_text()
        for path,name in [(good,'batch-good'),(bad_remote,'fail-fixture'),(unsubmitted,'must-not-submit')]:
            path.write_text(source.replace('name: product','name: '+name))
        batch=invoke(['create','-f',str(good),'-f',str(bad_remote),'-f',str(unsubmitted)],3)
        assert [x['submission'] for x in batch['items']]==['accepted','unknown','not-submitted']
        def calls(): return json.load(urllib.request.urlopen(address+'/calls'))
        before=len(calls())
        invoke(['read','product','fixture','-o','yaml'],6)
        invoke(['list','file'],6)
        invoke(['repository','sync','--wait','ready'],6)
        bad=temp/'bad.md';bad.write_text('---\nkind: Product\n---\n')
        invoke(['create','-f',str(temp/'Product.md'),'-f',str(bad)],2)
        assert len(calls())==before,'Preflight failure performed a network request'
        recorded=calls();assert any('repositories(' in x['query'] for x in recorded)
        assert sum(x['query']=='{echo}' for x in recorded)>=8
        collection = subprocess.run([shutil.which('postman') or 'postman', 'collection', 'run', str(ROOT/'postman/collections/gitstore'), '--env-var', 'baseUrl='+address], cwd=ROOT, text=True, capture_output=True, timeout=30)
        print(collection.stdout, end='')
        if collection.stderr: print(collection.stderr, end='')
        assert collection.returncode == 0, 'Saved contract collection failed'
        print(f'PASS: actual gs CLI exercised {len(recorded)} controlled GraphQL requests, failure scenarios, authoring/read/delete and zero-write gates.')
finally:
    proc.terminate()
    try: proc.wait(timeout=5)
    except subprocess.TimeoutExpired: proc.kill();proc.wait()
