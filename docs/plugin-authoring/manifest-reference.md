# Manifest reference

`<data_dir>/plugins/<id>/manifest.json` describes the plugin to
PluginManager. Every install or hot-reload reads this file first.

## Required fields

| Field         | Type     | Notes                                                                 |
| ------------- | -------- | --------------------------------------------------------------------- |
| `id`          | string   | Reverse-DNS, e.g. `"com.example.demo"`. Doubles as the directory name.|
| `name`        | string   | Human-readable. Shown in the Plugin Manager.                          |
| `version`     | string   | Semver. e.g. `"0.1.0"`.                                               |

## Optional fields

| Field          | Type      | Notes                                                                 |
| -------------- | --------- | --------------------------------------------------------------------- |
| `author`       | string    |                                                                       |
| `description`  | string    | One-line summary shown alongside the permission list at install time. |
| `permissions`  | string[]  | Lowercase names — see [permission-reference.md](./permission-reference.md). |
| `kind`         | string    | `"wasm"` or `"webview"`. Default: native (load `<id>.{so,dll,dylib}`).|
| `entry`        | object    | For `kind="webview"`: `{ "webview": "index.html" }`.                   |
| `provides`     | object    | For data-only profile packs: `{ "profile_dir": "vendor" }`.            |

## Dispatch order

PluginManager picks a load path by scanning the manifest in this
order:

1. `provides.profile_dir` set → data-only profile pack. No code runs;
   the pack is registered as `ORCA_SLOT_PROFILE_PACK` and the dir
   gets passed to `Presets::load_vendor_configs_from_json`.
2. `kind == "webview"` → metadata-only registration. The actual
   `wxWebView` is spawned by Slic3r::GUI when it mounts the page.
3. `kind == "wasm"` → loaded into wasmtime via `WasmPlugin::load`,
   which runs `orca_plugin_check_debug_consistent` (if exported),
   `orca_plugin_register`, and `orca_plugin_unregister` on dtor.
4. Default → `dlopen` `<id>_<version>.{so,dll,dylib}` (preferred)
   then `<id>.{so,dll,dylib}` (fallback). Runs the same three
   mandatory C exports.

## Examples

### Minimal native

```json
{
  "id":      "com.example.minimal",
  "name":    "Minimal Plugin",
  "version": "0.1.0"
}
```

### WASM with permissions

```json
{
  "id":          "com.example.demo-wasm",
  "name":        "Demo WASM Plugin",
  "version":     "0.1.0",
  "kind":        "wasm",
  "permissions": ["ui_attach", "settings_read"]
}
```

### WebView

```json
{
  "id":          "com.example.dashboard",
  "name":        "Printer Dashboard",
  "version":     "1.0.0",
  "kind":        "webview",
  "entry":       { "webview": "index.html" },
  "permissions": ["device_control"]
}
```

### Data-only profile pack

```json
{
  "id":          "com.example.bambu-profiles",
  "name":        "Bambu Profile Pack",
  "version":     "0.5.0",
  "permissions": ["profiles_install"],
  "provides":    { "profile_dir": "vendor" }
}
```

## Validation

The Plugin Manager's `Install...` flow validates `id`, `version`, and
`permissions` shape. Missing `id` → `IdMissing`. Malformed JSON →
`ManifestMalformed`. The runtime loaders re-validate against the
specific kind they handle (e.g. missing `.wasm` file for a wasm
manifest → `NotFound`).
