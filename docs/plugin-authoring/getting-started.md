# Getting started

OrcaSlicer plugins are loaded from `<data_dir>/plugins/<id>/`. Each
plugin ships a `manifest.json` plus exactly one of:

- A native shared library (`<id>.so` / `<id>.dll` / `<id>.dylib`).
- A WebAssembly module (`<id>.wasm`) with `"kind": "wasm"`.
- A static HTML bundle with `"kind": "webview"` and
  `"entry": { "webview": "index.html" }`.
- A `provides.profile_dir` declaration for data-only profile packs.

PluginManager dispatches on these manifest fields in order: profile
pack → webview → wasm → native.

## Pick an authoring path

| Goal                                     | Use                                       |
| ---------------------------------------- | ----------------------------------------- |
| Slice-pipeline observer or interceptor   | Rust SDK (cdylib)                         |
| G-code postprocessor                     | Rust SDK (cdylib) or WASM                 |
| New printer agent (Klipper / Bambu / …)  | Rust SDK (cdylib)                         |
| New settings page / sidebar / device tab | Rust SDK (cdylib) or webview              |
| Live dashboard / printer monitor         | Webview (TypeScript)                      |
| Bundle a vendor's profile pack           | Data-only manifest (no code)              |

## Rust scaffold

```sh
cargo install cargo-generate
cargo generate --git https://github.com/SoftFever/OrcaSlicer \
    templates/cargo-generate/orca-plugin-rust \
    --name my-orca-plugin

cd my-orca-plugin
cargo build --release
```

Drop the resulting `target/release/libmy_orca_plugin.so` and
`plugin.json` under `<data_dir>/plugins/com.example.my-orca-plugin/`.
Restart OrcaSlicer; the Plugin Manager (Help → Plugins…) lists the
plugin.

## Webview scaffold

```sh
npm create orca-plugin@latest my-dashboard
cd my-dashboard
pnpm install
pnpm run build
```

The build emits `<id>-<version>.orcaplugin` at the project root. Open
OrcaSlicer → Help → Plugins… → Install… and pick the file.

## Install via the GUI

Plugin Manager's **Install...** button accepts `.orcaplugin` zips:

1. Picks the manifest out of the zip and prompts you for permission.
2. Refuses if a slice is currently running (the host can't yank slot
   vtables out from under the pipeline).
3. Extracts to `<data_dir>/plugins/<id>/` and loads the plugin
   immediately.

## What runs where

- `register` / `unregister` run on the engine's main thread.
- Pipeline observer slots fire on whatever TBB worker reaches the
  step. Use synchronization primitives if you share state.
- Event subscribers fire on the publishing thread. For wasm and
  webview, the SDK marshals onto a single-threaded callback.
- WebView callbacks run on the UI thread. Slow work belongs in a
  Worker or behind `requestIdleCallback`.

## Where to look next

- [Slot reference](./slot-reference.md) — what each
  `ORCA_SLOT_*` does + the vtable shape.
- [Permission reference](./permission-reference.md) — exact
  semantics of every `ORCA_PERM_*` bit.
- [Manifest reference](./manifest-reference.md) — the manifest
  schema PluginManager validates.
- [Examples](./examples/) — runnable copies of common patterns.
