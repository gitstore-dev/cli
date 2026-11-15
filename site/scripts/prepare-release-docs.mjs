/** Trusted proposal preparation. Treat release and proposal trees as data only. */
import {readFile, writeFile, mkdir, mkdtemp, rm, lstat} from 'node:fs/promises';
import {join, resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {fileURLToPath} from 'node:url';
import {update, sha256, compareVersions} from './update-release.mjs';
import {assessCapabilities, baselineIdentity} from './capability-audit.mjs';
import {renderReleaseContent, normalizeReleaseContent, generatedSourcePaths} from './release-content.mjs';

export const guideSources = ['docs/README.md','docs/installation.md','docs/configuration.md','docs/remote-cli.md','docs/watch.md','docs/local-runtime.md','docs/local-runtime-configuration.md'];
export const auditedSourcePaths = [...new Set([...guideSources,...generatedSourcePaths])];
export const proposalPaths = [...new Set([...generatedSourcePaths,...guideSources,'site/release.json','site/reference/gs.md','site/content-audit.json','site/release-evidence.json'])];
export const json = value => `${JSON.stringify(value,null,2)}\n`;
export const sourceHashes = sources => Object.fromEntries(auditedSourcePaths.map(p=>[p,sha256(normalizeReleaseContent(sources[p]))]));

export function assessProposal({repoRoot, baselineAudit, release, sources, candidateAudit=baselineAudit}) {
  if(baselineAudit.schemaVersion!==2 || !baselineAudit.reviewedRelease) throw new Error('Missing reviewed capability baseline');
  const report=assessCapabilities({repoRoot,baselineRelease:baselineAudit.reviewedRelease,release,audit:baselineAudit});
  if(report.baseline.fingerprint!==baselineAudit.capabilityFingerprint) throw new Error('Reviewed capability baseline fingerprint mismatch');
  const hashes=sourceHashes(sources);
  report.authoredSources=hashes;
  report.authoredChanges=auditedSourcePaths.filter(p=>hashes[p]!==baselineAudit.sourceSha256[p]);
  report.noticesChanged=JSON.stringify(candidateAudit.commandNotices)!==JSON.stringify(baselineAudit.commandNotices);
  if(report.authoredChanges.length || report.noticesChanged) report.status='review-required';
  return report;
}

export function proposedAudit({baselineAudit,candidateAudit,release,report,sources,repoRoot}) {
  if(report.status==='unchanged') return baselineAudit;
  const audit={...baselineAudit,commandNotices:candidateAudit.commandNotices,schemaVersion:2,
    version:release.version,commandSourceCommit:release.sourceCommit,reviewedRelease:baselineIdentity(release),sourceSha256:sourceHashes(sources)};
  const self=assessCapabilities({repoRoot,baselineRelease:audit.reviewedRelease,release,audit});
  audit.capabilityFingerprint=self.fingerprint;
  audit.notes=['Proposed capability baseline: requires current-head human maintainer approval through Docs release eligibility before merging. See site/release-evidence.json.',...baselineAudit.notes.filter(n=>!n.startsWith('Proposed capability baseline:'))];
  return audit;
}

export async function validateLocalPaths(repoRoot) {
  for(const p of proposalPaths) {
    const parts=p.split('/');
    for(let i=1;i<=parts.length;i++) {
      try {const stat=await lstat(join(repoRoot,...parts.slice(0,i)));
        if(stat.isSymbolicLink() || (i<parts.length ? !stat.isDirectory() : !stat.isFile())) throw new Error(`Nonordinary proposal input: ${p}`);
      } catch(e){if(e.code!=='ENOENT')throw e; if(i<parts.length)throw new Error(`Missing proposal directory: ${p}`);}
    }
  }
}

export async function prepareReleaseDocs({repoRoot,baselineAudit,baselineRelease,tag,updateRelease=update}) {
  await validateLocalPaths(repoRoot);
  const staging=await mkdtemp(join(tmpdir(),'docs-proposal-'));
  try {
    await mkdir(join(staging,'reference'),{recursive:true});
    for(const p of ['release.json','reference/gs.md']) await writeFile(join(staging,p),await readFile(join(repoRoot,'site',p)));
    const current=JSON.parse(await readFile(join(staging,'release.json'),'utf8'));
    if(compareVersions(current.version,baselineRelease.version)<0) throw new Error('Proposal release is older than trusted main');
    const result=await updateRelease({tag,site:staging});
    if(result.reason==='stale') return {changed:false,tag:current.tag,version:current.version,reviewRequired:true,files:[],reason:'stale'};
    const release=JSON.parse(await readFile(join(staging,'release.json'),'utf8'));
    if(compareVersions(release.version,baselineRelease.version)<0 || compareVersions(release.version,current.version)<0) throw new Error('Release documentation downgrade refused');
    const candidateAudit=JSON.parse(await readFile(join(repoRoot,'site/content-audit.json'),'utf8'));
    const sources=Object.fromEntries(await Promise.all([...new Set([...guideSources,...generatedSourcePaths])].map(async p=>[p,await readFile(join(repoRoot,p),'utf8')])));
    Object.assign(sources,renderReleaseContent({release,sources}));
    const report=assessProposal({repoRoot,baselineAudit,release,sources,candidateAudit});
    const audit=proposedAudit({baselineAudit,candidateAudit,release,report,sources,repoRoot});
    const output={...sources,'site/release.json':await readFile(join(staging,'release.json'),'utf8'),'site/reference/gs.md':await readFile(join(staging,'reference/gs.md'),'utf8'),'site/content-audit.json':json(audit),'site/release-evidence.json':json(report)};
    const files=[];
    // All remote validation, generation and evidence checks finish before writes.
    for(const [p,body] of Object.entries(output)) {
      let old; try {old=await readFile(join(repoRoot,p),'utf8');} catch(e){if(e.code!=='ENOENT')throw e;}
      if(body!==old) files.push(p);
    }
    for(const p of files) await writeFile(join(repoRoot,p),output[p]);
    return {changed:files.length>0,tag:release.tag,version:release.version,reviewRequired:report.status==='review-required',files,reason:result.reason};
  } finally {await rm(staging,{recursive:true,force:true});}
}
if(process.argv[1] && resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
  try {
    const args=process.argv.slice(2),opts={};
    for(let i=0;i<args.length;i+=2){if(!['--repo-root','--baseline-release','--baseline-audit','--tag'].includes(args[i])||!args[i+1]||opts[args[i]])throw new Error('Invalid preparation arguments');opts[args[i]]=args[i+1];}
    const baselineAudit=JSON.parse(await readFile(opts['--baseline-audit'],'utf8'));
    const baselineRelease=JSON.parse(await readFile(opts['--baseline-release'],'utf8'));
    if(baselineRelease.schemaVersion!==1) throw new Error('Invalid trusted release snapshot');
    console.log(JSON.stringify(await prepareReleaseDocs({repoRoot:resolve(opts['--repo-root']),baselineAudit,baselineRelease,tag:opts['--tag']})));
  }catch(e){console.error(`Release documentation preparation failed: ${e.message}`);process.exitCode=1;}
}
