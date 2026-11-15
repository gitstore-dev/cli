import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import { generatedSourcePaths, normalizeReleaseContent, renderReleaseContent } from './release-content.mjs';

const sources = Object.fromEntries(await Promise.all(generatedSourcePaths.map(async (source) =>
  [source, await readFile(new URL(`../../${source}`, import.meta.url), 'utf8')])));

function fixture(version = '0.1.0-alpha.7', layout = 'shared') {
  const tag = `v${version}`;
  const assets = [];
  const add = (name, kind, os, arch, logicalName = name) => assets.push({
    name, logicalName, kind, os, arch, sha256: 'a'.repeat(64), size: 1024,
    url: `https://github.com/gitstore-dev/cli/releases/download/${tag}/${name}`,
  });
  for (const arch of ['x64', 'arm64']) {
    const deb = `gitstore_${version.replace('-', '~')}-1_${arch === 'x64' ? 'amd64' : arch}.deb`;
    add(deb.replaceAll('~', '.'), 'deb', 'linux', arch, deb);
    add(`gitstore-${version}-${arch === 'x64' ? 'x86_64' : 'aarch64'}.${arch === 'x64' ? 'x86_64' : 'aarch64'}.rpm`, 'rpm', 'linux', arch);
    for (const os of ['darwin', 'linux']) add(`gitstore-npm-${version}-${os}-${arch}.tgz`, 'npm', os, arch);
  }
  return { version, tag, releaseUrl: `https://github.com/gitstore-dev/cli/releases/tag/${tag}`, assets,
    verification: { layout, bundle: layout === 'shared' ? 'release.sigstore.jsonl' : null, checksum: 'SHA256SUMS' } };
}

test('mechanical docs render idempotently without mutating input', () => {
  const release = fixture();
  const before = structuredClone(sources);
  const rendered = renderReleaseContent({ release, sources });
  assert.deepEqual(sources, before);
  assert.deepEqual(renderReleaseContent({ release, sources: rendered }), rendered);
  assert.deepEqual(renderReleaseContent({ release, sources: new Map(Object.entries(rendered)) }), rendered);
  assert.deepEqual(Object.keys(rendered).sort(), [...generatedSourcePaths].sort());
  for (const source of generatedSourcePaths) assert.equal(normalizeReleaseContent(rendered[source]), normalizeReleaseContent(sources[source]));
});

test('new versions use exact hosted asset URLs and cover every architecture explicitly', () => {
  const release = fixture('0.2.0-alpha.8');
  const rendered = renderReleaseContent({ release, sources });
  const installation = rendered['docs/installation.md'];
  for (const asset of release.assets) assert.ok(installation.includes(asset.url), `Missing ${asset.name}`);
  assert.match(installation, /gitstore_arch=amd64/);
  assert.match(installation, /gitstore_arch=x86_64/);
  assert.match(installation, /gitstore_platform=darwin-arm64/);
  assert.match(installation, /--global --ignore-scripts/);
  assert.match(installation, /brew trust --formula gitstore-dev\/cli\/gitstore/);
  assert.match(installation, /Both commands should report `0\.2\.0-alpha\.8`/);
  assert.ok(!installation.includes('/download/v0.1.0-alpha.7/'));
  assert.match(rendered['README.md'], /0\.2\.0-alpha\.8 prerelease/);
  assert.match(rendered['docs/README.md'], /Direct packages are pinned to 0\.2\.0-alpha\.8/);
  assert.ok(!/https:\/\/[^\s'"]*~/.test(installation), 'Downloads must use Debian hosted aliases rather than signed logical names');
});

test('verification reflects selected legacy and shared inventories', () => {
  const legacy = renderReleaseContent({ release: fixture('0.1.0-alpha.6', 'per-subject'), sources })['docs/installation.md'];
  assert.match(legacy, /--bundle \.\/PACKAGE\.sigstore\.jsonl/);
  assert.match(legacy, /`SHA256SUMS` checksums and a matching/);
  const current = renderReleaseContent({ release: fixture(), sources })['docs/installation.md'];
  assert.match(current, /--bundle \.\/release\.sigstore\.jsonl/);
  assert.match(current, /one shared\n`release\.sigstore\.jsonl`/);
});

test('authored guidance and historical diagnostic/qualification notes stay unchanged', () => {
  const rendered = renderReleaseContent({ release: fixture('0.1.0-alpha.8'), sources });
  for (const source of generatedSourcePaths) {
    assert.equal(normalizeReleaseContent(rendered[source]), normalizeReleaseContent(sources[source]));
  }
  for (const phrase of [
    'If an older alpha.5 installation instead reports',
    'Alpha.6\ncorrects that diagnostic',
    'Older alpha.5/alpha.6 releases',
  ]) assert.ok(rendered['docs/installation.md'].includes(phrase));
  assert.ok(rendered['docs/README.md'].includes('Alpha.6 native core acceptance'));
  assert.ok(rendered['README.md'].includes('Alpha.5 native core\nacceptance'));
  assert.notEqual(normalizeReleaseContent(`${sources['README.md']}\nChanged capability claim.\n`), normalizeReleaseContent(sources['README.md']));
});

test('missing/duplicate platforms, tampered URL/name/digest and invalid verification fail closed', () => {
  const invalid = [];
  const missing = fixture(); missing.assets.pop(); invalid.push(missing);
  const duplicate = fixture(); duplicate.assets.push({ ...duplicate.assets[0] }); invalid.push(duplicate);
  const alias = fixture(); alias.assets[0].name = alias.assets[0].logicalName; invalid.push(alias);
  const url = fixture(); url.assets[0].url = url.assets[0].url.replace('/v0.1.0-alpha.7/', '/v0.1.0-alpha.6/'); invalid.push(url);
  const arch = fixture(); arch.assets[0].arch = 'arm64'; invalid.push(arch);
  const sha = fixture(); sha.assets[0].sha256 = 'tampered'; invalid.push(sha);
  const injection = fixture(); injection.assets[0].name = "file'; curl bad"; invalid.push(injection);
  const missingVerification = fixture(); delete missingVerification.verification; invalid.push(missingVerification);
  const badBundle = fixture(); badBundle.verification.bundle = 'unexpected.jsonl'; invalid.push(badBundle);
  const inconsistentBundle = fixture(); inconsistentBundle.verification.layout = 'per-subject'; invalid.push(inconsistentBundle);
  const badChecksum = fixture(); badChecksum.verification.checksum = 'OTHER'; invalid.push(badChecksum);
  const badVersion = fixture(); badVersion.version = '1.0.0\n'; invalid.push(badVersion);
  for (const release of invalid) assert.throws(() => renderReleaseContent({ release, sources }));
});

test('unknown, malformed, misplaced, missing and duplicate markers cannot hide authored edits', () => {
  const release = fixture();
  const source = sources['README.md'];
  for (const text of [
    source.replaceAll('readme-release', 'arbitrary-template'),
    source.replace('readme-release:start', 'readme-release: start'),
    source.replace('<!-- release-docs:readme-release:end -->', ''),
    source + '\n<!-- release-docs:readme-release:start -->\nDuplicate\n<!-- release-docs:readme-release:end -->',
    source.replaceAll('readme-release', 'install-version'),
  ]) assert.throws(() => renderReleaseContent({ release, sources: { ...sources, 'README.md': text } }));
  assert.throws(() => normalizeReleaseContent('<!-- release-docs:unknown:start -->\nBody\n<!-- release-docs:unknown:end -->'));
});
