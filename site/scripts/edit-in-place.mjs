// Editing a page's text in the browser while the site runs with `npm run dev`.
//
// A small script in the page lets a person click a block of a manual page (a paragraph, a
// heading, a list item, a table cell), change its Markdown and save. This finds the block in the
// page's source file by its words and writes the change back. None of it is part of a built
// site: the integration does nothing unless the command is `dev`.
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const site = dirname(dirname(fileURLToPath(import.meta.url)));
const pages = join(site, 'src', 'content', 'docs');

// The source file of the page at an address: /manual/first-mod/ is manual/first-mod.mdx or .md.
function sourceOf(address) {
  const name = decodeURIComponent(String(address)).replace(/^\/+|\/+$/g, '');
  const tries = name ? [`${name}.mdx`, `${name}.md`, `${name}/index.mdx`, `${name}/index.md`] : ['index.mdx', 'index.md'];
  for (const one of tries) {
    const full = resolve(pages, one);
    if (full.startsWith(pages + sep) && existsSync(full)) return full;
  }
  throw new Error('This page has no source file among the site\'s pages.');
}

// Reference pages are written by the generator: their text is in the schema.
function refuseGenerated(full) {
  const inside = relative(pages, full).split(sep);
  if (inside[0] === 'reference' && !inside[1].startsWith('index.')) {
    throw new Error(`This page is generated. Its text is in the schema, in schema/${inside[1].replace(/\.mdx?$/, '.yaml')}.`);
  }
}

// Words alone: what a block's Markdown and its drawn text have in common.
const words = (text) => text.replace(/\]\([^)]*\)/g, ']').toLowerCase().replace(/[^a-z0-9]+/g, '');

// The blocks of a file: [{from, to}] in characters, each the Markdown a person would edit.
function blocks(text) {
  const out = [];
  const lines = text.split('\n');
  let at = 0, fence = null, run = null;
  let inFront = lines[0].trim() === '---';
  const close = () => {
    if (run) out.push(run);
    run = null;
  };
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i], start = at, end = at + line.length;
    at = end + 1;
    const bare = line.trim();
    if (inFront) {
      if (i > 0 && bare === '---') inFront = false;
      // The words on a splash page's buttons are `text` values in the page's settings.
      const label = /^(\s*-?\s*text:\s*)(.+?)\s*$/.exec(line);
      if (label && !/^["']/.test(label[2])) out.push({ from: start + label[1].length, to: start + label[1].length + label[2].length });
      continue;
    }
    const fenced = /^(```|~~~)/.exec(bare);
    if (fence) {
      if (fenced && bare.startsWith(fence)) fence = null;
      continue;
    }
    if (fenced) {
      close();
      fence = fenced[1];
      continue;
    }
    const indent = line.length - line.trimStart().length;
    if (!bare || /^(import |export |<\/?[A-Za-z][^>]*>$|\{\/\*)/.test(bare)) {
      close();
      // A component's title is drawn as text too: `<IconCard title="...">`.
      const title = /title="([^"]*)"/.exec(line);
      if (title) out.push({ from: start + title.index + 7, to: start + title.index + 7 + title[1].length });
      continue;
    }
    const heading = /^(#{1,6}\s+)/.exec(bare);
    if (heading) {
      close();
      out.push({ from: start + indent + heading[1].length, to: end });
      continue;
    }
    if (bare.startsWith('|')) {
      close();
      if (/^\|[\s:|-]+\|?$/.test(bare)) continue; // the row under a table's header
      let cell = start + line.indexOf('|') + 1;
      for (;;) {
        const next = text.indexOf('|', cell);
        if (next < 0 || next >= end) break;
        const raw = text.slice(cell, next);
        if (raw.trim()) out.push({ from: cell + (raw.length - raw.trimStart().length), to: next - (raw.length - raw.trimEnd().length) });
        cell = next + 1;
      }
      continue;
    }
    const item = /^([-*+]|\d+[.)])\s+/.exec(bare);
    if (item) {
      close();
      run = { from: start + indent + item[0].length, to: end };
      continue;
    }
    if (run) run.to = end;
    else run = { from: start + indent, to: end };
  }
  close();
  return out;
}

function find(full, drawn) {
  const text = readFileSync(full, 'utf8');
  const wanted = words(drawn);
  if (!wanted) throw new Error('There is no text here to edit.');
  const found = blocks(text).filter((block) => words(text.slice(block.from, block.to)) === wanted);
  if (!found.length) throw new Error('This text was not found in the page\'s source. It may come from a component or from the page\'s settings.');
  return { text, ...found[0] };
}

// A front matter value, by its key: `title`, or `tagline` under `hero`.
function frontMatter(full, key, value) {
  const text = readFileSync(full, 'utf8');
  const end = text.indexOf('\n---', 3);
  const line = new RegExp(`^(\\s*${key}:[ \\t]*)(.*)$`, 'm');
  const found = text.startsWith('---') && end > 0 ? line.exec(text.slice(0, end)) : null;
  if (!found) throw new Error(`The page's settings have no ${key}.`);
  if (value === undefined) {
    const raw = found[2].trim();
    if (raw.startsWith('"')) {
      try {
        return JSON.parse(raw);
      } catch {
        return raw;
      }
    }
    return raw.startsWith("'") ? raw.slice(1, -1).replace(/''/g, "'") : raw;
  }
  const plain = /^[A-Za-z0-9][\w .,()'-]*$/.test(value);
  writeFileSync(full, text.slice(0, found.index) + found[1] + (plain ? value : JSON.stringify(value)) + text.slice(found.index + found[0].length), 'utf8');
  return value;
}

async function body(request) {
  let text = '';
  for await (const chunk of request) text += chunk;
  return JSON.parse(text || '{}');
}

export function editInPlace() {
  return {
    name: 'edit-in-place',
    hooks: {
      'astro:config:setup': ({ command, injectScript }) => {
        if (command !== 'dev') return;
        injectScript('page', readFileSync(join(site, 'scripts', 'edit-in-place.client.js'), 'utf8'));
      },
      'astro:server:setup': ({ server }) => {
        server.middlewares.use('/__edit', async (request, response) => {
          const answer = (status, value) => {
            response.statusCode = status;
            response.setHeader('content-type', 'application/json');
            response.end(JSON.stringify(value));
          };
          try {
            if (request.method !== 'POST') return answer(405, { error: 'Not a way to ask this.' });
            const sent = await body(request);
            const full = sourceOf(sent.page);
            refuseGenerated(full);
            if (sent.key) {
              if (sent.text === undefined) return answer(200, { text: frontMatter(full, sent.key) });
              return answer(200, { text: frontMatter(full, sent.key, String(sent.text).trim()) });
            }
            // What is drawn names the block, both to read it and to write it.
            const { text, from, to } = find(full, String(sent.drawn));
            if (sent.text === undefined) return answer(200, { text: text.slice(from, to) });
            if (text.slice(from, to) !== sent.was) throw new Error('The page has changed since it was drawn. Reload it and try again.');
            writeFileSync(full, text.slice(0, from) + String(sent.text) + text.slice(to), 'utf8');
            return answer(200, { saved: true });
          } catch (error) {
            answer(400, { error: error.message });
          }
        });
      },
    },
  };
}
