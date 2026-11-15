/** Conservative, read-only capability evidence from Git objects.
 *
 * This is an equality check against a previously reviewed release, never a claim
 * that changed source is safe. It runs no code from either release. Publication
 * and queue-only files are excluded explicitly; build and qualification changes
 * require review even when command help happens to stay the same.
 */
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const POLICY = 'capability-inputs-v1';
const HASH = /^[a-f0-9]{64}$/;
const COMMIT = /^[a-f0-9]{40}$/;
const VERSION = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?$/;
const SAFE_PATH = /^[A-Za-z0-9_.\/-]+$/;
const REQUIRED = ['Cargo.toml', 'Cargo.lock', 'version.txt', 'rust-toolchain.toml',
  'crates/gs/Cargo.toml', 'crates/gs/src/cli.rs', 'daemon/CMakeLists.txt',
  'runtime/profiles/core.json', 'packaging/platforms.json', 'docs/contracts/command-matrix.md'];
const EXCLUDED_FILES = new Set([
  // These files deliver already qualified bytes; they cannot alter their build.
  'runtime/packaging/homebrew_delivery.py', 'runtime/packaging/publication.py',
  'scripts/publish-release.py', 'scripts/queue-homebrew.py', 'scripts/check-publication-environment.py',
  '.github/workflows/docs-release.yml', '.github/workflows/website.yml',
  '.github/workflows/release-please.yml', '.github/workflows/pr-title-lint.yml',
]);
const ROOT_DOCUMENTATION = new Set(['AGENTS.md', 'CHANGELOG.md', 'CONTRIBUTING.md', 'LICENSE', 'README.md',
  '.gitignore', 'release-please-config.json', '.release-please-manifest.json']);
const digest = (bytes) => createHash('sha256').update(bytes).digest('hex');
const canonical = (value) => JSON.stringify(value);
function requireThat(value, message) { if (!value) throw new Error(message); }

export function isCapabilityInput(path, extraEvidence = []) {
  if (EXCLUDED_FILES.has(path)) return false;
  if (path.startsWith('site/') || path.startsWith('tests/') || /(?:^|\/)tests?(?:\/|_)/.test(path) || /(?:^|\/)test[^/]*\.(?:rs|c|h|py|sh)$/.test(path)) return false;
  if (extraEvidence.includes(path)) return true;
  return /^(?:crates\/|daemon\/|runtime\/|packaging\/|scripts\/|examples\/|\.cargo\/|\.github\/workflows\/|docs\/contracts\/|docs\/ADRs\/|docs\/contributing\/validation\/)/.test(path) ||
    /^(?:Cargo\.(?:toml|lock)|rust-toolchain(?:\.toml)?|version\.txt|CMakeLists\.txt|Makefile|\.gitmodules|\.gitattributes|\.dockerignore)$/.test(path) ||
    // New root files default to review; only known non-capability records escape.
    (!path.includes('/') && !ROOT_DOCUMENTATION.has(path));
}

/** Normalize only this repository's package version, not dependencies/features. */
export function normalizeInput(path, content, version) {
  const text = Buffer.isBuffer(content) ? content.toString('utf8') : content;
  if (path === 'version.txt') {
    requireThat(text === `${version}\n`, 'version.txt does not match selected release');
    return '<own-package-version>\n';
  }
  if (path === 'Cargo.toml') {
    let inPackage = false, count = 0;
    const lines = text.split('\n').map((line) => {
      if (/^\[/.test(line)) inPackage = line.trim() === '[workspace.package]';
      if (inPackage && /^version\s*=/.test(line)) {
        requireThat(line === `version = "${version}"`, 'Workspace version does not match selected release');
        count++;
        return 'version = "<own-package-version>"';
      }
      return line;
    });
    requireThat(count === 1, 'Expected one workspace package version');
    return lines.join('\n');
  }
  if (path === 'Cargo.lock') {
    let count = 0;
    const blocks = text.split(/(?=^\[\[package\]\]$)/m).map((block) => {
      if (!/^name = "gs"$/m.test(block)) return block;
      requireThat(!/^source\s*=/m.test(block), 'Own package unexpectedly has registry source');
      requireThat(block.match(/^version\s*=/gm)?.length === 1 && block.includes(`\nversion = "${version}"\n`), 'Lockfile own version does not match selected release');
      count++;
      return block.replace(`\nversion = "${version}"\n`, '\nversion = "<own-package-version>"\n');
    });
    requireThat(count === 1, 'Expected one gs lockfile package');
    return blocks.join('');
  }
  if (path === '.github/workflows/release.yml') {
    // Do not discard the whole release workflow: build flags, runners, matrices,
    // qualification and producer checks are capability evidence. Only complete
    // top-level publish/homebrew job blocks are delivery-only.
    let jobs = false, skip = false;
    return text.split('\n').filter((line) => {
      if (line === 'jobs:') jobs = true;
      else if (/^[^\s#]/.test(line)) { jobs = false; skip = false; }
      if (jobs && /^  [A-Za-z][\w-]*:\s*$/.test(line)) skip = /^  (?:publish|homebrew):\s*$/.test(line);
      return !skip;
    }).join('\n');
  }
  return content;
}

/** Pure helper; inputs are trusted caller-provided Git blob snapshots. */
export function fingerprintInputs(entries, { version, extraEvidence = [] } = {}) {
  requireThat(VERSION.test(version), 'Invalid release version');
  const inputs = [];
  const seen = new Set();
  for (const entry of entries) {
    if (!isCapabilityInput(entry.path, extraEvidence)) continue;
    requireThat(SAFE_PATH.test(entry.path) && !entry.path.split('/').includes('..') && !seen.has(entry.path), 'Invalid or duplicate capability path');
    requireThat(['100644', '100755'].includes(entry.mode), `Capability input is not an ordinary Git file: ${entry.path}`);
    seen.add(entry.path);
    const normalized = normalizeInput(entry.path, entry.content, version);
    inputs.push({ path: entry.path, mode: entry.mode, sha256: digest(normalized) });
  }
  for (const path of [...REQUIRED, ...extraEvidence]) requireThat(seen.has(path), `Missing capability input: ${path}`);
  inputs.sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
  return { fingerprint: digest(canonical({ policy: POLICY, inputs })), inputs };
}

export function compareInputs(before, after) {
  const old = new Map(before.map((entry) => [entry.path, entry]));
  const next = new Map(after.map((entry) => [entry.path, entry]));
  return [...new Set([...old.keys(), ...next.keys()])].sort().flatMap((path) => {
    const a = old.get(path), b = next.get(path);
    if (a?.sha256 === b?.sha256 && a?.mode === b?.mode) return [];
    return [{ path, change: !a ? 'added' : !b ? 'deleted' : 'modified',
      beforeSha256: a?.sha256 ?? null, afterSha256: b?.sha256 ?? null,
      beforeMode: a?.mode ?? null, afterMode: b?.mode ?? null }];
  });
}

export function platformInventory(release) {
  requireThat(Array.isArray(release.assets) && release.assets.length > 0, 'Missing published platform inventory');
  const rows = release.assets.map(({ kind, os, arch }) => {
    requireThat(['archive', 'deb', 'rpm', 'npm', 'formula', 'manifest', 'source', 'relink', 'binary'].includes(kind) &&
      ['linux', 'darwin', 'any'].includes(os) && ['x64', 'arm64', 'any'].includes(arch), 'Unknown published inventory kind/platform');
    return `${kind}/${os}/${arch}`;
  }).sort();
  // Binary inventories can contain both gs and gitstored at the same tuple.
  // Preserve multiplicity to detect removed companion binaries as well.
  return rows;
}

/** Compact reviewed identity; no download URLs, sizes or rebuilt-byte digests. */
export function baselineIdentity(release) {
  requireThat(release && VERSION.test(release.version) && release.tag === `v${release.version}` &&
    COMMIT.test(release.sourceCommit) && HASH.test(release.reference?.sha256), 'Invalid reviewed release identity');
  platformInventory(release);
  return { version: release.version, tag: release.tag, sourceCommit: release.sourceCommit,
    reference: { sha256: release.reference.sha256 },
    assets: release.assets.map(({ kind, os, arch }) => ({ kind, os, arch }))
      .sort((a, b) => canonical(a) < canonical(b) ? -1 : canonical(a) > canonical(b) ? 1 : 0) };
}

function git(repoRoot, args, input) {
  return execFileSync('git', ['--no-replace-objects', '-C', repoRoot, ...args], {
    input, maxBuffer: 64 * 1024 * 1024, stdio: ['pipe', 'pipe', 'pipe'],
    env: { ...process.env, GIT_NO_REPLACE_OBJECTS: '1', GIT_OPTIONAL_LOCKS: '0' },
  });
}

function gitInputs(repoRoot, commit, extraEvidence) {
  const rows = git(repoRoot, ['ls-tree', '-rz', '--full-tree', commit]).toString('utf8').split('\0').filter(Boolean).map((row) => {
    const match = /^(\d{6}) (\w+) ([a-f0-9]{40})\t([\s\S]+)$/.exec(row);
    requireThat(match, 'Malformed Git tree');
    return { mode: match[1], type: match[2], oid: match[3], path: match[4] };
  });
  const selected = rows.filter(({ path }) => isCapabilityInput(path, extraEvidence));
  requireThat(selected.length > 0 && selected.length <= 5000, 'Capability inventory is empty or oversized');
  requireThat(selected.every(({ type, mode }) => type === 'blob' && ['100644', '100755'].includes(mode)), 'Capability tree contains nonordinary inputs');
  const stream = git(repoRoot, ['cat-file', '--batch'], selected.map(({ oid }) => `${oid}\n`).join(''));
  let offset = 0;
  const entries = selected.map((row) => {
    const end = stream.indexOf(10, offset);
    requireThat(end >= 0, 'Incomplete Git batch header');
    const header = /^([a-f0-9]{40}) blob (\d+)$/.exec(stream.subarray(offset, end).toString('ascii'));
    requireThat(header && header[1] === row.oid, 'Git blob identity mismatch');
    const size = Number(header[2]);
    requireThat(Number.isSafeInteger(size) && size <= 8 * 1024 * 1024 && end + 1 + size < stream.length, 'Oversized or incomplete Git blob');
    const content = stream.subarray(end + 1, end + 1 + size);
    requireThat(stream[end + 1 + size] === 10, 'Malformed Git blob boundary');
    offset = end + 2 + size;
    return { ...row, content };
  });
  requireThat(offset === stream.length, 'Unexpected Git batch data');
  return { entries, rows };
}

function validateIdentity(repoRoot, release) {
  requireThat(release && VERSION.test(release.version) && release.tag === `v${release.version}` && COMMIT.test(release.sourceCommit), 'Invalid release identity');
  requireThat(HASH.test(release.reference?.sha256), 'Invalid bundled reference digest');
  const commit = git(repoRoot, ['rev-parse', '--verify', `${release.sourceCommit}^{commit}`]).toString('ascii').trim();
  requireThat(commit === release.sourceCommit, 'Missing or mismatched source commit');
  const tag = git(repoRoot, ['rev-parse', '--verify', `refs/tags/${release.tag}^{commit}`]).toString('ascii').trim();
  requireThat(tag === release.sourceCommit, 'Published tag/source identity mismatch');
}

/** Caller supplies the previously reviewed audit, never release-owned code/data. */
export function assessCapabilities({ repoRoot, baselineRelease, release, audit }) {
  const report = { schemaVersion: 1, policy: POLICY, status: 'review-required',
    baseline: { version: baselineRelease?.version, sourceCommit: baselineRelease?.sourceCommit },
    source: { version: release?.version, sourceCommit: release?.sourceCommit },
    fingerprint: null, changedInputs: [], ignoredChanges: [] };
    validateIdentity(repoRoot, baselineRelease);
    validateIdentity(repoRoot, release);
    requireThat(audit?.version === baselineRelease.version && audit.commandSourceCommit === baselineRelease.sourceCommit,
      'Capability audit is not bound to the reviewed baseline release');
    requireThat(Array.isArray(audit.commandNotices) && audit.commandNotices.length > 0, 'Missing reviewed capability notices');
    const extraEvidence = [...new Set(audit.commandNotices.map(({ evidence }) => evidence))].sort();
    requireThat(extraEvidence.every((path) => typeof path === 'string' && SAFE_PATH.test(path) && !path.split('/').includes('..') &&
      !path.startsWith('site/') && !EXCLUDED_FILES.has(path)), 'Invalid capability notice evidence');
    report.auditNoticesSha256 = digest(canonical(audit.commandNotices));
    const old = gitInputs(repoRoot, baselineRelease.sourceCommit, extraEvidence);
    const next = gitInputs(repoRoot, release.sourceCommit, extraEvidence);
    const a = fingerprintInputs(old.entries, { version: baselineRelease.version, extraEvidence });
    const b = fingerprintInputs(next.entries, { version: release.version, extraEvidence });
    const virtual = (value) => [
      { path: '$reference', mode: 'data', sha256: value.reference.sha256 },
      { path: '$platform-inventory', mode: 'data', sha256: digest(canonical(platformInventory(value))) },
    ];
    a.inputs.push(...virtual(baselineRelease));
    b.inputs.push(...virtual(release));
    const fingerprint = (inputs) => digest(canonical({ policy: POLICY, inputs }));
    report.baseline.fingerprint = fingerprint(a.inputs);
    report.source.fingerprint = fingerprint(b.inputs);
    report.fingerprint = report.source.fingerprint;
    report.changedInputs = compareInputs(a.inputs, b.inputs);
    const allOld = new Map(old.rows.map(({ path, oid, mode }) => [path, `${mode}/${oid}`]));
    const allNext = new Map(next.rows.map(({ path, oid, mode }) => [path, `${mode}/${oid}`]));
    report.ignoredChanges = [...new Set([...allOld.keys(), ...allNext.keys()])].filter((path) =>
      !isCapabilityInput(path, extraEvidence) && allOld.get(path) !== allNext.get(path)).sort();
    report.status = report.changedInputs.length ? 'review-required' : 'unchanged';
  return report;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const args = process.argv.slice(2), options = new Map();
    requireThat(args.length % 2 === 0, 'Expected --baseline FILE --release FILE --audit FILE [--repo-root DIR]');
    for (let i = 0; i < args.length; i += 2) {
      requireThat(['--baseline', '--release', '--audit', '--repo-root'].includes(args[i]) && !options.has(args[i]), 'Unknown or duplicate assessment option');
      options.set(args[i], args[i + 1]);
    }
    const read = (key) => { requireThat(options.has(key), `Missing ${key}`); return JSON.parse(readFileSync(options.get(key), 'utf8')); };
    const report = assessCapabilities({ repoRoot: resolve(options.get('--repo-root') ?? '.'),
      baselineRelease: read('--baseline'), release: read('--release'), audit: read('--audit') });
    console.log(JSON.stringify(report, null, 2));
  } catch (error) {
    // Missing objects, identity/evidence corruption and malformed inputs are
    // hard failures. They cannot become approvable 'review-required' reports.
    console.error(`Capability assessment failed: ${error.message.startsWith('Command failed: git') ? 'Required Git evidence is unavailable' : error.message}`);
    process.exitCode = 1;
  }
}
