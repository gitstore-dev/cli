// Exercise the actual dependency/condition graph, without executing release jobs.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const yaml = require(require.resolve('yaml', { paths: [process.env.GITSTORE_RELEASE_PLEASE_MODULE] }));
const root = process.argv[2];
const read = name => yaml.parse(fs.readFileSync(path.join(root,'.github/workflows',name),'utf8'));
const workflow = read('release.yml');
const reusable = read('release-qualification.yml');
const runtime = read('runtime.yml');
assert.equal(workflow.on.workflow_dispatch.inputs.phase.default,'pipeline');
assert.deepEqual(workflow.on.push.tags,['v*']);
assert.equal(workflow.jobs.qualification.uses,'./.github/workflows/release-qualification.yml');
assert.equal(runtime.jobs.qualification.uses,workflow.jobs.qualification.uses);
assert.deepEqual(workflow.jobs.qualification.needs,['identity','assemble']);
assert.equal(workflow.jobs.publish.environment,'release-publication');
assert.deepEqual(workflow.jobs.homebrew.needs,'publish');
assert(!JSON.stringify(reusable).includes('secrets.'));
for (const job of Object.values(reusable.jobs)) {
  assert(Object.values(job.permissions || {}).every(permission=>permission==='read'));
}
assert.deepEqual(reusable.jobs.qualification.strategy.matrix.include.map(r=>`${r.host}/${r.arch}`).sort(),
  ['ubuntu:24.04/x64','ubuntu:24.04/arm64','fedora:44/x64','fedora:44/arm64','macos-15/arm64','macos-15-intel/x64'].sort());
assert.equal(reusable.jobs['qualification-complete'].needs,'qualification');
const collector = reusable.jobs['qualification-complete'].steps.find(s=>s.with?.pattern);
const receipt = reusable.jobs.qualification.steps.find(s=>s.with?.name?.startsWith('release-receipt-'));
assert(receipt);
assert.equal(collector.with.pattern,'release-receipt-*');
assert(!'qualification-controls-linux-x64'.startsWith(collector.with.pattern.slice(0,-1)));
// Implicit success() is part of GitHub's default job condition. Explicit status
// functions permit recovery jobs after intentionally skipped build dependencies.
function simulate(event,phase,failures={}) {
  const github={event_name:event}; const inputs={phase}; const results={};
  for (const [name,job] of Object.entries(workflow.jobs)) {
    const dependencies=Array.isArray(job.needs)?job.needs:job.needs?[job.needs]:[];
    const needs=Object.fromEntries(dependencies.map(id=>[id,{result:results[id],outputs:{tap_pr:'123'}}]));
    if(name==='identity'){results[name]=failures[name]||'success';continue;}
    const expression=job.if || 'true';
    const explicitStatus=/\b(always|success|failure|cancelled)\(/.test(expression);
    const allowed=(explicitStatus || dependencies.every(id=>results[id]==='success')) &&
      Function('github','inputs','needs','always',`return (${expression})`)(github,inputs,needs,()=>true);
    results[name]=allowed?(failures[name]||'success'):'skipped';
  }
  return results;
}
for(const [event,phase] of [['push',''],['workflow_dispatch','pipeline']]) {
  const result=simulate(event,phase);
  for(const name of ['build','assemble','qualification','qualify','attest','publish','homebrew']) assert.equal(result[name],'success');
  for(const failed of ['identity','build','assemble','qualification','qualify','attest']) {
    const result=simulate(event,phase,{[failed]:'failure'});
    assert.equal(result.publish,'skipped',`${failed} must block publication`);
    assert.equal(result.homebrew,'skipped');
  }
  assert.equal(simulate(event,phase,{qualification:'skipped'}).attest,'skipped');
}
const build=simulate('workflow_dispatch','build');
for(const name of ['qualification','qualify','attest','publish','homebrew']) assert.equal(build[name],'skipped');
const promote=simulate('workflow_dispatch','promote');
for(const name of ['build','assemble','qualification']) assert.equal(promote[name],'skipped');
for(const name of ['qualify','attest','publish','homebrew']) assert.equal(promote[name],'success');
const delivery=simulate('push','',{homebrew:'failure'});
assert.equal(delivery.publish,'success');
assert.equal(simulate('push','',{publish:'failure'}).homebrew,'skipped');
console.log(JSON.stringify({pipeline:true,buildOnly:true,recovery:true,failedGatesBlock:true,isolatedReceipts:true}));
