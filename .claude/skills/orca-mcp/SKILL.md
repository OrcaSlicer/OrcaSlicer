---
name: orca-mcp
description: "Drive a running OrcaSlicer through its MCP server - reproduce a GitHub issue, capture before/after screenshots, script deterministic GUI steps, build parts in the Design (CAD) tab. Use when you need to run the app and see or interact with it."
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

On macOS, build with `cmake --build build/arm64 --config Release --target all --` and run
`tools/mcp/orca_mcp.py launch --print-config` (there is no `--xvfb`); it finds the app bundle's
binary under `build/<arch>`.

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
- `--cad` turns the Design (CAD) tab on, which `cad_call` needs. The app reads that setting only at
  startup, so pass it on the launch, also when you reuse a datadir.
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
   For per-object overrides, use `"scope": "object", "object": 0`. "Settings, presets and
   calibration" below covers object filaments, saving and comparing presets, the project's and the
   plates' own settings, preferences and calibration tests.
5. Do the steps, in this order of preference: typed tools, then
   `list_actions {"query": "arrange"}` with `run_action {"id": "..."}`, then the GUI tools. A
   Speed Dial command with a path param skips its file dialog:
   `run_action {"id": "orca_command:export_stl:orca", "param": "{\"path\": \"/abs/x.stl\", \"overwrite\": true}"}`.
   A shape takes its size the same way, with nothing selected:
   `run_action {"id": "orca_command:add_primitive_cube:orca", "param": "40 40 10"}`. Place it with
   `transform`: `translate`, `rotate`, `scale`, `size` (`[80, null, null]` keeps the proportions),
   `plate` to move it to another plate, or `volume` to move one part instead of the object. The
   object stays on the bed and on the plate it lands on, as after a move in the 3D view.
   Add a part, negative part, modifier or support blocker/enforcer to an object with `add_part
   {"object": 0, "type": "negative", "shape": "cylinder", "size": [6.2, 3.2], "position": [128, 128,
   1.6]}` (`position` is the part's centre on the bed), or `"path"` for a model file. Add parts before
   instances: an object with several is refused.
   The object list's menu commands act on what `select` picked: `obj_type_modifier` (and `_part`,
   `_negative`, `_support_blocker`, `_support_enforcer`, `_precise_seam`) after `select {"volumes":
   [[0, 1]]}`; `obj_rename` with the name as param; `obj_set_instances` with the total count;
   `obj_instances_to_objects`, `obj_assemble` (two or more objects); `obj_order_by_height` (param
   `all` for every plate). Choosing the order objects already have reverses it, so read the reply's
   `order` and `descending`. Indices change after these; list objects again.
   Arrange with `arrange {}` (every unlocked plate), `arrange {"plates": 1, "spacing": 10,
   "rotate": false}` (one plate; the options stay saved, as in the Arrange options panel) or
   `arrange {"fill": 0}` (fill object 0's plate with instances of it). It waits for the job; list
   objects again afterwards, since an arrange may reorder them. Check placement with
   `list_objects`: per instance `bbox` and `mirrored`, per part `position`, `bbox` and
   `scaled_volume` on the bed (instance 0) and the `filament` it prints with.
   Height range modifiers: `set_layer_ranges {"object": 0, "layer_config_ranges": [{"min_z": 0,
   "max_z": 15, "values": {"layer_height": 0.28}}, {"min_z": 15, "max_z": 60, "values":
   {"sparse_infill_density": "25%", "extruder": 2}}]}` replaces all of them (`[]` removes them);
   ranges may touch but not overlap. `list_objects` shows them as `layer_config_ranges`.
   Variable layer height: `set_layer_height_profile {"object": 0, "action": "adaptive", "quality":
   0.5}`, then `"smooth"` (`radius`, `keep_min`) or `"reset"`; `list_objects` shows the result as
   `layer_height_profile` (point count and extreme heights). Inside a height range Adaptive keeps the
   range's `layer_height`, and once an object has a profile it decides the layer heights.
6. Slice with `slice {}`. Look at `status` and at `plates[].outcome`: `ok`, `error`, `cancelled`,
   `empty` or `skipped`. An `ok` plate carries the print time, `filament_g`/`filament_m` with the
   flush, `layers`, `max_z`, `filament_changes`, and `filaments[i]` (slot i + 1) with its
   `model_g`, `support_g`, `flush_g` and `tower_g`, so grams per colour or the purge waste need no
   G-code or legend. Read the legend's numbers with `get_preview {"include": ["features", "layers",
   "objects", "view"]}`: time and grams per feature type, per-layer Z, height and time (paged),
   grams per object, and the colour ranges (highest volumetric flow, shortest layer time).
   Layer G-codes go on the sliced layers, as the Preview layer slider's menu places them:
   `set_layer_gcode {"add": [{"layer": 26, "type": "pause"}, {"z": 10.2, "type": "custom", "gcode":
   "M104 S215"}, {"layer": 60, "type": "change_filament", "filament": 2}]}` (also `remove` by layer or
   z, and `clear`). `layer` is 1-based as the slider numbers it, `z` the layer whose top is at or above
   it; the reply lists the plate's codes with both. Slice again before exporting.
7. Point the view:
   `set_view {"tab": "preview", "camera": "iso", "layer": 40, "view_type": "feature_type"}`.
   Use `"camera": "fit_objects"` to frame the model. `"layer_z": 3.2` shows up to the layer that
   covers that height (where a custom G-code at 3.2 mm goes); `"legend": {"Sparse infill": false,
   "travel": true}` hides or shows legend rows, named as `get_preview` lists them. A `view_type` is
   picked as in the legend's combo, so Line Type shows every feature type again: give `legend` in
   the same call to hide some. `get_state.preview` reads back the view type and the layer range
   with its Z.
8. Take a screenshot:
   `screenshot {"target": "canvas", "save_path": "/abs/work/before.png", "overwrite": true}`. Use
   `"target": "window"` for the whole UI, `"dialog"` for the topmost modal, and a widget's ref or
   locator (`{"label": "Slice plate"}`) for one widget or a window that is not modal. The reply carries
   a scaled-down image; open the full-size PNG with Read to look closer.
9. Save the session as a script with `export_script {"path": "/abs/work/repro.json"}`. Failed calls
   are left out, a job that failed after its reply included. A call whose job is still running is
   left out too and named in the reply's `left_out_running`; wait for it and export again.
   `export_script {"clear": true}` at the start drops the calls made while you explored. Now edit
   the script:
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
- While an operation runs (a slice, an export, or a background job such as an arrange that a call
  started, however it started), other changes are refused. `wait` for its job, or `cancel` it. A
  background job a person started (typing in the Text gizmo) refuses changes too; `wait {"for":
  "idle"}`. Reads still answer.
- A Speed Dial command that did nothing (no object selected) is an error, with the reason.
- Before a screenshot after anything else that runs in the background, call `wait {"for": "idle"}`.
- Prefer typed tools, then `run_action`, then clicks. Clicks are the most fragile path.
- GUI tools act on a `target`: a ref from the latest `ui_snapshot` (`"w12"`), or a locator such
  as `{"option": "layer_height"}` (a setting field), `{"label": "Slice plate"}`,
  `{"label": "Show splash screen", "within": "Preferences"}`,
  `{"menu": "File>Export>Export G-code"}`, `{"gltoolbar": "arrange"}`, `{"gizmo": "Move"}`,
  `{"imgui": "label"}` or `{"imgui_name": "name"}`. Examples: `ui_snapshot {"query": "Arrange"}` (`"root": "dialog"` for the
  topmost modal), `ui_click {"target": "w12"}`,
  `ui_input {"target": {"option": "layer_height"}, "value": "0.16"}`, `ui_key {"keys": "Ctrl+Z"}`
  (to the topmost dialog when one is open), `dialog_respond {"button": "ok"}`. A target is scrolled
  into view first, as a person would, so a following screenshot shows it.
- Menus a click or key opens (right-click menus, a button's menu) are never shown: the reply lists
  them as `popup_menu`. Run an item with the same call plus `"popup_item": "Add Modifier>Cylinder"`,
  for instance `ui_mouse {"action": "click", "button": "right", "at": {"object": 0, "spot": "top"},
  "popup_item": "Add Modifier>Cylinder"}`; the reply says `ran`. A repeated label takes `#2`. A menu
  that opened with no input call to list it is in `get_state.closed_menus`.
- Rows of lists, trees and tables are `row` nodes in `ui_snapshot`: the object list (shown while the
  sidebar's Process switch is on Objects, `ui_input {"target": {"class": "SwitchButton", "label":
  "Process"}, "value": true}`), Compare presets, the object settings table, drop-down lists. Target
  one by its last labels: `ui_click {"target": {"row": "Cube>Generic-Cylinder"}}` selects it as a
  click does (`"modifiers": ["ctrl"]` adds it), `"column": "Printable"` acts on that cell, `"button":
  "right"` with `popup_item` runs its menu, `ui_input ... "value": true` ticks its checkbox. Shown
  popups (the print button's menu, an open drop-down list) follow their window in `ui_snapshot`;
  `"root": "popup"` lists only them.
- Plate icons in the Prepare view are targets too: `ui_click {"target": {"plate_icon": "lock",
  "plate": 1}}` (`plate`, `delete`, `orient`, `arrange`, `lock`, `settings`, `name`,
  `move_to_front`, `filament_map`, `imex_mode`; plates count from 0). Lock replies `locked`, arrange
  and orient wait for their job (`done`), settings replies the Plate Settings dialog it opened, and
  `"button": "right"` on `"plate"` lists the plate menu (`popup_item` runs one of its items).
- `webview_eval` runs in a modal dialog's page only for the setup wizard and the preset bundle
  dialogs (`run_action` ids `orca_command:help_setup_wizard:orca`, `orca_command:preset_bundle:orca`,
  with `"await": false` in a script); without a target it uses that dialog's
  web view. Commit such a page by clicking its own button from the script: `dialog_respond` only
  closes the dialog, and the reply says `closed: true` when the script closed it.
- Refs go stale when the UI changes, and after a language change. Take a new snapshot (narrowed
  with `"query"`) before each GUI action. Scripts store locators, not refs.
- `dialog_auto_answer {"rules": [{"match": "<regex>", "button": "no"}]}` replaces the rules your
  session set before, and an empty list clears them. It presses a wx dialog's button as
  `dialog_respond` does (Transfer keeps the edits), and answers a native one unseen. Inside a
  script, the rules last until the script ends. `get_state.auto_answer_rules` lists the rules in
  force, with how often each answered.
- To upload to a print host (OctoPrint, Moonraker, PrusaLink…), set `host_type` and `print_host` with
  `set_settings {"scope": "printer"}` (and `"bbl_use_printhost": true` on a Bambu printer), then
  `run_action {"id": "orca_command:send_gcode:orca"}` opens the Send dialog: set the name with
  `ui_input {"target": {"class": "wxTextCtrl", "within": "Send G-code to printer host"}, "value":
  "part.gcode", "submit": false}`, keeping the suffix it proposes (`.gcode.3mf` when the printer's
  `use_3mf` is on, or it asks to confirm), and press `dialog_respond {"button": "Upload"}` (or
  "Upload and Print"). The upload runs in the background: poll `get_state.uploads` until its row reads
  `completed` or `error`; an error also opens a dialog to close with `dialog_respond`.
- Paths are absolute. Output directories must exist, and an existing file is replaced only with
  `"overwrite": true`. In a `run_action` param, pass the JSON string
  `{"path": …, "overwrite": true}`.
- When something fails, `get_log {"level": "warning"}` shows why. Startup problems go to
  `<datadir>/mcp-launch.log`.
- Never point `--datadir` at your real OrcaSlicer profile.

## Settings, presets and calibration

`tools/mcp/tests/presets.json`, `multicolor.json`, `plates.json` and `calibration.json` use each of
these; copy their steps.

- Object and part overrides take only the keys the object list's Add Settings menu offers there
  (region keys for a part or modifier); others, such as `print_sequence`, are refused. `"extruder": 2`
  sets the object's filament (`"scope": "volume"` a part's) and leaves the selection alone.
  `get_settings` gives each override's `effective` value and its `source`.
- A value whose meta lists `variants` (one entry per nozzle variant, as most X1C speeds and
  temperatures have) takes a single number for the entries in use (`meta.active`), as the tab does;
  `{"index": i, "value": 250}` sets another entry and an array sets them all.
  `set_settings` replies with every key it `changed`, including the app's own adjustments.
- `get_settings {"compare": ":saved"}` lists the unsaved edits (`":parent"` or a preset name compares
  with that preset), and `"preset"` reads a preset that is not selected. A `null` value takes a
  preset key back to its saved value (`"reset_to": "parent"`: the parent's).
- Save tuned settings with `save_preset {"type": "filament", "name": "My PLA"}` (`"overwrite": true`
  replaces a preset that exists, as on a rerun); to keep only some edits, first
  `set_settings {"scope": "filament", "reset_all": true, "values": {...}}`. Remove presets you made
  with `delete_preset {"type": "filament", "name": "My PLA"}` (a printer must be selected first).
- With unsaved edits, `select_preset` opens the Transfer-or-discard prompt unless you pass
  `"changes": "transfer"` or `"discard"` (with several filament slots a slot's switch asks nothing, and
  the edits stay on the preset the filament tab edits); `new_project {"discard_changes": true,
  "changes": "discard"}` also drops modified presets, which it otherwise keeps.
- The project's own settings (plate type, slot colours, flushing volumes and multiplier, prime tower
  position per plate, which slots are mixed) use `"scope": "project"`:
  `set_settings {"scope": "project", "values": {"filament_colour": {"index": 1, "value": "#000000"}, "curr_bed_type": "Textured PEI Plate"}}`.
  Set `flush_volumes_matrix` last: a colour change recalculates it.
- A plate's own settings use `"scope": "plate"` with `plate`:
  `set_settings {"scope": "plate", "plate": 1, "values": {"curr_bed_type": "Textured PEI Plate", "print_sequence": "by object", "locked": true, "name": "Batch"}}`
  (null goes back to the global setting); `list_plates` reads every plate back, with its layer
  G-codes. Add or delete filament slots with `run_action orca_command:filament_add:orca` and
  `orca_command:filament_delete:orca` (`param`: the 1-based slot).
- Read preferences with `get_settings {"scope": "preferences", "keys": ["export_statistics"]}`; change
  one in the dialog:
  `run_action {"id": "orca_command:open_preferences:orca", "param": "developer:export_statistics"}`,
  then `ui_input {"target": {"name": "export_statistics", "within": "Preferences"}, "value": true}`
  and `dialog_respond {"button": "close"}`. Checkboxes and number or text fields save this way; most
  of the dialog's drop-down lists do not.
- Import preset files with `run_action orca_command:import_configs:orca` and a `.json`, `.zip` or
  `.orca_*` path (`{"path": ..., "on_conflict": "overwrite"}` replaces presets that exist).
  `orca_command:export_config:orca` with a directory still opens the export dialog: tick presets by
  `name` with `ui_input`, then press OK with `dialog_respond`.
- Make a calibration test with
  `calibrate {"test": "pressure_advance", "method": "pattern", "speeds": [50, 100], "discard_changes": true, "changes": "discard"}`
  rather than its dialog; it prints with filament slot 1, and its reply's `result_keys` say where the
  measured value goes. To keep that value, `new_project {"discard_changes": true, "changes": "discard"}`,
  then `set_settings` it and `save_preset`.

## Gizmos

Every gizmo is driven like a person drives it: its icon, its panel's controls, clicks on the model.
`tools/mcp/tests/gizmos.json` does it for all of them; copy its steps.

- Select the object (`select`), then `ui_click {"target": {"gizmo": "Seam"}}`. `ui_snapshot` lists
  the gizmos of the shown 3D view, `[toolbar]` or `[no toolbar icon]` (Simplify, and SVG on a selected
  SVG part). A second click closes the gizmo (`"active": false`); Text on an object with no text
  creates a text part and replies `"created": true`. `get_state.gizmo` names the open one.
- `ui_snapshot` lists each ImGui panel with its items and text, in reading order. A control is
  found by its label (a field with none takes the text left of it on its row, `{"imgui": "Brush size"}`)
  or its name (`{"imgui_name": "Fill"}` for an icon button, `{"imgui_name": "extruder_color_2"}`).
  Each item shows its kind and value: `slider`, `drag`, `input`, `multiline`, `combo`, `checkbox`,
  `radio`, `toggle`, `button`; `[no typing]` marks a slider that takes no typed value.
- `ui_input` sets a control by its kind and reads the value back: a combo by option text or 0-based
  index, a slider
  through the number field beside it, multiline text with `\n`, a checkbox, radio or toggle with
  true/false. A control below the visible part of a panel is scrolled into view first; a list that
  only draws what shows (Texture Displacement's library) scrolls with `ui_mouse` `wheel` over it.
- Click the model at a point instead of a pixel: `ui_mouse {"at": {"object": 0, "spot": "top"}}`, or
  `"point": [x, y, z]` in the part's mesh coordinates (`"frame": "world"` for scene coordinates;
  add `"volume"` for a part; modifiers and negative parts too). It fails, naming what is in the way,
  when the point is hidden, below the bed or cut away; turn the camera with `set_view` (from below,
  the bed hides nothing). `"check": "point"` skips that, for a point that is not on the surface (a cut
  plane). Either way it fails when the point lies under a panel or notification, which would take the
  click. Strokes: `"action": "drag"` with `"path"` and `"to"` points. `"modifiers": ["shift"]` holds
  Shift as a key (Measure's point picking); `"button": "right"` paints blockers or erases.
- The panel's fields set grabber values (Position, rotation, scale, cut height); grabbers are not
  dragged. File pickers inside a gizmo: `gizmo_file` (`svg_change`, `svg_save_as`,
  `texture_import`); add an SVG with `run_action add_primitive_svg` and a path.
- The assemble view: `set_view {"view": "assemble"}`, gizmo targets with `"canvas": "assemble"`,
  and its Return button (`{"imgui": "Return"}`) or `set_view {"view": "3d"}` to leave.
- Check the result in `list_objects`: per part `painted` (supports, seam, fuzzy_skin, mmu, texture),
  `triangles`, `mesh_volume`, `text`, `svg`; per object `brim_ears`. After Simplify or a texture
  preview, `wait {"for": "idle"}` first.

## Design tab (CAD)

Launch with `--cad`. Every call is `cad_call {"method": "<name>", "params": {...}}`, and
`tools/mcp/tests/cad_*.json` has working examples of each method; copy their steps.

- Look up a method's params with `cad_call {"method": "describe_tools", "params": {"method": ["extrude", "hole"]}}`
  or `{"query": "sketch"}`, not the whole list. `describe_scene` lists the features, bodies,
  variables, undo depth and `modeling_origin`.
- Base planes `"XY"`, `"XZ"` and `"YZ"` pass through `modeling_origin` (the bed centre), for sketches
  and features alike, so sketch and feature coordinates are offsets from it. `{"datum": i}` is a
  Plane feature, `{"body": b, "face": f}` a planar face. Replies carry the `frame` used.
  `query_topology`, `measure` and bounding boxes are in world coordinates.
- Sketch to solid: `sketch_begin {"plane": "XY"}`, then `sketch_add` with `rect`, `entities` (lines,
  arcs, circles) or a shape `type` (`polygon`, `slot`, `ellipse`, `ellipse_arc`, `spline`). Shape
  the sketch with `sketch_fillet`, `sketch_chamfer`, `sketch_trim`, `sketch_transform` and
  `sketch_constrain`, then `sketch_commit`. Its reply gives `sketch_index` and `closed_loops`.
  Then `extrude {"sketch": 0, "distance": 10}` (`"loop": i` for one region; by default it joins the
  body the sketch touches) or `revolve {"sketch": 0}` (about its only centerline, or `axis_entity`).
  Committing an empty sketch fails and keeps it open; `sketch_cancel` leaves it.
- A call that fails is an error and changes nothing. The methods in `describe_tools`'
  `legacy_ok_false` (`fillet`, `chamfer`, `boolean`, `pattern`, `shell` and others) answer
  `{"ok": false, "error": …}` instead, so check `ok` after those.
- Face and edge ids from `query_topology` expire when the model rebuilds. Pass back the
  `generation` you read them with, or query again before each call that takes one. Its filters
  (`kinds`, `edge_kinds`, `direction`, `at_extreme`, `convex`, `adjacency`) find a face or edge
  without paging through all of them. Deleting or moving features renumbers features and bodies.
- Holes and threads: `hole`, `hole_styled` and `hole_standard {"designation": "M3"}` take `at`, a
  world point or a bore (`{"body": b, "face": f}`) or rim (`{"body": b, "edge": e}`) whose axis they
  share, instead of `x`/`y`. `thread {"body": 0, "face": f}` taps a bore with the nearest standard.
  `thread`, `sweep` and `loft` can take seconds and return a job: wait for it.
- Read back with `mass_properties {"body": 0}` (volume), `measure` (`min_distance`),
  `describe_feature {"feature": i}`, `sketch_describe {"sketch": i}` and `sketch_constraints`.
  Edit with `set_feature {"feature": 1, "fields": {"distance": 15}}` (the card's fields, within
  `ranges`), `set_variable` and `set_feature_expr`, `feature_delete`, `feature_move`,
  `feature_set_enabled`, `feature_rename`, `body_set {"body": 0, "name": …, "color": "#3080ff",
  "visible": …}` and `body_delete`.
- `cad_call undo` / `redo` undo the Design history, or the open sketch's; `run_action undo` undoes
  Prepare's.
- Hand off with `commit {}` (Commit to Plate; `"mode": "bodies"` makes one object per body,
  `"replace": [i]` deletes those Prepare objects in the same undo step), then `list_objects`.
  `export_step {"path": …}` writes STEP; `import_step` and `import_mesh` take absolute paths;
  `new_design {"discard_changes": true}` starts over.
- `export_script` leaves out failed calls and drops `generation`, so a session replays as is.
- See the design with `set_view {"tab": "design", "camera": "iso"}` (`default` is its Home view,
  `fit_selection` frames the pick), then `screenshot {"target": "canvas"}`, which shows the Design
  view and its status line. `ui_mouse {"canvas": "design", "x": …, "y": …}` clicks the view at
  pixels (a left click picks, a right click lists the offer's verbs as `popup_menu`; model points
  are Prepare's), and `ui_key` reaches the tab's shortcuts: Escape closes an open card.
- A change in the Design tab marks the project modified: save it, or pass `"discard_changes": true`
  to `open_files` and `new_project`.

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

## macOS notes

- The app opens on your desktop and takes the focus while it starts. The GUI tools synthesize their
  input inside the app and never move the real pointer.
- `canvas` screenshots are exact. `window`, `dialog` and widget screenshots come back `approximate`:
  they are drawn from the window's content view, without the title bar, and web views may be blank.
- A native dialog (a file or folder picker, a message box) holds the UI thread until a person answers
  it. The call that opened it returns `blocked_by_dialog` with the dialog marked `native: true` in
  `dialogs`, and until the dialog closes every tool that needs the UI thread fails, naming it. Register `dialog_auto_answer` before
  any step that can open one. A folder picker is matched by its prompt, such as
  "Choose Download Directory".
- `Ctrl` and `Cmd` both mean Command, as in the app's shortcuts, and `RawCtrl` is the Control key.
  This holds for `ui_key` chords and for the `modifiers` of `ui_click` and `ui_mouse` (`ctrl`,
  `cmd`, `rawctrl`), so a `ctrl` click multi-selects as Command-click does.
