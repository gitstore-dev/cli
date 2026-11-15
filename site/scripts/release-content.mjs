/** Mechanical release prose only. Authored guidance and historical facts stay outside markers. */
export const generatedSourcePaths = ['README.md', 'docs/README.md', 'docs/installation.md'];

const sourceKeys = {
  'README.md': ['readme-release'],
  'docs/README.md': ['docs-intro'],
  'docs/installation.md': ['install-brew-target', 'install-deb', 'install-rpm', 'install-npm', 'install-version', 'install-verification'],
};
const keys = new Set(Object.values(sourceKeys).flat());
const marker = /^<!-- release-docs:([a-z][a-z-]*):(start|end) -->$/gm;

function blocks(text) {
  const result = [];
  let open;
  const seen = new Set();
  const matches = [...text.matchAll(marker)];
  // Marker-like comments must not silently become unprotected authored prose.
  if ((text.match(/<!--\s*release-docs:/g) ?? []).length !== matches.length) throw new Error('Malformed release-docs marker');
  for (const match of matches) {
    const [, key, kind] = match;
    if (!keys.has(key)) throw new Error(`Unknown release-docs key: ${key}`);
    if (kind === 'start') {
      if (open || seen.has(key)) throw new Error(`Nested or duplicate release-docs block: ${key}`);
      open = { key, start: match.index + match[0].length };
      seen.add(key);
    } else {
      if (!open || open.key !== key) throw new Error(`Unmatched release-docs end: ${key}`);
      result.push({ ...open, end: match.index });
      open = undefined;
    }
  }
  if (open) throw new Error(`Unclosed release-docs block: ${open.key}`);
  return result;
}

/** Stable input for the authored-prose audit; bodies are checked separately by rendering. */
export function normalizeReleaseContent(text) {
  let normalized = text;
  for (const block of blocks(text).reverse()) {
    normalized = normalized.slice(0, block.start) + '\n[generated release content]\n' + normalized.slice(block.end);
  }
  return normalized;
}

function validateRelease(release) {
  if (!release || typeof release.version !== 'string' || release.version.trim() !== release.version || !/^\d+\.\d+\.\d+(?:-[\da-zA-Z]+(?:[.-][\da-zA-Z]+)*)?$/.test(release.version) || release.tag !== `v${release.version}`) throw new Error('Invalid selected release version/tag');
  const releaseUrl = `https://github.com/gitstore-dev/cli/releases/tag/${release.tag}`;
  if (release.releaseUrl !== releaseUrl) throw new Error('Invalid selected release URL');
  const verification = release.verification;
  if (!verification || verification.checksum !== 'SHA256SUMS' ||
    !((verification.layout === 'shared' && verification.bundle === 'release.sigstore.jsonl') ||
      (verification.layout === 'per-subject' && verification.bundle === null))) {
    throw new Error('Invalid or missing release verification layout');
  }
  if (!Array.isArray(release.assets)) throw new Error('Missing release assets');
  const required = new Map([
    ['deb', ['linux-x64', 'linux-arm64']],
    ['rpm', ['linux-x64', 'linux-arm64']],
    ['npm', ['darwin-arm64', 'darwin-x64', 'linux-arm64', 'linux-x64']],
  ]);
  const packages = new Map();
  const names = new Set();
  for (const asset of release.assets) {
    if (!required.has(asset.kind)) continue;
    if (typeof asset.name !== 'string' || asset.name.trim() !== asset.name || !/^[a-zA-Z\d_.~+-]+$/.test(asset.name) || names.has(asset.name)) throw new Error('Invalid or duplicate package filename');
    names.add(asset.name);
    if (asset.url !== `https://github.com/gitstore-dev/cli/releases/download/${release.tag}/${asset.name}` ||
      typeof asset.sha256 !== 'string' || asset.sha256.length !== 64 || !/^[a-f\d]{64}$/.test(asset.sha256) || !Number.isSafeInteger(asset.size) || asset.size <= 0) throw new Error(`Invalid published asset metadata: ${asset.name}`);
    const platform = `${asset.os}-${asset.arch}`;
    if (!required.get(asset.kind).includes(platform)) throw new Error(`Unsupported package platform: ${asset.kind}/${platform}`);
    const identity = `${asset.kind}/${platform}`;
    if (packages.has(identity)) throw new Error(`Duplicate package platform: ${identity}`);
    let validName;
    if (asset.kind === 'deb') {
      const arch = asset.arch === 'x64' ? 'amd64' : 'arm64';
      validName = asset.name.endsWith(`_${arch}.deb`) && typeof asset.logicalName === 'string' && asset.logicalName.replaceAll('~', '.') === asset.name;
    } else if (asset.kind === 'rpm') {
      validName = asset.name.endsWith(`.${asset.arch === 'x64' ? 'x86_64' : 'aarch64'}.rpm`) && asset.logicalName === asset.name;
    } else {
      validName = asset.name.endsWith(`-${platform}.tgz`) && asset.logicalName === asset.name;
    }
    if (!validName) throw new Error(`Package filename/platform mismatch: ${asset.name}`);
    packages.set(identity, asset);
  }
  for (const [kind, platforms] of required) {
    for (const platform of platforms) {
      if (!packages.has(`${kind}/${platform}`)) throw new Error(`Missing package platform: ${kind}/${platform}`);
    }
  }
  return packages;
}

function packageCases(packages, kind, entries, variable) {
  return entries.map(([selector, platform]) => `  ${selector}) ${variable}='${packages.get(`${kind}/${platform}`).url}' ;;`).join('\n');
}

function renderBlocks(release, packages) {
  const { version, releaseUrl } = release;
  const label = version.startsWith('0.1.0-') ? version.slice('0.1.0-'.length) : version;
  const bundle = release.verification.layout === 'shared' ? release.verification.bundle : 'PACKAGE.sigstore.jsonl';
  return {
    'readme-release': `The [${label} ${version.includes('-') ? 'prerelease' : 'release'}](${releaseUrl})\nis available for Linux and macOS; macOS supports remote/control commands only.`,
    'docs-intro': `Install the matching ${label} CLI/daemon pair using\n[Homebrew, APT, DNF or npm](installation.md), or follow\n[building from source](contributing/building.md) for contributor builds.\nConfirm the Homebrew formula version before installing; the tap can lag a new\nrelease while its update is reviewed. Direct packages are pinned to ${label}.`,
    'install-brew-target': 'The tap can lag a newly published release while its formula update is reviewed.\n' +
      'After adding the tap, run `brew info gitstore-dev/cli/gitstore` and confirm it\n' +
      `offers \`${version}\` before installing or upgrading. If it still offers an older\n` +
      'version, use the matching npm, APT or DNF package below, or wait for the formula update.',
    'install-deb': `Download the ${label} package for your architecture and install it:\n\n` +
      '```sh\n# Choose amd64 (x86_64) or arm64 explicitly.\ngitstore_arch=amd64\ncase "$gitstore_arch" in\n' +
      packageCases(packages, 'deb', [['amd64', 'linux-x64'], ['arm64', 'linux-arm64']], 'gitstore_url') +
      '\n  *) printf \'%s\\n\' \'Choose amd64 or arm64\' >&2; exit 1 ;;\nesac\n' +
      'curl --fail --location --output gitstore.deb "$gitstore_url" &&\n  sudo apt install ./gitstore.deb\n```',
    'install-rpm': `Install ${label} directly from GitHub:\n\n` +
      '```sh\n# Choose x86_64 or aarch64 (arm64) explicitly.\ngitstore_arch=x86_64\ncase "$gitstore_arch" in\n' +
      packageCases(packages, 'rpm', [['x86_64', 'linux-x64'], ['aarch64', 'linux-arm64']], 'gitstore_url') +
      '\n  *) printf \'%s\\n\' \'Choose x86_64 or aarch64\' >&2; exit 1 ;;\nesac\nsudo dnf install "$gitstore_url"\n```',
    'install-npm': 'With Node.js and npm installed, select the package for your OS and processor:\n\n' +
      '```sh\n# Choose darwin-arm64, darwin-x64, linux-arm64 or linux-x64 explicitly.\ngitstore_platform=darwin-arm64\ncase "$gitstore_platform" in\n' +
      packageCases(packages, 'npm', ['darwin-arm64', 'darwin-x64', 'linux-arm64', 'linux-x64'].map((platform) => [platform, platform]), 'gitstore_url') +
      '\n  *) printf \'%s\\n\' \'Choose a supported OS and processor\' >&2; exit 1 ;;\nesac\n' +
      'npm install --global --ignore-scripts --prefix "$HOME/.local" "$gitstore_url"\nexport PATH="$HOME/.local/bin:$PATH"\n```',
    'install-version': `Both commands should report \`${version}\`. These checks do not start services,\ndownload images or require credentials.`,
    'install-verification': `The [release page](${releaseUrl}) provides \`SHA256SUMS\` checksums and ` +
      (release.verification.layout === 'shared' ? 'one shared\n`release.sigstore.jsonl` attestation bundle for the published packages and inventories.' : 'a matching\n`PACKAGE.sigstore.jsonl` attestation bundle for each published file.') +
      '\nWith GitHub CLI installed, download a package and the bundle for its release, then verify:\n\n' +
      '```sh\ngh attestation verify ./PACKAGE --repo gitstore-dev/cli \\\n' +
      `  --bundle ./${bundle}\n\`\`\``,
  };
}

/** Returns a new object keyed by repository-relative paths; does not write any files. */
export function renderReleaseContent({ release, sources }) {
  const packages = validateRelease(release);
  const bodies = renderBlocks(release, packages);
  const input = sources instanceof Map ? Object.fromEntries(sources) : sources;
  if (!input || typeof input !== 'object') throw new Error('Missing release content sources');
  const result = { ...input };
  for (const source of generatedSourcePaths) {
    const text = input[source];
    if (typeof text !== 'string') throw new Error(`Missing generated source: ${source}`);
    const found = blocks(text);
    const actual = found.map(({ key }) => key).sort();
    if (JSON.stringify(actual) !== JSON.stringify([...sourceKeys[source]].sort())) throw new Error(`Release-docs markers do not match expected keys in ${source}`);
    let rendered = text;
    for (const block of found.reverse()) {
      rendered = rendered.slice(0, block.start) + `\n${bodies[block.key]}\n` + rendered.slice(block.end);
    }
    result[source] = rendered;
  }
  return result;
}
