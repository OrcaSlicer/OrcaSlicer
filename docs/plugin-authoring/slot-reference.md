# Slot reference

Every slot kind has a documented vtable shape in
[`engine/include/orca/plugin_api.h`](../../engine/include/orca/plugin_api.h).
This file maps the C names to plain-English semantics + the Rust SDK
or webview equivalent.

| Slot                              | Hex   | Vtable                            | When it fires                                                                     |
| --------------------------------- | ----- | --------------------------------- | --------------------------------------------------------------------------------- |
| `ORCA_SLOT_PIPELINE_OBSERVER`     | 0x001 | `orca_slot_pipeline_observer_t`   | Each of the 9 `orca_pipeline_step_t` anchors during `Print::process`.            |
| `ORCA_SLOT_PIPELINE_INTERCEPTOR`  | 0x002 | `orca_slot_pipeline_interceptor_t`| Same anchors, but the disposition (Proceed / Skip / Abort) is folded across slots.|
| `ORCA_SLOT_GCODE_FILTER`          | 0x003 | `orca_slot_gcode_filter_t`        | After `Print::export_gcode`, before any external `gcode_post_process_script`.    |
| `ORCA_SLOT_PLACEHOLDER_PROVIDER`  | 0x004 | (host imports only)               | Top of `Print::export_gcode`, with the active `PlaceholderParser` on TLS.        |
| `ORCA_SLOT_PRINTER_AGENT`         | 0x010 | `orca_slot_printer_agent_t`       | When the user picks the agent type for a configured printer.                      |
| `ORCA_SLOT_PROFILE_PACK`          | 0x020 | `orca_slot_profile_pack_t`        | Once at load — the dir path is passed straight to `Presets::load_vendor_configs`. |
| `ORCA_SLOT_SETTINGS_PAGE`         | 0x100 | `orca_slot_settings_page_t`       | When the Tab is built (TabPrint / TabFilament / TabPrinter).                      |
| `ORCA_SLOT_DEVICE_TAB`            | 0x101 | `orca_slot_device_tab_t`          | When the top-level tabpanel is built.                                             |
| `ORCA_SLOT_MENU_COMMAND`          | 0x102 | `orca_slot_menu_command_t`        | When the menubar is built.                                                        |
| `ORCA_SLOT_SIDEBAR_PANEL`         | 0x103 | `orca_slot_sidebar_panel_t`       | When the Plater sidebar is built.                                                 |

## Disposition fold for interceptors

The pipeline interceptor folds dispositions across all slots for a
given step:

- Any `ABORT` wins. The pipeline halts and Print::process returns.
- Otherwise any `SKIP` wins. The step is skipped.
- Otherwise `PROCEED`. The step runs as usual.

## UI slot builder ABI

Settings page / sidebar / device tab slots use the same
`orca_ui_builder_t` vtable to populate their container. From Rust or
webview, you call the builder's `add_*` methods imperatively:

```rust
// Rust — inside orca_slot_settings_page_t::on_build
unsafe { (*b).add_label(page, c"Connection".as_ptr()); }
unsafe { (*b).push_group(page, c"Network".as_ptr()); }
unsafe { (*b).add_text_field(
    page, c"Host or IP".as_ptr(), c"host".as_ptr(),
    c"192.168.1.42".as_ptr()); }
unsafe { (*b).pop_group(page); }
```

The wxWidgets host implementation lives in
[`src/slic3r/GUI/PluginUiBuilder.cpp`](../../src/slic3r/GUI/PluginUiBuilder.cpp).
Values are addressable via the plugin-chosen key; the host owns
persistence. `on_value_changed` fires on the UI thread whenever any
field mutates.

## Permission gates

Permission requirements per slot:

- Any UI slot — `ORCA_PERM_UI_ATTACH`.
- Pipeline interceptor — `ORCA_PERM_SLICE_INTERCEPT`.
- G-code filter — `ORCA_PERM_GCODE_MODIFY`.
- Printer agent — `ORCA_PERM_DEVICE_CONTROL`.
- Profile pack — `ORCA_PERM_PROFILES_INSTALL`.
- Raw event subscriber — `ORCA_PERM_EVENTS_RAW`.

The host enforces these for wasm guests via import gates. For native
plugins they are display-only; PluginPermissionDialog surfaces the
declared set so the user can refuse install.
