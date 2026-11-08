// Run the exact pinned engine with fixture history and no network or mutations.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
for (const protocol of ['node:http', 'node:https']) {
  require(protocol).request = () => { throw new Error('Release fixture attempted network access'); };
  require(protocol).get = () => { throw new Error('Release fixture attempted network access'); };
}
const base = process.env.GITSTORE_RELEASE_PLEASE_MODULE;
assert(path.isAbsolute(base));
assert.equal(require(path.join(base, 'package.json')).version, '17.3.0');
require(base); // Initialize the published entry point before internal strategies.
const load = name => require(path.join(base, 'build/src', name));
const {Simple} = load('strategies/simple');
const {Version} = load('version');
const {TagName} = load('util/tag-name');
const {PrereleaseVersioningStrategy} = load('versioning-strategies/prerelease');
const {parseConventionalCommits} = load('commit');
const root = process.argv[2];
const config = JSON.parse(fs.readFileSync(path.join(root, 'release-please-config.json')));
const logger = {info(){}, debug(){}, warn(){}, error(){}};
const github = {repository:{owner:'gitstore-dev',repo:'cli'}};
const commits = parseConventionalCommits([{sha:'a'.repeat(40), message:'feat: ship the reviewed CLI increment', files:['Cargo.toml']}], logger);

async function proposal(previous, override) {
  const strategy = new Simple({github, path:'.', targetBranch:'main',
    packageName:'', includeComponentInTag:false, includeVInTag:true, logger,
    releaseAs:override, extraFiles:config.packages['.']['extra-files'],
    versioningStrategy:new PrereleaseVersioningStrategy({prerelease:true,
      prereleaseType:'alpha', bumpMinorPreMajor:true, logger})});
  const result = await strategy.buildReleasePullRequest(commits,
    {tag:new TagName(Version.parse(previous)),sha:'b'.repeat(40),notes:'previous'});
  assert(result);
  const version = result.version.toString();
  const files = {};
  for (const update of result.updates) {
    assert(['version.txt','Cargo.toml','Cargo.lock','CHANGELOG.md'].includes(update.path));
    files[update.path] = await update.updater.updateContent(fs.readFileSync(path.join(root, update.path), 'utf8'), logger);
  }
  assert.equal(files['version.txt'].trim(), version);
  assert(files['Cargo.toml'].includes(`version = "${version}"`));
  assert(files['Cargo.lock'].includes(`name = "gs"\n# x-release-please-start-version\nversion = "${version}"`));
  assert(!files['Cargo.lock'].includes('name = "clap"\nversion = "' + version + '"'));
  return version;
}
(async () => {
  const first = await proposal('0.1.0-alpha.0', config.packages['.']['release-as']);
  const second = await proposal(first, undefined);
  assert.equal(first,'0.1.0-alpha.1');
  assert.equal(second,'0.1.0-alpha.2');
  console.log(JSON.stringify({engine:'17.3.0',first,second,network:false,publication:false}));
})().catch(error => { console.error(error); process.exitCode = 1; });
