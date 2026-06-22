import { defineConfig } from 'vite';

// Vite emits a single dist/ tree the packaging script bundles into
// the .orcaplugin zip alongside manifest.json. Static-only builds —
// no dev server in the published artifact.
export default defineConfig({
  build: {
    outDir:     'dist',
    emptyOutDir: true,
    rollupOptions: {
      input: 'index.html',
    },
  },
});
