#!/usr/bin/env node
// Phase 5.4.2 — `npm create orca-plugin@latest <name>` scaffolder.
//
// Copies the embedded template/ tree to <target>, prompts for plugin
// identity values, and applies {{placeholder}} substitution to every
// text file. Binary files (none today) would copy verbatim.

import { readFile, writeFile, mkdir, cp, stat } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createInterface } from 'node:readline/promises';
import { stdin as input, stdout as output } from 'node:process';

const __dirname  = dirname(fileURLToPath(import.meta.url));
const TEMPLATE   = join(__dirname, '..', 'template');

const args   = process.argv.slice(2);
const target = resolve(args[0] ?? 'my-orca-plugin');

const rl = createInterface({ input, output });
async function ask(prompt, dflt) {
  const a = (await rl.question(`${prompt} [${dflt}]: `)).trim();
  return a === '' ? dflt : a;
}

const defaultId = target.split(/[\\/]/).pop()
  .replace(/[^a-z0-9-]/gi, '-')
  .toLowerCase();

const values = {
  plugin_id:          await ask('Plugin id (reverse-DNS)', `com.example.${defaultId}`),
  plugin_name:        await ask('Plugin name',              defaultId),
  plugin_description: await ask('One-line description',     'An OrcaSlicer plugin.'),
  author:             await ask('Author',                   ''),
};
rl.close();

// 1. Copy the template tree verbatim.
await mkdir(target, { recursive: true });
await cp(TEMPLATE, target, { recursive: true });

// 2. Walk the tree and apply placeholder substitution to every text file.
async function* walk(dir) {
  const { readdir } = await import('node:fs/promises');
  for (const name of await readdir(dir, { withFileTypes: true })) {
    const full = join(dir, name.name);
    if (name.isDirectory()) yield* walk(full);
    else                    yield full;
  }
}

function expand(s) {
  return s.replace(/\{\{(\w+)\}\}/g, (_, key) => values[key] ?? `{{${key}}}`);
}

for await (const file of walk(target)) {
  // Read as text; if it's binary the parser will produce mojibake but
  // there are no binary template files today.
  const orig = await readFile(file, 'utf8');
  const next = expand(orig);
  if (next !== orig) await writeFile(file, next);
}

console.log(`\nScaffolded ${target}`);
console.log(`Next steps:`);
console.log(`  cd ${target}`);
console.log(`  pnpm install`);
console.log(`  pnpm run build`);
console.log(`  # drops <id>-<version>.orcaplugin at the repo root.`);
