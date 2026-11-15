import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFileSync } from 'node:child_process';
import { compareVersions, selectRelease, parseChecksums, validateInventory, validateReference,
  validateSnapshot, sha256, update } from './update-release.mjs';

const source = 'a'.repeat(40);
const reference = '## gs\n\n```text\nGitStore CLI\nUsage: gs [OPTIONS] <COMMAND>\n```\n\n## gs auth login\n\n```text\nLogin to a remote GitStore installation with explicitly selected credentials.\nUsage: gs auth login [OPTIONS]\n```\n';

async function fixture(t, current = false) {
  const root = await mkdtemp(join(tmpdir(), 'gitstore-docs-test-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const stage = join(root, 'stage');
  await mkdir(join(stage, 'share/reference'), { recursive: true });
  await writeFile(join(stage, 'share/reference/gs.md'), reference);
  const archive = join(root, 'reference.tar.gz');
  execFileSync('tar', ['-czf', archive, '-C', stage, 'share/reference/gs.md']);
  const archiveBytes = await readFile(archive);
  const payloads = new Map();
  const version = '0.1.0-alpha.6';
  const inventory = { schemaVersion: 1, tag: `v${version}`, version, sourceRevision: source, artifacts: [] };
  if (current) inventory.installationInventories = {};
  for (const os of ['darwin', 'linux']) for (const arch of ['arm64', 'x64']) {
    const installName = `gitstore-${version}-${os}-${arch}.install.json`;
    const install = { schemaVersion: 1, applicationVersion: version, sourceRevision: source, os, arch, files: [] };
    if (current) inventory.installationInventories[installName] = install;
    const kinds = ['archive', 'npm', ...(current ? [] : ['manifest']), ...(os === 'linux' ? ['deb', 'rpm'] : [])];
    for (const kind of kinds) {
      const filename = kind === 'deb' ? `gitstore_0.1.0~alpha.6-1_${arch}.deb` :
        kind === 'manifest' ? installName : `gitstore-${version}-${os}-${arch}.${kind === 'archive' ? 'tar.gz' : kind}`;
      const bytes = kind === 'archive' ? archiveBytes : Buffer.from(kind === 'manifest' ? JSON.stringify(install) : filename);
      inventory.artifacts.push({ filename, format: kind, os, arch, applicationVersion: version,
        sourceRevision: source, bytes: bytes.length, sha256: sha256(bytes), installInventory: installName });
      payloads.set(filename.replaceAll('~', '.'), bytes);
    }
  }
  const formula = Buffer.from('class Gitstore < Formula; end');
  inventory.artifacts.push({ filename: 'gitstore.rb', format: 'formula', os: 'any', arch: 'any',
    applicationVersion: version, sourceRevision: source, bytes: formula.length, sha256: sha256(formula) });
  payloads.set('gitstore.rb', formula);
  const inventoryBytes = Buffer.from(JSON.stringify(inventory));
  payloads.set('release-inventory.json', inventoryBytes);
  const checksums = new Map(inventory.artifacts.map((a) => [a.filename, a.sha256]));
  checksums.set('release-inventory.json', sha256(inventoryBytes));
  payloads.set('SHA256SUMS', Buffer.from([...checksums].map(([name, hash]) => `${hash}  ${name}\n`).join('')));
  if (current) payloads.set('release.sigstore.jsonl', Buffer.from('shared verification bundle fixture'));
  else for (const name of [...payloads.keys()]) payloads.set(`${name}.sigstore.jsonl`, Buffer.from('per-subject bundle fixture'));
  const release = { draft: false, prerelease: true, tag_name: `v${version}`, published_at: '2026-11-14T00:45:05Z',
    html_url: `https://github.com/gitstore-dev/cli/releases/tag/v${version}`,
    assets: [...payloads].map(([name, bytes]) => ({ name, size: bytes.length, digest: `sha256:${sha256(bytes)}`,
      browser_download_url: `https://github.com/gitstore-dev/cli/releases/download/v${version}/${name}` })) };
  const site = join(root, 'site');
  await mkdir(site);
  const get = (path) => path.startsWith('commits/') ? { sha: source } : [release];
  const fetchAsset = async (asset, directory) => {
    const file = join(directory, asset.name);
    await writeFile(file, payloads.get(asset.name));
    return file;
  };
  return { inventory, checksums, release, payloads, site, get, fetchAsset };
}

test('semver selects published prereleases, excluding drafts and stale event order', () => {
  assert(compareVersions('0.1.0-alpha.10', '0.1.0-alpha.6') > 0);
  assert(compareVersions('0.1.0', '0.1.0-rc.2') > 0);
  assert.equal(compareVersions('0.1.0-alpha.6', '0.1.0-alpha.6'), 0);
  const rows = [
    { tag_name: 'v0.1.0-alpha.6', published_at: '2026-01-02', prerelease: true },
    { tag_name: 'v0.1.0-alpha.7', draft: true, published_at: null },
    { tag_name: 'v0.1.0-alpha.5', published_at: '2026-01-03', prerelease: true },
  ];
  assert.equal(selectRelease(rows).tag_name, 'v0.1.0-alpha.6');
  assert.throws(() => selectRelease(rows, 'v0.1.0-alpha.7'), /No matching published/);
});

for (const current of [false, true]) test(`validates ${current ? 'embedded current' : 'legacy manifest'} inventory and Debian aliases`, async (t) => {
  const f = await fixture(t, current);
  const assets = validateInventory(f.inventory, f.release, source, f.checksums);
  const deb = assets.find((a) => a.kind === 'deb');
  assert(deb.logicalName.includes('~'));
  assert(!deb.name.includes('~'));
  assert.equal(assets.filter((a) => a.kind === 'archive').length, 4);
});

test('rejects missing, mismatched, duplicated and unknown assets or identities', async (t) => {
  const f = await fixture(t);
  assert.throws(() => validateInventory(f.inventory, { ...f.release, assets: f.release.assets.slice(1) }, source, f.checksums), /Missing or mismatched/);
  const bad = structuredClone(f.inventory);
  bad.artifacts[0].sha256 = 'b'.repeat(64);
  assert.throws(() => validateInventory(bad, f.release, source, f.checksums), /checksum mismatch/);
  assert.throws(() => validateInventory(f.inventory, f.release, 'b'.repeat(40), f.checksums), /identity mismatch/);
  bad.artifacts = [...f.inventory.artifacts, f.inventory.artifacts[0]];
  assert.throws(() => validateInventory(bad, f.release, source, f.checksums), /Duplicate/);
  assert.throws(() => parseChecksums('bad checksum'), /Malformed/);
  assert.throws(() => validateInventory(f.inventory, { ...f.release,
    assets: f.release.assets.filter((a) => !a.name.endsWith('.sigstore.jsonl')) }, source, f.checksums), /Missing release verification bundle/);
});

test('reference rejects malformed headings, duplicates and unterminated fences', () => {
  assert.equal(validateReference(reference).size, 2);
  assert.throws(() => validateReference(`${reference}\n## gs\n`), /duplicate/);
  assert.throws(() => validateReference(`${reference}\n<script>alert(1)</script>`), /Malformed/);
  assert.throws(() => validateReference(`${reference}\n\x60\x60\x60text\n`), /Incomplete/);
});

test('imports once, duplicate event preserves bytes, stale event never downgrades', async (t) => {
  const f = await fixture(t);
  assert.deepEqual(await update(f), { changed: true, tag: f.release.tag_name });
  const metadata = JSON.parse(await readFile(join(f.site, 'release.json'), 'utf8'));
  const bytes = await readFile(join(f.site, 'reference/gs.md'));
  validateSnapshot(metadata, bytes);
  assert.equal(bytes.toString(), reference);
  assert.deepEqual(await update(f), { changed: false, reason: 'duplicate', tag: f.release.tag_name });
  metadata.version = '0.1.0-alpha.10';
  metadata.tag = 'v0.1.0-alpha.10';
  await writeFile(join(f.site, 'release.json'), JSON.stringify(metadata));
  assert.deepEqual(await update(f), { changed: false, reason: 'stale', tag: metadata.tag });
});

test('tampered download fails before changing committed files', async (t) => {
  const f = await fixture(t);
  await update(f);
  const original = await readFile(join(f.site, 'release.json'));
  const name = [...f.payloads.keys()].find((n) => n.endsWith('.tar.gz'));
  f.payloads.set(name, Buffer.from('tampered'));
  await assert.rejects(update(f), /Downloaded asset digest mismatch/);
  assert((await readFile(join(f.site, 'release.json'))).equals(original));
  assert.equal((await readFile(join(f.site, 'reference/gs.md'))).toString(), reference);
});

test('same immutable tag with changed inventory fails instead of silently replacing', async (t) => {
  const f = await fixture(t);
  await update(f);
  const original = JSON.parse(await readFile(join(f.site, 'release.json'), 'utf8'));
  await writeFile(join(f.site, 'release.json'), JSON.stringify({ ...original, sourceCommit: 'b'.repeat(40) }));
  await assert.rejects(update(f), /immutable release changed/);
});

for (const current of [false, true]) test(`records verified ${current ? 'shared' : 'per-subject'} bundle layout`, async (t) => {
  const f = await fixture(t, current);
  await update(f);
  const metadata = JSON.parse(await readFile(join(f.site, 'release.json'), 'utf8'));
  assert.deepEqual(metadata.verification, { layout: current ? 'shared' : 'per-subject',
    bundle: current ? 'release.sigstore.jsonl' : null, checksum: 'SHA256SUMS' });
  const bytes = await readFile(join(f.site, 'reference/gs.md'));
  validateSnapshot(metadata, bytes);
  const legacy = structuredClone(metadata);
  delete legacy.verification;
  validateSnapshot(legacy, bytes);
  for (const bad of [null, { layout: 'shared', bundle: null, checksum: 'SHA256SUMS' },
    { layout: 'per-subject', bundle: 'release.sigstore.jsonl', checksum: 'SHA256SUMS' },
    { layout: 'shared', bundle: 'release.sigstore.jsonl', checksum: '../SHA256SUMS' }]) {
    assert.throws(() => validateSnapshot({ ...metadata, verification: bad }, bytes), /verification layout/);
  }
});

test('same-tag migration adds only missing verification metadata after full validation', async (t) => {
  const f = await fixture(t, true);
  await update(f);
  const expected = await readFile(join(f.site, 'release.json'));
  const legacy = JSON.parse(expected);
  delete legacy.verification;
  await writeFile(join(f.site, 'release.json'), `${JSON.stringify(legacy, null, 2)}\n`);
  assert.deepEqual(await update(f), { changed: true, tag: f.release.tag_name });
  assert((await readFile(join(f.site, 'release.json'))).equals(expected));
  assert.deepEqual(await update(f), { changed: false, reason: 'duplicate', tag: f.release.tag_name });
});

test('migration never conceals immutable field, reference, or asset-byte changes', async (t) => {
  for (const change of ['metadata', 'reference', 'asset', 'verification']) {
    const f = await fixture(t, true);
    await update(f);
    const metadata = JSON.parse(await readFile(join(f.site, 'release.json'), 'utf8'));
    delete metadata.verification;
    if (change === 'metadata') metadata.publishedAt = '2026-01-01T00:00:00Z';
    if (change === 'verification') metadata.verification = { layout: 'per-subject', bundle: null, checksum: 'SHA256SUMS' };
    if (change === 'reference') await writeFile(join(f.site, 'reference/gs.md'), reference + '\n');
    if (change === 'asset') f.payloads.set('gitstore.rb', Buffer.from('tampered'));
    const original = `${JSON.stringify(metadata, null, 2)}\n`;
    await writeFile(join(f.site, 'release.json'), original);
    await assert.rejects(update(f), /immutable release changed|reference differs|digest mismatch/);
    assert.equal(await readFile(join(f.site, 'release.json'), 'utf8'), original);
  }
});

test('narrow queue verification downloads only inventory, checksum and digest-checked reference archive', async (t) => {
  const f = await fixture(t, true);
  const downloaded = [];
  const fetchAsset = async (asset, directory) => { downloaded.push(asset.name); return f.fetchAsset(asset, directory); };
  await update({ ...f, fetchAsset, verifyAllAssets: false });
  const archive = f.inventory.artifacts.find(a => a.format === 'archive' && a.os === 'darwin' && a.arch === 'arm64').filename;
  assert.deepEqual(downloaded, ['release-inventory.json', 'SHA256SUMS', archive]);
  const narrow = await readFile(join(f.site, 'release.json'));
  assert.deepEqual(await update(f), { changed: false, reason: 'duplicate', tag: f.release.tag_name });
  assert((await readFile(join(f.site, 'release.json'))).equals(narrow));
  const original = await readFile(join(f.site, 'release.json'));
  f.payloads.set(archive, Buffer.from('tampered reference archive'));
  await assert.rejects(update({ ...f, verifyAllAssets: false }), /Downloaded asset digest mismatch/);
  assert((await readFile(join(f.site, 'release.json'))).equals(original));
});

test('narrow verification still rejects missing or mismatched un-downloaded metadata and bundles', async (t) => {
  for (const change of ['asset-missing', 'asset-digest', 'bundle-missing', 'bundle-digest', 'bundle-url']) {
    const f = await fixture(t, true);
    await update(f);
    const original = await readFile(join(f.site, 'release.json'));
    const asset = f.release.assets.find(a => a.name === (change.startsWith('bundle') ? 'release.sigstore.jsonl' : 'gitstore.rb'));
    if (change.endsWith('missing')) f.release.assets.splice(f.release.assets.indexOf(asset), 1);
    else if (change === 'asset-digest') asset.digest = 'sha256:' + 'b'.repeat(64);
    else if (change === 'bundle-digest') asset.digest = 'not-a-digest';
    else asset.browser_download_url = 'https://untrusted.example/bundle';
    await assert.rejects(update({ ...f, verifyAllAssets: false }), /Missing|mismatched|metadata|URL/);
    assert((await readFile(join(f.site, 'release.json'))).equals(original));
  }
});

test('narrow mode cannot migrate a legacy snapshot without all-asset verification', async (t) => {
  const f = await fixture(t, true);
  await update(f);
  const legacy = JSON.parse(await readFile(join(f.site, 'release.json'), 'utf8'));
  delete legacy.verification;
  const original = JSON.stringify(legacy);
  await writeFile(join(f.site, 'release.json'), original);
  await assert.rejects(update({ ...f, verifyAllAssets: false }), /migration requires full asset validation/);
  assert.equal(await readFile(join(f.site, 'release.json'), 'utf8'), original);
});
