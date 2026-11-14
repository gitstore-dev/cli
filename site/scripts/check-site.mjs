import { readdir, readFile, stat } from 'node:fs/promises';
import { resolve, dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { load } from 'cheerio';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../dist');
async function walk(path) { const files = await readdir(path, { withFileTypes: true }); return (await Promise.all(files.map(f => f.isDirectory() ? walk(join(path, f.name)) : join(path, f.name)))).flat(); }
const files = await walk(root); const pages = new Map(); const errors = [];
for (const file of files.filter(f => f.endsWith('.html'))) pages.set(file, load(await readFile(file, 'utf8')));
for (const [file, $] of pages) {
  const canonical = $('link[rel=canonical]').attr('href');
  if (!canonical?.startsWith('https://cli.gitstore.dev/')) errors.push(`${file}: invalid canonical ${canonical}`);
  for (const link of $('a[href], img[src], script[src], link[href]').toArray()) {
    const href = $(link).attr('href') ?? $(link).attr('src');
    const url = new URL(href, new URL(relative(root, file).replace(/index\.html$/, ''), 'https://cli.gitstore.dev/'));
    if (url.origin !== 'https://cli.gitstore.dev') continue;
    const target = url.pathname === '/404/' ? join(root, '404.html') : join(root, decodeURIComponent(url.pathname), url.pathname.endsWith('/') ? 'index.html' : '');
    if (!files.includes(target)) { errors.push(`${relative(root,file)}: missing ${href}`); continue; }
    if (url.hash && pages.has(target)) {
      const id = decodeURIComponent(url.hash.slice(1));
      if (!pages.get(target)('[id]').toArray().some(el => pages.get(target)(el).attr('id') === id)) errors.push(`${relative(root,file)}: missing anchor ${href}`);
    }
  }
}
for (const file of files.filter(f => /hero.*\.(avif|webp)$/.test(f))) if ((await stat(file)).size > 200_000) errors.push(`${file}: hero exceeds 200 KB`);
for (const path of ['pagefind/pagefind.js','sitemap-index.xml','robots.txt','404.html','social-card.png']) if (!files.includes(join(root,path))) errors.push(`Missing ${path}`);
if (errors.length) throw new Error(errors.join('\n'));
console.log(`Checked ${pages.size} HTML pages, internal destinations/anchors, metadata and hero budgets.`);
