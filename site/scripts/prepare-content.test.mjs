import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { auditedSourcePaths, commandCapabilityNotice, guides, prepareContent, rewriteDestination, rewriteLinks, splitReference, validateAudit } from './prepare-content.mjs';
import { baselineIdentity } from './capability-audit.mjs';
import { generatedSourcePaths, normalizeReleaseContent, renderReleaseContent } from './release-content.mjs';

const reference = `## gs\n\n\`\`\`text\nRoot\nUsage: gs [OPTIONS]\n\n## gs fake-inside-help\n\`\`\`\n\n## gs auth\n\n\`\`\`text\nUsage: gs auth [COMMAND]\n\`\`\`\n\n## gs auth login\n\n\`\`\`text\nUsage: gs auth login [OPTIONS]\n\`\`\`\n`;
const fingerprint = 'b'.repeat(64);
function selectedRelease(version = '0.1.0-alpha.6') {
  const tag = `v${version}`, assets = [];
  const add = (name, kind, os, arch, logicalName = name) => assets.push({ name, logicalName, kind, os, arch,
    sha256: 'a'.repeat(64), size: 1024, url: `https://github.com/gitstore-dev/cli/releases/download/${tag}/${name}` });
  for (const arch of ['x64', 'arm64']) {
    const deb = `gitstore_${version.replace('-', '~')}-1_${arch === 'x64' ? 'amd64' : arch}.deb`;
    add(deb.replaceAll('~', '.'), 'deb', 'linux', arch, deb);
    add(`gitstore-${version}.${arch === 'x64' ? 'x86_64' : 'aarch64'}.rpm`, 'rpm', 'linux', arch);
    for (const os of ['darwin', 'linux']) add(`gitstore-npm-${version}-${os}-${arch}.tgz`, 'npm', os, arch);
  }
  return { version, tag, sourceCommit: 'a'.repeat(40), releaseUrl: `https://github.com/gitstore-dev/cli/releases/tag/${tag}`, assets,
    reference: { path: 'reference/gs.md', sha256: createHash('sha256').update(reference).digest('hex') },
    verification: { layout: 'per-subject', bundle: null, checksum: 'SHA256SUMS' } };
}
const release = selectedRelease();
const audit = { schemaVersion: 2, version: release.version, commandSourceCommit: release.sourceCommit,
  reviewedRelease: baselineIdentity(release), capabilityFingerprint: fingerprint, reviewedSources: auditedSourcePaths,
  sourceSha256: Object.fromEntries(auditedSourcePaths.map((source) => [source, 'c'.repeat(64)])),
  commandNotices: [{ commands: ['gs auth login'], title: 'Provider limits', message: 'Only explicit static users.', evidence: 'crates/gs/src/auth.rs' }] };
const install = `# Install\n\nBoth commands should report \`${release.version}\`.\n[Release](${release.releaseUrl})\n`;
const templates = Object.fromEntries(await Promise.all(generatedSourcePaths.map(async (source) =>
  [source, await readFile(new URL(`../../${source}`, import.meta.url), 'utf8')])));
const unchangedAssessment = () => ({ status: 'unchanged', baseline: { fingerprint }, source: { fingerprint } });

async function fixture(t, selected = release) {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'gs-site-content-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const root = path.join(directory, 'site');
  await mkdir(path.join(root, 'reference'), { recursive: true });
  await mkdir(path.join(directory, 'docs'), { recursive: true });
  const sources = renderReleaseContent({ release: selected, sources: Object.fromEntries(auditedSourcePaths.map((source) =>
    [source, templates[source] ?? '# Canonical title\n\nFresh canonical text. [Install](installation.md)\n'])) });
  const fixtureAudit = { ...audit, sourceSha256: Object.fromEntries(Object.entries(sources).map(([source, text]) =>
    [source, createHash('sha256').update(normalizeReleaseContent(text)).digest('hex')])) };
  await Promise.all([
    writeFile(path.join(root, 'reference/gs.md'), reference),
    writeFile(path.join(root, 'release.json'), JSON.stringify(selected)),
    writeFile(path.join(root, 'content-audit.json'), JSON.stringify(fixtureAudit)),
    ...Object.entries(sources).map(([source, text]) => writeFile(path.join(directory, source), text)),
  ]);
  return { root, directory, sources, fixtureAudit };
}

test('nested command routes come from actual headings, never help text', () => {
  const sections = splitReference(reference);
  assert.deepEqual(sections.map(({ route }) => route), ['/manual/', '/manual/auth/', '/manual/auth/login/']);
  assert.match(sections[0].help, /## gs fake-inside-help/);
});

test('malformed, duplicate, incomplete and disconnected references fail closed', () => {
  for (const value of [
    '',
    reference.replace('## gs\n', '## gitstore\n'),
    reference + '\n## gs auth\n',
    reference.replace('## gs auth\n', '## gs other\n'),
    reference.replace('Usage: gs auth login [OPTIONS]', ''),
    reference.slice(0, -5),
  ]) assert.throws(() => splitReference(value));
});

test('canonical guide links become routes, contributor and asset links stay on GitHub', () => {
  assert.equal(rewriteDestination('local-runtime.md#before-you-start', 'docs/installation.md', 'main'), '/guides/local-runtime/#before-you-start');
  assert.equal(rewriteDestination('contributing/building.md', 'docs/installation.md', 'main'), 'https://github.com/gitstore-dev/cli/blob/main/docs/contributing/building.md');
  assert.equal(rewriteDestination('../examples/config.toml', 'docs/configuration.md', 'main'), 'https://github.com/gitstore-dev/cli/blob/main/examples/config.toml');
  assert.equal(rewriteDestination('#credentials', 'docs/configuration.md', 'main'), '#credentials');
  assert.equal(rewriteDestination('https://example.org', 'docs/configuration.md', 'main'), 'https://example.org');
  assert.throws(() => rewriteDestination('../../secrets', 'docs/configuration.md', 'main'));
});

test('rewriting preserves fenced shell text and handles reference-style links', () => {
  const input = '[Guide](installation.md)\n```sh\necho "[Guide](installation.md)"\n```\n[reference]: watch.md#bounds-and-credentials "Watch"';
  assert.equal(rewriteLinks(input, 'docs/README.md', 'main'), '[Guide](/install/)\n```sh\necho "[Guide](installation.md)"\n```\n[reference]: /guides/watch/#bounds-and-credentials "Watch"');
});

test('schema 2 binds the reviewed capability baseline and matching installation pointers', () => {
  assert.doesNotThrow(() => validateAudit(release, audit, install));
  assert.throws(() => validateAudit(release, { ...audit, schemaVersion: 1 }, install), /schemaVersion 2/);
  assert.throws(() => validateAudit(release, { ...audit, version: '0.1.0-alpha.7' }, install), /reviewed baseline/);
  assert.throws(() => validateAudit(release, { ...audit, reviewedSources: [] }, install), /exact guide and generated source allowlist/);
  assert.throws(() => validateAudit(release, { ...audit, sourceSha256: {} }, install), /authored source hashes/);
  assert.throws(() => validateAudit(release, audit, install.replaceAll('alpha.6', 'alpha.5')), /Installation version/);
  assert.throws(() => validateAudit(release, audit, `${install}[Old](https://github.com/gitstore-dev/cli/releases/tag/v0.1.0-alpha.5)`), /different release/);
  assert.throws(() => validateAudit(release, { ...audit, commandSourceCommit: 'd'.repeat(40) }, install), /capability audit source/);
  const packageTemplate = `https://github.com/gitstore-dev/cli/releases/download/${release.tag}/gitstore_0.1.0.alpha.6-1_\${gitstore_arch}.deb`;
  const missing = { ...release, assets: release.assets.filter(({ kind }) => kind !== 'deb') };
  assert.throws(() => validateAudit(missing, audit, `${install}${packageTemplate}`), /asset absent/);
  assert.doesNotThrow(() => validateAudit(release, audit, `${install}${packageTemplate}`));
});

test('audited gates appear on exact commands and all reserved component child pages', async () => {
  const realAudit = JSON.parse(await readFile(new URL('../content-audit.json', import.meta.url), 'utf8'));
  const selected = { sourceCommit: realAudit.commandSourceCommit };
  assert.match(commandCapabilityNotice('gs update', realAudit, selected), /UNSUPPORTED_CAPABILITY[\s\S]+DEP-009/);
  for (const component of ['admin', 'storefront']) {
    for (const suffix of ['', ' serve', ' config', ' config validate']) {
      const notice = commandCapabilityNotice(`gs ${component}${suffix}`, realAudit, selected);
      assert.match(notice, /Unavailable:/);
      assert.match(notice, new RegExp(`/blob/${realAudit.commandSourceCommit}/crates/gs/src/deployment.rs`));
    }
  }
  assert.equal(commandCapabilityNotice('gs administrator', realAudit, selected), '');
  assert.match(commandCapabilityNotice('gs oidc config validate', realAudit, selected), /separate development evidence/);
  assert.match(commandCapabilityNotice('gs watch', realAudit, selected), /DEP-018/);
  assert.match(commandCapabilityNotice('gs auth login', realAudit, selected), /Browser\/device\/OIDC discovery/);
  assert.match(commandCapabilityNotice('gs list', realAudit, selected), /File listing is unavailable/);
  assert.equal(commandCapabilityNotice('gs context list', realAudit, selected), '');
});

test('build imports canonical source, warns about capability gates and validates reference integrity', async (t) => {
    const { root, directory } = await fixture(t);
    assert.deepEqual(await prepareContent(root, { assess: unchangedAssessment }), { guides: 7, commands: 3 });
    const manual = await readFile(path.join(root, 'src/content/docs/manual/auth/login/index.md'), 'utf8');
    assert.match(manual, /title: "gs auth login"/);
    assert.match(manual, /does not establish server, provider or host support/);
    assert.match(manual, /Parent command: `gs auth`/);
    const guide = await readFile(path.join(root, 'src/content/docs/guides/watch/index.md'), 'utf8');
    assert.match(guide, /Fresh canonical text/);
    assert.match(guide, /\[Install\]\(\/install\/\)/);
    assert.match(guide, /controlled-test\/provider qualifications/);
    await writeFile(path.join(root, 'reference/gs.md'), `${reference}\nTampered`);
    await assert.rejects(() => prepareContent(root, { assess: unchangedAssessment }), /digest mismatch/);
    await writeFile(path.join(directory, 'docs/watch.md'), 'Unreviewed behavior');
    await assert.rejects(() => prepareContent(root, { assess: unchangedAssessment }), /Canonical authored content changed/);
});

test('later release with identical capability evidence carries the reviewed baseline and normalized prose hashes', async (t) => {
  const next = selectedRelease('0.1.0-alpha.8');
  const { root, fixtureAudit } = await fixture(t, next);
  assert.equal(fixtureAudit.version, '0.1.0-alpha.6');
  let called = false;
  const assess = (options) => {
    called = true;
    assert.equal(options.baselineRelease.version, '0.1.0-alpha.6');
    assert.equal(options.release.version, '0.1.0-alpha.8');
    return unchangedAssessment();
  };
  assert.deepEqual(await prepareContent(root, { assess }), { guides: 7, commands: 3 });
  assert(called);
});

test('all generated blocks are checked even when normalization intentionally excludes their bodies', async (t) => {
  for (const source of generatedSourcePaths) {
    const { root, directory, sources } = await fixture(t);
    const modified = sources[source].replace(/(<!-- release-docs:[a-z-]+:start -->\n)/, '$1Unverified generated claim.\n');
    assert.equal(normalizeReleaseContent(modified), normalizeReleaseContent(sources[source]));
    await writeFile(path.join(directory, source), modified);
    await assert.rejects(() => prepareContent(root, { assess: unchangedAssessment }), /Generated release content is stale or modified/);
  }
});

test('root README authored prose and complete hash allowlist participate in the audit', async (t) => {
  const { root, directory, sources } = await fixture(t);
  await writeFile(path.join(directory, 'README.md'), `${sources['README.md']}\nNew unreviewed platform claim.\n`);
  await assert.rejects(() => prepareContent(root, { assess: unchangedAssessment }), /Canonical authored content changed: review README.md/);
});

test('stored capability hashes are verified against both baseline and selected Git evidence', async (t) => {
  const { root } = await fixture(t);
  const assessBaselineTamper = () => ({ ...unchangedAssessment(), baseline: { fingerprint: 'e'.repeat(64) } });
  await assert.rejects(() => prepareContent(root, { assess: assessBaselineTamper }), /Reviewed capability fingerprint does not match/);
  const assessChanged = () => ({ status: 'review-required', baseline: { fingerprint }, source: { fingerprint: 'e'.repeat(64) } });
  await assert.rejects(() => prepareContent(root, { assess: assessChanged }), /Selected release changes capability inputs/);
  await assert.rejects(() => prepareContent(root, { assess: () => { throw new Error('Missing source object'); } }), /Missing source object/);
});
