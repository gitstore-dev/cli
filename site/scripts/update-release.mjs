/** Import a published release as data. Never builds or executes release binaries. */
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { readFile, writeFile, mkdir, rename, mkdtemp, rm } from 'node:fs/promises';
import { createReadStream } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const REPO = 'gitstore-dev/cli';
const SITE = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const HASH = /^[a-f0-9]{64}$/;
const SOURCE = /^[a-f0-9]{40}$/;
const NAME = /^[A-Za-z0-9_.~-]+$/;
const MAX_ASSET = 1024 ** 3;
export const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');
function assert(condition, message) { if (!condition) throw new Error(message); }

export function parseVersion(version) {
  const match = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?$/.exec(version);
  assert(match, `Invalid version: ${version}`);
  const pre = match[4]?.split('.');
  assert(!pre || pre.every((v) => v && (!/^\d+$/.test(v) || v === '0' || !v.startsWith('0'))), 'Invalid prerelease');
  return { core: match.slice(1, 4).map(BigInt), pre };
}

export function compareVersions(a, b) {
  const x = parseVersion(a), y = parseVersion(b);
  for (let i = 0; i < 3; i++) if (x.core[i] !== y.core[i]) return x.core[i] > y.core[i] ? 1 : -1;
  if (!x.pre || !y.pre) return x.pre ? -1 : y.pre ? 1 : 0;
  for (let i = 0; i < Math.max(x.pre.length, y.pre.length); i++) {
    const p = x.pre[i], q = y.pre[i];
    if (p === undefined || q === undefined) return p === undefined ? -1 : 1;
    if (p === q) continue;
    const pn = /^\d+$/.test(p), qn = /^\d+$/.test(q);
    if (pn && qn) return BigInt(p) > BigInt(q) ? 1 : -1;
    if (pn !== qn) return pn ? -1 : 1;
    return p > q ? 1 : -1;
  }
  return 0;
}

export function selectRelease(releases, tag) {
  const published = releases.filter((r) => !r.draft && r.published_at && /^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(r.tag_name));
  published.sort((a, b) => compareVersions(b.tag_name.slice(1), a.tag_name.slice(1)));
  const selected = tag ? published.find((r) => r.tag_name === tag) : published[0];
  assert(selected, 'No matching published release (drafts are excluded)');
  return selected;
}

export function parseChecksums(bytes) {
  const checksums = new Map();
  for (const line of bytes.toString('utf8').trimEnd().split('\n')) {
    const row = /^([a-f0-9]{64})  ([A-Za-z0-9_.~-]+)$/.exec(line);
    assert(row && !checksums.has(row[2]), 'Malformed or duplicate checksum');
    checksums.set(row[2], row[1]);
  }
  return checksums;
}

function hostedName(logical, kind) {
  // Only Debian filenames have GitHub's documented publisher alias. Never
  // generalize replacement to source/archive names or accept alias collisions.
  return kind === 'deb' ? logical.replaceAll('~', '.') : logical;
}

export function validateInventory(inventory, release, sourceCommit, checksums) {
  assert(!release.draft && release.published_at, 'Release is not published');
  assert(SOURCE.test(sourceCommit) && inventory.schemaVersion === 1 &&
    inventory.sourceRevision === sourceCommit && inventory.tag === release.tag_name &&
    inventory.tag === `v${inventory.version}`, 'Release tag/source identity mismatch');
  parseVersion(inventory.version);
  assert(Array.isArray(inventory.artifacts) && inventory.artifacts.length > 0 && inventory.artifacts.length < 200,
    'Invalid artifact inventory');
  const remote = new Map();
  for (const asset of release.assets) {
    assert(NAME.test(asset.name) && !remote.has(asset.name) && Number.isSafeInteger(asset.size) &&
      asset.size > 0 && asset.size <= MAX_ASSET && /^sha256:[a-f0-9]{64}$/.test(asset.digest), 'Invalid GitHub asset metadata');
    assert(asset.browser_download_url === `https://github.com/${REPO}/releases/download/${release.tag_name}/${asset.name}`,
      'Unexpected asset URL');
    remote.set(asset.name, asset);
  }
  const names = new Set();
  const assets = inventory.artifacts.map((row) => {
    const name = hostedName(row.filename, row.format);
    assert(NAME.test(row.filename) && !names.has(name), 'Duplicate or invalid artifact');
    names.add(name);
    assert(row.applicationVersion === inventory.version && row.sourceRevision === sourceCommit &&
      HASH.test(row.sha256) && checksums.get(row.filename) === row.sha256, 'Artifact identity/checksum mismatch');
    assert(['archive', 'deb', 'rpm', 'npm', 'formula', 'binary', 'manifest', 'source', 'relink'].includes(row.format) &&
      ['linux', 'darwin', 'any'].includes(row.os) && ['x64', 'arm64', 'any'].includes(row.arch), 'Unknown artifact format/platform');
    const asset = remote.get(name);
    assert(asset && asset.size === row.bytes && asset.digest === `sha256:${row.sha256}`, `Missing or mismatched asset: ${name}`);
    return { name, logicalName: row.filename, url: asset.browser_download_url,
      sha256: row.sha256, size: row.bytes, kind: row.format, os: row.os, arch: row.arch };
  });
  for (const os of ['darwin', 'linux']) for (const arch of ['arm64', 'x64']) {
    for (const kind of ['archive', 'npm']) assert(assets.filter((a) => a.kind === kind && a.os === os && a.arch === arch).length === 1,
      `Missing ${os}/${arch} ${kind}`);
    if (os === 'linux') for (const kind of ['deb', 'rpm'])
      assert(assets.filter((a) => a.kind === kind && a.os === os && a.arch === arch).length === 1, `Missing Linux ${kind}`);
  }
  assert(assets.filter((a) => a.kind === 'formula').length === 1, 'Missing Homebrew formula');
  const signedSubjects = [...names, 'release-inventory.json', 'SHA256SUMS'];
  const shared = remote.has('release.sigstore.jsonl');
  const bundles = shared ? ['release.sigstore.jsonl'] : signedSubjects.map((name) => `${name}.sigstore.jsonl`);
  assert(bundles.every((name) => remote.has(name)), 'Missing release verification bundle');
  const expected = new Set([...signedSubjects, ...bundles]);
  assert([...remote.keys()].every((name) => expected.has(name)), 'Unexpected release asset');
  const embedded = inventory.installationInventories;
  if (embedded !== undefined) {
    assert(embedded && typeof embedded === 'object' && Object.keys(embedded).length === 4, 'Invalid embedded installation inventories');
    for (const os of ['darwin', 'linux']) for (const arch of ['arm64', 'x64']) {
      const name = `gitstore-${inventory.version}-${os}-${arch}.install.json`;
      const install = embedded[name];
      assert(install && install.applicationVersion === inventory.version && install.sourceRevision === sourceCommit &&
        install.os === os && install.arch === arch && Array.isArray(install.files), 'Embedded install identity mismatch');
    }
  } else {
    assert(assets.filter((a) => a.kind === 'manifest').length === 4, 'Missing legacy installation inventories');
  }
  return assets.sort((a, b) => a.name.localeCompare(b.name));
}

export function validateReference(reference) {
  assert(Buffer.byteLength(reference) > 100 && Buffer.byteLength(reference) <= 4 * 1024 * 1024 && !reference.includes('\0'), 'Invalid reference bounds');
  let fence = false;
  const commands = new Set();
  for (const line of reference.split('\n')) {
    if (line === '```text') { assert(!fence, 'Nested reference fence'); fence = true; continue; }
    if (line === '```') { assert(fence, 'Unexpected reference fence'); fence = false; continue; }
    if (!fence && line) {
      const command = /^## (gs(?: [a-z][a-z0-9-]*)*)$/.exec(line);
      assert(command && !commands.has(command[1]), 'Malformed or duplicate command heading');
      commands.add(command[1]);
    }
  }
  assert(!fence && commands.has('gs') && commands.has('gs auth login'), 'Incomplete command reference');
  return commands;
}

export function validateSnapshot(metadata, reference) {
  assert(metadata.schemaVersion === 1 && metadata.tag === `v${metadata.version}` && SOURCE.test(metadata.sourceCommit), 'Invalid release snapshot');
  parseVersion(metadata.version);
  assert(metadata.releaseUrl === `https://github.com/${REPO}/releases/tag/${metadata.tag}` &&
    Number.isFinite(Date.parse(metadata.publishedAt)), 'Invalid published release URL/date');
  assert(metadata.reference.path === 'reference/gs.md' && sha256(reference) === metadata.reference.sha256, 'Reference snapshot digest mismatch');
  validateReference(reference.toString('utf8'));
  assert(Array.isArray(metadata.assets) && metadata.assets.length > 0 && HASH.test(metadata.inventorySha256), 'Invalid snapshot inventory');
  const names = new Set();
  for (const a of metadata.assets) {
    assert(!names.has(a.name), 'Duplicate snapshot asset');
    names.add(a.name);
    assert(NAME.test(a.name) && NAME.test(a.logicalName) && HASH.test(a.sha256) && Number.isSafeInteger(a.size) && a.size > 0 && a.size <= MAX_ASSET &&
    a.url === `https://github.com/${REPO}/releases/download/${metadata.tag}/${a.name}`, 'Invalid snapshot asset');
  }
  if (Object.hasOwn(metadata, 'verification')) {
    const value = metadata.verification;
    assert(value && Object.keys(value).sort().join(',') === 'bundle,checksum,layout' &&
      value.checksum === 'SHA256SUMS' &&
      ((value.layout === 'shared' && value.bundle === 'release.sigstore.jsonl') ||
       (value.layout === 'per-subject' && value.bundle === null)), 'Invalid release verification layout');
  }
}

const gh = (...args) => execFileSync('gh', args, { maxBuffer: 16 * 1024 * 1024, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
const api = (path) => JSON.parse(gh('api', `repos/${REPO}/${path}`));
async function download(asset, directory) {
  const path = join(directory, asset.name);
  // gh handles authentication without exposing tokens or forwarding authorization
  // to arbitrary archive-controlled URLs. No archive extraction writes to disk.
  execFileSync('gh', ['release', 'download', asset.tag, '--repo', REPO, '--pattern', asset.name, '--dir', directory],
    { maxBuffer: 1024 * 1024, stdio: ['ignore', 'pipe', 'pipe'] });
  return path;
}

export async function verifyDownloadedAsset(path, asset) {
  let bytes = 0;
  const digest = createHash('sha256');
  for await (const chunk of createReadStream(path)) { bytes += chunk.length; assert(bytes <= MAX_ASSET, 'Oversized asset'); digest.update(chunk); }
  assert(bytes === asset.size && digest.digest('hex') === asset.sha256, `Downloaded asset digest mismatch: ${asset.name}`);
}

export async function update({ tag, site = SITE, get = api, fetchAsset = download, verifyAllAssets = true } = {}) {
  assert(typeof verifyAllAssets === 'boolean', 'verifyAllAssets must be boolean');
  let existing;
  try { existing = JSON.parse(await readFile(join(site, 'release.json'), 'utf8')); }
  catch (error) { if (error.code !== 'ENOENT') throw error; }
  const releases = [];
  for (let page = 1; page <= 20; page++) {
    const batch = get(`releases?per_page=100&page=${page}`);
    assert(Array.isArray(batch) && batch.length <= 100, 'Malformed release listing');
    releases.push(...batch);
    if (batch.length < 100) break;
    assert(page < 20, 'Release listing exceeded bound');
  }
  const release = selectRelease(releases, tag);
  const version = release.tag_name.slice(1);
  if (existing && compareVersions(version, existing.version) < 0) return { changed: false, reason: 'stale', tag: existing.tag };
  const sourceCommit = get(`commits/${encodeURIComponent(release.tag_name)}`).sha;
  const directory = await mkdtemp(join(tmpdir(), 'gitstore-docs-release-'));
  try {
    const fetchRemote = async (name) => {
      const asset = release.assets.find((a) => a.name === name);
      assert(asset && /^sha256:[a-f0-9]{64}$/.test(asset.digest), `Missing release subject: ${name}`);
      const expected = { ...asset, tag: release.tag_name, sha256: asset.digest.slice(7) };
      const path = await fetchAsset(expected, directory);
      await verifyDownloadedAsset(path, expected);
      return path;
    };
    const inventoryPath = await fetchRemote('release-inventory.json');
    const inventoryBytes = await readFile(inventoryPath);
    const checksumPath = await fetchRemote('SHA256SUMS');
    const checksums = parseChecksums(await readFile(checksumPath));
    assert(checksums.get('release-inventory.json') === sha256(inventoryBytes), 'Inventory checksum mismatch');
    const inventory = JSON.parse(inventoryBytes);
    const assets = validateInventory(inventory, release, sourceCommit, checksums);
    const archive = assets.find((a) => a.kind === 'archive' && a.os === 'darwin' && a.arch === 'arm64');
    let archivePath;
    for (const asset of assets) {
      if (!verifyAllAssets && asset !== archive) continue;
      const path = await fetchAsset({ ...asset, tag: release.tag_name }, directory);
      await verifyDownloadedAsset(path, asset);
      if (asset === archive) archivePath = path;
    }
    // Verification bundles are retained in their published legacy/shared layout.
    // Their transport digests are checked here; this importer does not claim to
    // substitute for the release producer's cryptographic provenance gate.
    for (const asset of release.assets) {
      if (asset.name === 'release-inventory.json' || asset.name === 'SHA256SUMS' || assets.some((a) => a.name === asset.name)) continue;
      if (verifyAllAssets) await fetchRemote(asset.name);
    }
    // All remote subjects and bundle URLs/digests/layout were checked by
    // validateInventory above, including in the trusted queue's narrow mode.
    // Narrow mode verifies downloaded reference bytes, not other artifact bytes.
    const shared = release.assets.some((asset) => asset.name === 'release.sigstore.jsonl');
    const verification = { layout: shared ? 'shared' : 'per-subject',
      bundle: shared ? 'release.sigstore.jsonl' : null, checksum: 'SHA256SUMS' };
    // Listing first detects duplicate names and confirms the selected member is
    // ordinary; stdout-only extraction cannot overwrite source files or links.
    const listing = execFileSync('tar', ['-tzf', archivePath], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 }).trimEnd().split('\n');
    assert(listing.filter((n) => n === 'share/reference/gs.md').length === 1 &&
      listing.every((n) => !n.startsWith('/') && !n.split('/').includes('..')), 'Unsafe or missing archive reference');
    const reference = execFileSync('tar', ['-xOzf', archivePath, 'share/reference/gs.md'], { maxBuffer: 4 * 1024 * 1024 });
    validateReference(reference.toString('utf8'));
    const metadata = { schemaVersion: 1, version, tag: release.tag_name, sourceCommit,
      publishedAt: release.published_at, releaseUrl: release.html_url,
      inventorySha256: sha256(inventoryBytes), assets,
      reference: { path: 'reference/gs.md', sha256: sha256(reference) }, verification };
    validateSnapshot(metadata, reference);
    const json = `${JSON.stringify(metadata, null, 2)}\n`;
    if (existing && existing.tag === metadata.tag) {
      const { verification: ignored, ...legacy } = metadata;
      const migration = !Object.hasOwn(existing, 'verification');
      assert(JSON.stringify(existing) === JSON.stringify(migration ? legacy : metadata), 'Selected immutable release changed; investigate manually');
      assert((await readFile(join(site, 'reference/gs.md'))).equals(reference), 'Committed reference differs from selected release');
      if (!migration) return { changed: false, reason: 'duplicate', tag: metadata.tag };
      assert(verifyAllAssets, 'Verification metadata migration requires full asset validation');
    }
    await mkdir(join(site, 'reference'), { recursive: true });
    // Complete validation precedes every write. Metadata is the final commit
    // marker; only reviewed, passing PRs can reach the deployed site.
    await writeFile(join(site, 'reference/gs.md.tmp'), reference);
    await writeFile(join(site, 'release.json.tmp'), json);
    await rename(join(site, 'reference/gs.md.tmp'), join(site, 'reference/gs.md'));
    await rename(join(site, 'release.json.tmp'), join(site, 'release.json'));
    return { changed: true, tag: metadata.tag };
  } finally { await rm(directory, { recursive: true, force: true }); }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const args = process.argv.slice(2);
    if (args.length === 1 && args[0] === '--check') {
      validateSnapshot(JSON.parse(await readFile(join(SITE, 'release.json'), 'utf8')), await readFile(join(SITE, 'reference/gs.md')));
      console.log('Committed release snapshot verified');
    } else {
      assert(args.length === 0 || (args.length === 2 && args[0] === '--tag'), 'Usage: update-release.mjs [--tag vVERSION | --check]');
      console.log(JSON.stringify(await update({ tag: args[1] })));
    }
  } catch (error) { console.error(`Release documentation update failed: ${error.message}`); process.exitCode = 1; }
}
