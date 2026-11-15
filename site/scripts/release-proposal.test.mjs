import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import test from 'node:test';
import {assessProposal,proposedAudit,auditedSourcePaths,sourceHashes} from './prepare-release-docs.mjs';
import {validateChangedFiles,validateIdentity,validateProposalData} from './check-docs-proposal.mjs';
import {renderReleaseContent} from './release-content.mjs';
const root=fileURLToPath(new URL('../../',import.meta.url));
async function fixture(){
 const read=p=>readFile(new URL(`../../${p}`,import.meta.url),'utf8');
 const release=JSON.parse(await read('site/release.json'));
 const baselineAudit=JSON.parse(await read('site/content-audit.json'));
 const sources=Object.fromEntries(await Promise.all(auditedSourcePaths.map(async p=>[p,await read(p)])));
 Object.assign(sources,renderReleaseContent({release,sources}));
 return {repoRoot:root,release,baselineAudit,sources,candidateAudit:baselineAudit,reference:await read('site/reference/gs.md')};
}
test('trusted baseline carries forward unchanged capabilities; generated versions do not alter prose hashes',async()=>{
 const f=await fixture(); const report=assessProposal(f);
 assert.equal(report.status,'unchanged');assert.deepEqual(report.authoredChanges,[]);
 assert.deepEqual(sourceHashes(f.sources),f.baselineAudit.sourceSha256);
 assert.deepEqual(proposedAudit({...f,report}),f.baselineAudit);
 validateProposalData({...f,audit:f.baselineAudit,evidence:report});
});
test('authored changes require review even when bundled command help is identical',async()=>{
 const f=await fixture();f.sources['docs/remote-cli.md']+='\nNew support claim.\n';
 const report=assessProposal(f);assert.equal(report.status,'review-required');assert.deepEqual(report.authoredChanges,['docs/remote-cli.md']);
 const audit=proposedAudit({...f,report});validateProposalData({...f,audit,evidence:report});
 assert.throws(()=>validateProposalData({...f,audit,evidence:{...report,status:'unchanged'}}),/trusted assessment/);
});
test('proposed audit cannot self-approve changed capability notices',async()=>{
 const f=await fixture();f.candidateAudit=structuredClone(f.baselineAudit);f.candidateAudit.commandNotices[0].message='Changed support claim';
 const report=assessProposal(f);assert.equal(report.status,'review-required');assert.equal(report.noticesChanged,true);
 const audit=proposedAudit({...f,report});validateProposalData({...f,audit,evidence:report});
 assert.throws(()=>validateProposalData({...f,audit,evidence:{...report,noticesChanged:false,status:'unchanged'}}),/trusted assessment/);
});
test('generated content tampering and altered reviewed baseline fail closed',async()=>{
 const f=await fixture();const evidence=assessProposal(f);
 f.sources['docs/installation.md']=f.sources['docs/installation.md'].replace('Both commands should report','Both commands MUST report');
 assert.throws(()=>validateProposalData({...f,audit:f.baselineAudit,evidence}),/Noncanonical generated/);
 f.baselineAudit={...f.baselineAudit,capabilityFingerprint:'0'.repeat(64)};
 assert.throws(()=>assessProposal(f),/baseline fingerprint mismatch/);
});
test('only ordinary allowed documentation changes and exact server head are eligible',()=>{
 validateChangedFiles([{filename:'README.md',status:'modified'}]);
 for(const f of [{filename:'.github/workflows/docs-release.yml',status:'modified'},{filename:'site/release.json',status:'renamed',previous_filename:'x'}])assert.throws(()=>validateChangedFiles([f]),/Unexpected proposal/);
 const head='a'.repeat(40),repo={full_name:'gitstore-dev/cli'};
 const pr={number:55,state:'open',draft:false,head:{sha:head,ref:'codex/docs-release-update',repo},base:{ref:'main',repo}};
 assert.equal(validateIdentity(pr,55,head),true);
 assert.throws(()=>validateIdentity(pr,55,'b'.repeat(40)),/stale/);
 assert.throws(()=>validateIdentity({...pr,head:{...pr.head,repo:{full_name:'other/cli'}}},55,head),/Invalid/);
 assert.equal(validateIdentity({...pr,head:{...pr.head,ref:'ordinary'}},55,head),false);
});

test('preparation is idempotent, preserves reviewed prose, and refuses stale main floors before writes',async t=>{
 const {mkdtemp,mkdir,writeFile,rm}=await import('node:fs/promises');
 const {tmpdir}=await import('node:os');const {join,dirname}=await import('node:path');
 const {execFileSync}=await import('node:child_process');
 const {prepareReleaseDocs,proposalPaths}=await import('./prepare-release-docs.mjs');
 const f=await fixture(),repoRoot=await mkdtemp(join(tmpdir(),'docs-orchestration-test-'));
 t.after(()=>rm(repoRoot,{recursive:true,force:true}));
 for(const p of proposalPaths){try{const bytes=await readFile(join(root,p));await mkdir(dirname(join(repoRoot,p)),{recursive:true});await writeFile(join(repoRoot,p),bytes);}catch(e){if(e.code!=='ENOENT')throw e;}}
 await writeFile(join(repoRoot,'.git'),`gitdir: ${execFileSync('git',['rev-parse','--absolute-git-dir'],{cwd:root,encoding:'utf8'}).trim()}\n`);
 const options={repoRoot,baselineAudit:f.baselineAudit,baselineRelease:f.release,updateRelease:async()=>({changed:false,reason:'duplicate'})};
 await rm(join(repoRoot,'site/release-evidence.json'),{force:true});
 const first=await prepareReleaseDocs(options);assert.ok(first.files.includes('site/release-evidence.json'));
 const second=await prepareReleaseDocs(options);assert.equal(second.changed,false);assert.deepEqual(second.files,[]);
 const stale=await prepareReleaseDocs({...options,updateRelease:async()=>({changed:false,reason:'stale'})});
 assert.equal(stale.changed,false);assert.equal(stale.reason,'stale');assert.deepEqual(stale.files,[]);
 const before=await readFile(join(repoRoot,'site/release.json'),'utf8');
 await assert.rejects(()=>prepareReleaseDocs({...options,baselineRelease:{...f.release,version:'999.0.0'}}),/older than trusted main/);
 assert.equal(await readFile(join(repoRoot,'site/release.json'),'utf8'),before);
 await assert.rejects(()=>prepareReleaseDocs({...options,updateRelease:async()=>{throw new Error('Tampered asset');}}),/Tampered asset/);
 assert.equal(await readFile(join(repoRoot,'site/release.json'),'utf8'),before);
 const {symlink}=await import('node:fs/promises');
 await rm(join(repoRoot,'site/release-evidence.json'));await symlink(join(repoRoot,'site/release.json'),join(repoRoot,'site/release-evidence.json'));
 await assert.rejects(()=>prepareReleaseDocs(options),/Nonordinary proposal input/);
 assert.equal(await readFile(join(repoRoot,'site/release.json'),'utf8'),before);
});
