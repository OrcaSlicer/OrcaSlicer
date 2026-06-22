// Packaging script — runs after `vite build`. Bundles dist/ +
// manifest.json into a single .orcaplugin (zip) at the repo root,
// ready to drop into PluginManagerDialog's Install... button.

import { createWriteStream } from 'node:fs';
import { readFile }          from 'node:fs/promises';
import { dirname, join }     from 'node:path';
import { fileURLToPath }     from 'node:url';
import archiver              from 'archiver';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');
const DIST = join(ROOT, 'dist');

const manifest = JSON.parse(await readFile(join(ROOT, 'manifest.json'), 'utf8'));
const outPath  = join(ROOT, `${manifest.id}-${manifest.version}.orcaplugin`);

await new Promise((resolve, reject) => {
  const out = createWriteStream(outPath);
  const zip = archiver('zip', { zlib: { level: 9 } });

  out.on('close', resolve);
  zip.on('warning', (e) => {
    if (e.code === 'ENOENT') return;
    reject(e);
  });
  zip.on('error', reject);

  zip.pipe(out);
  zip.file(join(ROOT, 'manifest.json'), { name: 'manifest.json' });
  zip.directory(DIST, false);   // contents at zip root
  zip.finalize();
});

console.log(`Packed ${outPath}`);
