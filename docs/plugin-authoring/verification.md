# Verifying your plugin

`orca-plugin-verify` is a Rust CLI that loads your built `.orcaplugin` against
the real engine ABI and reports any drift between what your manifest claims
and what your binary actually does. The marketplace publish pipeline runs the
same checks before accepting a submission — running them locally is the
fastest way to catch regressions before they bounce back to you.

## Installation

```bash
cargo install --locked orca-plugin-verify
```

`cargo install` puts the binary on your `$PATH`. The TypeScript scaffold's
`pnpm run verify` script and the Rust scaffold's GitHub Actions workflow both
invoke it through `$PATH`, so installing it once is enough for both.

## Usage

```bash
orca-plugin-verify ./dist/com.example.foo.orcaplugin
orca-plugin-verify ./staging/com.example.foo            # already-unpacked dir
orca-plugin-verify --smoke ./dist/com.example.foo.orcaplugin
orca-plugin-verify --json ./dist/com.example.foo.orcaplugin
```

The `path` argument accepts either a `.orcaplugin` zip or an already-unpacked
plugin directory. Zips are extracted to a tempdir and cleaned up on exit.

## What it checks

| Check          | What it verifies                                                                   | Status when violated |
| -------------- | ---------------------------------------------------------------------------------- | -------------------- |
| `layout`       | `manifest.json` exists; binary present per declared `kind`                         | FAIL                 |
| `manifest`     | id reverse-DNS-shaped; version valid semver; permissions in known set              | FAIL                 |
| `schema`       | optional `settings.schema.json` parses as JSON Schema 2020-12                      | FAIL                 |
| `abi`          | binary loads, calls `orca_plugin_register`, returns `ORCA_OK`                      | FAIL                 |
| `slots`        | `manifest.provides[]` equals the slot kinds the binary actually registered (see data-kind exception below) | FAIL                 |
| `permissions`  | every slot-implied + import-implied permission is declared in `manifest.permissions` | FAIL / WARN          |
| `smoke`        | (opt-in) plugin survives load → unload → reload cycle                              | FAIL                 |

The `permissions` check is HARD for wasm guests (the imports section is
authoritative) but BEST-EFFORT for native binaries (we can only inspect
dynamic-library references via `goblin`; the real enforcement happens at
runtime when the engine routes calls through the host vtable).

## Required `manifest.json` fields

The verifier expects this shape at minimum:

```json
{
  "id":          "com.example.demo",
  "version":     "0.1.0",
  "kind":        "native",
  "permissions": ["network"],
  "provides":    ["printer_agent"]
}
```

`kind` defaults to `"native"` if omitted. `provides[]` is what the slot check
uses to detect drift — if you register a slot from your `register` function,
list its kind here. Known kind strings:

```
pipeline_observer     pipeline_interceptor
gcode_filter          placeholder_provider
printer_agent         profile_pack
settings_page         device_tab
menu_command          sidebar_panel
```

### `slots` check — data-kind exception

For `kind: data` plugins, the slots check trusts the manifest because there
is no binary to handshake. The check runs in a separate, manifest-only
mode:

- `provides[]` MUST be non-empty — a data plugin that doesn't advertise
  anything is rejected (`data plugin must declare at least one slot it
  provides`).
- Slot kinds that require a runtime registration — `printer_agent`,
  `pipeline_observer`, `pipeline_interceptor`, `gcode_filter`,
  `placeholder_provider`, `settings_page`, `device_tab`, `menu_command`,
  `sidebar_panel` — are rejected for data plugins, because there's no
  binary to register them.
- Data-loadable slot kinds — currently just `profile_pack` (loaded by
  `PresetBundle::load_vendor_configs_from_json` from the staged tree) —
  are accepted, and the check emits `[SKIP] slots` with a detail like
  `data plugin: manifest asserts ["profile_pack"]`.

For `kind: native | wasm | hybrid` (and `webview` when it registers slots),
the original equality check still applies: the recorded ABI-handshake slot
set must match `provides[]`.

Known permission strings: `network`, `filesystem_read`, `filesystem_write`,
`settings_read`, `settings_write`, `profiles_install`, `device_control`,
`slice_intercept`, `gcode_modify`, `ui_attach`, `events_raw`.

## Interpreting failures

### `[FAIL] slots — binary registered [16] but manifest doesn't declare them`

The number is the numeric ORCA_SLOT_* constant: 0x10 = 16 = `printer_agent`.
Add the missing kind name to your manifest's `provides[]`.

### `[FAIL] permissions — missing permissions: 0x40 (device_control)`

A slot you registered requires a permission you didn't declare. Add it to
`permissions[]`. `printer_agent` always needs `device_control`; UI slots
always need `ui_attach`; `pipeline_interceptor` needs `slice_intercept`;
`gcode_filter` needs `gcode_modify`; `profile_pack` needs `profiles_install`.

### `[FAIL] abi — orca_plugin_register returned non-OK (...)`

Your plugin's `register` function returned an error before completing slot
registrations. Most common cause: an unwrap or panic in `on_register` that
gets caught at the FFI boundary. Add `ctx.log_error(...)` calls inside your
register path and re-run; the verifier's stub host vtable will print them
to stderr.

### `[FAIL] manifest — version "1.0.0-dev+meta" is not valid semver`

Use `1.0.0-dev.0` (semver build-metadata segments need a literal `+`, but
identifiers may not include `.` after the `+`). See semver.org for the full
grammar.

## JSON output

`--json` emits the same report as a structured object on stdout. Designed
for CI scripting:

```bash
orca-plugin-verify --json dist/foo.orcaplugin | jq '.checks[] | select(.status == "fail")'
```

The schema:

```json
{
  "checks": [
    {
      "name":   "layout",
      "status": "pass",      // pass | fail | warn | skip
      "detail": ""           // empty on pass; human-readable text otherwise
    }
  ]
}
```

Exit code is 0 if every check is pass / warn / skip, non-zero on any fail.

## Limitations

- **Native ABI handshake is Linux/macOS only.** Windows support follows the
  Phase 7.2 marketplace publish pipeline (we use `LoadLibraryA` for the
  load step but the symbol-export plumbing the SDK relies on takes more
  setup on Windows; the verifier SKIPs that check on Windows runners).
- **Smoke fire reloads the binary but does not invoke each slot vtable.**
  Slot vtables receive engine-owned context objects we can't safely
  synthesize outside the engine; that level of testing happens inside the
  in-engine sandbox the marketplace runs against each submission.
- **Webview UI bundles are layout-checked only.** The verifier confirms
  `ui/index.html` parses but does not boot a webview to evaluate scripts.
  Use `pnpm run dev` from the scaffold for that.
