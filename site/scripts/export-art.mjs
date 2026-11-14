// Reproducible responsive encodes and social-card composition; never redraw the logo.
import sharp from 'sharp';
import { readFile, writeFile } from 'node:fs/promises';
const [light, dark] = process.argv.slice(2);
if (!light || !dark) throw new Error('Usage: node scripts/export-art.mjs LIGHT.png DARK.png');
for (const [theme, source] of [['light',light],['dark',dark]]) {
  for (const width of [640,1200]) {
    await sharp(source).resize(width).avif({quality:45,effort:7}).toFile(`public/art/hero-${theme}-${width}.avif`);
    await sharp(source).resize(width).webp({quality:70,effort:6}).toFile(`public/art/hero-${theme}-${width}.webp`);
  }
}
const art = await sharp(light).resize(700,630,{fit:'cover'}).toBuffer();
const logo = await sharp(await readFile('public/branding/logo-light.svg')).resize(100,100).png().toBuffer();
const text = Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="630"><text x="60" y="235" fill="#3c624d" font-family="sans-serif" font-size="22" letter-spacing="3">GITSTORE CLI</text><text x="60" y="320" fill="#252922" font-family="sans-serif" font-size="48" font-weight="bold">Your catalog.</text><text x="60" y="385" fill="#3c624d" font-family="Georgia,serif" font-style="italic" font-size="46">Your command line.</text><text x="60" y="545" fill="#666b60" font-family="sans-serif" font-size="22">cli.gitstore.dev</text></svg>`);
await sharp({create:{width:1200,height:630,channels:3,background:'#f7f6f0'}}).composite([{input:art,left:500,top:0},{input:logo,left:50,top:60},{input:text,left:0,top:0}]).png({compressionLevel:9,palette:true}).toFile('public/social-card.png');
