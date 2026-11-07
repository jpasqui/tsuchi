// Generates the reference pages from the schema and puts them among the site's pages.
// The pages are made by tools/generate.py, so this needs Python 3.11 with PyYAML.
import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readdirSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const site = dirname(dirname(fileURLToPath(import.meta.url)));
const root = dirname(site);
const made = join(root, 'build', 'gen', 'docs', 'reference');
const pages = join(site, 'src', 'content', 'docs', 'reference');

const pythons = process.platform === 'win32' ? [['py', '-3.11'], ['python']] : [['python3'], ['python']];
let ran = false;
for (const [command, ...first] of pythons) {
  const done = spawnSync(command, [...first, join(root, 'tools', 'generate.py')], { cwd: root, stdio: 'inherit' });
  if (done.status === 0) {
    ran = true;
    break;
  }
}
if (!ran || !existsSync(made)) {
  console.error('The reference pages could not be generated. tools/generate.py needs Python 3.11 with PyYAML.');
  process.exit(1);
}

// Only the generated pages go: the folder's own index page is written by hand and stays.
mkdirSync(pages, { recursive: true });
for (const name of readdirSync(pages)) {
  if (name.endsWith('.mdx') && name !== 'index.mdx') rmSync(join(pages, name));
}
let count = 0;
for (const name of readdirSync(made)) {
  if (!name.endsWith('.mdx')) continue;
  cpSync(join(made, name), join(pages, name));
  count += 1;
}
console.log(`${count} reference pages`);

// Every service has to be in one layer of the sidebar. The layers and how a service gets one are in sections.mjs.
const { categories } = await import('../src/data/sections.mjs');
const listed = categories.flatMap((category) => category.services);
const services = readdirSync(made).filter((name) => name.endsWith('.mdx')).map((name) => name.slice(0, -4));
const missing = services.filter((name) => !listed.includes(name));
const unknown = listed.filter((name) => !services.includes(name));
const twice = listed.filter((name, at) => listed.indexOf(name) !== at);
if (missing.length || unknown.length || twice.length) {
  if (missing.length) console.error(`Its schema file names no layer that src/data/sections.mjs lists: ${missing.join(', ')}`);
  if (unknown.length) console.error(`In a category and not a service: ${unknown.join(', ')}`);
  if (twice.length) console.error(`In more than one category: ${twice.join(', ')}`);
  process.exit(1);
}
