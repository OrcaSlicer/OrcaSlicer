---
name: orca-mcp
description: "Drive a running OrcaSlicer through its MCP server - reproduce a GitHub issue, capture before/after screenshots, script deterministic GUI steps. Use when you need to run the app and see or interact with it."
---

# Driving OrcaSlicer through its MCP server

OrcaSlicer has a built-in MCP server. With it you can launch the app, load files, pick presets,
change settings, slice, click through the GUI and take screenshots, then save the session as a
script that replays the same steps. `docs/HLSD/mcp-server.md` describes the design, and
`tools/mcp/README.md` the harness.

## Launch

Build Release (`cmake --build build --config Release --target all --` on Linux), then:

```bash
tools/mcp/orca_mcp.py launch --xvfb --print-config
```

On Windows, build with `build_win.bat` and run `python tools/mcp/orca_mcp.py launch --print-config`
(there is no `--xvfb`); it finds `build-clang\src\Release\orca-slicer.exe` too.

`launch` starts `build/src/Release/orca-slicer` (or `--bin PATH`) with `--mcp` and a fresh
temporary datadir. The datadir is seeded with a finished setup wizard and one printer, Bambu Lab X1
Carbon 0.4 by default. To use another printer, pass
`--printer "<model>@<nozzle>" --vendor <profile vendor> --filament "<preset>"`. `--xvfb` runs the
app on a private X display. `launch` waits until the app is ready, then prints the session as
JSON: `url`, `port`, `token`, `pid` and `datadir`. With `--print-config` it also prints the
`claude mcp add` line.

Other launch options:

- `--datadir DIR` reuses a datadir. Pass the first launch's datadir on every relaunch, so the
  presets stay identical.
- `--port N` sets the port, needed when 13619 is taken, for example by your own OrcaSlicer with MCP
  enabled.
- Arguments after `--` go to the app.
- `tools/mcp/orca_mcp.py stop` ends the app and its Xvfb.

You can talk to the app in two ways:

- **The harness**, which works from any shell at any time:
  `tools/mcp/orca_mcp.py call <tool> '<json args>'`. Add `--wait` to follow a returned job to its
  result. It also has shortcuts: `screenshot /abs/out.png --target canvas`, `snapshot`, `tools`
  and `run-script FILE --out DIR`.
- **Claude Code tools**, registered with
  `claude mcp add orcaslicer -- /abs/build/src/Release/orca-slicer --mcp-stdio --datadir DIR`.
  The stdio bridge reads the port and token of the instance running on that datadir every time it
  connects, so relaunches and token changes need no new registration. Claude Code loads MCP
  servers when a session starts, so register before you start the session that will use them.

## Reproduce an issue

1. Read the issue (`gh issue view <N> --repo OrcaSlicer/OrcaSlicer --comments`). Note the
   printer, process and filament presets, the changed settings and the steps. Download any
   attached project or model into a work folder. Every path you pass must be absolute.
2. Call `get_state`. Check `ready`, `presets`, `dialogs` and `display_backend`. After a forced
   stop, the app asks at startup whether to restore the unsaved project. Clear that and later
   prompts with
   `dialog_auto_answer {"rules": [{"match": "restore", "button": "no"}, {"match": "unsaved changes", "button": "no"}]}`;
   a rule also answers a matching dialog that is already open.
3. Load the files with `open_files {"paths": ["/abs/work/issue.3mf"]}`. A `.3mf` opens as a project,
   with its own presets and plates. Add `"as": "geometry"` to import only its objects, and
   `"discard_changes": true` to replace a modified project.
4. Configure. Find presets with `list_presets {"type": "printer", "query": "Voron"}` and select one
   with `select_preset {"type": "printer", "name": "<as listed>"}`; filaments take
   `filament_slot`. Find setting keys with `get_settings {"query": "seam"}`, then change them with
   `set_settings {"scope": "print", "values": {"wall_loops": 3, "sparse_infill_density": "15%"}}`.
   For per-object overrides, use `"scope": "object", "object": 0`.
5. Do the steps, in this order of preference: typed tools, then
   `list_actions {"query": "arrange"}` with `run_action {"id": "..."}`, then the GUI tools. A
   Speed Dial command with a path param skips its file dialog:
   `run_action {"id": "orca_command:export_stl:orca", "param": "{\"path\": \"/abs/x.stl\", \"overwrite\": true}"}`.
6. Slice with `slice {}`. Look at `status` and at `plates[].outcome`: `ok` (with print time and
   filament), `error`, `cancelled`, `empty` or `skipped`.
7. Point the view:
   `set_view {"tab": "preview", "camera": "iso", "layer": 40, "view_type": "feature_type"}`.
   Use `"camera": "fit_objects"` to frame the model.
8. Take a screenshot:
   `screenshot {"target": "canvas", "save_path": "/abs/work/before.png", "overwrite": true}`. Use
   `"target": "window"` for the whole UI, `"dialog"` for the topmost modal, and a widget's ref or
   locator (`{"label": "Slice plate"}`) for one widget or a window that is not modal. The reply carries
   a scaled-down image; open the full-size PNG with Read to look closer.
9. Save the session as a script with `export_script {"path": "/abs/work/repro.json"}`. Failed calls
   are left out; `export_script {"clear": true}` at the start drops the calls made while you
   explored. Now edit the script:
   - add `expect`s that hold when the bug is fixed, so the script fails before the fix and passes
     after it;
   - change the screenshot step's file to `after.png`;
   - replace downloaded paths with `${script_dir}/…` if you keep the files beside the script.
10. Fix the code, rebuild, `stop`, and launch again with the same `--datadir`. Then run
    `tools/mcp/orca_mcp.py run-script /abs/work/repro.json --out /abs/work`. It prints each step's
    result and exits 0 when the script passes. Without the harness, run
    `orca-slicer --datadir DIR --mcp-script repro.json --mcp-script-out /abs/work` on a display
    (under Xvfb, set `DISPLAY` and `GDK_BACKEND=x11`). It writes `result.json` and the
    screenshots, closes, and exits with 0 (passed), 1 (a step failed) or 2 (invalid script).
11. Compare `before.png` and `after.png` (Read both), and check each step's result.

## Rules

- **Never retry a call that returned `{"status": "running" | "blocked_by_dialog", "job": "jN"}`.**
  The call is still running. Use `wait {"for": "job", "job": "jN"}` to get its result; each `wait`
  lasts at most 50 s, so repeat it as needed. With the harness, use `call … --wait`. Retrying is
  safe only when the error says the call was not run.
- **`blocked_by_dialog`** lists the dialog's `title`, `message` and `buttons`. Answer it with
  `dialog_respond`, then wait for the job. A dialog marked `native: true` (message boxes, file
  pickers) can't be clicked, so register `dialog_auto_answer` before the step that opens it, or use
  a typed tool that needs no dialog. In a script, put `dialog_auto_answer` first. A file picker's
  text is its prompt, such as "Choose one or more files", not the name of the menu item that opened it.
- "A modal dialog is open; answer it first" means `get_state.dialogs` lists what is in the way.
- While a slice or G-code export runs, other changes are refused. `wait` for it, or `cancel` it.
- Before a screenshot after arrange, orient or anything else that runs in the background, call
  `wait {"for": "idle"}`.
- Prefer typed tools, then `run_action`, then clicks. Clicks are the most fragile path.
- GUI tools act on a `target`: a ref from the latest `ui_snapshot` (`"w12"`), or a locator such
  as `{"option": "layer_height"}` (a setting field), `{"label": "Slice plate"}`,
  `{"label": "Show splash screen", "within": "Preferences"}`,
  `{"menu": "File>Export>Export G-code"}`, `{"gltoolbar": "arrange"}`, `{"gizmo": "Move"}` or
  `{"imgui": "label"}`. Examples: `ui_snapshot {"query": "Arrange"}` (`"root": "dialog"` for the
  topmost modal), `ui_click {"target": "w12"}`,
  `ui_input {"target": {"option": "layer_height"}, "value": "0.16"}`, `ui_key {"keys": "Ctrl+Z"}`
  (to the topmost dialog when one is open), `dialog_respond {"button": "ok"}`. A target is scrolled
  into view first, as a person would, so a following screenshot shows it.
- Refs go stale when the UI changes, and after a language change. Take a new snapshot (narrowed
  with `"query"`) before each GUI action. Scripts store locators, not refs.
- `dialog_auto_answer {"rules": [{"match": "<regex>", "button": "no"}]}` replaces the rules your
  session set before, and an empty list clears them. Inside a script, the rules last until the
  script ends. `get_state.auto_answer_rules` lists the rules in force, with how often each answered.
- Paths are absolute. Output directories must exist, and an existing file is replaced only with
  `"overwrite": true`. In a `run_action` param, pass the JSON string
  `{"path": …, "overwrite": true}`.
- When something fails, `get_log {"level": "warning"}` shows why. Startup problems go to
  `<datadir>/mcp-launch.log`.
- Never point `--datadir` at your real OrcaSlicer profile.

## Script format

```jsonc
{ "orca_script": 1, "name": "repro #12345",
  "vars": {"model": "${script_dir}/part.3mf"},          // ${vars.model} below
  "steps": [
    {"tool": "dialog_auto_answer", "args": {"rules": [{"match": "unsaved", "button": "no"}]}},
    {"tool": "open_files", "args": {"paths": ["${vars.model}"], "discard_changes": true}},
    {"tool": "set_settings", "args": {"scope": "print", "values": {"wall_loops": 3}}},
    {"tool": "slice", "expect": {"/plates/0/outcome": {"eq": "ok"}}},
    {"tool": "set_view", "args": {"tab": "preview", "camera": "iso"}},
    {"tool": "screenshot", "args": {"target": "canvas", "save_path": "${out_dir}/after.png", "overwrite": true}}
  ]}
```

The placeholders are `${script_dir}`, `${out_dir}`, `${env:NAME}`, `${vars.NAME}` and
`${jobs.NAME}`. A step that returns a job is awaited, up to `timeout_s` (default 120), before its
`expect` is checked. To deal with a dialog the step opens, give it `"await": false, "save_as": "x"`,
answer the dialog in the next steps, then add
`{"tool": "wait", "args": {"for": "job", "job": "${jobs.x}"}}`. `expect` maps a JSON pointer into
the step's result to matchers: `eq ne gt ge lt le contains regex exists empty`. `"optional": true`
lets a step fail without stopping the run.

## Linux display notes

- `--xvfb` runs on X11, so `window` and `dialog` screenshots are exact while the window fits on the
  screen. The default screen is 1600×1000; change it with `--width`/`--height`, or resize with
  `set_view {"window_size": [1400, 900]}`.
- Under a Wayland session, without `--xvfb`, `get_state.display_backend` is `wayland`. There,
  `window` and `dialog` screenshots come back `approximate`: they are drawn from the widget tree,
  and web views can be blank. Use `"target": "canvas"`, which is exact everywhere, or launch with
  `GDK_BACKEND=x11`.
- On X11, OrcaSlicer's own popups and tooltips above the window are drawn into the capture, as on
  screen. A part covered by another application's window comes out black, and the result is
  `approximate`. Xvfb has nothing else on top.
- When launching by hand on a fresh datadir, set `SSL_CERT_FILE` (the harness does), or a CA-bundle
  prompt blocks startup.
- Testing in the Fedora or Debian VMs follows the machine's own notes and is not covered here.

## Windows notes

- `window` and `dialog` screenshots come from `PrintWindow`, so they are exact even when other
  windows cover OrcaSlicer, web views and the 3D views included. A minimized window can't be captured.
- The app takes the foreground for a moment while it starts, so keystrokes typed elsewhere then can
  reach it. The GUI tools synthesize their input inside the app and never move the real pointer.
