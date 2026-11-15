/** Run only from trusted main. Read proposed blobs; never check out or execute them. */
import {execFileSync} from 'node:child_process';
import {readFile,mkdtemp,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {update,selectRelease,validateSnapshot} from './update-release.mjs';
import {renderReleaseContent,generatedSourcePaths} from './release-content.mjs';
import {assessProposal,proposedAudit,proposalPaths,auditedSourcePaths,json} from './prepare-release-docs.mjs';
const REPO='gitstore-dev/cli';
const run=(cmd,args,cwd)=>execFileSync(cmd,args,{cwd,encoding:'utf8',maxBuffer:16*1024*1024,stdio:['ignore','pipe','pipe']});
const api=p=>JSON.parse(run('gh',['api',`repos/${REPO}/${p}`]));
export function validateIdentity(pr,number,head){
 if(pr.head?.ref!=='codex/docs-release-update')return false;
 if(pr.number!==number||pr.state!=='open'||pr.draft||pr.base?.ref!=='main'||pr.base?.repo?.full_name!==REPO||pr.head?.repo?.full_name!==REPO||pr.head.sha!==head)throw new Error('Invalid or stale rolling documentation proposal');
 return true;
}
export function validateChangedFiles(files){
 if(!Array.isArray(files)||!files.length||files.length>proposalPaths.length)throw new Error('Invalid proposal changed file count');
 for(const f of files)if(!proposalPaths.includes(f.filename)||!['added','modified'].includes(f.status)||f.previous_filename)throw new Error(`Unexpected proposal path: ${f.filename}`);
}
export function validateProposalData({repoRoot,baselineAudit,release,reference,sources,audit,evidence}){
 validateSnapshot(release,Buffer.from(reference));
 const generated=renderReleaseContent({release,sources});
 for(const p of generatedSourcePaths)if(generated[p]!==sources[p])throw new Error(`Noncanonical generated content: ${p}`);
 const expected=assessProposal({repoRoot,baselineAudit,release,sources,candidateAudit:audit});
 if(json(expected)!==json(evidence))throw new Error('Proposed release evidence differs from trusted assessment');
 const expectedAudit=proposedAudit({baselineAudit,candidateAudit:audit,release,report:expected,sources,repoRoot});
 if(json(expectedAudit)!==json(audit))throw new Error('Proposed audit differs from trusted assessment');
 return expected;
}
export async function checkProposal({number,head,repoRoot=process.cwd(),get=api,verifyRelease=update}){
 if(!Number.isSafeInteger(number)||number<1||!/^[a-f0-9]{40}$/.test(head))throw new Error('Invalid proposal arguments');
 const pr=get(`pulls/${number}`);
 if(!validateIdentity(pr,number,head))return {status:'not-applicable',head};
 const files=get(`pulls/${number}/files?per_page=100`); validateChangedFiles(files);
 run('git',['-c','core.hooksPath=/dev/null','fetch','--no-tags','origin',`pull/${number}/head`],repoRoot);
 const resolved=run('git',['rev-parse','FETCH_HEAD'],repoRoot).trim();
 if(resolved!==head)throw new Error('Proposal head changed during fetch');
 const blob=p=>{
   const row=run('git',['ls-tree',head,'--',p],repoRoot);
   if(!/^100644 blob [a-f0-9]{40}\t/.test(row)||row.trimEnd().split('\t')[1]!==p)throw new Error(`Nonordinary proposal blob: ${p}`);
   return run('git',['show',`${head}:${p}`],repoRoot);
 };
 const release=JSON.parse(blob('site/release.json'));
 const releases=[];
 for(let page=1;page<=20;page++){const batch=get(`releases?per_page=100&page=${page}`);if(!Array.isArray(batch)||batch.length>100)throw new Error('Invalid release listing');releases.push(...batch);if(batch.length<100)break;if(page===20)throw new Error('Release listing exceeded bound');}
 const latest=selectRelease(releases);
 if(latest.tag_name!==release.tag)return {status:'superseded',head,tag:release.tag,latestTag:latest.tag_name};
 const staging=await mkdtemp(join(tmpdir(),'docs-check-'));
 try{
   await verifyRelease({tag:release.tag,site:staging,verifyAllAssets:false});
   const published=JSON.parse(await readFile(join(staging,'release.json'),'utf8'));
   if(json(release)!==json(published))throw new Error('Proposal metadata differs from verified published release');
   const reference=blob('site/reference/gs.md');
   if(reference!==await readFile(join(staging,'reference/gs.md'),'utf8'))throw new Error('Proposal reference differs from published archive');
   const baselineAudit=JSON.parse(await readFile(join(repoRoot,'site/content-audit.json'),'utf8'));
   const sources=Object.fromEntries(auditedSourcePaths.map(p=>[p,blob(p)]));
   const report=validateProposalData({repoRoot,baselineAudit,release,reference,sources,audit:JSON.parse(blob('site/content-audit.json')),evidence:JSON.parse(blob('site/release-evidence.json'))});
   if(get(`pulls/${number}`).head.sha!==head)throw new Error('Proposal head changed during validation');
   return {status:'valid',head,sourceCommit:release.sourceCommit,baselineSourceCommit:baselineAudit.commandSourceCommit,evidenceStatus:report.status,latestTag:latest.tag_name,tag:release.tag};
 }finally{await rm(staging,{recursive:true,force:true});}
}
if(process.argv[1]&&resolve(process.argv[1])===fileURLToPath(import.meta.url)){
 try{const args=process.argv.slice(2);if(args.length!==4||args[0]!=='--pr'||args[2]!=='--head')throw new Error('Usage: --pr NUMBER --head SHA');console.log(JSON.stringify(await checkProposal({number:Number(args[1]),head:args[3]})));}
 catch(e){console.error(`Documentation proposal rejected: ${e.message}`);process.exitCode=1;}
}
