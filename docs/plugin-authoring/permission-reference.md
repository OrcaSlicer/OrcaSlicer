# Permission reference

`ORCA_PERM_*` bits declared in `plugin_api.h`. Plugins request the set
they need; the user grants (or refuses) at install time. Declare the
smallest accurate set: the install dialog lists them verbatim.

| Bit | Name                       | What it lets the plugin do                                                              |
| --- | -------------------------- | --------------------------------------------------------------------------------------- |
|  0  | `network`                  | Open outbound HTTP, WebSocket, mDNS, and arbitrary sockets.                             |
|  1  | `filesystem_read`          | Read files anywhere the user account can reach.                                         |
|  2  | `filesystem_write`         | Create or overwrite files anywhere the user account can reach.                          |
|  3  | `settings_read`            | Read your current Print / Filament / Printer settings.                                  |
|  4  | `settings_write`           | Change your current Print / Filament / Printer settings.                                |
|  5  | `profiles_install`         | Add new vendor / printer / filament profiles to the slicer.                             |
|  6  | `device_control`           | Send commands to your 3D printer (start/cancel print, raw G-code).                      |
|  7  | `slice_intercept`          | Halt or alter the slicing pipeline mid-run via `ORCA_SLOT_PIPELINE_INTERCEPTOR`.        |
|  8  | `gcode_modify`             | Rewrite generated G-code via `ORCA_SLOT_GCODE_FILTER`.                                  |
|  9  | `ui_attach`                | Add settings pages, menu entries, sidebar panels, or device tabs.                       |
| 10  | `events_raw`               | Subscribe to **every** event the engine emits, including future ones (advanced).        |

## Enforcement

- **Native plugins** — honor-system. The engine surfaces the declared
  permissions through `PluginPermissionDialog` so the user can refuse
  install, but a native plugin can do whatever the host process can.
- **WASM plugins** — enforced. The wasmtime import gates in
  `engine/src/runtime/WasmImports.cpp` refuse calls whose permission
  bit isn't in the plugin's `ImportContext::permissions`.
- **Webview plugins** — partial. The `presets_get_string` /
  `presets_set_string` / `load_profile_pack` actions check the bit;
  free-form network access via `fetch()` is not gated by the engine
  (the webview is a wxWebView with the system's network stack).

## Declaring permissions

### In `plugin.json`

```json
{
  "id": "com.example.demo",
  "permissions": ["network", "settings_read", "ui_attach"]
}
```

### In Rust

```rust
#[orca_plugin(
    id          = "com.example.demo",
    version     = "0.1.0",
    permissions = ["network", "settings_read", "ui_attach"],
)]
pub struct Demo;
```

The macro maps the lowercase names to ORCA_PERM_* bits at compile time
(`permissions_to_bits` in
[orca-plugin-macros](../../sdk/rust/orca-plugin-macros/src/lib.rs)).
