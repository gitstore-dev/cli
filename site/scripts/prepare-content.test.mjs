import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { commandCapabilityNotice, guides, prepareContent, rewriteDestination, rewriteLinks, splitReference, validateAudit } from './prepare-content.mjs';

const reference = `## gs\n\n\`\`\`text\nRoot\nUsage: gs [OPTIONS]\n\n## gs fake-inside-help\n\`\`\`\n\n## gs auth\n\n\`\`\`text\nUsage: gs auth [COMMAND]\n\`\`\`\n\n## gs auth login\n\n\`\`\`text\nUsage: gs auth login [OPTIONS]\n\`\`\`\n`;
const release = { version: '0.1.0-alpha.6', tag: 'v0.1.0-alpha.6', releaseUrl: 'https://github.com/gitstore-dev/cli/releases/tag/v0.1.0-alpha.6', reference: { path: 'reference/gs.md', sha256: createHash('sha256').update(reference).digest('hex') } };
const audit = { version: release.version, reviewedSources: guides.map(({ source }) => source) };
const install = `# Install\n\nBoth commands should report \`${release.version}\`.\n[Release](${release.releaseUrl})\n`;

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

test('selected releases need an explicit content audit and matching installation pointers', () => {
  assert.doesNotThrow(() => validateAudit(release, audit, install));
  assert.throws(() => validateAudit({ ...release, version: '0.1.0-alpha.7' }, audit, install), /Review canonical docs/);
  assert.throws(() => validateAudit(release, { ...audit, reviewedSources: [] }, install), /exact guide allowlist/);
  assert.throws(() => validateAudit(release, audit, install.replaceAll('alpha.6', 'alpha.5')), /Installation version/);
  assert.throws(() => validateAudit(release, audit, `${install}[Old](https://github.com/gitstore-dev/cli/releases/tag/v0.1.0-alpha.5)`), /different release/);
  assert.throws(() => validateAudit({ ...release, sourceCommit: 'another-source' }, audit, install), /capability audit source/);
  const packageTemplate = `https://github.com/gitstore-dev/cli/releases/download/${release.tag}/gitstore_0.1.0.alpha.6-1_\${gitstore_arch}.deb`;
  assert.throws(() => validateAudit(release, audit, `${install}${packageTemplate}`), /asset absent/);
  const withAssets = { ...release, assets: ['amd64', 'arm64'].map((arch) => ({ name: `gitstore_0.1.0.alpha.6-1_${arch}.deb` })) };
  assert.doesNotThrow(() => validateAudit(withAssets, audit, `${install}${packageTemplate}`));
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

test('build imports canonical source, warns about capability gates and validates reference integrity', async () => {
  const fixture = await mkdtemp(path.join(os.tmpdir(), 'gs-site-content-'));
  const root = path.join(fixture, 'site');
  try {
    await mkdir(path.join(root, 'reference'), { recursive: true });
    await mkdir(path.join(fixture, 'docs'), { recursive: true });
    const sources = Object.fromEntries(guides.map(({ source }) => [source, source.endsWith('installation.md') ? install : '# Canonical title\n\nFresh canonical text. [Install](installation.md)\n']));
    const fixtureAudit = { ...audit, sourceSha256: Object.fromEntries(Object.entries(sources).map(([source, text]) => [source, createHash('sha256').update(text).digest('hex')])) };
    await Promise.all([
      writeFile(path.join(root, 'reference/gs.md'), reference),
      writeFile(path.join(root, 'release.json'), JSON.stringify(release)),
      writeFile(path.join(root, 'content-audit.json'), JSON.stringify(fixtureAudit)),
      ...Object.entries(sources).map(([source, text]) => writeFile(path.join(fixture, source), text)),
    ]);
    assert.deepEqual(await prepareContent(root), { guides: 7, commands: 3 });
    const manual = await readFile(path.join(root, 'src/content/docs/manual/auth/login/index.md'), 'utf8');
    assert.match(manual, /title: "gs auth login"/);
    assert.match(manual, /does not establish server, provider or host support/);
    assert.match(manual, /Parent command: `gs auth`/);
    const guide = await readFile(path.join(root, 'src/content/docs/guides/watch/index.md'), 'utf8');
    assert.match(guide, /Fresh canonical text/);
    assert.match(guide, /\[Install\]\(\/install\/\)/);
    assert.match(guide, /controlled-test\/provider qualifications/);
    await writeFile(path.join(root, 'reference/gs.md'), `${reference}\nTampered`);
    await assert.rejects(() => prepareContent(root), /digest mismatch/);
    await writeFile(path.join(fixture, 'docs/watch.md'), 'Unreviewed behavior');
    await assert.rejects(() => prepareContent(root), /Canonical guide changed/);
  } finally {
    await rm(fixture, { recursive: true, force: true });
  }
});
