import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import test from 'node:test';
import { assessCapabilities, baselineIdentity, compareInputs, fingerprintInputs, isCapabilityInput, normalizeInput } from './capability-audit.mjs';

const v7 = '0.1.0-alpha.7', v8 = '0.1.0-alpha.8';
const reference = 'a'.repeat(64);
const cargo = (version) => `[workspace]\nmembers = ["crates/gs"]\n\n[workspace.package]\nversion = "${version}"\n\n[profile.release]\nlto = true\n`;
const lock = (version) => `version = 4\n\n[[package]]\nname = "gs"\nversion = "${version}"\ndependencies = ["http"]\n\n[[package]]\nname = "http"\nversion = "1.0.0"\nsource = "registry+https://example.invalid"\n`;
const baseFiles = (version) => ({
  'Cargo.toml': cargo(version), 'Cargo.lock': lock(version), 'version.txt': `${version}\n`,
  'rust-toolchain.toml': '[toolchain]\nchannel = "1.96"\n',
  'crates/gs/Cargo.toml': '[package]\nname = "gs"\nversion.workspace = true\n',
  'crates/gs/src/cli.rs': 'pub fn supported() -> bool { false }\n',
  'daemon/CMakeLists.txt': 'add_executable(gitstored src/main.c)\n',
  'daemon/src/main.c': 'int main(void) { return 0; }\n',
  'runtime/profiles/core.json': '{"image":"sha256:abc"}\n',
  'runtime/packaging/release-policy.md': 'Only qualified native hosts are supported.\n',
  'packaging/platforms.json': '{"rows":[{"os":"linux","arch":"x64"}]}\n',
  'docs/contracts/command-matrix.md': 'Updates are unavailable.\n',
  '.github/workflows/release.yml': 'name: Release\njobs:\n  build:\n    runs-on: ubuntu-24.04\n    steps:\n      - run: cargo build --release\n  publish:\n    steps:\n      - run: publish\n',
});

async function fixture(t) {
  const repoRoot = await mkdtemp(join(tmpdir(), 'gs-capability-audit-'));
  t.after(() => rm(repoRoot, { recursive: true, force: true }));
  const git = (...args) => execFileSync('git', ['-C', repoRoot, ...args], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }).trim();
  git('init', '-q');
  git('config', 'user.name', 'Synthetic Test');
  git('config', 'user.email', 'synthetic@example.invalid');
  const write = async (path, text) => { await mkdir(dirname(join(repoRoot, path)), { recursive: true }); await writeFile(join(repoRoot, path), text); };
  for (const [path, text] of Object.entries(baseFiles(v7))) await write(path, text);
  git('add', '.'); git('commit', '-qm', 'Baseline source'); git('tag', `v${v7}`);
  const baselineRelease = { version: v7, tag: `v${v7}`, sourceCommit: git('rev-parse', 'HEAD'), reference: { sha256: reference },
    assets: [{ kind: 'archive', os: 'linux', arch: 'x64' }] };
  const audit = { version: v7, commandSourceCommit: baselineRelease.sourceCommit,
    commandNotices: [{ commands: ['gs update'], evidence: 'crates/gs/src/cli.rs', title: 'Unavailable', message: 'Safe updates are gated.' }] };
  const release = structuredClone(baselineRelease);
  const commit = async (changes = {}) => {
    await write('Cargo.toml', cargo(v8)); await write('Cargo.lock', lock(v8)); await write('version.txt', `${v8}\n`);
    for (const [path, text] of Object.entries(changes)) {
      if (text === null) await rm(join(repoRoot, path)); else await write(path, text);
    }
    git('add', '-A'); git('commit', '-qm', 'Selected release source'); git('tag', `v${v8}`);
    Object.assign(release, { version: v8, tag: `v${v8}`, sourceCommit: git('rev-parse', 'HEAD') });
  };
  return { repoRoot, baselineRelease, release, audit, commit, git, write };
}

test('own version-only bump is unchanged and deterministic, despite different release inventory digests', async (t) => {
  const f = await fixture(t); await f.commit();
  f.release.inventorySha256 = 'b'.repeat(64);
  const result = assessCapabilities(f);
  assert.equal(result.status, 'unchanged');
  assert.deepEqual(result.changedInputs, []);
  assert.equal(result.baseline.fingerprint, result.source.fingerprint);
  assert.deepEqual(result, assessCapabilities(f));
  assert.deepEqual(result, assessCapabilities({ ...f, baselineRelease: baselineIdentity(f.baselineRelease), release: baselineIdentity(f.release) }));
});

test('reviewed baseline identity stores only capability identity and validates its shape', () => {
  const release = { version: v7, tag: `v${v7}`, sourceCommit: 'a'.repeat(40), reference: { sha256: reference, path: 'reference/gs.md' },
    publishedAt: '2026-11-14', assets: [{ kind: 'archive', os: 'linux', arch: 'x64', url: 'https://example.invalid', size: 42, sha256: 'b'.repeat(64) }] };
  const baseline = baselineIdentity(release);
  assert.deepEqual(Object.keys(baseline), ['version', 'tag', 'sourceCommit', 'reference', 'assets']);
  assert.deepEqual(baseline.assets, [{ kind: 'archive', os: 'linux', arch: 'x64' }]);
  assert.deepEqual(baseline.reference, { sha256: reference });
  assert.throws(() => baselineIdentity({ ...release, sourceCommit: 'bad' }), /Invalid reviewed/);
});

for (const [name, change, expected] of [
  ['behavior changes with identical help', { 'crates/gs/src/cli.rs': 'pub fn supported() -> bool { true }\n' }, 'crates/gs/src/cli.rs'],
  ['daemon behavior', { 'daemon/src/main.c': 'int main(void) { return 6; }\n' }, 'daemon/src/main.c'],
  ['dependency update', { 'Cargo.lock': lock(v8).replace('version = "1.0.0"', 'version = "2.0.0"') }, 'Cargo.lock'],
  ['build feature change', { 'Cargo.toml': cargo(v8).replace('lto = true', 'lto = false') }, 'Cargo.toml'],
  ['native build flag', { 'daemon/CMakeLists.txt': 'add_compile_definitions(NEW_BACKEND)\n' }, 'daemon/CMakeLists.txt'],
  ['runtime profile pin', { 'runtime/profiles/core.json': '{"image":"sha256:def"}\n' }, 'runtime/profiles/core.json'],
  ['platform declaration', { 'packaging/platforms.json': '{"rows":[{"os":"linux","arch":"arm64"}]}\n' }, 'packaging/platforms.json'],
  ['support contract', { 'docs/contracts/command-matrix.md': 'Updates are supported.\n' }, 'docs/contracts/command-matrix.md'],
  ['build workflow flag', { '.github/workflows/release.yml': 'jobs:\n  build:\n    steps:\n      - run: cargo build --release --features experimental\n' }, '.github/workflows/release.yml'],
  ['global build flag after a delivery job', { '.github/workflows/release.yml': `${baseFiles(v8)['.github/workflows/release.yml']}env:\n  RUSTFLAGS: --cfg experimental\n` }, '.github/workflows/release.yml'],
  ['qualification support record', { 'runtime/packaging/release-policy.md': 'Other native hosts are supported.\n' }, 'runtime/packaging/release-policy.md'],
  ['new qualification workflow', { '.github/workflows/release-qualification.yml': 'jobs:\n  qualify:\n    runs-on: ubuntu-24.04\n' }, '.github/workflows/release-qualification.yml'],
  ['new production input', { 'crates/gs/src/new_backend.rs': 'pub const ENABLED: bool = true;\n' }, 'crates/gs/src/new_backend.rs'],
  ['deleted production input', { 'daemon/src/main.c': null }, 'daemon/src/main.c'],
]) test(`${name} requires review`, async (t) => {
  const f = await fixture(t); await f.commit(change);
  const result = assessCapabilities(f);
  assert.equal(result.status, 'review-required');
  const input = result.changedInputs.find(({ path }) => path === expected);
  assert(input);
  assert.notEqual(input.beforeSha256, input.afterSha256);
});

test('tests, generated website, publication and queue-only changes are explicitly ignored', async (t) => {
  const f = await fixture(t);
  await f.commit({ 'tests/new_behavior.rs': 'test-only', 'crates/gs/tests/new_behavior.rs': 'test-only',
    'site/content-audit.json': 'generated release audit', 'CHANGELOG.md': 'New version',
    'runtime/packaging/homebrew_delivery.py': 'delivery-only', 'scripts/queue-homebrew.py': 'queue-only',
    '.github/workflows/docs-release.yml': 'website-only',
    '.github/workflows/release.yml': baseFiles(v8)['.github/workflows/release.yml'].replace('run: publish', 'run: publish-and-enqueue') });
  const result = assessCapabilities(f);
  assert.equal(result.status, 'unchanged');
  assert(result.ignoredChanges.includes('scripts/queue-homebrew.py'));
  assert(result.ignoredChanges.includes('tests/new_behavior.rs'));
});

test('reference and actual published platform inventory participate independently of source', async (t) => {
  const f = await fixture(t); await f.commit();
  f.release.reference.sha256 = 'b'.repeat(64);
  f.release.assets.push({ kind: 'archive', os: 'linux', arch: 'arm64' });
  const result = assessCapabilities(f);
  assert.equal(result.status, 'review-required');
  assert.deepEqual(result.changedInputs.map(({ path }) => path), ['$platform-inventory', '$reference']);
});

test('unavailable source objects, tag identity and audit binding fail closed', async (t) => {
  const f = await fixture(t); await f.commit();
  const invalidCases = [
    { ...f, release: { ...f.release, sourceCommit: 'c'.repeat(40) } },
    { ...f, release: { ...f.release, sourceCommit: f.baselineRelease.sourceCommit } },
    { ...f, audit: { ...f.audit, version: v8 } },
    { ...f, audit: { ...f.audit, commandSourceCommit: f.release.sourceCommit } },
    { ...f, release: { ...f.release, reference: { sha256: 'not-a-digest' } } },
  ];
  for (const invalid of invalidCases) assert.throws(() => assessCapabilities(invalid));
});

test('missing, unsafe or forged notice evidence cannot become approvable review evidence', async (t) => {
  const f = await fixture(t); await f.commit();
  for (const evidence of ['crates/gs/src/missing.rs', '../../secret', 'site/release.json', 'scripts/queue-homebrew.py']) {
    assert.throws(() => assessCapabilities({ ...f, audit: { ...f.audit, commandNotices: [{ evidence }] } }));
  }
  const g = await fixture(t);
  await g.commit({ 'crates/gs/src/cli.rs': null });
  assert.throws(() => assessCapabilities(g), /Missing capability input/);
});

test('own-version normalizer never masks dependencies, source declarations or wrong version identity', () => {
  assert.equal(normalizeInput('Cargo.toml', cargo(v7), v7), normalizeInput('Cargo.toml', cargo(v8), v8));
  assert.equal(normalizeInput('Cargo.lock', lock(v7), v7), normalizeInput('Cargo.lock', lock(v8), v8));
  assert.notEqual(normalizeInput('Cargo.lock', lock(v8), v8), normalizeInput('Cargo.lock', lock(v8).replace('version = "1.0.0"', 'version = "2.0.0"'), v8));
  assert.throws(() => normalizeInput('Cargo.toml', cargo(v8), v7), /version does not match/);
  assert.throws(() => normalizeInput('Cargo.lock', lock(v8).replace('name = "gs"', 'name = "gs"\nsource = "registry+https://example.invalid"'), v8), /registry source/);
});

test('tree fingerprint records executable modes and rejects links, duplicate or absent inputs', () => {
  const entries = Object.entries(baseFiles(v7)).map(([path, content]) => ({ path, content, mode: '100644' }));
  const a = fingerprintInputs(entries, { version: v7 });
  const changedMode = entries.map((entry) => entry.path === 'crates/gs/src/cli.rs' ? { ...entry, mode: '100755' } : entry);
  const b = fingerprintInputs(changedMode, { version: v7 });
  assert.notEqual(a.fingerprint, b.fingerprint);
  assert.equal(compareInputs(a.inputs, b.inputs)[0].path, 'crates/gs/src/cli.rs');
  assert.throws(() => fingerprintInputs([...entries, entries[0]], { version: v7 }), /duplicate/);
  assert.throws(() => fingerprintInputs(entries.slice(1), { version: v7 }), /Missing/);
  assert.throws(() => fingerprintInputs([{ ...entries[0], mode: '120000' }, ...entries.slice(1)], { version: v7 }), /ordinary Git file/);
  assert(isCapabilityInput('new-build-input.json'));
});

test('Git committed source controls assessment; workspace edits are never executed or fingerprinted', async (t) => {
  const f = await fixture(t); await f.commit();
  await f.write('crates/gs/src/cli.rs', 'uncommitted behavior');
  await f.write('scripts/build-release-platform.py', 'raise RuntimeError("must never run")');
  assert.equal(assessCapabilities(f).status, 'unchanged');
  assert.match(await readFile(join(f.repoRoot, 'scripts/build-release-platform.py'), 'utf8'), /must never run/);
});
