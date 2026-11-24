# Slicing, layers and the Preview

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_slice_preview`
`test_slice_preview.py` · Python, 832 checks · in `sweep.json` as `{"path": "test_slice_preview.py", "timeout_s": 1500}`

**Tests:** Slicing and what a slice leaves: the plates `plate_slice` and the Slice and Preview and Slice All Plates
actions slice, with no objects, with a plate that fails validation and with two plates; the statistics a slice reports,
for one colour and two; the Preview's tables, legend and layers as `plate_slice_stats`, `view_set`, the Go to layer
action and `app_state` give them, on a plate the Preview shows and on one it has not shown, and on a G-code file opened
for viewing; a legend row clicked, canvas captures taken right after a layer or view type change, and `view_set`'s
refusals of the Preview's fields; height range modifiers, variable layer heights and a plate's layer G-codes, each shown
to reach the G-code and to be kept by a 3MF; every command that takes a plate index, given one the project lacks and one
it has; the display switches, the sidebar, the legend fold and the G-code window of `view_set`, shown on the 3D view and
kept in the app's configuration; and `app_state`'s account, network plug-in and configuration folder. It runs on the
default printer, the Generic Klipper Printer 0.4 nozzle with 0.20mm Standard @MyKlipper and Generic PLA (0.2 mm layers,
a first layer of 0.2 mm, 15 % crosshatch sparse infill, the prime tower on, `travel_speed` 350 mm/s,
`machine_pause_gcode` `PAUSE`, layer heights 0.08 to 0.32 mm, no silent-mode estimate). Nothing it checks needs a Bambu
Lab printer's own behaviour, and its G-code is read as a printer outside the Bambu network writes it. Its sweep entry
sets `timeout_s` 1500: it slices about thirty times, three of them a Benchy.

**Starts from:** `start(client)`, whose `presets()` must be `{printer: "MyKlipper 0.4 nozzle", process: "0.20mm Standard
@MyKlipper", filaments: ["Generic PLA @System"]}`, and `filament_list` read back: slot 1 `#26A69A` and the flush
multiplier 0.3, the baseline's, since the purge between two slots, and so the depth of the two-colour slices' prime
tower, follows both (To settle). The baseline's `auto_slice_after_change` false has a plate sliced only when a row
slices it, and its view (the default layout, the display switches, the projection, the folds) is the one the view
switches scenario starts from. Beyond the baseline the test registers two auto-answer rules, below. Its files go into a
temporary folder of its own, new each run.

**Leaves:** In the app's presets, the Klipper printer with two Generic PLA slots (the layer G-codes scenario's
`slots(2)`, the last change of the slots), slot 1 `#26A69A` and slot 2 `#00C1AE`, the flush multiplier 0.3 (the G-code
file opened for viewing was sliced with it and keeps it), and no user preset: its edits are an object's, a range's or a
plate's, kept in its projects. In the project, the app's account scenario's new, empty one. In the recent projects,
`ranges.3mf`, `profile.3mf` and `codes.3mf` of its temporary folder, which is deleted at its end with the canvas
captures it saves there; the G-code file it opens for viewing does not join them (`Plater` adds no file it opens only
for viewing). The folders the app remembers, `app.last_export_path` and the `recent` section's `last_opened_folder` and
`settings_folder`, name that folder. The display switches, projection, sidebar and legend as the baseline has them,
which the view switches scenario turns back step by step, and the sidebar at the default layout's width; the Preview's
last view type (`preview_last_view_type`) `feature_type`; and its two auto-answer rules, which the next `start()`
clears. The layer slider's one-layer mode, which `start()` does not reset, is off: the legend row scenario turns it
off again in a `finally` once its step 9 has turned it on.

**Reads:**
- `plates()` as in `test_objects`, with each plate's `sliced` from `plate_list` added. `objects()` as in `test_objects`,
  with each object's `filament` and the `offset` of its instance 0 (`object_list`'s `instances[0].offset`) added.
- `shown()` gives `app_state`'s `tab`, `plates.current` and `preview` (`plate`, `layers`, `z`, `layer_count`,
  `view_type`), or `preview` null.
- `gcode(path)` (new) reads an exported G-code file, as a printer outside the Bambu network has it written. Its header,
  between `; HEADER_BLOCK_START` and `; HEADER_BLOCK_END`: `total layer number`, `max_z_height` and `filament` (the
  slots the print uses); and the estimate, in seconds, of the `; estimated printing time (normal mode) = ` line, which
  this G-code writes after its configuration block at the end of the file. Its layers, one per block opened by a whole
  line `;LAYER_CHANGE`, in order, each with `z` (`;Z:`), `height` (the block's first `;HEIGHT:`; a bridge states its own
  thickness in another such line further down the layer), its tool lines (a whole line `T0` or `T1`), the line after
  each `;PAUSE_PRINT` and after each `;CUSTOM_GCODE`, and for each feature type (`;TYPE:Sparse infill`) the XY length
  in mm of its extruding `G1` moves. The lines `;BEFORE_LAYER_CHANGE` and `;AFTER_LAYER_CHANGE` of the Klipper
  printer's layer change G-codes are not layer starts. The `T0` that selects the first filament comes before layer 1
  and is not read; the end G-code, `PRINT_END`, read as part of the last layer, holds no tool line.
- `toolpaths_z(path)` (new) gives the lowest and highest vertex `z` of an OBJ that `project_export {format:
  "toolpaths_obj"}` wrote, and `obj_facts(path)` (in `checks.py`) its first line, its `mtllib` line, its numbers of `v`
  and `f` lines and its highest vertex z.
- `capture()` (new) gives the PNG bytes and the width of `window_screenshot {target: "canvas", include_image: false}`
  once two captures in a row, taken back to back with `until()`, are byte-identical; 30 s without that fail the check,
  and so does a capture whose reply has `approximate` or a `note`, such as a 3D view that came back blank.
- `slice_notice_gone()` (new) waits until `widget_list {query: "Slice complete", include_menus: false}` lists no
  node, asking every 0.25 s: the notification a slice leaves has gone. After 15 s it goes on without failing, since
  the notification counts down only while the main window is active, and one that stays draws the same in every
  capture.
- `conf(key)` (new) gives the `app` section's value of `key` in `<datadir>/OrcaSlicer.conf`, the datadir the session
  file names (the job's data folder), read with `until()` every 0.1 s until it holds the expected value, since the app
  writes the file at an idle moment; after 30 s it gives the value read last, which the row's check then fails on.
- `snapshots()` gives the undo snapshots taken since its last call, from `tests/fixtures/ops_undo.py`.
- `slots(n)` deletes filament slots with `filament_delete`, from the last, until one is left, then adds slots with
  `add_slot()` until the project has `n`, slot k coloured `SLOT_COLOURS[k-1]`: slot 2 `#00C1AE`, the colour a new app
  gives its first added slot. It checks that `filament_list` lists `n` slots in exactly those colours, slot 1 the
  baseline's `#26A69A`. It makes no mixed slot, and `start()` leaves none: it deletes the mixed slots an earlier test
  left, such as the two broken ones `test_filaments` leaves.

At its start the test registers two auto-answer rules, `Spiral mode only works` → `yes` and `unsaved changes. Would you
like to save` → `No`, besides the baseline. Every scenario starts with `project_new {discard_changes:
true}` and `view_set {tab: "prepare"}` and checks that `plates()` is `"" 0 *`, `shown()` has tab `prepare` and
`preview` null, except two that go on from the plates and slice of the scenario before them: a plate the Preview has
not shown, after the slicing by route, and the Preview's layers, after the tables. The scenarios run in the order
below, and naming one of those two alone runs the one before it first.
The app applies a change of the model or of a plate's codes to the slicing state half a second after the call
(`schedule_background_process()`), and only then marks a plate's slice out of date. So a step that moves an object,
changes its ranges or settings, or edits layer G-codes, and whose row then reads `plates()` or refuses an export or
statistics for an out-of-date slice, calls `app_wait {for: "idle"}` first, which waits for that update; the wait must be
met.

In the tables a plate is written as in `test_objects`, with `S` on a plate whose slice is up to date: `"" 1 * S` ·
`"" 1` is a current sliced plate with one object, then an unsliced one. A slice's reply is written by its plates'
index and outcome, `[0 ok, 1 empty]`; `status` is `done` in every reply. Shown layers `1–25 (0.2–5.0)` are
`shown().preview.layers` `[1, 25]` and `z` `[0.2, 5.0]`; a `view_set` reply's `layers` `[first, last, count]` is written
`[1, 25, 100]`. "Layer n" of a G-code is its n-th layer. A cube is a 20 mm cube from `object_add_primitive {shape:
"cube", size: [20, 20, 20]}`, which slices to 100 layers of 0.2 mm, layer n at z 0.2·n; cube "left" is that call
with `name: "left"`, and a cube the rows do not name is named `Cube`. An `ok` plate in a slice's reply is also checked
for its `layers`: 100 on a plate of cubes alone, unless the row gives another count. A refusal is written with its
code, or in quotes with the words it must hold where it has none; after every refusal the row's state is read again
whole.

**Scenario: nothing to slice, and a slice that fails**

A Preview that holds no toolpaths of the current plate has no layers to show, whatever an earlier slice left in
its layer slider (the operations layer's fix under To settle).

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 1 | `view_set {layer_range: [2, 5]}` | `"" 0 *` | refused, `no_layers`; tab `preview`, `preview` null |
| 2 | `view_set {layer_z: 5.0}`, then `view_set {layer_percent: 50}` | `"" 0 *` | each refused, `no_layers`, in the words of step 1; tab `preview` |
| 3 | `view_set {tab: "prepare"}`, then the Slice and Preview action (`action_run`) | `"" 0 *` | `[0 empty]`, `ran` `orca_command:slice_and_preview:orca`; tab `preview` |
| 4 | the Slice All Plates action | `"" 0 *` | `[0 empty]` |
| 5 | `plate_slice`, then `plate_slice {plates: "all"}` | `"" 0 *` | `[0 empty]` each |
| 6 | `project_export {format: "gcode", path}` | `"" 0 *` | refused, `no_objects`; no file at the path |
| 7 | two cubes at `position` [90, 90, 0] and [150, 90, 0], then `plate_set_settings {plate: 0, values: {spiral_mode: true}}`, which the spiral rule answers | `"" 2 *` | (set up for the next step, whose error shows the setting took; `test_objects` checks the setting and its question) |
| 8 | `plate_slice` | `"" 2 *` | `[0 error]`, its `error` holding `Please select "By object" print sequence to print multiple objects in spiral vase mode.`; `app_state.slicing` false |
| 9 | the Slice and Preview action, then the Slice All Plates action | `"" 2 *` | `[0 error]` each, with that text |
| 10 | `project_export {format: "gcode", path}`, then `{format: "sliced_3mf", path}` to another path | `"" 2 *` | refused, `slice_has_errors`, then `sliced_file_not_written` (`Plater::export_gcode_3mf()` writes nothing for a plate whose slice failed); no file at either path |
| 11 | `view_set {layer: 10}`, `{layer_range: [2, 5]}`, `{layer_z: 5.0}`, and the Go to layer action with `{percent: 50}` | `"" 2 *` | each refused, `no_layers`; tab `preview`, `preview` null |

**Scenario: slicing by plate and by route**

`snapshots()` is read just before steps 1, 3 and 7 and the export of step 4 to set its mark, and after them; the rows
give what it lists. A move takes the snapshot `Transform object`. Selecting a plate takes `select partplate!` snapshots,
which these rows do not count (a slice of another plate, Slice All and `plate_select` each select plates).

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 0 | cube "a", `plate_add` (which makes plate 1 current), cube "b" with `position` [x, y, 0] at plate 1's middle, its `origin` plus half its `size` from `plate_list`, then `plate_select {plate: 0}` | `"" 1 *` · `"" 1` | `objects()`: a on plate 0, b on plate 1 and none outside; b's `offset` x and y are that middle, to 0.001: an object placed by numbers lands where they say |
| 1 | `plate_slice` | `"" 1 * S` · `"" 1` | `[0 ok]` with `layers` 100; tab `preview`; snapshots: none, so a slice of the current plate takes none |
| 2 | `view_set {tab: "prepare"}`, `plate_slice {plates: 1}` | `"" 1 S` · `"" 1 * S` | `[1 ok]` with `layers` 100: the plate named is made current |
| 3 | `object_transform {object: 0, translate: [5, 0, 0]}`, `app_wait {for: "idle"}` | `"" 1` · `"" 1 * S` | a move makes its own plate's slice out of date, and no other's; snapshots: `Transform object` alone |
| 4 | `plate_select {plate: 0}`, then `project_export {format: "gcode", path}` | `"" 1 *` · `"" 1 S` | refused, `plate_not_sliced`; no file; snapshots: none, so a refused export takes none |
| 5 | `plate_slice_stats` | as after 4 | refused, "Plate 0 has no up-to-date slice" |
| 6 | `plate_slice {plates: "all"}` | `"" 1 S` · `"" 1 * S` | `[0 ok, 1 ok]`, each with `layers` 100; Slice All selects each plate in turn and leaves the last one current |
| 7 | `view_set {tab: "prepare"}`, `object_transform` of both cubes, `translate: [5, 0, 0]`, `app_wait {for: "idle"}`, then the Slice and Preview action | `"" 1` · `"" 1 * S` | `[1 ok]` with `layers` 100: the current plate alone; snapshots: `Transform object` twice and nothing else |
| 8 | the Slice All Plates action | `"" 1 S` · `"" 1 * S` | `[0 ok, 1 ok]`, each with `layers` 100 |
| 9 | `project_export {format: "gcode", path}` | as after 8 | written; `gcode()` gives `total layer number` 100 and 100 layers |

**Scenario: a plate the Preview has not shown, and a G-code file opened for viewing**

It goes on from the end of the slicing by route, without a new project: cubes a and b on plates 0 and 1, both sliced by
step 8's Slice All Plates, `"" 1 S` · `"" 1 * S`.

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 1 | `view_set {tab: "prepare"}`, `plate_select {plate: 0}`, `view_set {tab: "preview"}`, `app_wait {for: "idle"}` | `"" 1 * S` · `"" 1 S` | the wait is met; `shown()`: `preview` `plate` 0, `layer_count` 100 |
| 2 | `view_set {tab: "prepare"}`, `plate_select {plate: 1}` | `"" 1 S` · `"" 1 * S` | `shown()` `preview` null: the Preview still holds plate 0's toolpaths, which no read gives as plate 1's |
| 3 | `plate_slice_stats` | as after 2 | `plate` 1, `summary.layers` 100, `view` null, `note` holding "has not loaded this plate's toolpaths" |
| 4 | `plate_slice_stats {plate: 0}` | as after 2 | `plate` 0, `summary.layers` 100, `view` null, `note` holding "show plate 0 there" |
| 5 | `view_set {tab: "preview"}`, `app_wait {for: "idle"}` | as after 2 | `shown()` `preview` `plate` 1; `plate_slice_stats {include: ["view"]}` gives `view.layer_count` 100 and no `note` |
| 6 | `project_export {format: "gcode", path}` | as after 2 | `gcode()`: `total layer number` 100, the estimate within 1 s of plate 1's `time_s` in the reply of the routes' step 8 |
| 7 | `project_open {path, discard_changes: true}` of that file, `app_wait {for: "idle"}` | `"" 0 *`, not sliced | the save rule's `uses` rises by 1; `shown()` `preview.layer_count` 100; `plate_slice_stats {include: ["features", "layers", "view"]}`: `summary.layers` 100, `summary.time_s` within 1 s of the file's `total estimated time`, `features.Outer wall.time_s` > 0, `layers.total` 100, `view.layer_count` 100. The plate shows the file's toolpaths without being sliced |
| 8 | `project_new {discard_changes: true}` | `"" 0 *` | `shown()` `preview` null |

**Scenario: slice statistics, one colour and two**

`slots(2)`, then cube "left" at [90, 90, 0]: `"" 1 *`, left on filament 1.

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 1 | `plate_slice` | `"" 1 * S` | `[0 ok]`: `layers` 100; `max_z` 20.0 to 0.001; `filament_changes` 0; no `extruder_changes`; `filaments` exactly two rows, slots 1 and 2; slot 1 `model_g` > 0, `support_g`, `flush_g` and `tower_g` 0, `g` = `model_g`; slot 2 `m`, `g`, `model_g`, `support_g`, `flush_g` and `tower_g` all 0; `filament_g` = slot 1's `g` |
| 2 | `project_export {format: "gcode", path}` | as after 1 | `gcode()`: header `total layer number` 100, `max_z_height` 20.00, `filament` `1`, the estimate within 1 s of the reply's `time_s`; 100 layers, layer n at z 0.2·n with height 0.2; no tool line in any layer |
| 3 | cube "right" at [150, 90, 0], which is selected, then `keyboard_press {target: {name: "OrcaSceneCanvas"}, keys: "2"}` | `"" 2 *` | reply `to` `wxGLCanvas`; `objects()`: left on filament 1, right on filament 2 |
| 4 | `object_select {none: true}`, `plate_slice` | `"" 2 * S` | `[0 ok]`: `layers` 100; `filament_changes` 100; both slots' `model_g` and `tower_g` > 0, `flush_g` and `support_g` 0: the purge goes into the prime tower; `filament_g` within 0.02 of the slots' `g` added up |
| 5 | `project_export {format: "gcode", path}` | as after 4 | `gcode()`: header `filament` slots 1 and 2, in either order (the Klipper printer's G-code writes `2,1`), `total layer number` 100, the estimate within 1 s of step 4's `time_s`; each of the 100 layers holds exactly one tool line, and no two layers in a row hold the same one |
| 6 | `plate_slice_stats {include: ["objects"]}` | as after 4 | two rows: `left` with `object` 0 and `right` with `object` 1, each `instance` 0 and `g` > 0 |

**Scenario: the Preview's tables and legend**

`slots(1)`, cube "cube", `plate_slice`: `[0 ok]`, `"" 1 * S`. Then `view_set {view_type: "feature_type", legend:
{travel: false}, layer_range: [1, 100]}`, which replies `[1, 100, 100]`.

| # | Step | Also checked |
|---|---|---|
| 1 | `plate_slice_stats {include: ["features", "layers", "objects", "view"], limit: 5}` | `plate` 0; `summary.layers` 100, `filament_g` > 0, `travel_mm` > 0, `travel_time_s` > 0; `features`: `Outer wall` with `time_s` > 0, `percent` in (0, 100) and `g` > 0, `Travel` with `mm` > 0 and `time_s` within 0.5 of `summary.travel_time_s` (one travel time, rounded to 0.01 s and to the second), and every row's `percent` adding up to at most 101, each being a share of one print time rounded to 0.1; `layers`: `total` 100, `next_offset` 5, five items, item i with `layer` i+1, `z` 0.2·(i+1), `height` 0.2, `time_s` > 0 and `g` > 0; `objects`: one row, `name` `cube`, `object` 0, `instance` 0, `g` > 0; `view`: `view_type` `feature_type`, `time_mode` `normal`, `layers` [1, 100], `layer_count` 100, `legend.features` `Outer wall` and `Sparse infill` true, `legend.options.travel` false, `ranges.height` from at most 0.2 to at least 0.2, the top of `ranges.volumetric_flow_rate` and `ranges.layer_time_linear` > 0, the top of `ranges.speed` below 350, the travel speed |
| 2 | `plate_slice_stats {include: ["layer_features"], offset: 99}` | one item: `layer` 100, `z` 20.0, `height` 0.2, `features.Top surface` > 0; no `next_offset`, no `features`, no `view` |
| 3 | `view_set {view_type: "feature_type", legend: {"Sparse infill": false, travel: true}, layer_z: 5.0}` | reply `legend` `{"Sparse infill": false, "travel": true}`, `layers` `[1, 25, 100]`, `z` [0.2, 5.0] |
| 4 | `plate_slice_stats {include: ["view"]}` | `legend.features` `Sparse infill` false and `Outer wall` true, `legend.options.travel` true, `layers` [1, 25]; the top of `ranges.speed` is 350, the travel speed: a travel shown counts in the ranges |
| 5 | `view_set {view_type: "feature_type", legend: {travel: false}}`, then `plate_slice_stats {include: ["view"]}` | `Sparse infill` true (a Line Type pick shows every feature type), travel false, the top of `ranges.speed` below 350 again |
| 6 | `view_set {layer: 5}` | `shown()`: tab `preview`, `preview` `plate` 0, `1–5 (0.2–1.0)`, `layer_count` 100, `view_type` `feature_type` |
| 7 | `view_set {view_type: "fan_speed"}` | `shown()` `view_type` `fan_speed`, `1–5`; `plate_slice_stats {include: ["view"]}` gives `view_type` `fan_speed`, `layers` [1, 5] and no `features` |

**Scenario: the Preview's layers**

It goes on from the tables' cube and its slice, without a new project: `"" 1 * S`. Then `view_set {layer_range: [1,
100]}`, which replies `[1, 100, 100]`: shown `1–100 (0.2–20.0)`, where the tables left `1–5`. After each step `shown()`
gives tab `preview` and the layers the row names; a reply of the Go to layer action is `view_set`'s, with `ran`.

| # | Step | Reply | Shown after |
|---|---|---|---|
| 1 | the Go to layer action, `{percent: 0}` | `[1, 1, 100]`, `z` [0.2, 0.2] | `1–1 (0.2–0.2)` |
| 2 | the same, `{percent: 50}` | `[1, 51, 100]` | `1–51 (0.2–10.2)`: the top is the layer `round(percent · 99 / 100) + 1` |
| 3 | the same, `{percent: 100}` | `[1, 100, 100]` | `1–100 (0.2–20.0)` |
| 4 | the same, `{percent: 25.5}` | `[1, 26, 100]` | `1–26 (0.2–5.2)` |
| 5 | the same, `{percent: 150}` | refused, "layer_percent must be <= 100" | as after 4 |
| 6 | the same, `{percent: -5}` | refused, "layer_percent must be >= 0" | as after 4 |
| 7 | the same, `{percent: "abc"}` | refused, "layer_percent must be of type number" | as after 4 |
| 8 | the same, with no `args` | refused, "takes exactly its percent" | as after 4 |
| 9 | `view_set {layer_range: [20, 60]}` | `[20, 60, 100]` | `20–60 (4.0–12.0)` |
| 10 | the Go to layer action, `{percent: 80}` | `[20, 80, 100]` | `20–80 (4.0–16.0)`: the bottom of the range stays |
| 11 | the same, `{percent: 10}` | `[11, 11, 100]` | `11–11 (2.2–2.2)`: a top below the bottom takes the bottom with it |
| 12 | `view_set {layer: 1}`, then `view_set {layer: 50}` | `[1, 50, 100]` | `1–50 (0.2–10.0)`: `layer` shows from layer 1 |
| 13 | `view_set {layer: 9999}` | `[1, 100, 100]` | `1–100 (0.2–20.0)` |
| 14 | `view_set {layer_range: [5, 20]}` | `[5, 20, 100]` | `5–20 (1.0–4.0)` |
| 15 | `view_set {layer_range: [5]}` | refused, "layer_range must be [first, last]" | as after 14 |
| 16 | `view_set {layer_z: 5.0}` | `[1, 25, 100]`, `z` [0.2, 5.0] | `1–25 (0.2–5.0)` |
| 17 | `view_set {layer_range: [20, 25]}`, then `project_export {format: "toolpaths_obj", path}` | `[20, 25, 100]` | `toolpaths_z()` within [3.6, 5.2], its highest at least 4.8: the toolpaths written are the layers shown |
| 18 | `view_set {layer_range: [1, 100]}`, then the same export to another path | `[1, 100, 100]` | reply `bytes` the file's size; the `.mtl` of the file's name beside it; `obj_facts()`: first line `# G-Code Toolpaths`, `mtllib ./<name>.mtl`, more than 0 `v` and `f` lines, highest vertex z 20.1 ± 0.01, the top layer at 20 plus half its 0.2 mm height; `toolpaths_z()` from at most 0.2 |
| 19 | `view_set {tab: "prepare"}`, then `view_set {layer: 40}` | `[1, 40, 100]` | `1–40 (0.2–8.0)`; tab `preview` |
| 20 | `view_set {tab: "prepare"}`, then the Go to layer action, `{percent: 30}` | `[1, 31, 100]` | `1–31 (0.2–6.2)`; tab `preview` |
| 21 | `project_new {discard_changes: true}`, then `view_set {layer: 10}` and the Go to layer action, `{percent: 50}` | each refused, `no_layers` | `plates()` `"" 0 *`, read after `project_new` and again after the refusals; tab `preview`, `preview` null: the slider's layers of the cube's slice no longer count once the Preview holds no toolpaths of the current plate |

**Scenario: a legend row clicked, captures right after a change, and layers asked between tops or backwards**

`slots(1)`. The configuration's `preview_last_view_type`, which `conf()` reads, holds whatever view type an earlier
scenario or test picked last, so after the slice the scenario picks Line Type and checks that the configuration reads
it before the picks it checks. `legend()` is `plate_slice_stats {include: ["view"]}`'s `view.legend.features`. After
each step `shown()` gives tab `preview` and the layers the row names, written as in the layers. The legend's reply to
`view_set {legend}` and its read-back are the tables' steps 3 and 4.

| # | Step | Shown after | Also checked |
|---|---|---|---|
| 1 | `view_set {legend: {travel: false}}` | `preview` null | refused, "slice first"; `plates()` `"" 0 *` |
| 2 | a cube, `plate_slice`, then `view_set {view_type: "feature_type", layer_range: [1, 100]}` | `1–100 (0.2–20.0)` | `[0 ok]`, its plate without `silent_time_s`: the Klipper printer has no silent mode; reply `[1, 100, 100]`; `conf("preview_last_view_type")` `feature_type` |
| 3 | `widget_list {query: "Sparse infill"}`, which draws the legend, then `widget_click {target: {imgui_name: "Sparse infill"}}` | as after 2 | `legend()` `Sparse infill` false: a click on its row hides the feature type |
| 4 | `view_set {view_type: "speed"}` | `1–100 (0.2–20.0)`, `view_type` `speed` | `conf("preview_last_view_type")` `speed`; `legend()` `Sparse infill` still false |
| 5 | `view_set {view_type: "feature_type"}` | as after 2 | `legend()` `Sparse infill` true: picking Line Type shows every feature type; `conf("preview_last_view_type")` `feature_type` |
| 6 | `slice_notice_gone()`, `mouse_gesture {action: "move", x: 430, y: 80}`, then one `replay_run` of eight changes, each a `view_set` followed by two canvas captures (`window_screenshot {target: "canvas", save_path}`): `layer_range` [1, 40], `view_type` `speed`, `layer_range` [64, 64], `view_type` `height`, `layer_range` [20, 100], `view_type` `fan_speed`, `layer_range` [1, 100], `view_type` `feature_type` | `1–100 (0.2–20.0)`, `view_type` `feature_type` | the replay passes; the two captures after each change are byte-identical, and from the second change on, the first capture after a change differs from the last one before it: each change shows, settled, in the capture taken right after it |
| 7 | `view_set {layer_z: 1.05}` | `1–6 (0.2–1.2)` | reply `[1, 6, 100]`, `z` [0.2, 1.2]: a height between two layer tops shows up to the upper one |
| 8 | `view_set {layer_range: [60, 20]}` | `60–60 (12.0–12.0)` | reply `[60, 60, 100]`, `z` [12.0, 12.0]: a range given backwards shows its first layer alone |
| 9 | `keyboard_press {keys: "L"}`, which turns the layer slider's one-layer mode on, then `view_set {layer_range: [2, 5]}` | `2–2 (0.4–0.4)` | reply `[2, 5, 100]`, `z` [0.4, 0.4]: the reply gives the range asked for, and the slider shows its first layer alone |
| 10 | `keyboard_press {keys: "L"}` | `1–100 (0.2–20.0)`, `view_type` `feature_type` | leaving one-layer mode shows every layer |
| 11 | `view_set` with `{layer_z: 80}`, `{layer: 3, layer_z: 1.0}`, `{view_type: "speed", legend: {"Infill": false}}`, `{legend: {travel: "yes"}}` and `{time_mode: "silent"}` | as after 10, read after each | refused in turn with "above the top layer (20.0 mm)", "not both", the print's rows (the text names `Sparse infill` and `travel`), "true (show) or false (hide)" and "no separate silent-mode estimate"; `legend()` `Sparse infill` true after each |
| 12 | `view_set {time_mode: "normal"}` | as after 10 | reply `time_mode` `normal`: normal mode is always offered |

**Scenario: height range modifiers**

`slots(1)` and a cube. The control is step 18, the cube sliced once its ranges are gone: its `gcode()` layer 30's
`Sparse infill` length, L, greater than 0, which step 8's comparison is made with. `object_list` shows the cube with no
`layer_config_ranges`. Ranges are written `[min_z, max_z] {values}`, with the values a range shows in `object_list`;
after every step `object_list`'s `layer_config_ranges` is compared with the row whole.

| # | Step | Ranges after | Also checked |
|---|---|---|---|
| 1 | `object_set_height_ranges {object: 0, ranges: [{min_z: 10, max_z: 20}, {min_z: 0, max_z: 10}]}` | `[0, 10] {layer_height "0.2", extruder "0"}` · `[10, 20] {layer_height "0.2", extruder "0"}` | reply `height` 20 to 0.01, its ranges the same, sorted, and no `warnings` |
| 2 | `settings_set {target: "height_range", object: 0, min_z: 0, max_z: 10, values: {sparse_infill_density: "40%"}}` | `[0, 10]` adds `sparse_infill_density "40%"` | reply `applied.sparse_infill_density` `"40%"`, `height_range` `{min_z: 0, max_z: 10}`; `settings_get {target: "object", object: 0, keys: ["sparse_infill_density"]}` value null: a range's setting is not the object's |
| 3 | the same for `[10, 20]`, `values: {layer_height: 0.1}` | `[0, 10] {layer_height "0.2", extruder "0", sparse_infill_density "40%"}` · `[10, 20] {layer_height "0.1", extruder "0"}` | reply `applied.layer_height` `"0.1"`, `height_range` `{min_z: 10, max_z: 20}` |
| 4 | `undo` | `[10, 20]` back to `layer_height "0.2"`, `[0, 10]` as after 3 | one step each |
| 5 | `redo`, then the ranges of step 1 again | as after 3 | the ranges a call names again keep their settings |
| 6 | `undo` | as after 4 | the same ranges took no undo step, so undo takes back step 3's |
| 7 | `redo`, `plate_slice` | as after 3 | `[0 ok]`, `layers` 150 ± 1 |
| 8 | `project_export {format: "gcode", path}` | as after 3 | `gcode()`: every layer up to z 10.0 has height 0.2, every layer from z 10.2 has height 0.1 (to 0.001), and as many layers as the slice reported; layer 30 (z 6.0) has at least 1.8 L of `Sparse infill`: 40 % where the object has 15 % |
| 9 | `object_set_height_ranges {object: 0, ranges: [{min_z: 0, max_z: 15}, {min_z: 12, max_z: 20}]}` | as after 3 | refused, "overlap" |
| 10 | `settings_set` on `[0, 10]`, `values: {layer_height: 0.5}` | as after 3 | refused, "layer_height 0.5 is outside this printer's 0.08-0.32" |
| 11 | the same, `values: {extruder: 9}` | as after 3 | refused, "Filament 9 does not exist" |
| 12 | the same, `values: {enable_support: true}` | as after 3 | refused, "not on a height range" |
| 13 | `object_set_height_ranges {object: 0, ranges: [{min_z: 5, max_z: 5}]}` | as after 3 | refused, "A height range needs 0 <= min_z < max_z" |
| 14 | `slots(2)`, then `settings_set` on `[10, 20]`, `values: {extruder: 2}`, then `project_set_settings {values: {wipe_tower_y: {index: 0, value: 150}}}` | `[10, 20]` holds `extruder "2"` | reply `applied.extruder` `"2"`, `height_range` `{min_z: 10, max_z: 20}`; `plate_list`'s `wipe_tower` for plate 0 `[165, 150]`: the prime tower moved forward, where its depth for the 0.1 mm layers fits the bed (To settle) |
| 15 | `plate_slice`, `project_export {format: "gcode", path}` | as after 14 | `[0 ok]` with `layers` 150 ± 1, `filament_changes` 1 and slot 2's `model_g` > 0; `gcode()`: one tool line in all the layers, `T1`, in the first layer above 10 mm (z 10.1) |
| 16 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 14 | the ranges and their settings are kept in the project file |
| 17 | `object_set_height_ranges {object: 0, ranges: []}` | none | reply `layer_config_ranges` [] |
| 18 | `plate_slice`, `project_export {format: "gcode", path}` | none | `[0 ok]`, `layers` 100; `gcode()`: 100 layers of height 0.2, no tool line, layer 30's `Sparse infill` L greater than 0 |

**Scenario: variable layer height**

`slots(1)`, `object_add_handy_model {model: "3dbenchy"}` (48 mm high, 240 layers at 0.2 mm), `plate_slice`: `[0 ok]`
with `layers` 240. The Preview is shown. A profile is written `{points, min_height, max_height}` as `object_list`'s
`layer_height_profile` gives it, and `none` when the object has none; after every step `object_list` is compared with
the row.

| # | Step | Profile after | Also checked |
|---|---|---|---|
| 1 | `object_set_variable_layer_height {object: 0, action: "adaptive"}` | points P > 250, `min_height` < 0.2 and at least 0.08, `max_height` > 0.2 and at most 0.32 | reply `height` 48 to 0.01, no `warnings`; tab `prepare` |
| 2 | the same with `quality: 0` | more points than P | |
| 3 | the same with `quality: 1` | fewer points than P, `max_height` 0.32 | |
| 4 | `plate_slice`, `project_export {format: "gcode", path}` | as after 3 | `[0 ok]` with a `layers` count other than 240; `gcode()`: as many layers as the slice reported, every height from 0.08 to 0.32 (to 0.001), more than one height, the last layer at z 48 ± 0.3: the slicer prints the profile, not 240 layers of 0.2 mm |
| 5 | `... {action: "smooth", radius: 10, keep_min: true}` | `max_height` < 0.32 | |
| 6 | `undo` | `max_height` 0.32 | smooth is one undo step |
| 7 | `redo`, then `... {action: "reset"}` | none | reply has no `layer_height_profile` |
| 8 | `... {action: "reset"}` again, then `undo` | `max_height` < 0.32 | the second reset changed nothing and took no step, so undo brings back step 5's profile |
| 9 | `... {action: "reset"}` | none | |
| 10 | `... {action: "smooth", quality: 0.3}` | none | refused, "quality goes with action adaptive" |
| 11 | `view_set {canvas: "assemble"}`, `... {action: "adaptive"}`, `view_set {canvas: "3d"}` | none | the adaptive call refused, "Leave the assemble view first" |
| 12 | `object_set_height_ranges {object: 0, ranges: [{min_z: 0, max_z: 20}, {min_z: 20, max_z: 48}]}`, `layer_height` 0.12 on the first and 0.28 on the second | none | `object_list` ranges `[0, 20] {layer_height "0.12", extruder "0"}` and `[20, 48] {layer_height "0.28", extruder "0"}` |
| 13 | `... {action: "smooth"}` | `min_height` 0.12, `max_height` 0.28 | reply `warnings` holding "decides its layer heights": smooth starts from the ranges' heights |
| 14 | `... {action: "adaptive", quality: 0}` | `min_height` 0.12 | the ranges stay: `[20, 48]` still `layer_height "0.28"` |
| 15 | `project_save` to a new file, `project_new`, `project_open` of the file | `min_height` 0.12, more than 100 points | the profile is kept in the project file |
| 16 | `plate_slice`, `project_export {format: "gcode", path}` | as after 15 | `[0 ok]` with at least 172 `layers` (48 mm at 0.28 mm at most); `gcode()`: as many layers as the slice reported, the lowest height 0.12 and the highest at most 0.28 (to 0.001) |

**Scenario: layer G-codes**

`slots(2)` and a cube. Codes are written `layer:type`, with `@z`, the custom G-code in quotes and the filament after
`F`: `26:pause@5.2` · `51:custom@10.2 "M104 S215"`. After every step `plate_list`'s `layer_gcode` for plate 0 is
compared with the row whole, and the reply's `layer_gcode` too where the call went through. `plate_list` gives a
code's `layer` only while the plate's slice is up to date, which after a code is added it no longer is, so its codes
are compared without `layer` except after step 14; the reply's codes always carry it.

| # | Step | Codes after | Also checked |
|---|---|---|---|
| 1 | `plate_set_custom_gcode {add: [{layer: 26, type: "pause"}]}` | none | refused, "has no up-to-date slice to place codes on"; tab `prepare`, `app_state.slicing` false: a refused call starts no slice |
| 2 | `plate_slice`, `view_set {tab: "prepare"}` | none | `[0 ok]` with `layers` 100 |
| 3 | `plate_set_custom_gcode {add: [{layer: 26, type: "pause"}, {z: 10.1, type: "custom", gcode: "M104 S215"}]}`, `app_wait {for: "idle"}` | `26:pause@5.2` · `51:custom@10.2 "M104 S215"` | reply `plate` 0, `layers` 100, `changed` true, `note` holding "slice it again"; tab `preview`; `app_state.project.dirty` true; `plates()` `"" 1 *`, not sliced: a code makes the slice out of date. A height takes the layer whose top is at or above it |
| 4 | `... {add: [{z: 5.2, type: "pause"}]}` | as after 3 | `changed` false: the plate stays editable while the Preview shows it |
| 5 | `... {remove: [{z: 5.2}], add: [{layer: 51, type: "custom", gcode: "M104 S220"}]}` | `51:custom@10.2 "M104 S220"` | `changed` true |
| 6 | `... {add: [{layer: 101, type: "pause"}]}` | as after 5 | refused, "layer must be 1..100" |
| 7 | `... {add: [{layer: 51, type: "pause"}]}` | as after 5 | refused, "layer 51 has a custom code; remove it first" |
| 8 | `... {remove: [{layer: 30}]}` | as after 5 | refused, "No code on layer 30" |
| 9 | `... {add: [{layer: 30, type: "change_filament", filament: 9}]}` | as after 5 | refused, "filament must be 1..2" |
| 10 | `... {plate: 1, add: [{layer: 30, type: "pause"}]}` | as after 5 | refused, "Plate 1 does not exist (the project has plate 0 only)" |
| 11 | `view_set {tab: "prepare"}`, `... {add: [{layer: 30, type: "pause"}]}` | as after 5 | refused, "has no up-to-date slice to place codes on"; tab `prepare`, `app_state.slicing` false |
| 12 | `plate_slice` (`[0 ok]` with `layers` 100), then `... {add: [{layer: 26, type: "pause"}, {layer: 76, type: "change_filament", filament: 2}]}` | `26:pause@5.2` · `51:custom@10.2 "M104 S220"` · `76:change_filament@15.2 F2` | the filament change's `color` is slot 2's, the second entry of `filament_colour` from `project_get_settings` |
| 13 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 12 | the codes are kept in the project file; the plate is not sliced |
| 14 | `plate_slice`, `project_export {format: "gcode", path}` | as after 12, `plate_list` now giving `layer` 26, 51 and 76 | `[0 ok]` with `layers` 100; `gcode()`: layer 26 alone holds a `;PAUSE_PRINT` line, followed by `PAUSE`, the printer's `machine_pause_gcode`; layer 51 alone a `;CUSTOM_GCODE` line, followed by `M104 S220`; layer 76 holds the only tool line, `T1` |
| 15 | `... {clear: true}` | none | `changed` true |
| 16 | `plate_slice`, `project_export {format: "gcode", path}` | none | `[0 ok]` with `layers` 100; `gcode()`: no `;PAUSE_PRINT`, no `;CUSTOM_GCODE` and no tool line in any layer |

**Scenario: a plate index the project lacks, and one it has**

A cube on plate 0, then `plate_add` twice and `plate_select {plate: 1}`: `"" 1` · `"" 0 *` · `"" 0`. `snapshots()` is
read and lists the setup's snapshots, so the reader sees them. The cube's `offset` is read once from `object_list` as
O. Each refusal of steps 1 to 9 leaves `plates()` as at the start, the cube on plate 0 at O, tab `prepare` and no file
at the export's path. Those of plate 5 and -1 say "Plate 5 does not exist (the project has plates 0..2)", or
"Plate -1 ..." for -1.

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 1 | `plate_slice {plates: 5}`, then `{plates: -1}` | `"" 1` · `"" 0 *` · `"" 0` | each refused, `no_such_plate` |
| 2 | `project_export {format: "stl", path, plates: 5}`, then `plates: -1`, then `{format: "gcode", path, plates: 5}` to a G-code path | as at the start | each refused, `no_such_plate`: the STL export's through `select_plate()`, the G-code export's by `Ops::start_gcode_export()` itself, before the checks it makes on the plate named; no file at the G-code path either |
| 3 | `object_arrange {plates: 5}`, then `{plates: -1}` | as at the start | each refused, by its words |
| 4 | `plate_slice {plates: "abc"}`, `project_export {format: "stl", path, plates: "abc"}` and `object_arrange {plates: "abc"}` | as at the start | each refused, in the words `plates is "all", "current" or a plate index`: a string other than those two names no plate (the fix under To settle) |
| 5 | `plate_get_settings {plate: 5, keys: ["curr_bed_type"]}` | as at the start | refused, by its words |
| 6 | `object_list {plate: 5}` | as at the start | refused, by its words |
| 7 | `object_transform {object: 0, plate: 5}` | as at the start | refused, by its words and "action_run plate_add makes one" |
| 8 | `plate_slice_stats {plate: 5}` | as at the start | refused, by its words |
| 9 | `plate_set_custom_gcode {plate: 5, add: [{layer: 1, type: "pause"}]}` | as at the start | refused, by its words |
| 10 | `snapshots()` | as at the start | none: no refused call took an undo snapshot |
| 11 | `project_export {format: "stl", path, plates: 2}` | `"" 1` · `"" 0` · `"" 0 *` | written, `bytes` > 0: the plate named is made current |
| 12 | `plate_get_settings {plate: 0}` | as after 11 | reply `plate` 0: reading a plate leaves the current one |
| 13 | `object_list {plate: 0}` | as after 11 | `plates` lists plate 0 alone, holding the cube, and there is no `outside` |
| 14 | `object_arrange {plates: 0}` | `"" 1 *` · `"" 0` · `"" 0` | the plate arranged is made current; the cube inside plate 0's rectangle (`origin` and `size` from `plate_list`) |
| 15 | `plate_select {plate: 2}`, `plate_slice {plates: 0}` | `"" 1 * S` · `"" 0` · `"" 0` | `[0 ok]` with `layers` 100; tab `preview` |
| 16 | `object_arrange {plates: 2}` | as after 15 | refused, "Plate 2 has no objects to arrange"; tab `prepare`, which the call shows before it checks the plate |
| 17 | `object_transform {object: 0, plate: 2}`, `app_wait {for: "idle"}` | `"" 0 *` · `"" 0` · `"" 1` | the cube on plate 2, its `offset` its offset after 14 plus plate 2's `origin` less plate 0's (to 0.001); plate 0 stays current |

**Scenario: view switches**

A 30 mm sphere "ball" (`object_add_primitive {shape: "sphere", size: [30]}`), selected, and `view_set {camera:
"iso", orbit: [0, -25]}`, the iso direction lowered to 20° above the bed (To settle). `app_state.view` is written as
the switches that are on; at the start, those the baseline sets, the app's defaults: `gridlines`, `axes`, `navigator`
and `gcode_window`, with `projection` `perspective`, `section.on` false, `sidebar_collapsed` and `legend_collapsed`
false. C0 is `capture()` at the start. After every step `app_state.view` is compared with the row whole, and the reply
gives the switch's value as the row does.

| # | Step | View after | Also checked |
|---|---|---|---|
| 1 | `view_set {labels: true}` | start + `labels` | `capture()` differs from C0; `conf("show_labels")` `true` |
| 2 | `view_set {labels: false}` | start | `conf("show_labels")` `false` |
| 3 | the same, on then off, for `overhangs` (`show_overhang`), `outline` (`show_outline`) and `wireframe` (not kept) | as 1 and 2 for each | |
| 4 | `view_set {gridlines: false}`, then `{gridlines: true}` | start − `gridlines`, then start | as 1 and 2, the other way round: `conf("show_plate_gridlines")` `false`, then `true` |
| 5 | the same for `axes` (`show_axes`) and `navigator` (`show_3d_navigator`) | as 4 for each | |
| 6 | `view_set {gridlines: false, axes: false, navigator: false}` | start − all three | the reply gives all three false |
| 7 | `view_set {gridlines: true, axes: true, navigator: true}` | start | `capture()` is C0, byte for byte: each switch of steps 1 to 6, turned back, draws the view as before |
| 8 | `view_set {projection: "orthographic"}` | start, `projection` `orthographic` | `app_state.camera.type` `ortho`; `capture()` differs from C0 |
| 9 | `view_set {projection: "perspective"}` | start | `app_state.camera.type` `perspective` |
| 10 | `view_set {section: {on: true}}` | start, `section.on` true | tab `prepare`; `capture()` differs from C0 |
| 11 | `view_set {section: {on: false}}` | start | `capture()` is C0 |
| 12 | `view_set {sidebar_collapsed: true}` | start, `sidebar_collapsed` true | `capture()` wider than C0 |
| 13 | `view_set {sidebar_collapsed: false}` | start | `capture()` as wide as C0 |
| 14 | `view_set {sidebar_width: W + 150}`, W being `app_state.view.sidebar_width` at the start (the default layout's, which `start()` sets), at least 390 | start, `sidebar_width` W + 150 | `capture()` narrower than C0 |
| 15 | `view_set {layout: "default"}` | start, `sidebar_width` D, at least 390 | `capture()` as wide as C0 |
| 16 | `view_set {sidebar_width: W + 150}`, then `{sidebar_width: "default"}` | start, `sidebar_width` D | the default width is the one the default layout gives |
| 17 | `plate_slice {preview: true}` | start | `[0 ok]` with `layers` 150 ± 1 (a 30 mm sphere); tab `preview`; after `slice_notice_gone()`, C1 is `capture()` |
| 18 | `view_set {legend_collapsed: true}` | start, `legend_collapsed` true | `capture()` differs from C1 |
| 19 | `view_set {legend_collapsed: false}` | start | `capture()` is C1 |
| 20 | `view_set {gcode_window: false}`, then `{gcode_window: true}` | start − `gcode_window`, then start | `conf("show_gcode_window")` `false`, then `true` |
| 21 | `view_set {view_type: "speed", legend_collapsed: true}` | start, `legend_collapsed` true | a fold asked with a view type stays |
| 22 | `view_set {view_type: "feature_type"}` | start | a view type picked alone unfolds the legend |

**Scenario: the app's account, network plug-in and configuration folder**

The configuration folder is compared with the folder the app was launched on, on every platform, rather than
checked not to be a person's own folder (`APPDATA` or `~` plus `OrcaSlicer`), which could fail only on Windows.

| # | Step | Checked |
|---|---|---|
| 1 | `app_state` | `account` `{signed_in: false, bambu_signed_in: false}`; `network_plugin` `{installed: false, loaded: false, version: "", error: ""}`; `config_folder` the datadir the session file names, compared as normalised paths |
| 2 | `app_log {lines: 1}` | its `file` lies in the `log` folder of `config_folder` |

**To settle in review:**
- A layer asked of a Preview with none (nothing to slice, steps 1, 2 and 11; the layers, step 21) still shows the
  Preview tab before it is refused, and an arrange refused for its plate (plate index, step 16) still shows Prepare.
  Both are this branch's code (`view_set` in `McpToolsView.cpp` and `start_arrange()` in `McpToolsScene.cpp`, commit
  d64e10f4dd, not on `main`) and both are its design, not a bug: a field of the Preview shows the Preview, as a
  person's Go to layer pick does, and an arrangement runs on Prepare, as the toolbar's Arrange does. The rows expect
  that tab.
- The layers of a Preview that holds no toolpaths of the current plate (the layers, step 21; nothing to slice, steps
  1, 2 and 11). A bug in this branch's code, fixed when the test is written. `Ops::layer_count()` (`PlateOps.cpp`,
  commit 0616068e26, not on `main`) trusts the Preview's layer slider once it has values, and the Preview hides its
  sliders without clearing them when a plate has no layers (`Preview::load_print_as_fff()`, `main`'s code since
  1555904bef, left as it is). So after the app's first slice, `view_set {layer}` and the Go to layer action on an
  empty project, or on a plate whose slice failed, reply with the last print's layers while `app_state`'s `preview`
  is null. The fix, in `Ops::layer_count()`: refuse with `no_layers` also unless the Preview's G-code viewer holds
  the current plate's slice result (`has_data()`, and `last_result_id()` is the id of the plate's
  `get_slice_result()`), the test `app_state`'s `preview` makes through `viewer_holds()`. The rows expect
  `no_layers` in both places, so the first scenario no longer depends on running before any slice.
- The refusals of a plate index the project lacks carry the code `no_such_plate` from `plate_slice`, `project_export`
  and the plate commands, but none from `object_arrange`, `plate_get_settings`, `object_list`, `object_transform`,
  `plate_slice_stats` and `plate_set_custom_gcode`, whose text is the same. A refusal without a code is not a bug:
  those are matched by their words.
- A `plates` string other than "all" taken as the current plate (plate index, step 4). A bug in this branch's code,
  fixed when the test is written: `plate_slice`'s handler (`McpToolsScene.cpp`, commit 0616068e26) and
  `project_export`'s (`McpToolsProject.cpp`, commits dc522e59e4 and 5e5571a99f), none on `main`, read `{plates:
  "abc"}` as the current plate. The fix, in both handlers: refuse a string other than "all" and "current" with
  `object_arrange`'s words, `plates is "all", "current" or a plate index` (`start_arrange()`, `McpToolsScene.cpp`),
  before anything changes. The row expects that refusal from all three commands.
- The two-colour slice (statistics, step 4) changes filament once a layer: the tool order puts each layer's last
  filament first on the next, so 100 changes and one tool line a layer. The replay this takes the place of checked
  only "more than 100" on 128 layers.
- The top of the speed colour range with travel shown is 350, the Klipper process's `travel_speed` in mm/s; the colour
  ranges count travel only while the legend shows it. With travel hidden, every extrusion speed is lower: the fastest,
  the process's 200 mm/s walls and infill, tops out near 180.
- A new slice keeps the layer slider's span when its top layer is as high as the last print's and the slider was off
  its ends (`Preview::update_layers_slider()`, `main`'s code since 1555904bef), so after the statistics and tables
  scenarios a cube's slice can show `1–5`. That is the app's own behaviour, not a bug: the scenarios that read layers
  set `[1, 100]` after their slice, or at the start, instead of expecting it.
- The exported toolpaths' vertices lie within half a layer of the layers shown; the bounds allow a whole layer. The
  OBJ's highest vertex (the layers, step 18): `ToolpathsObjExporter::cross_section()` centres each extrusion's cross
  section on the path vertex, whose z is the layer's top (the half-height shift in `ViewerImpl` is for drawing only), so
  the top layer at 20 reaches 20.1.
- The captures right after a change (a legend row clicked, step 6). One replay makes the calls back to back, which
  seldom leaves the app an idle frame between a change and its first capture, so a capture drawn before the change
  shows as a difference between the change's two captures. The legend click leaves ImGui's highlight on its row until
  the pointer moves, and on a busy machine that move can come between two captures (measured: the only difference
  between a pair was the row's outline), so the pointer first moves to the empty space above the plate. Byte equality
  needs pixel-identical frames, with no animation and no caret.
- The toolpaths an export writes right after a layer change (the layers, step 17). A bug in this branch's code, left
  for a fix: `Ops::export_toolpaths_obj()` (`ExportOps.cpp`, not on `main`) writes the G-code viewer's visible range,
  which takes the layer slider's values only when the canvas next draws (`GLCanvas3D::_apply_gcode_sliders()`, commit
  39705d7bb9, not on `main`). `view_set {layer_range: [20, 25]}` followed at once by the export writes every layer
  (z 0.1 to 20.1); after a canvas screenshot it writes 3.9 to 5.1. The fix: apply the pending slider values to the
  viewer before the export (a public `GLCanvas3D::apply_gcode_sliders()` called from `Ops::export_toolpaths_obj()`).
  The row expects the fixed behaviour.
- The Slice complete notification in the captures after a slice (a legend row clicked, step 6; view switches, steps
  17 to 19). It shows for 3 s (`SlicingProgressNotification::get_duration()`) and then fades for 2
  (`FADING_OUT_DURATION`), so a capture taken while it fades differs from the next. On the GPU the captures of both
  scenarios fit in the 3 s; with software OpenGL (Xvfb) a capture takes up to 200 ms (measured: step 6's replay 2.0 s
  against 0.7 s on KWin), which put the fade inside step 6's pairs and between C1 and step 19. So both wait for it to
  go first, by what `widget_list` lists rather than by time. It counts down only while the main window is active
  (`NotificationManager::update_notifications()`), so on a desktop where another window is active it stays as it is
  and the wait gives up after 15 s; a window that becomes active during the captures would still start the fade among
  them. On `--headless` and `--xvfb` each app's window is the only one on its display and stays active.
- A display switch turned back draws the 3D view as before it was changed, so the capture is byte-identical: every
  check that a capture is C0 or C1 (view switches, steps 7, 11 and 19). If a switch leaves a trace (the
  navigator's or another ImGui window's position), that check falls back to "differs from the capture taken with the
  switch changed".
- The camera of the view switches. The overhang highlight colours a face whose normal points more than 31° below the
  horizontal (`support_threshold_angle` 30, plus the one degree `GLVolumeCollection::get_selection_support_normal_z()`
  adds, `main`'s code). From the iso view, 45° above the bed, the part of the sphere facing the camera reaches only 45°
  below its equator, so the highlight is a band on the sphere's rim too thin to change a pixel: the capture with
  overhangs on is byte-identical to C0. From 20° above the bed the band from 31° to 70° below the equator faces the
  camera and shows, and every other switch shows too. The setup lowers the camera so.
- The start of the view switches: `AppConfig`'s defaults, which the baseline gives, since the switches, the projection
  and the window layout are kept in the configuration an earlier test on the data folder may have changed. The network
  plug-in's start: not installed, so neither loaded nor failed, on any of the sweep's data folders: no test installs
  it (`preferences_set` refuses `installed_networking`) and none signs in.
- The sidebar's default width is 39 em (`AuiMgr::sidebar_pane_info()`), which depends on the display's scale or font,
  so it is checked as at least 390 px and as the width Reset Window Layout gives; the baseline's default layout makes
  it also the start width W.
- A G-code file opened for viewing gives the same estimate as the slice it was exported from, and the file's header
  the slice's: the processor reads the file's moves as it read the slice's. Times are compared within 1 s, the
  header giving whole seconds.
- `object_transform {plate}` moves an instance to another plate and leaves the current plate (plate index, step 17):
  `transform_instance()` selects no plate.
- The layer 30 comparison of the height ranges: the Klipper process's crosshatch infill grows with its density, 40 %
  measuring 2.2 times 15 % at layer 30; the check asks for 1.8 times. The control, step 18, has two slots and the
  prime tower moved, but prints on slot 1 alone, with no tool change (its check), so its layer 30 is the cube's own.
- The prime tower of the height ranges (step 14). A range on slot 2 makes a tower, whose depth is worked out for its
  thinnest layers with a tool change, here the range's 0.1 mm; on the Klipper printer's 250 mm bed the tower's default
  place, x 165 and y about 216 to 219 (`plate_list`'s `wipe_tower`), leaves too little room behind it, and the slice
  fails "Prime tower is partially outside the printable area, and it cannot be printed." That is the app's own
  behaviour, which a person meets and answers by moving the tower, not a bug: the step moves it to y 150, where it
  fits, through the project's `wipe_tower_y`, which the next new project resets. The two-colour statistics and the
  layer G-codes, on 0.2 mm layers, fit at the default place.
- The slots' colours and the flush multiplier (Starts from, `slots()`). The app works out the purge from the slots'
  colours and multiplies it by `flush_multiplier`, and the purge sizes the prime tower of the two-colour slices.
  `start()` gives slot 1 the baseline's `#26A69A` and the multiplier 0.3, and `slots()` adds slot 2 with `add_slot()`
  in `#00C1AE`, the colour a new app gives its first added slot, so the tower is the one measured on a new app whatever
  ran before. `filament_add` alone would colour slot 2 with the next colour of a palette the app's run cycles through
  (`Plater::get_next_color_for_filament()`), which depends on how many slots the run added before: in an app where
  `test_filaments` had added slots, it gave a lighter colour, the purge grew and the statistics' tower ran off the bed.
  Other slot colours fail so too, "Prime tower is partially outside the printable area, and it cannot be printed."
  (measured with slot 1 `#FF0000`): red to `#00C1AE` is a 515 mm³ purge where `#26A69A` to `#00C1AE` is 89, and with
  slot 1 red the layer G-codes' step 14 fails too. The two-colour checks ask only whether a slot purges into the tower
  (`tower_g` > 0, `flush_g` 0), not how much, so a purge within the range that fits leaves them unchanged.
- The G-code of a printer outside the Bambu network (`gcode()`): its tags are the compatible ones of
  `GCodeProcessor::Reserved_Tags_compatible` (`;LAYER_CHANGE`, `;Z:`, `;HEIGHT:`, `;TYPE:`, `;PAUSE_PRINT`,
  `;CUSTOM_GCODE`), its estimate is the `; estimated printing time (normal mode)` line written after the moves rather
  than the header's `total estimated time`, and its header's `filament` lists the slots of
  `ToolOrdering::all_extruders()` in that order, `2,1` for the two-colour cubes, so the rows compare the slots, not
  their order. Its tool changes purge into the prime tower, so a slot's purge counts in `tower_g` and `flush_g` is 0.

**Not checked:**
- The tables' arithmetic (a slot's grams as its four uses, the totals, the shares of the print time, layer heights,
  paging, the unique-name rule) and a height's layer: unit tests in `tests/slic3rutils/test_mcp_results.cpp`.
- `filament_m`, `filament_cost`, `prepare_time_s`, the travel time and the grams' values: they follow the filament profile and the
  slicer's paths; they are checked as present and positive or as sums.
- The colour ranges other than speed and height: the tops of `volumetric_flow_rate` and `layer_time_linear` are
  checked only above 0. Their values follow the slicer's flow and cooling; the speed range is the one with a value
  the profile fixes, the travel speed.
- Adaptive and smoothed layer height profiles and their slices are checked by properties (more or fewer points by
  quality, heights within the printer's 0.08 to 0.32 mm, the ranges' 0.12 and 0.28 kept, layer counts that differ
  from the flat profile's), not exact point or layer counts, which follow the Benchy's mesh and the algorithm.
- The sidebar's default width itself: it is 39 em, and an em is 10 px times the display's scale on Windows and macOS
  but a font's width on Linux, so the test checks it as at least 390 px and as what Reset Window Layout gives. A
  change of the 39 em passes.
- The captures after a change (a legend row clicked, step 6) show that the canvas drew the change and then held still,
  not that it shows the right layers or legend; the first change is compared with no capture before it.
- Which of the view switches' steps 1 to 6 left a trace when step 7's capture is not C0: a switch turned back is
  compared with C0 once all of them are back, not one at a time.
- `view_set {layer}` and the Go to layer action on a fresh app with no slice, in the same words, and the STL and
  sliced 3MF exports of an empty project: `test_operation_routes`. A refused layer leaving the legend's fold and the
  G-code window: `test_additions_refusals`.
- The journal steps of the Slice and Preview, Slice All Plates and Go to layer actions, and `plate_goto`: no test
  reads them; `test_speed_dial` checks the journal step of Go to tab, which collects a value as Go to layer does.
- `plate_select`'s, `plate_rename`'s, `plate_lock`'s, `plate_duplicate`'s and `plate_delete`'s refusals of a plate
  the project lacks: `test_menu_items_refusals`. A rename making its plate current, and a plate's spiral mode
  with its question: `test_objects`.
- How many `select partplate!` snapshots a plate selection takes (two when the plate changes, one from the plater
  and one from the plate list): only snapshots the rows name are counted.
- A cancelled slice: `test_runtime_calls`. A superseded slice (another slice or an edit while it runs) needs a
  second caller timed against the slice, and is left out.
- Silent mode: the print has no silent-mode estimate (`GCodeProcessor` enables only its normal-mode machine), so
  `time_mode` cannot switch; a legend row clicked, step 11, checks its refusal.
- The G-code window on screen: it shows only while the moves slider is off its end, which no tool sets. It is
  checked through `app_state` and the configuration.
- The camera fields: `test_set_view_camera`. A sidebar floating in its own window: `test_sidebar_floating`.
- A layer G-code of `type: "template"`: the Klipper printer's `template_custom_gcode` is empty. That the codes take no
  undo step: an `undo` after one would take back the step before it, the cube's.
- The notifications in `plate_slice`'s reply: the validation error is checked through the plate's `error`.
- What the 3D view looks like: captures are compared only with each other. The Add Cube action and its size dialog:
  the cubes are added with their size given, and `test_gui_tools` adds the action's cube.
- A Bambu Lab printer's G-code (its `; CHANGE_LAYER` tags, the header's time line, the start G-code's filament load):
  the G-code is read as the Klipper printer writes it.
- The recent projects, the folders the app remembers and the user presets: the test leaves entries there (Leaves) and
  reads none of them.

**Strength:** Strong — what the profile fixes (layers, heights, slots, codes, switches) is compared exactly and read
back from the G-code or the 3MF; what the slicer works out (grams, times, adaptive profiles) is held to stated bounds.
