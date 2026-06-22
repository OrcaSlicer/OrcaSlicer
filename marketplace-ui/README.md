# marketplace-ui

OrcaSlicer plugin marketplace UI. Vite + React 19 SPA loaded by the desktop
client at `<resources>/marketplace-ui/index.html` via `file://`.

## Toolchain

- React 19
- TypeScript 6 (strict, `noUncheckedIndexedAccess`, `exactOptionalPropertyTypes`)
- Vite 8 with relative `base: './'` so the bundle works under `file://`
- Tailwind CSS 4 via `@tailwindcss/vite` (no PostCSS / autoprefixer)
- `@tanstack/react-query` 5 for bridge data fetching
- pnpm package manager

## Commands

```bash
pnpm install
pnpm dev          # vite dev server at http://localhost:5173
pnpm typecheck    # tsc --noEmit
pnpm build        # typecheck + vite build -> dist/
pnpm preview      # serve the built dist/
```

## Bridge contract

At runtime the host injects `window.orca.marketplace` implementing the
`OrcaMarketplaceBridge` interface declared in `src/types.ts`. When that bridge
is absent (running `pnpm dev` in a plain browser), `src/api/bridge.ts`
transparently falls back to in-memory fixture data so every page renders.

## Deploy

`pnpm build` produces a flat `dist/` containing `index.html`, hashed JS/CSS,
and any static assets. The desktop install step copies these to
`<resources>/marketplace-ui/`. All imports use relative paths so the bundle
works under any URL scheme.

## Routes (hash-based)

- `#/` catalog
- `#/installed` installed plugins
- `#/plugin/<id>` plugin detail
