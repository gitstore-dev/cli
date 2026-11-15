import { createHash } from 'node:crypto';
import { mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { assessCapabilities, baselineIdentity } from './capability-audit.mjs';
import { generatedSourcePaths, normalizeReleaseContent, renderReleaseContent } from './release-content.mjs';

export const guides = [
  { source: 'docs/README.md', route: '/guides/', title: 'Guides', order: 1 },
  { source: 'docs/installation.md', route: '/install/', title: 'Install GitStore CLI', order: 1 },
  { source: 'docs/configuration.md', route: '/guides/configuration/', title: 'Configuration and authentication', order: 2 },
  { source: 'docs/remote-cli.md', route: '/guides/remote-cli/', title: 'Remote operations', order: 3 },
  { source: 'docs/watch.md', route: '/guides/watch/', title: 'Typed resource watches', order: 4 },
  { source: 'docs/local-runtime.md', route: '/guides/local-runtime/', title: 'Linux local deployments', order: 5 },
  { source: 'docs/local-runtime-configuration.md', route: '/guides/local-runtime-configuration/', title: 'Local runtime configuration', order: 6 },
];
export const auditedSourcePaths = [...new Set([...guides.map(({ source }) => source), ...generatedSourcePaths])];
const routeBySource = new Map(guides.map(({ source, route }) => [source, route]));
const repository = 'https://github.com/gitstore-dev/cli';
const siteRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

/** Map canonical Markdown paths to website routes; everything else stays on GitHub. */
export function rewriteDestination(destination, source, revision) {
  if (/^(?:[a-z][a-z\d+.-]*:|\/\/|#|\/)/i.test(destination)) return destination;
  const match = /^([^?#]*)(\?[^#]*)?(#.*)?$/.exec(destination);
  if (!match) throw new Error(`Invalid Markdown destination: ${destination}`);
  const [, pathname, query = '', fragment = ''] = match;
  const target = path.posix.normalize(path.posix.join(path.posix.dirname(source), pathname));
  if (target === '..' || target.startsWith('../')) throw new Error(`Link escapes repository: ${destination}`);
  const route = routeBySource.get(target);
  if (route) return `${route}${query}${fragment}`;
  return `${repository}/blob/${revision}/${target}${query}${fragment}`;
}

/** Rewrite prose links only, preserving shell text and parser help literally. */
export function rewriteLinks(markdown, source, revision) {
  return outsideFences(markdown, (line) => line
    .replace(/(!?\[[^\]\n]*\]\()(<[^>]*>|[^\s)]+)([^)]*\))/g, (_, open, target, close) => {
      const bracketed = target.startsWith('<');
      const value = bracketed ? target.slice(1, -1) : target;
      const rewritten = rewriteDestination(value, source, revision);
      return `${open}${bracketed ? `<${rewritten}>` : rewritten}${close}`;
    })
    .replace(/^(\s{0,3}\[[^\]]+\]:\s*)(\S+)(.*)$/, (_, open, target, close) =>
      `${open}${rewriteDestination(target, source, revision)}${close}`));
}

function outsideFences(markdown, transform) {
  let fence;
  return markdown.split('\n').map((line) => {
    const start = /^ {0,3}(`{3,}|~{3,})(.*)$/.exec(line);
    if (fence) {
      if (start && start[1][0] === fence[0] && start[1].length >= fence.length && !start[2].trim()) fence = undefined;
      return line;
    }
    if (start) { fence = start[1]; return line; }
    return transform(line);
  }).join('\n');
}

/** The bundled reference is Markdown, not a list of lines to grep inside help fences. */
export function splitReference(markdown) {
  const sections = [];
  let current;
  let fence;
  for (const line of markdown.replaceAll('\r\n', '\n').split('\n')) {
    const marker = /^ {0,3}(`{3,}|~{3,})(.*)$/.exec(line);
    if (fence) {
      current?.lines.push(line);
      if (marker && marker[1][0] === fence[0] && marker[1].length >= fence.length && !marker[2].trim()) fence = undefined;
      continue;
    }
    if (marker) {
      if (!current) throw new Error('Reference help fence appears before its command heading');
      fence = marker[1];
      current.lines.push(line);
      continue;
    }
    const heading = /^## (gs(?: [a-z][a-z\d-]*)*)\s*$/.exec(line);
    if (heading) {
      const command = heading[1];
      if (sections.some((section) => section.command === command)) throw new Error(`Duplicate reference command: ${command}`);
      current = { command, segments: command.split(' ').slice(1), lines: [] };
      sections.push(current);
    } else if (/^##\s/.test(line)) {
      throw new Error(`Malformed reference command heading: ${line}`);
    } else if (current) {
      current.lines.push(line);
    } else if (line.trim()) {
      throw new Error('Unexpected content before root command');
    }
  }
  if (fence) throw new Error('Unclosed reference code fence');
  if (!sections.length || sections[0].command !== 'gs') throw new Error('Reference must begin with gs');
  for (const section of sections) {
    const help = section.lines.join('\n').trim();
    if (!/^```text\n[\s\S]+\n```$/.test(help) || !help.includes('Usage:')) throw new Error(`Malformed reference help: ${section.command}`);
    if (section.segments.length > 1) {
      const parent = section.command.slice(0, section.command.lastIndexOf(' '));
      if (!sections.some(({ command }) => command === parent)) throw new Error(`Missing reference parent: ${parent}`);
    }
    section.help = help;
    section.route = `/manual/${section.segments.length ? `${section.segments.join('/')}/` : ''}`;
    delete section.lines;
  }
  return sections;
}

function frontmatter(title, description, order = 1) {
  return `---\ntitle: ${JSON.stringify(title)}\ndescription: ${JSON.stringify(description)}\nsidebar:\n  order: ${order}\n---\n\n`;
}

function releaseNotice(release) {
  return `:::caution[Early alpha · CLI ${release.version}]\n` +
    `This site documents the [selected published release](${release.releaseUrl}). ` +
    'Use independent backups: upgrade compatibility, data preservation and recovery are not guaranteed. ' +
    'macOS supports remote/control commands only. Native local core qualification covers Ubuntu 24.04 and Fedora 44 on x86_64 and arm64; optional components have separate limits.\n:::\n\n';
}

function capabilityNotice(source) {
  if (source.endsWith('installation.md') || source.endsWith('README.md')) return '';
  if (source.includes('local-runtime')) {
    return ':::note[Linux core profile and optional components]\n' +
      'Local workloads require the Linux prerequisites below. The managed core profile pins GitStore alpha.8; selecting a remote alpha.9 contract does not upgrade it. ' +
      'Optional OIDC instructions describe separate development evidence, not additional native host qualification. Admin and Storefront remain gated.\n:::\n\n';
  }
  return ':::note[Server and profile prerequisites]\n' +
    'Use the public endpoints and explicit contract profile of your GitStore server. Alpha.9 profile features below retain their documented controlled-test/provider qualifications; they are not a claim of universal server or host compatibility. ' +
    'Browser/device/OIDC discovery, safe author updates, correlated remote waits, Collection watch/mutations and File upload/list remain unavailable.\n:::\n\n';
}

export function commandCapabilityNotice(command, audit, release) {
  return (audit.commandNotices ?? [])
    .filter((notice) => notice.commands?.includes(command) || notice.prefixes?.some((prefix) => command === prefix || command.startsWith(`${prefix} `)))
    .map((notice) => `:::caution[${notice.title}]\n${notice.message} ` +
      `[Release source](${repository}/blob/${release.sourceCommit}/${notice.evidence}).\n:::\n\n`)
    .join('');
}

export function validateAudit(release, audit, install) {
  if (audit.schemaVersion !== 2) throw new Error('Content audit requires schemaVersion 2 and reviewed capability evidence');
  baselineIdentity(release);
  const baseline = baselineIdentity(audit.reviewedRelease);
  if (audit.version !== baseline.version || audit.commandSourceCommit !== baseline.sourceCommit) throw new Error('Command capability audit source/version does not match its reviewed baseline');
  if (!/^[a-f0-9]{64}$/.test(audit.capabilityFingerprint)) throw new Error('Missing or malformed reviewed capability fingerprint');
  if (JSON.stringify(audit.reviewedSources) !== JSON.stringify(auditedSourcePaths)) throw new Error('Content audit must cover the exact guide and generated source allowlist in order');
  if (JSON.stringify(Object.keys(audit.sourceSha256 ?? {}).sort()) !== JSON.stringify([...auditedSourcePaths].sort()) ||
    Object.values(audit.sourceSha256).some((value) => !/^[a-f0-9]{64}$/.test(value))) throw new Error('Missing or malformed authored source hashes');
  if (!install.includes(`Both commands should report \`${release.version}\``)) throw new Error('Installation version does not match selected release; update canonical docs/installation.md');
  const downloads = [...install.matchAll(/https:\/\/github\.com\/gitstore-dev\/cli\/releases\/(?:download|tag)\/(v[^/\s)"`]+)/g)].map((match) => match[1]);
  if (downloads.some((tag) => tag !== release.tag)) throw new Error('Installation points at a different release; update canonical docs/installation.md');
  const names = new Set((release.assets ?? []).map(({ name }) => name));
  for (const match of install.matchAll(/https:\/\/github\.com\/gitstore-dev\/cli\/releases\/download\/v[^/]+\/([^\s"'`)]+)/g)) {
    const template = match[1];
    const architectures = template.endsWith('.deb') ? ['amd64', 'arm64'] : ['x86_64', 'aarch64'];
    const filenames = template.includes('${gitstore_platform}')
      ? ['darwin-arm64', 'darwin-x64', 'linux-arm64', 'linux-x64'].map((platform) => template.replace('${gitstore_platform}', platform))
      : template.includes('${gitstore_arch}')
        ? architectures.map((architecture) => template.replace('${gitstore_arch}', architecture))
        : [template];
    for (const filename of filenames) {
      if (!names.has(filename)) throw new Error(`Installation references an asset absent from the selected release: ${filename}`);
    }
  }
}

export async function prepareContent(root = siteRoot, { assess = assessCapabilities } = {}) {
  const repoRoot = path.dirname(root);
  const release = JSON.parse(await readFile(path.join(root, 'release.json'), 'utf8'));
  const audit = JSON.parse(await readFile(path.join(root, 'content-audit.json'), 'utf8'));
  const sources = Object.fromEntries(await Promise.all(auditedSourcePaths.map(async (source) => [source, await readFile(path.join(repoRoot, source), 'utf8')])));
  validateAudit(release, audit, sources['docs/installation.md']);
  for (const source of auditedSourcePaths) {
    if (createHash('sha256').update(normalizeReleaseContent(sources[source])).digest('hex') !== audit.sourceSha256[source]) {
      throw new Error(`Canonical authored content changed: review ${source} and update content-audit.json`);
    }
  }
  const rendered = renderReleaseContent({ release, sources });
  for (const source of generatedSourcePaths) {
    if (rendered[source] !== sources[source]) throw new Error(`Generated release content is stale or modified: ${source}`);
  }
  const capability = assess({ repoRoot, baselineRelease: audit.reviewedRelease, release, audit });
  if (capability.baseline.fingerprint !== audit.capabilityFingerprint) throw new Error('Reviewed capability fingerprint does not match its Git evidence');
  if (capability.source.fingerprint !== audit.capabilityFingerprint || capability.status !== 'unchanged') {
    throw new Error('Selected release changes capability inputs; review and update the capability audit');
  }
  const reference = await readFile(path.join(root, release.reference.path), 'utf8');
  if (createHash('sha256').update(reference).digest('hex') !== release.reference.sha256) throw new Error('Committed command reference digest mismatch');
  const commands = splitReference(reference);
  const output = path.join(root, 'src/content/docs');
  // This directory is generated and ignored. Never recursively remove authored source.
  for (const directory of ['guides', 'install', 'manual']) await rm(path.join(output, directory), { recursive: true, force: true });
  for (const guide of guides) {
    const body = rewriteLinks(sources[guide.source].replace(/^# .+\n+/, ''), guide.source, 'main');
    const sourceLink = `${repository}/blob/main/${guide.source}`;
    const content = frontmatter(guide.title, `${guide.title} for GitStore CLI ${release.version}.`, guide.order) +
      releaseNotice(release) + capabilityNotice(guide.source) + body + `\n\n---\n\n[Edit the canonical guide on GitHub](${sourceLink}). Imported at build time from \`${guide.source}\`.\n`;
    const filename = path.join(output, guide.route.slice(1), 'index.md');
    await mkdir(path.dirname(filename), { recursive: true });
    await writeFile(filename, content);
  }
  for (const command of commands) {
    const children = commands.filter((candidate) => candidate.segments.length === command.segments.length + 1 && candidate.command.startsWith(`${command.command} `));
    let content = frontmatter(command.command, `Published command help for ${command.command} in GitStore CLI ${release.version}.`) + releaseNotice(release) +
      ':::note[Command syntax and capability support]\n' +
      `This help was extracted from the ${release.tag} distribution without running the binary. ` +
      'A command appearing in parser help does not establish server, provider or host support. ' +
      'Check [remote prerequisites](/guides/configuration/) and [Linux runtime limits](/guides/local-runtime/). ' +
      'macOS local deployments are unavailable; optional Admin/Storefront and unsupported command operations remain gated.\n:::\n\n' +
      commandCapabilityNotice(command.command, audit, release) + command.help + '\n';
    if (!command.segments.length) content += '\n## All commands\n\n' + commands.slice(1).map((child) => `- [\`${child.command}\`](${child.route})`).join('\n') + '\n';
    else if (children.length) content += '\n## Subcommands\n\n' + children.map((child) => `- [\`${child.command}\`](${child.route})`).join('\n') + '\n';
    if (command.segments.length) {
      const parent = commands.find((candidate) => candidate.command === command.command.slice(0, command.command.lastIndexOf(' ')));
      content += `\n[Parent command: \`${parent.command}\`](${parent.route})\n`;
    }
    const filename = path.join(output, command.route.slice(1), 'index.md');
    await mkdir(path.dirname(filename), { recursive: true });
    await writeFile(filename, content);
  }
  await writeFile(path.join(output, '404.md'), await readFile(path.join(siteRoot, '404.md'), 'utf8'));
  return { guides: guides.length, commands: commands.length };
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  console.log(await prepareContent());
}
