# Presets and calibration

The design of this area's sweep tests. The rules every entry follows and the grades are in
[TESTS.md](../TESTS.md).

## `test_presets`
`test_presets.py` · Python

**Tests:** The presets of the three types, through the tools a person's preset work maps to: selecting a preset by
the name its selector shows, hidden ones included (`preset_select`); listing presets with filters (`preset_list`);
what a setting is and where it lives (`settings_get`, `settings_list`); saving, overwriting, reverting and deleting
presets (`preset_save`, `settings_set`, `preset_delete`); the unsaved-changes prompt of a preset switch and of a new
project, answered ahead (`preset_changes`) or in the dialog; the same selections through the plugin API's
`orca.host.preset.process` and `.printer` `begin_update(name)`; a preset kept inside the project; exporting a
filament with the Export Preset Bundle dialog and importing preset files (`preset_import`); a printer variant picked
by its nozzle diameter and the filament grouping of a printer with one extruder; a preference changed in its
dialog; and that the presets selected, a user preset and an unsaved edit are what the G-code is sliced with. Then
the Calibration menu's tests through `calibration_generate_test`, every test and method: the project each makes, the
parameters it uses, the presets and objects it changes, the warnings for values it rounds or the printer may not
take, the layer height it lowers to reach a speed, and the refusals that leave the project and presets as they were;
`preset_changes` "keep" and "discard", including the filament tab on another slot's modified preset; a test printing
with another filament (`filament_slot`), and the mixed and missing slots it refuses; on the ToolChanger, the extruder
that slot prints on and the settings the test takes from it, and, there and on the Klipper after it, the page an
extruder setting is on, as the settings reads and the Speed Dial give it; and, through the
exported G-code, that the temperature, pressure advance and retraction towers change their value band by band. A
calibration test is first a change to the presets this test reads (`presets_changed`), so the calibration tests are
this test's scenarios. It runs on the default printer, the Generic Klipper Printer 0.4, and switches to other
printers of the Custom vendor: no step needs a Bambu Lab printer's own behaviour. The Klipper's presets
have what the steps exercise: processes with an entry per extruder variant (three), a filament with an entry per
variant it lists (six), other nozzles that share its process, and, in the ToolChanger's own Generic PLA, a filament
that fits other printers than PLA does. Its scenarios run in the order written, and some count on what an earlier
one left: the switching rows on what the Klipper 0.8 was last used with, which the hidden presets scenario's step 2
writes, and most scenarios after saving a filament on the presets it saves.

**Starts from:** `start(client)`, which every scenario's start makes again and compares with `{printer: KLIPPER,
process: KLIPPER_STD, filaments: [GENERIC_PLA]}`, which the rows write `Klipper | Std | PLA`. Of the baseline, the
scenarios count on Developer mode off, Simple mode, and `auto_arrange` and `remember_printer_config` on: each
scenario's start checks Developer mode and the mode; the preference scenario reads `auto_arrange`; the switching rows
count on `remember_printer_config`; the prompt lists only the settings the mode shows, and one scenario counts on a
setting Simple mode hides. Then, once, before the first scenario:
- `printer_install` of the Generic Klipper Printer's 0.2, 0.6 and 0.8 nozzles and of the Generic Marlin Printer's
  0.4 nozzle, `preset_select` of the Klipper, then `printer_uninstall` of the same four, each with `preset_changes:
  "discard"`: the hidden presets scenario and the listing need them hidden, and an earlier test may have installed
  or shown them. The uninstall reloads the presets, which hides them however they were shown (see **To settle**).
  The replies are not checked: what the two calls change depends on the tests before.
- `preset_delete` of each preset of a name this test saves that is listed, which a run of this test that failed on
  this folder left: the filaments "MCP Test PLA", "MCP Test PLA 2", "MCP Test PETG", "MCP Export PLA", "MCP Imported
  PLA", "MCP Print PLA" and "MCP Calib PLA" (which keep and discard step 1 saves without `overwrite`, and a run that
  failed before its step 6 leaves), the processes "MCP Delete Child" and then "MCP Delete Base", and the printer "MCP
  Test Printer", selected first since `preset_delete` takes only the selected printer. Then `start(client)` again.

The hidden presets and nozzle diameter scenarios set `remember_printer_config` false after their start, and the next
scenario's start sets it back. With it on, a printer switch selects again the process and filaments the printer was
last used with, which the data folder keeps per printer and which the tests before this one may have written; with
it off, the switch keeps the process and filament that fit the new printer, so those two scenarios do
not depend on that. The switching rows run with it on and land on what the Klipper 0.8 was last used with, Std and
PLA, which hidden presets step 2 wrote.

**Leaves:** the user filament "MCP Print PLA", the one preset a passing run keeps: the others it saves, it deletes in
the deleting, exporting and keep and discard scenarios. A run that fails between keep and discard steps 1 and 6 leaves
"MCP Calib PLA", and the Klipper's memory with a second slot holding it; a later test's `start()` sets its own slots.
The G-code files the calibration scenarios export are in the test's own folder. Developer mode off, `auto_arrange` and
`remember_printer_config` on and Simple mode, as the baseline has them: keep and discard and another extruder, the
last two scenarios, start with `start()`, which turns back what an earlier scenario left changed, such as
`remember_printer_config`, which the hidden presets and nozzle diameter scenarios turn off. The Generic Klipper
Printer's 0.2, 0.6 and 0.8 nozzles and the Generic Marlin Printer uninstalled, and the ToolChanger not listed, as the
start found it; a run that fails in another extruder leaves the ToolChanger listed, which the next run's start hides.
What each printer it selected was last used with: the Klipper's 0.4, 0.6 and 0.8
nozzles Std and PLA, the Klipper's 0.2 nozzle what the start's install selected with it (0.08mm Extra Fine @MyKlipper
and PLA in a run after `test_operation_routes`), the Marlin 0.20mm Standard @MyMarlin and PLA, the ToolChanger TC Std
and TC PLA in five slots, and an entry for "MCP Test Printer", which deleting the preset leaves. In the recent projects, `inside.3mf` in the test's own folder, which is gone once the
test ends, so the Recent Projects menu does not list it. A new project with `Klipper | Std | PLA` in one slot.

**Terms:** printers `Klipper` (MyKlipper 0.4 nozzle), `Klipper 0.6` (MyKlipper 0.6 nozzle), `Klipper 0.8`
(MyKlipper 0.8 nozzle), `Marlin` (MyMarlin 0.4 nozzle), `ToolChanger` (MyToolChanger 0.4 nozzle); processes `Std`
(0.20mm Standard @MyKlipper), `Opt` (0.16mm Optimal @MyKlipper), `TC Std` (0.20mm Standard @MyToolChanger); filaments
`PLA` (Generic PLA @System), `TC PLA` (Generic PLA @MyToolChanger). Presets the test
makes are written by their full names ("MCP Test PLA"). A value written `v×n` is n entries, each v, joined by commas:
`220×6` is "220,220,220,220,220,220".

**Reads:**
- `selection()` gives `app_state.presets` (the printer, the process and each slot's filament) and the unsaved edits of
  the preset each tab edits: the keys `settings_get {type, compare: ":saved"}` lists, each once, for the process and
  the printer, and for the filament the same read with `filament_slot` for each slot in turn, since a read without a
  slot sees slot 1's preset and sees its edits only while the filament tab edits it. Each key comes with its whole
  value, as `settings_get {type, keys: [key]}` (with the slot) reads it. It is written `printer | process |
  filaments`, a preset's unsaved keys and their values in brackets after it: `Klipper | Std[wall_loops 5] | PLA` is
  the Klipper, and Std with `wall_loops` edited to 5, and PLA in the one slot; `PLA, MCP Calib PLA[filament_cost 22]`
  is two slots, the filament tab editing the second. A value with spaces is quoted. After every step `selection()` is
  compared with its row whole, keys and values.
- `value(type, key, preset=None, slot=None)` gives one item's `value` from `settings_get {type, keys: [key],
  include_meta: false}`, with `preset` the saved preset of that name and with `slot` that slot's, as OrcaSlicer writes
  it.
- `listed(type, **filters)` gives every item of `preset_list` for the filters, reading page after page (`limit` 500 and
  `offset`), each item whole. A step that reads a list other tests add to, the user presets above all, picks this
  test's own items from it by name, as the step says.
- `dialogs()` gives the open dialogs from `app_state.dialogs` as `{title, buttons}`, a button by its label.
- `plugin(code)` runs `code` with `script_run` and gives what it returns; the code catches what the plugin API raises
  and returns `{raised: {type, message}}`, so the test asserts the raise instead of losing it. It replaces
  `ops_presets.preset_api`, which returns every raise as data.
- `dial(key)` gives the Speed Dial's actions for the printer setting `key`, one per extruder: the items of
  `action_list {include_settings: true}`, page after page, whose `id` begins `orca_setting:<key>#`, each as
  `(id, section)`; an id is `orca_setting:<key>#<n>:5`, n the extruder's index and 5 the printer settings' type.
  `numbered()` gives the items of the same listing whose `section` names a numbered extruder page,
  `Printer : Extruder <n> : …`.
- `gcode_config(path)` gives the `; key = value` lines between `; CONFIG_BLOCK_START` and `; CONFIG_BLOCK_END` of an
  exported G-code file as a dict, quotes around a value removed; `gcode_heights(path)` gives the heights of its `;Z:`
  lines in order, the line the Klipper's G-code writes at each layer change.

A call that the unsaved-changes prompt holds replies `blocked_by_dialog`. The test then reads `dialogs()`, answers
with `dialog_answer`, and waits for the call's job with `wait`; a wait that runs out of time fails. A refusal is
matched by its code where the operations layer gives one, otherwise by its words, and is always followed by
`selection()`.

Every scenario before the calibration scenarios starts with `start(client)` and checks that it returns the start
above, that the project has one filament slot and no objects, that `app_state.mode` is `simple` and that
`preferences_get` reads `developer_mode` false.

**Scenario: hidden presets** (runs first)

With `remember_printer_config` off (see **Starts from**).

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `preset_list {type: printer}` | `Klipper \| Std \| PLA` | the Klipper is listed with `selected: true`, and neither the Klipper 0.8 nor the Marlin is listed: the start's uninstall hid them, and nothing has shown them since |
| 2 | `preset_select {type: printer, name: "MyKlipper 0.8 nozzle"}` | `Klipper 0.8 \| Std \| PLA` | reply `shown` Klipper 0.8; no dialog opened. Std lists the 0.8 nozzle among its printers and PLA fits every printer, so both stay |
| 3 | `preset_select` of Klipper | `Klipper \| Std \| PLA` | |
| 4 | `preset_select {type: printer, name: "MyMarlin 0.4 nozzle", preset_changes: "discard"}` | `Marlin \| 0.20mm Standard @MyMarlin \| PLA` | reply `shown` Marlin. Std does not fit the Marlin, so the process moves to the Marlin's preset of the same alias, 0.20mm Standard; PLA fits it and stays |
| 5 | `preset_select` of Klipper, then Std, then PLA in slot 1 | `Klipper \| Std \| PLA` | |

A hidden filament shown by name, and the name a slot's selector shows, are `test_filaments`' (its hidden filaments
scenario): `Ops::resolve_preset()` shows a hidden preset of any type through `show_hidden_preset()`, so the printers
here stand for it.

**Scenario: listing presets**

The selection stays `Klipper | Std | PLA` throughout; each step checks it. The lists of steps 1 to 6 hold the user
presets earlier tests left as well; every check reads items by name or a property every item has, so those items
decide nothing.

| # | Step | Checked |
|---|---|---|
| 1 | `listed(filament, vendor: "Generic", filament_type: "PLA", include_uninstalled: true)` | PLA is among them, "Generic PLA @MyToolChanger" is not; every item has `vendor` "Generic" and `filament_type` "PLA" |
| 2 | the same with `vendor: "generic", filament_type: "pla"` | the same names in the same order as step 1 |
| 3 | the same as step 1 with `include_incompatible: true` | the names of step 1 and more, "Generic PLA @MyToolChanger" among them with `compatible: false`; every item of step 1 is there without a `compatible` field: what step 1 leaves out is what does not fit the Klipper |
| 4 | `listed(printer, include_uninstalled: true)` | "MyKlipper 0.2 nozzle" is listed with `installed: false`; every item with `system: true` has `vendor`, `printer_model` and `nozzle`; the Klipper item has `vendor` "Custom", `printer_model` "Generic Klipper Printer", `nozzle` "0.4" and `selected: true` |
| 5 | `listed(filament, printer: "MyToolChanger 0.4 nozzle", include_uninstalled: true)` against `listed(filament, include_uninstalled: true)` | "Generic PLA @MyToolChanger" is in the first list only and PLA in the second only: the ToolChanger's own Generic PLA takes PLA's place for it |
| 6 | the first of step 5 with `include_incompatible: true` | PLA is listed with `compatible: false` and "Generic PLA @MyToolChanger" without a `compatible` field: compatibility is judged against the printer named, not the one selected |
| 7 | `preset_list {type: printer, printer: "MyToolChanger 0.4 nozzle"}` | refused, "printer lists the filaments or processes that fit a printer preset, not printers" |
| 8 | `preset_list {type: process, filament_type: "PLA"}` | refused, "filament_type filters filaments (type filament)" |

**Scenario: what a setting is**

The selection stays `Klipper | Std | PLA` throughout; each step checks it.

| # | Step | Checked |
|---|---|---|
| 1 | `settings_get {type: process, keys: [seam_slope_type, no_such_key]}` | one item, `seam_slope_type`, whose `meta.description` holds "scarf joint", `meta.where` is "Quality > Seam" and `meta.labels[1]` "Contour"; `unknown_keys` is `[no_such_key]` |
| 2 | `settings_list_pages {type: printer}`, then `settings_list {type: printer, where: "Extruder > Retraction"}` | `pages` holds a page "Extruder" with a group "Retraction" whose `settings` is the number of items the listing gives, at least 1; the listing holds `retraction_length`, with `meta.where` "Extruder > Retraction" |
| 3 | `settings_list_pages {type: process}`, then `settings_list {type: process, where: "Others > Brim"}`, then `settings_get {query: "seam"}`, then `settings_list_pages {type: object}` against `settings_list {type: object, include_meta: true}`, every page | `pages` holds a page "Others" with a group "Brim"; the listing holds `brim_type`; the `settings_get` is refused with code `invalid_argument`, as it takes no query; the object's groups count, added up, the settings of its listing that have a `meta.where` (a setting no settings page shows, such as `enforce_support_layers`, is in the listing alone), and its listing is shorter than the process's: the pages of what an object takes |
| 4 | `settings_get {keys: [nozzle_temperature, layer_height, nozzle_diameter, curr_bed_type, no_such_key], include_meta: false}` | items, in this order: `nozzle_temperature` type `filament` `220×6`, `layer_height` type `process` "0.2", `nozzle_diameter` type `printer` "0.4"; `unknown_keys` `[curr_bed_type, no_such_key]`; `type_of` `{curr_bed_type: project}` |
| 5 | `settings_get {type: process, keys: [nozzle_temperature]}` | no items; `type_of` `{nozzle_temperature: filament}` |
| 6 | `settings_set {values: {nozzle_temperature: 215}}` | refused, `"nozzle_temperature" is a filament setting: settings_set takes it with type "filament"`; `value(filament, nozzle_temperature)` is `220×6` |
| 7 | `settings_list {type: process, where: "Quality > Seam"}`, against the whole `settings_list {type: process, include_meta: true}`, every page | its keys are exactly the keys whose `meta.where` is "Quality > Seam" in the whole listing, `seam_position` and `seam_slope_type` among them; each item has a `meta.description`, and its `value` is the one the whole listing gives the key |
| 8 | `settings_get {type: process, keys: [fuzzy_skin]}` | `meta.values[1]` "external" and `meta.labels[1]` "Contour" |

**Scenario: saving a filament**

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `settings_set {type: filament, values: {filament_cost: 21.5, slow_down_layer_time: 6, temperature_vitrification: 50}}` | `Klipper \| Std \| PLA[filament_cost 21.5, slow_down_layer_time 6, temperature_vitrification 50]` | |
| 2 | `preset_save {type: filament, name: "MCP Test PLA"}` | `Klipper \| Std \| MCP Test PLA` | reply `saved` "MCP Test PLA", `user` true, `project` false, `inherits` PLA, `selected` true, `filament_slots` `[1]`; `listed(filament, query: "MCP Test PLA")` is one item, `selected`, `user`, not `modified`, inheriting PLA; the saved preset reads `filament_cost` "21.5", `slow_down_layer_time` "6" and `temperature_vitrification` "50"; its `compatible_printers` names the Klipper alone: a filament saved from one that fits every printer is made to fit the printer it was saved on |
| 3 | `preset_list` with `source: user`; with `source: system, query: "MCP Test PLA"`; with `source: project` | as after 2 | of the user filaments, those of the names this test saves are "MCP Test PLA" alone; the other two list nothing |
| 4 | `settings_set` `filament_cost` 22, then `preset_save {type: filament, name: "MCP Test PLA"}` | as after 2 | saved without `overwrite`, as the tab's plain Save; `settings_get {type: filament, compare: ":parent", keys: [filament_cost, slow_down_layer_time, nozzle_temperature]}` gives `right` PLA, `total` 2, and the items `filament_cost` "22" (other "20") and `slow_down_layer_time` "6" (other "4") |
| 5 | `settings_set` `filament_cost` 23, then `preset_save {type: filament, name: "MCP Test PLA 2"}` | `Klipper \| Std \| MCP Test PLA 2` | reply `filament_slots` `[1]`; "MCP Test PLA" is still listed, not selected, and `value(filament, filament_cost, preset: "MCP Test PLA")` is "22" |
| 6 | `preset_save {type: filament, name: "MCP Test PLA"}` | as after 5 | refused, `Preset "MCP Test PLA" already exists; pass overwrite: true to replace it`; "MCP Test PLA" still reads "22" |
| 7 | the same with `overwrite: true` | `Klipper \| Std \| MCP Test PLA` | reply `selected` true; `filament_cost` reads "23" both edited and with `preset: "MCP Test PLA"` |

**Scenario: dropping and reverting edits**

The scenario first selects "MCP Test PLA" (filament cost 23) in slot 1.

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `settings_set {type: process, values: {wall_loops: 7, sparse_infill_density: "37%"}}` | `Klipper \| Std[sparse_infill_density 37%, wall_loops 7] \| MCP Test PLA` | |
| 2 | `settings_set {type: process, reset_all: true, values: {wall_loops: 7}}` | `Klipper \| Std[wall_loops 7] \| MCP Test PLA` | reply `reverted` holds `sparse_infill_density` and `wall_loops` and nothing else (the unsaved keys before the reset, the one set again included), `applied` `{wall_loops: "7"}`; `sparse_infill_density` reads "15%" |
| 3 | `settings_set {type: process, reset_all: true}` | `Klipper \| Std \| MCP Test PLA` | reply `reverted` `[wall_loops]`; `wall_loops` reads "3" |
| 4 | the same | as after 3 | reply `reverted` `[]` |
| 5 | `settings_set` `wall_loops` 6, then `settings_set {type: process, reset_all: true, values: {wall_loops: "six"}}` | `Klipper \| Std[wall_loops 6] \| MCP Test PLA` | the second call refused, "Invalid value for wall_loops": a bad value fails before the reset |
| 6 | `settings_set {type: process, reset_all: true, values: {wall_loops: 5, sparse_infill_density: "25%"}}`, then `settings_set {type: process, values: {wall_loops: null}}` | `Klipper \| Std[sparse_infill_density 25%] \| MCP Test PLA` | the second reply `applied` and `changed` give `wall_loops` "3": null takes a key back to its saved value |
| 7 | `settings_set {type: process, values: {wall_loops: 4}, reset_to: "parent"}` | as after 6 | refused, "reset_to applies to the keys given as null"; `wall_loops` reads "3" |
| 8 | `settings_set {type: filament, values: {filament_cost: null}, reset_to: "parent"}` | `Klipper \| Std[sparse_infill_density 25%] \| MCP Test PLA[filament_cost 20]` | reply `applied` `filament_cost` "20"; `settings_get {type: filament, compare: ":parent", keys: [filament_cost]}` gives `total` 0 |
| 9 | `settings_set {type: filament, values: {filament_cost: null}}` | as after 6 | reply `applied` `filament_cost` "23" |
| 10 | `settings_get {type: process, keys: [outer_wall_speed]}` | as after 6 | `meta.variants` `["Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Extra High Flow"]`, `meta.active` `[0]`, `value` `120×3` |
| 11 | `settings_set {type: process, values: {outer_wall_speed: 250}}` | `Klipper \| Std[outer_wall_speed 250,120,120, sparse_infill_density 25%] \| MCP Test PLA` | reply `applied` "250,120,120": one value writes the entry in use, as typing into the tab's field does |
| 12 | `settings_set {type: process, values: {outer_wall_speed: [1, 2]}}` | as after 11 | refused, "outer_wall_speed holds 3 values, one per extruder variant" |
| 13 | `settings_set {type: process, values: {outer_wall_speed: {index: 1, value: 300}}}` | `Klipper \| Std[outer_wall_speed 250,300,120, sparse_infill_density 25%] \| MCP Test PLA` | reply `applied` and `changed` "250,300,120" |
| 14 | `settings_set {type: filament, values: {nozzle_temperature: {index: 0, value: 215}}}` | `Klipper \| Std[outer_wall_speed 250,300,120, sparse_infill_density 25%] \| MCP Test PLA[nozzle_temperature 215,220,220,220,220,220]` | reply `applied` "215,220,220,220,220,220"; `settings_get {type: filament, compare: ":saved", keys: [nozzle_temperature]}` gives `total` 1 and one item, `index` 0, `value` "215", `other` "220": filaments are compared entry by entry |
| 15 | `settings_set {type: filament, reset_all: true}` | as after 13 | reply `changed` `nozzle_temperature` `220×6` |
| 16 | an auto-answer rule answering "Spiral mode only works" with Yes, then `settings_set {type: process, values: {spiral_mode: true}}` | printer, process and filament as after 13; the process's unsaved keys are `outer_wall_speed` 250,300,120 and every key of `changed` (below) with its value there | the rule's use count goes from 0 to 1: the question was asked once. Reply `applied` `spiral_mode` "1"; `changed` gives `spiral_mode` "1", `wall_loops` "1", `top_shell_layers` "0" and `sparse_infill_density` "0%" |
| 17 | `settings_set {type: process, reset_all: true}` | `Klipper \| Std \| MCP Test PLA` | reply `changed` `spiral_mode` "0" |

**Scenario: reading presets that are not selected**

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `settings_set {type: process, reset_all: true, values: {wall_loops: 5, sparse_infill_density: "25%"}}` | `Klipper \| Std[sparse_infill_density 25%, wall_loops 5] \| PLA` | reply `changed` `{wall_loops: "5", sparse_infill_density: "25%"}` |
| 2 | `settings_get {type: process, compare: ":saved"}` | as after 1 | `total` 2, `left` "0.20mm Standard @MyKlipper (edited)", items `sparse_infill_density` "25%" (other "15%") and `wall_loops` "5" (other "3"), in that order |
| 3 | `settings_get {type: process, preset: <Std>, keys: [wall_loops]}`; `settings_get {type: process, preset: <Opt>, keys: [layer_height]}` | as after 1 | "3", the saved value without the edit, and "0.16"; reading Opt did not select it |
| 4 | `settings_get {type: process, compare: <Opt>, keys: [layer_height, wall_loops]}` | as after 1 | `right` Opt; items `layer_height` "0.2" (other "0.16") and `wall_loops` "5" (other "3") |

**Scenario: switching a process or a printer**

Each row starts as a scenario does, with `start(client)` and a check that `selection()` is `Klipper | Std | PLA`. It
then makes the edit the row names: `process` is `settings_set {type: process, values: {wall_loops: 5}}`, `printer` is
`settings_set {type: printer, values: {printer_notes: "MCP pin"}}`. The call is `preset_select` of the type and name
given; "asks" means it is given no `preset_changes`, is held by a dialog, and the dialog is answered as the row says.
A row whose selection after shows no edit also reads `wall_loops` "3", the saved value of Std and of Opt, so an edit
that was dropped is seen to be gone. The rows run with `remember_printer_config` on, and a switch to the Klipper 0.8
selects again what it was last used with: Std and PLA, from hidden presets step 2.

| # | Edit | Call | Selection after | Also checked |
|---|---|---|---|---|
| 1 | none | process Opt | `Klipper \| Opt \| PLA` | no dialog opened; the reply has no `discarded` and no `modified` |
| 2 | process | process Opt, `preset_changes: "transfer"` | `Klipper \| Opt[wall_loops 5] \| PLA` | reply `modified` `{process: {name: Opt, keys: [wall_loops]}}`, no `discarded` |
| 3 | process | process Opt, asks, answered Discard | `Klipper \| Opt \| PLA` | one dialog held the call, titled "Transfer or discard changes", with the buttons Transfer, Discard and Save; the job's reply `discarded` `{process: {name: Std, keys: [wall_loops]}}`, no `modified` |
| 4 | process | process Opt, asks, closed (`cancel`) | `Klipper \| Std[wall_loops 5] \| PLA` | the job ends refused, `selection_cancelled`, `Selecting "0.16mm Optimal @MyKlipper" was cancelled.` |
| 5 | process | process Std, the one the tab edits; asks, closed | `Klipper \| Std[wall_loops 5] \| PLA` | a dialog of the title and buttons of row 3 held it; refused, `selection_cancelled` |
| 6 | none | process "No Such Process" | `Klipper \| Std \| PLA` | refused, `preset_not_offered`, `The preset selector does not offer "No Such Process".` |
| 7 | none | process "0.16mm Optimal", Opt's alias | `Klipper \| Std \| PLA` | refused, `preset_not_offered`, `The preset selector does not offer "0.16mm Optimal".`, its details' `similar` starting with Opt: a compatible preset carries the name, under another name in the selector |
| 8 | none | process "0.20mm Standard @MyMarlin" | `Klipper \| Std \| PLA` | refused, `preset_incompatible`, `"0.20mm Standard @MyMarlin" is not compatible with the selected printer.` |
| 9 | printer | printer Klipper 0.8, `"transfer"` | `Klipper[printer_notes "MCP pin"] \| Std \| PLA` | refused, `transfer_not_offered`, "The unsaved changes cannot be transferred: a printer's changes stay with it." |
| 10 | printer | printer Klipper 0.8, asks, answered Discard | `Klipper 0.8 \| Std \| PLA` | the dialog's title as in row 3, its buttons Discard and Save, no Transfer; the job's reply `discarded` `{printer: {name: Klipper, keys: [printer_notes]}}` |
| 11 | printer | printer Klipper 0.8, asks, closed | `Klipper[printer_notes "MCP pin"] \| Std \| PLA` | refused, `selection_cancelled` |
| 12 | process | printer Klipper 0.8, `"transfer"` | `Klipper 0.8 \| Std[wall_loops 5] \| PLA` | no dialog opened; reply `modified` `{process: {name: Std, keys: [wall_loops]}}`: the edit moved with the switch |
| 13 | process | printer Klipper 0.8, `"discard"` | `Klipper 0.8 \| Std \| PLA` | reply `discarded` `{process: {name: Std, keys: [wall_loops]}}`: the Klipper 0.8's process is selected again from its saved values |
| 14 | process | printer Klipper 0.8, asks, answered Transfer | `Klipper 0.8 \| Std[wall_loops 5] \| PLA` | one dialog held the call, with the buttons Transfer, Discard and Save |
| 15 | process | printer Klipper 0.8, asks, closed | `Klipper 0.8 \| Std \| PLA` | the job's reply `selected` Klipper 0.8: closing the prompt does not stop this switch, and the edit is dropped as in row 13 |
| 16 | none | printer "Generic Klipper Printer", the model's name | `Klipper \| Std \| PLA` | refused, `preset_not_offered` |

Each row stands for a branch of the switch the tool and the plugin API share (`Ops::select_preset()`, its
`AnswerScope` answering the tab's prompt): for the process, no edit, Transfer ahead, the prompt answered Discard, the
prompt closed on another preset and on the one the tab edits, and the three refusals of a name; for the printer, its
own edit (no Transfer offered, Discard, closed) and the process edit it carries (Transfer and Discard ahead, Transfer
answered, closed). Discard ahead on the process tab, the prompt answered Transfer on it, a printer switch with no edit,
Discard ahead of a printer edit and a printer name no preset carries reach the code of the rows kept: answering ahead
is the same `AnswerScope` for every type (rows 2 and 13, and `test_filaments`' one slot with `X`, rows 7 and 8), the
answered prompt's reply is built as it is ahead, a switch with no edit is the hidden presets' step 2, and row 6 is the
name no preset carries.

**Scenario: switching through the plugin API**

Through `plugin("orca.host.preset.<process|printer>.begin_update(<name>).commit()")`, which returns the commit's
`target_name` and `selection_changed`, or what it raised. A row that names a switching row starts and makes its edit
as that row does, its selection after is that row's, and a refusal raises with the words of that row's refusal; rows 2
and 7 give their own. The plugin API has no `preset_changes`, so every switch with an edit asks.

The API's own code is the commit's fields and `throw_refusal()` (`PluginHostPresets.cpp`), which raises
`SelectionCancelled`, a `RuntimeError`, for a cancel and `ValueError` for a name no preset fits; behind it the switch
is the tool's `Ops::select_preset()`, with `Ask`. So the rows are one for each field and each raise: a switch of
each type (1, 7), the prompt answered (2), a cancel (3), the preset the tab edits by name and with none (4, 5), a
refusal of each code (6) and the slot forms (8). The switching rows' other branches reach that function as the tool's
rows do.

| # | As row | Checked |
|---|---|---|
| 1 | 1 | `target_name` Opt, `selection_changed` true |
| 2 | process edit, Opt, answered Transfer: selection `Klipper \| Opt[wall_loops 5] \| PLA` | the dialog's title and buttons as in row 3; `target_name` Opt |
| 3 | 4 | raises `RuntimeError` (the plugin API's `SelectionCancelled` is a `std::runtime_error`) with the words of row 4 |
| 4 | 5, given Std, the preset the tab edits | no dialog opens; `target_name` Std, `selection_changed` false; selection `Klipper \| Std[wall_loops 5] \| PLA` |
| 5 | 5, with no name | as 4 |
| 6 | 7 and 8 | `ValueError` with each row's words |
| 7 | no edit, printer Klipper 0.8: selection `Klipper 0.8 \| Std \| PLA` | no dialog opened; `target_name` Klipper 0.8, `selection_changed` true |
| 8 | none: `orca.host.preset.process.begin_update(1, <Opt>)`, then `orca.host.preset.process.update(1, "wall_loops", "4")` | `ValueError`, "The slot_number form of begin_update() is only available on orca.host.preset.filament", then `ValueError`, "The slot_number form of update() is only available on orca.host.preset.filament"; selection `Klipper \| Std \| PLA` after each |

**Scenario: what a switch carries**

It starts by selecting "MCP Test PLA" in slot 1 and setting `sparse_infill_density` "25%" on Std.

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `settings_set` `wall_loops` 4, then `preset_select {type: process, name: <Opt>, preset_changes: "transfer"}` | `Klipper \| Opt[sparse_infill_density 25%, wall_loops 4] \| MCP Test PLA` | reply `modified.process.keys` `[sparse_infill_density, wall_loops]`, no `discarded` |
| 2 | `settings_set {type: process, values: {seam_slope_type: "all"}}`, then `preset_select` of Std with `"transfer"` | `Klipper \| Std[sparse_infill_density 25%, wall_loops 4] \| MCP Test PLA` | reply `discarded` `{process: {name: Opt, keys: [seam_slope_type]}}`: Simple mode hides it, so the prompt does not list it and Transfer drops it, as in the GUI |
| 3 | `settings_set {type: filament, values: {slow_down_layer_time: 7}}`, then `preset_select` of Opt with `"discard"` | `Klipper \| Opt \| MCP Test PLA[slow_down_layer_time 7]` | reply `discarded.process.keys` `[sparse_infill_density, wall_loops]`, no `modified.process`, and `modified.filament.keys` `[slow_down_layer_time]`: a filament that fits the new process keeps its edit whatever the answer |
| 4 | `settings_set {type: filament, reset_all: true, values: {filament_type: "PETG"}}`, `preset_save {type: filament, name: "MCP Test PETG"}`, then `preset_select` of "MCP Test PLA" in slot 1 | `Klipper \| Opt \| MCP Test PLA` | `value(filament, filament_type, preset: "MCP Test PETG")` is "PETG" |
| 5 | `settings_set` `slow_down_layer_time` 8, then `preset_select {type: filament, name: "MCP Test PETG", filament_slot: 1, preset_changes: "discard"}` | `Klipper \| Opt \| MCP Test PETG` | reply `selected` "MCP Test PETG", `discarded` `{filament: {name: "MCP Test PLA", keys: [slow_down_layer_time]}}`. The refused Transfer to a filament of another type is `test_filaments`' (one slot with `A`, row 3) |

**Scenario: a new project and unsaved presets**

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `settings_set` `wall_loops` 6, then `project_new {discard_changes: true}` | `Klipper \| Std[wall_loops 6] \| PLA` | reply `modified` `{process: {name: Std, keys: [wall_loops]}}`, no `discarded`; `preset_list {type: process, query: "0.20mm Standard @MyKlipper"}` gives the item `modified: true` |
| 2 | `project_new {discard_changes: true, preset_changes: "keep"}` | as after 1 | reply as in step 1 |
| 3 | `project_new {discard_changes: true, preset_changes: "discard"}` | `Klipper \| Std \| PLA` | reply `discarded` `{process: {name: Std, keys: [wall_loops]}}`, no `modified`; the item has no `modified` |

**Scenario: a process kept inside the project**

It starts by selecting "MCP Test PLA" in slot 1. The lists it reads are those of the two names it saves, "MCP Project
Process" and "MCP Detached", each read with `query` set to the name: a user process of another test whose name holds
"MCP" stays out of them.

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `preset_save {type: process, name: "MCP Project Process", inside_project: true}` | `Klipper \| MCP Project Process \| MCP Test PLA` | reply `project` true, `user` false, `inherits` Std; `listed(process, query: "MCP Project Process")` is one item, `selected`, `project` true, `user` false |
| 2 | `preset_save {type: process, name: "MCP Detached", detach: true}` | as after 1 | refused, "Detach is offered only in Developer mode"; `listed(process, query: "MCP Detached")` is empty |
| 3 | the `toggle_developer_mode` action, the same save, the action again | `Klipper \| MCP Detached \| MCP Test PLA` | reply `project` true and no `inherits` |
| 4 | `object_add_primitive {shape: "cube", size: [20, 20, 20]}`, `project_save` to a new file, `inside.3mf`, `project_new {discard_changes: true}` | `Klipper \| Std \| MCP Test PLA` | the two lists are empty: presets inside a project leave with it |
| 5 | `project_open` of the file | `Klipper \| MCP Detached(inside.3mf) \| MCP Test PLA` | the two lists give "MCP Detached(inside.3mf)", `user` true and no `project` field, and "MCP Project Process", `project` true and `user` false; "MCP Test PLA" is `selected` and not `modified` |

**Scenario: deleting presets**

It starts with "MCP Test PLA" in slot 1. The presets it deletes are those the scenarios before made.

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `preset_delete {type: filament, name: <PLA>}` | `Klipper \| Std \| MCP Test PLA` | refused, `"Generic PLA @System" is a system preset`; `value(filament, filament_type, preset: <PLA>)` is "PLA" |
| 2 | `preset_delete {type: filament, name: "MCP Test PLA 2"}` | as after 1 | reply `deleted` "MCP Test PLA 2", `selected` "MCP Test PLA", `filaments` `["MCP Test PLA"]`; `listed(filament, query: "MCP Test PLA 2")` is empty |
| 3 | `settings_set {type: filament, values: {slow_down_layer_time: 11}}`, then `preset_delete` of "MCP Test PETG" | `Klipper \| Std \| MCP Test PLA[slow_down_layer_time 11]` | refused, `The edited filament "MCP Test PLA" has unsaved changes`; "MCP Test PETG" is still listed |
| 4 | `settings_set {type: filament, reset_all: true}`, `preset_select` of "MCP Test PETG" in slot 1, `slow_down_layer_time` 11, then `preset_delete` of "MCP Test PETG" | `Klipper \| Std \| PLA` | reply `selected` PLA, the preset it inherits from, and `discarded` `{filament: {name: "MCP Test PETG", keys: [slow_down_layer_time]}}`; it is no longer listed |
| 5 | `preset_delete` of "MCP Test PLA" | `Klipper \| Std \| PLA` | reply `selected` PLA; `listed(filament, query: "MCP Test")` holds none of "MCP Test PLA", "MCP Test PLA 2" and "MCP Test PETG" |
| 6 | Developer mode on, `preset_save {type: process, name: "MCP Delete Base", detach: true}`, Developer mode off | `Klipper \| MCP Delete Base \| PLA` | reply `user` true and no `inherits` |
| 7 | `settings_set` `wall_loops` 2, then `preset_save {type: process, name: "MCP Delete Child"}` | `Klipper \| MCP Delete Child \| PLA` | reply `inherits` "MCP Delete Base", `selected` true; the child reads `wall_loops` "2", where the base holds Std's 3 |
| 8 | `preset_delete` of "MCP Delete Base" | as after 7 | refused, `"MCP Delete Base" cannot be deleted while other presets inherit from it`; `listed(process, query: "MCP Delete")` has both |
| 9 | `preset_select` of Std, `wall_loops` 4, then `preset_delete` of "MCP Delete Child" | `Klipper \| Std[wall_loops 4] \| PLA` | refused, `The edited process "0.20mm Standard @MyKlipper" has unsaved changes`; both still listed |
| 10 | `settings_set {type: process, reset_all: true}`, then `preset_delete` of "MCP Delete Child" | `Klipper \| Std \| PLA` | reply `deleted` "MCP Delete Child", `selected` Std, no `discarded`: it was selected, deleted and the selection restored; "MCP Delete Base" alone is listed |
| 11 | `preset_select` of "MCP Delete Base", `wall_loops` 5, then `preset_delete` of it | `Klipper \| <P> \| PLA` | `<P>` is the reply's `selected`, a process other than "MCP Delete Base"; `discarded` `{process: {name: "MCP Delete Base", keys: [wall_loops]}}`; `listed(process, query: "MCP Delete")` is empty |
| 12 | `preset_select` of Std, then `preset_save {type: printer, name: "MCP Test Printer"}` | `MCP Test Printer \| Std \| PLA` | reply `user` true, `inherits` Klipper, `selected` true |
| 13 | `preset_select` of Klipper, then `preset_delete {type: printer, name: "MCP Test Printer"}` | `Klipper \| Std \| PLA` | refused, `"MCP Test Printer" is not the selected printer`; it is still listed |
| 14 | `preset_select` of "MCP Test Printer", then `preset_delete` of it | `Klipper \| Std \| PLA` | reply `selected` Klipper and no `attached`; it is no longer listed |

**Scenario: exporting and importing presets**

The test writes into a folder of its own, and writes there the file it imports, `imported_pla.json`: the fixture
`tests/fixtures/mcp_import_pla.json`, a user filament inheriting PLA with `filament_cost` 24, renamed "MCP Imported
PLA" (`name` and `filament_settings_id`), so that the fixture's own name, "MCP Import PLA", is never loaded.
`costs()` reads `filament_cost` of "MCP Export PLA" and "MCP Imported PLA" with `value(..., preset: name)`, "absent"
for one not listed. The selection stays `Klipper | Std | PLA` except in step 5.

| # | Step | Checked |
|---|---|---|
| 1 | `settings_set` `filament_cost` 25; Developer mode on, `preset_save {type: filament, name: "MCP Export PLA", detach: true}`, Developer mode off; `preset_select` of PLA in slot 1 | reply no `inherits`; costs 25, absent |
| 2 | `preset_export {path: <folder>}`; a click on the label "Filament presets(.zip)" within Export Preset Bundle; `widget_set_value` of "MCP Export PLA" in it to true; OK; the Info dialog answered OK | the Info dialog's message is "Export successful"; the export's job ends with `level` "success"; no dialog is left. The file "Filament presets.zip" holds an entry ending "MCP Export PLA.json" whose `filament_cost` is 25: the check through the file |
| 3 | `preset_import {path: <zip>}` | reply `message` "Kept the existing presets: MCP Export PLA."; costs 25, absent |
| 4 | `preset_delete` of "MCP Export PLA", then `preset_import` of the zip | reply `message` "Imported presets: MCP Export PLA."; it is listed with `user` true; costs 25, absent |
| 5 | `preset_select` of "MCP Export PLA" in slot 1, `filament_cost` 30, `preset_save` under its name, `preset_select` of PLA | costs 30, absent |
| 6 | `preset_import` of the zip | `message` "Kept the existing presets: MCP Export PLA."; costs 30, absent |
| 7 | the same with `on_conflict: "overwrite"` | `message` "Replaced presets: MCP Export PLA."; costs 25, absent: the file's preset replaced the changed one |
| 8 | `preset_import` of `imported_pla.json` | `message` "Imported presets: MCP Imported PLA."; it is listed inheriting PLA; costs 25, 24 |
| 9 | the same again, then with `on_conflict: "overwrite"` | `message` "Kept the existing presets: MCP Imported PLA.", then "Replaced presets: MCP Imported PLA."; costs 25, 24 |
| 10 | `preset_import` of a path that does not exist | refused, "file not found"; costs 25, 24 |
| 11 | `preset_import` of `tests/fixtures/heightmap.png` | refused, "Not a preset file: give a .json, .zip, .orca_printer, .orca_filament, .orca_bundle or .gcode"; costs 25, 24 |
| 12 | `preset_import` of a `.json` the test writes, `{"not": "a preset"}` | refused, `no_presets_imported`; costs 25, 24; no dialog open |
| 13 | `preset_import` of `imported_pla.json` with `overwrite: true` | refused, "unknown argument overwrite"; costs 25, 24 |
| 14 | `preset_import` of `imported_pla.json` with `on_conflict: "merge"` | refused, "on_conflict must be one of"; costs 25, 24 |
| 15 | `preset_delete` of both | neither "MCP Export PLA" nor "MCP Imported PLA" is listed |

**Scenario: a nozzle diameter, and one extruder's filament grouping**

With `remember_printer_config` off (see **Starts from**).

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `settings_set {type: printer, values: {nozzle_diameter: 0.6}}` | `Klipper 0.6 \| Std \| PLA` | reply `applied` `nozzle_diameter` "0.6"; `nozzle_diameter` and `printer_variant` read "0.6": the variant's printer preset was selected, as the sidebar's nozzle diameter does, though the start left it hidden. Std lists the 0.6 nozzle among its printers and PLA fits every printer, so both stay |
| 2 | `settings_set {type: printer, values: {nozzle_diameter: "0.4"}}` | `Klipper \| Std \| PLA` | reply `applied` `nozzle_diameter` "0.4" |
| 3 | `plate_get_settings {keys: [filament_map_mode, filament_map]}` | as after 2 | `filament_map_mode` value "Auto For Flush" with `meta.offered` empty; `filament_map` effective `[1]` |
| 4 | `plate_set_settings {values: {filament_map_mode: "Manual"}}` | as after 2 | refused, "filament_map_mode groups the filaments onto the extruders of a printer that has several; this one has one"; `plate_get_settings` as in step 3 |

**Scenario: what the print is sliced with**

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `settings_set` `filament_cost` 23, `preset_save {type: filament, name: "MCP Print PLA"}`, `preset_select` of Opt, `settings_set` `wall_loops` 4, `object_add_primitive {shape: "cube", size: [20, 20, 20]}` | `Klipper \| Opt[wall_loops 4] \| MCP Print PLA` | |
| 2 | `plate_slice`, then `project_export {format: "gcode", path: <file>}` | as after 1 | outcome `ok`. `gcode_config` of the file gives `printer_settings_id` Klipper, `print_settings_id` Opt, `filament_settings_id` "MCP Print PLA", `layer_height` "0.16", `wall_loops` "4" (Opt holds 3) and `filament_cost` "23": the selected presets, a user preset and an unsaved edit are what the G-code was made from. `gcode_heights` rise by 0.16 to 0.001 from each layer to the next, the first layer and the top one aside: the process's layer height is what the layers are printed at |

**Scenario: a preference changed in its dialog**

Its last step turns `auto_arrange` on again, as the start set it; a run that fails before that step leaves it off
for the next scenario's start to set back (see **Leaves**).

| # | Step | Checked |
|---|---|---|
| 1 | `preferences_get {keys: [auto_arrange, no_such_preference]}` | one item, `auto_arrange`, value true, as the start set it; `unknown_keys` `[no_such_preference]` |
| 2 | `settings_set {values: {auto_arrange: false}}` | refused, `Unknown setting "auto_arrange" for the process settings`; no dialog open; `auto_arrange` reads true |
| 3 | `window_open {window: "preferences", tab: "advanced"}` | refused, "tab must be one of"; no dialog open |
| 4 | `window_open {window: "preferences", tab: "behaviour", key: "auto_arrange"}`, `widget_set_value {target: {name: "auto_arrange", within: "Preferences"}, value: false}`, then Close | the call is held by a dialog titled Preferences; the control's reply `checked` false; the job ends; `auto_arrange` reads false |
| 5 | the same with `tab: "general"` and `value: true` | the control is found although it is on another tab; `checked` true; `auto_arrange` reads true |

The calibration scenarios follow. They test `calibration_generate_test` for every test and method of the Calibration
menu, on the Klipper, whose extruder is a direct drive and whose firmware has no junction deviation, so the pressure
advance defaults are a direct drive's and cornering sweeps jerk; a Klipper printer is one the tests write firmware
commands for (`SET_PRESSURE_ADVANCE`, and the input shaping tests, which slice on it). The temperature tower and keep
and discard start from `start(client)`, compared with `Klipper | Std | PLA`; the other tests start from a temperature
tower at its defaults, a project of its own. No stored state decides a check, the mode included: every call drops the
unsaved changes first, and the checks read the presets the start selects. In them `selection()` is compared on the
preset names and the filament's unsaved keys and values only: the process and printer keys a test changes depend on the
values the shipped profiles already hold (see **Not checked**); the keys each test sets on purpose are read with
`value()` instead. `objects()` gives each object as `{name, height}` from `object_list`, the height being `bbox.size` z.
`own(object, key)` gives the object's own value of a process key from `settings_get {target: "object", object, keys:
[key]}`. `gcode_layers(path)` gives the lines of an exported G-code file after its first `;LAYER_CHANGE` line, the line
the Klipper's G-code writes at each layer change, cut into bands where the test says. PLA holds six entries of each
per-variant key, one per extruder variant it lists, and Std three.

Each test's call is made with `discard_changes: true` and `preset_changes: "discard"` unless the step says otherwise.
Its reply's `project`, `params` and `names` are checked whole against the values the row writes out; `project` is
also read back from `app_state.project.name`, and `names` from `objects()`. "Sets" lists keys the test's code sets,
with the value it sets, written from the test's `Plater` function: each is read with `value()` after the call. "Every
entry" means each entry of a key with one per extruder or variant. Each row fixes the filament's changes whole,
from the code and the Generic PLA profile: the reply's `presets_changed.filament.changes` has exactly those keys and
values, and, since a call with `preset_changes: "discard"` drops every edit first, the selection's filament brackets
are those keys and values too.

A slice is `plate_slice` followed by a check that it ended `ok`, then, where the row reads the G-code,
`project_export {format: "gcode", path}` to a new file.

**Scenario: the temperature tower, and what is refused**

| # | Step | Checked |
|---|---|---|
| 1 | start: `start(client)` | it returns `Klipper \| Std \| PLA`; one filament slot, no objects |
| 2 | `calibration_generate_test {test: "temperature", start: 252, end: 232}`, without `discard_changes` or `preset_changes` | `project` "Nozzle temperature test"; `params` `{start: 250, end: 230, auto_scale: true}`; `warnings` exactly "start 252 °C rounded down to 250 °C, the tower's 5 °C blocks" and "end 232 °C rounded down to 230 °C, the tower's 5 °C blocks"; `names` `["temperature_tower.drc"]`, the tower 50 mm high to 0.01 (five 10 mm blocks), with its own `layer_height` "0.2"; `presets_changed.filament` names PLA and its `changes` are exactly `nozzle_temperature` and `nozzle_temperature_initial_layer`, both `250×6`; `result_keys` `{type: filament, keys: [nozzle_temperature, nozzle_temperature_initial_layer]}`; `modified.filament.keys` is those two keys; selection `Klipper \| Std \| PLA[nozzle_temperature 250×6, nozzle_temperature_initial_layer 250×6]`; `app_state.project.dirty` is true |
| 3 | a slice | the `M104 S<t>` values in the layers, zeros and repeats in a row dropped, are exactly 250, 245, 240, 235, 230 |
| 4 | `{test: "max_volumetric", discard_changes: true}` | refused, saying "Presets are modified" and naming `preset_changes`; its details' `modified.filament.keys` are `[nozzle_temperature, nozzle_temperature_initial_layer]` |
| 5 | `{test: "temperature", step: 5}` | refused, "step does not apply to temperature", with details `takes` `[start, end, auto_scale, test_model]` |
| 6 | `{test: "pressure_advance", start: 0.01, end: 0.05, step: 0.051}` | refused, "pressure_advance needs start >= 0, step >= 0.001 and end >= start + step; got start 0.01, end 0.05, step 0.051" |
| 7 | `{test: "retraction", preset_changes: "discard"}`, without `discard_changes` | refused, `unsaved_changes` |
| — | after each of 4 to 7 | the project is still "Nozzle temperature test" with the one tower, 50 mm high, its own `layer_height` "0.2"; the selection is still `Klipper \| Std \| PLA[nozzle_temperature 250×6, nozzle_temperature_initial_layer 250×6]` |

**Scenario: the other tests**

It starts with `{test: "temperature"}` at its defaults, which gives `params` `{start: 230, end: 190, auto_scale:
true}` and leaves the selection `Klipper | Std | PLA[nozzle_temperature 230×6, nozzle_temperature_initial_layer
230×6]`. Each row is then one call, made after the one before.

| # | Call | Checked |
|---|---|---|
| 1 | `{test: "max_volumetric", end: 15}` | `project` "Max volumetric speed test"; `params` `{start: 5, end: 15, step: 0.5}`; `names` `["SpeedTestStructure.drc"]`, 22 mm high to 0.01; `discarded.filament` is `{name: PLA, keys: [nozzle_temperature, nozzle_temperature_initial_layer]}`, and `nozzle_temperature` reads `220×6` again; sets filament `filament_max_volumetric_speed` `200×6` and `slow_down_layer_time` "0", which are the filament's whole `changes`, and process `spiral_mode` "1"; `result_keys` `{type: filament, keys: [filament_max_volumetric_speed]}`; a slice |
| 2 | `{test: "pressure_advance", end: 0.06}` | `project` "Pressure Advance Test"; `params` `{method: tower, start: 0, end: 0.06, step: 0.002}`; `names` `["tower_with_seam.drc"]`; sets filament `slow_down_layer_time` "1", the filament's whole changes; `result_keys` `{type: filament, keys: [enable_pressure_advance, pressure_advance]}`; a slice, whose `SET_PRESSURE_ADVANCE ADVANCE=<k>` values in the layers, repeats in a row dropped, are exactly 0, 0.002, 0.004 … 0.06, the 31 bands, each to 0.0001 |
| 3 | `{test: "pressure_advance", method: "line", print_numbers: false}` | `project` as row 2; `params` `{method: line, start: 0, end: 0.1, step: 0.002, print_numbers: false}`; `names` `["pressure_advance_test.drc"]`; sets process `wipe_inward` "0" and `retract_after_outer_wall` "0", and no filament key: the filament's changes are empty |
| 4 | `{test: "pressure_advance", method: "pattern", accelerations: [1000, 2000], speeds: [50, 100]}` | `project` as row 2; `params` `{method: pattern, start: 0, end: 0.08, step: 0.005, accelerations: [1000, 2000], speeds: [50, 100]}`; `names` `["pa_pattern_50_1000", "pa_pattern_100_1000", "pa_pattern_50_2000", "pa_pattern_100_2000"]`; sets filament `filament_retract_when_changing_layer` and `filament_wipe` 0 in every entry (the profile has nil), the filament's whole changes, and printer `wipe` and `retract_when_changing_layer` 0 in every entry; `result_keys` `{type: filament, keys: [enable_pressure_advance, pressure_advance, adaptive_pressure_advance, adaptive_pressure_advance_model]}`; a slice |
| 5 | `{test: "pressure_advance", method: "pattern"}` | `project` as row 2; `params` `{method: pattern, start: 0, end: 0.08, step: 0.005, accelerations: [3000], speeds: [120]}`, the first entries of Std's `outer_wall_acceleration` and `outer_wall_speed`: the pattern's defaults are the process's; `names` `["pa_pattern_120_3000"]`; sets as row 4 |
| 6 | `{test: "flow_ratio"}` | `project` "Orca YOLO Flow Calibration"; `params` `{method: yolo, top_surface_pattern: archimedeanchords}`; `names` "flowrate_0", "flowrate_0.01" to "flowrate_0.05", "flowrate_m0.01" to "flowrate_m0.05", the 11 of `Orca-LinearFlow.3mf` in its order; sets process `layer_height` "0.2", and no filament key (rows 6 to 9): the filament's changes are empty; `result_keys` `{type: filament, keys: [filament_flow_ratio]}`; each object's own `print_flow_ratio` is `(0.98 + m) / 0.98` to 0.0001, `m` being the number its name ends in ("m" for minus) and 0.98 the filament's flow ratio, and its own `top_surface_pattern` is "archimedeanchords"; no slice (see **Not checked**) |
| 7 | `{test: "flow_ratio", method: "yolo_perfectionist"}` | `project` "Orca YOLO Flow Calibration - Perfectionist version"; `params` `{method: yolo_perfectionist, top_surface_pattern: archimedeanchords}`; `names` the 16 of `Orca-LinearFlow_fine.3mf` in its order, "flowrate_0", "flowrate_0.005" to "flowrate_0.035" and "flowrate_m0.005" to "flowrate_m0.04"; flow ratios as in row 6 |
| 8 | `{test: "flow_ratio", method: "pass1"}` | `project` "Flowrate Test - Pass1"; `params` `{method: pass1, top_surface_pattern: archimedeanchords}`; `names` "flowrate_0", "flowrate_10", "flowrate_15", "flowrate_20", "flowrate_5", "flowrate_m10", "flowrate_m15", "flowrate_m20", "flowrate_m5", as `flowrate-test-pass1.3mf` holds them; each object's own `print_flow_ratio` is `1 + m / 100` to 0.0001 |
| 9 | `{test: "flow_ratio", method: "pass2", top_surface_pattern: "monotonic"}` | `project` "Flowrate Test - Pass2"; `params` `{method: pass2, top_surface_pattern: monotonic}`; `names` "flowrate_0", "flowrate_m1" to "flowrate_m9", as `flowrate-test-pass2.3mf` holds them; flow ratios as in row 8; each object's own `top_surface_pattern` is "monotonic" |
| 10 | `{test: "retraction", end: 1}` | `project` "Retraction"; `params` `{start: 0, end: 1, step: 0.1}`; `names` `["retraction_tower.drc"]`, with its own `layer_height` "0.2"; sets printer `use_firmware_retraction` "0", and no filament key: the filament's changes are empty; `result_keys` `{type: filament, keys: [filament_retraction_length]}`; a slice. Its layers are cut into bands at each `; Calib_Retraction_tower: Z_HEIGHT: <z>, length:<L>` line: the bands' lengths `L`, repeats in a row dropped, are exactly 0, 0.1 … 1, and in each band every deretraction (a `G1` with `E` and `F` alone and `E` above 0) restores exactly that band's `L`, to 0.001, and each `L` above 0 has at least one; the bands of 0 restore 0.8 alone, the printer's `retraction_length`: the first layer's retraction, made at the layer change before the tower's first line. The retraction the tower sets is the one the printer performs |
| 11 | `{test: "vfa", end: 160}` | `project` "VFA test"; `params` `{start: 40, end: 160, step: 10, auto_scale: true, auto_adjust: true}`; `names` `["vfa.drc"]`, with its own `layer_height` "0.2"; no `adjusted`: 160 mm/s is within what the filament's volumetric speed allows at 0.2 mm; sets filament `slow_down_layer_time` "0", the filament's whole changes, and process `spiral_mode` "1", `wall_loops` "1", `top_shell_layers` "0", `bottom_shell_layers` "1", `sparse_infill_density` "0%" and `enable_overhang_speed` 0 in every entry; `result_keys` `{type: printer, keys: [resonance_avoidance, min_resonance_avoidance_speed, max_resonance_avoidance_speed]}` |
| 12 | `{test: "vfa"}` | `project`, `names`, sets and `result_keys` as row 11; `params` `{start: 40, end: 200, step: 10, auto_scale: true, auto_adjust: true}`; `adjusted` `{layer_height: 0.16, reason: "the end speed (200 mm/s) exceeds the filament's maximum volumetric speed (12 mm³/s) at the default layer height (0.2 mm); the layer height of a profile of this printer reaches it"}`, and the object's own `layer_height` reads "0.16": the tower prints thinner layers to stay within the filament's volumetric speed; a slice |
| 13 | `{test: "vfa", end: 400, step: 20}` | `project`, `names`, sets and `result_keys` as row 11; `params` `{start: 40, end: 380, step: 20, auto_scale: true, auto_adjust: true}`; `adjusted` `{end: 380, layer_height: 0.08, reason: "the end speed (400 mm/s) exceeds the filament's maximum volumetric speed (12 mm³/s) even at the smallest layer height of this printer's profiles; the end speed was lowered to what it reaches"}`, and the object's own `layer_height` reads "0.08" |
| 14 | `{test: "vfa", end: 400, auto_adjust: false}` | `project`, `names`, sets and `result_keys` as row 11; `params` `{start: 40, end: 400, step: 10, auto_scale: true, auto_adjust: false}`; no `adjusted`; `warnings` exactly "the end speed (400 mm/s) exceeds the filament's maximum volumetric speed (12 mm³/s), which limits the outer wall to about 168 mm/s: the blocks above print at that speed (auto_adjust and auto_scale lower the layer height to reach it)"; the object's own `layer_height` is "0.2", half the nozzle, as without the warning |
| 15 | `{test: "input_shaping_freq", start: 20, end: 80}` | `project` "Input shaping Frequency test"; `params` `{start: 20, end: 80, start_y: 20, end_y: 80, damping: 0.15, shaper_type: "Default", test_model: "ringing_tower"}`; `names` `["ringing_tower.drc"]`; sets filament `slow_down_layer_time` "0", `slow_down_min_speed` "0", `slow_down_for_layer_cooling` "0", and `enable_pressure_advance` 1 and `pressure_advance` 0 in every entry (the test turns pressure advance on at 0 where the filament has it off, as PLA does), the filament's whole changes; printer `machine_max_jerk_x` and `machine_max_jerk_y` "9" (the test raises them to at least 5 on a Klipper printer, and the Klipper's 9 is above that) and `input_shaping_emit` "0"; process `spiral_mode` "1", `wall_loops` "1", `top_shell_layers` "0", `bottom_shell_layers` "1", `sparse_infill_density` "0%", `enable_overhang_speed` 0 in every entry, `accel_to_decel_enable` "1", `accel_to_decel_factor` "100%", `outer_wall_speed` 500 in every entry, the smaller of the printer's `machine_max_speed_x` and `machine_max_speed_y` first entries, and `default_acceleration` and `outer_wall_acceleration` 20000 in every entry, its `machine_max_acceleration_extruding` first entry; `result_keys` `{type: printer, keys: [input_shaping_emit, input_shaping_type, input_shaping_freq_x, input_shaping_freq_y, input_shaping_damp_x, input_shaping_damp_y]}`; a slice |
| 16 | `{test: "input_shaping_damp", freq_x: 48, freq_y: 42, test_model: "fast_tower"}` | `project` "Input shaping Damping test"; `params` `{freq_x: 48, freq_y: 42, start: 0, end: 0.4, shaper_type: "Default", test_model: "fast_tower"}`; `names` `["fast_tower_test.drc"]`; sets as row 15; a slice |
| 17 | `{test: "cornering", test_model: "scv_v2", end: 25}` | `project` "Cornering test"; `params` `{start: 1, end: 25, test_model: "scv_v2"}`; `names` `["SCV-V2.drc"]`; `warnings` exactly "end 25 is above 20: high values may cause layer shifts"; sets printer `machine_max_jerk_x` and `machine_max_jerk_y` "25", `input_shaping_emit` "1" and `input_shaping_type` "Disable", and filament `slow_down_layer_time` "0", `slow_down_min_speed` "0", `slow_down_for_layer_cooling` "0", `filament_max_volumetric_speed` "200", and `enable_pressure_advance` 1 and `pressure_advance` 0 in every entry, the filament's whole changes; `result_keys` `{type: printer, keys: [machine_max_jerk_x, machine_max_jerk_y]}`; a slice |

**Scenario: keep and discard**

| # | Step | Selection after | Also checked |
|---|---|---|---|
| 1 | `start(client)`, `add_slot` with `SLOT_COLOURS[1]`, `settings_set {type: filament, filament_slot: 2, values: {filament_cost: 21}}`, `preset_save {type: filament, name: "MCP Calib PLA", filament_slot: 2}`, `preset_select` of PLA in slot 1, then `settings_set {type: filament, filament_slot: 2, values: {filament_cost: 22}}` | `Klipper \| Std \| PLA, MCP Calib PLA[filament_cost 22]` | `add_slot` gives slot 2; `value(filament, filament_cost, preset: "MCP Calib PLA")` is "21", the saved cost |
| 2 | `{test: "retraction", preset_changes: "keep"}` | as after 1 | refused, `The filament tab edits "MCP Calib PLA", which has unsaved changes, and the test prints with filament slot 1 ("Generic PLA @System")`; `object_list` lists no objects; the saved cost still "21" |
| 3 | `{test: "retraction", preset_changes: "discard"}` | `Klipper \| Std \| PLA, MCP Calib PLA` | `presets_changed.filament.name` PLA and `tab_switched_from` "MCP Calib PLA"; `discarded.filament` is `{name: "MCP Calib PLA", keys: [filament_cost]}`; `names` `["retraction_tower.drc"]`; `value(filament, filament_cost, slot: 2)` is "21" |
| 4 | `settings_set {type: filament, values: {slow_down_layer_time: 7}}`, then `{test: "retraction", preset_changes: "keep"}` | `Klipper \| Std \| PLA[slow_down_layer_time 7], MCP Calib PLA` | `modified.filament.keys` is `[slow_down_layer_time]`; no `discarded`: the test ran with the edit, and the retraction test changes no filament setting |
| 5 | `project_new {discard_changes: true, preset_changes: "discard"}` | `Klipper \| Std \| PLA, MCP Calib PLA` | `discarded.filament.keys` hold `slow_down_layer_time`; no `modified`; it reads "4" |
| 6 | `{test: "temperature", filament_slot: 2, start: 240, end: 220}` | `Klipper \| Std \| PLA, MCP Calib PLA[nozzle_temperature 240×6, nozzle_temperature_initial_layer 240×6]` | `project` "Nozzle temperature test"; `params` `{start: 240, end: 220, auto_scale: true}`, no `filament_slot` in them; `presets_changed.filament` names "MCP Calib PLA" with `filament_slot` 2, `tab_switched_from` PLA, no `extruder` (the Klipper has one), and `changes` exactly `nozzle_temperature` and `nozzle_temperature_initial_layer`, both `240×6`; `object_list` lists `temperature_tower.drc` alone, its `filament` 2: the tower prints with slot 2, and slot 1's PLA is untouched |
| 7 | a slice | as after 6 | the `M104 S<t>` values in the layers, zeros and repeats in a row dropped, are exactly 240, 235, 230, 225, 220: slot 2's temperatures reach the G-code |
| 8 | `filament_add_mixed {components: [1, 2], ratios: [50, 50]}`, then `{test: "retraction", filament_slot: 3}` and `{test: "retraction", filament_slot: 4}`, then `filament_delete {filament_slot: 3}` and `project_new {discard_changes: true, preset_changes: "discard"}` | `Klipper \| Std \| PLA, MCP Calib PLA` | `slots()` is `0,0,1` after the add; slot 3 refused, `Filament slot 3 is a mixed filament`, and slot 4 refused, `filament_slot must be 1..2, the physical filaments`; after each refusal `object_list` still lists `temperature_tower.drc`, its `filament` 2; after the delete `slots()` is `0,0` and, before the `project_new`, the selection is as after 6: a refused slot changes nothing (read then, since a mixed slot's preset has no settings of its own to read) |
| 9 | `filament_delete {filament_slot: 2}`, then `preset_delete` of "MCP Calib PLA" | `Klipper \| Std \| PLA` | `level` "success"; `deleted` "MCP Calib PLA"; `preset_list {type: filament}` lists PLA, not `modified`, and no "MCP Calib PLA" |

**Scenario: another extruder, on the ToolChanger**

The ToolChanger has five extruders, one per filament slot, and on a multi-tool printer of no Bambu Lab vendor the
automatic filament map prints filament n on tool n (`bed_exclusion_extruder_for_filament()`), so `filament_slot` picks
the extruder. Tool 2 is given a 0.6 nozzle, so that what a test takes from its extruder differs from tool 1's: the max
volumetric speed tower's own `layer_height` is 0.8 × its extruder's nozzle and its `outer_wall_line_width` 1.75 ×, and
every entry of the printer's `max_layer_height` below that layer height is raised to it (`Plater::calib_max_vol_speed()`;
the ToolChanger's is 0.32 on every tool). The nozzle edit is kept into each test with `preset_changes: "keep"`, and each
test starts from `project_new {discard_changes: true, preset_changes: "discard"}` and the edit made again: a test that
kept the previous one's `spiral_mode` would ask the spiral mode question. TC PLA holds three entries of each per-variant
key, and every slot holds it, so the filament tab never moves.

A printer's extruder settings are on its one Extruder page, whatever its number of extruders: the page's controls
edit the extruder its switch picks. The settings index gives each extruder's copy of a setting a row of its own,
"Extruder <n>" on a printer of several, so that a Speed Dial hit picks that extruder. Steps 1 and 5 check both
sides, on the ToolChanger's five extruders and then on the Klipper's one, which follows a printer of more extruders
than it has. **The Extruder page check:** `settings_list_pages {type: printer}` lists one page whose name begins
with "Extruder", "Extruder", whose group "Retraction" counts 10; `settings_list {type: printer, where: "Extruder >
Retraction"}` lists exactly the ten keys of that group (`publishable_printer_retraction_options()`):
`retraction_length`, `retract_restart_extra`, `retraction_speed`, `deretraction_speed`, `retraction_minimum_travel`,
`retract_when_changing_layer`, `wipe`, `wipe_distance`, `retract_before_wipe` and `retract_after_wipe`, each with
`meta.where` "Extruder > Retraction".

| # | Step | Checked |
|---|---|---|
| 1 | `start(client)`, then `preset_select` of the ToolChanger | it returns `Klipper \| Std \| PLA`; then the selection is `ToolChanger \| TC Std \| TC PLA, TC PLA, TC PLA, TC PLA, TC PLA`, a slot per tool, and the printer's `nozzle_diameter` reads `0.4×5`; the Extruder page check holds; `dial(retraction_length)` is exactly five actions, `orca_setting:retraction_length#<n>:5` in section `Printer : Extruder <n+1> : Retraction` for n from 0 to 4 |
| 2 | `settings_set {type: printer, values: {nozzle_diameter: "0.4,0.6,0.4,0.4,0.4"}}`, then `{test: "max_volumetric", filament_slot: 2, preset_changes: "keep"}` | `project` "Max volumetric speed test"; `params` `{start: 5, end: 20, step: 0.5}`; `presets_changed.filament` is `{name: TC PLA, filament_slot: 2, extruder: 2, changes: {filament_max_volumetric_speed: 200×3, slow_down_layer_time: "0"}}`; the tower's own `layer_height` "0.48" and `outer_wall_line_width` "1.05", tool 2's 0.6 nozzle × 0.8 and × 1.75; the printer's `max_layer_height` reads `0.48×5`; `object_list` lists `SpeedTestStructure.drc` alone, its `filament` 2 |
| 3 | a slice | every tool the G-code picks, a `T<n>` at a line's start, is T1, tool 2 |
| 4 | `project_new {discard_changes: true, preset_changes: "discard"}`, the same `settings_set`, then `{test: "max_volumetric", preset_changes: "keep"}` | `presets_changed.filament` gives `filament_slot` 1 and `extruder` 1; the tower's own `layer_height` "0.32" and `outer_wall_line_width` "0.7", tool 1's 0.4 nozzle; `object_list` lists `SpeedTestStructure.drc`, its `filament` 1 |
| 5 | `start(client)`, then the printers hidden as `setup()` hides them (the hidden printers installed, the Klipper selected, the hidden printers uninstalled) | it returns `Klipper \| Std \| PLA`; `preset_list {type: printer}` lists the Klipper and not the ToolChanger; the Extruder page check holds; `dial(retraction_length)` is exactly one action, `orca_setting:retraction_length#0:5` in section `Printer : Extruder : Retraction`, and `numbered()` is empty: no setting is offered on a page of the ToolChanger's extruders 2 to 5 |

**To settle in review:**
- The page an extruder setting is on (another extruder, steps 1 and 5). `meta.where` is the page and group of the
  setting's row on the settings tab, and a printer of several extruders has one Extruder page whose controls a switch
  points at an extruder (`TabPrinter::activate_option()` in `Tab.cpp`); so `where` is "Extruder > …" on every
  printer, and the value of each extruder is in the setting's `value` and `meta.variants`, not on a page of its own.
  The Speed Dial keeps an action per extruder, since picking one selects that extruder. Step 5 runs after a printer
  of five extruders, so it catches rows the Klipper would keep for extruders it lacks: with them, the Speed Dial
  offers the Klipper's retraction settings on pages "Extruder 2" to "Extruder 5", and `settings_list` puts each on
  whichever of its rows the index's sort orders first.
- The hidden presets and how they are chosen (hidden presets; listing, step 4; nozzle, step 1). The Klipper's 0.6 and
  0.8 nozzles share its process and filament, so a switch to them changes the printer alone; the Marlin is another
  model with processes of its own. The test runs in an app earlier tests used, and one of them may have installed
  these printers or shown one: `test_operation_routes` shows a Klipper nozzle that is not installed through the
  plugin API. A preset `preset_select` shows stays shown, not installed, until the presets are reloaded
  (`show_hidden_preset()` in `PresetOps.cpp`), and an uninstall that finds the printers not installed changes
  nothing and reloads nothing. So the start installs them and uninstalls them again: the uninstall changes the
  installed printers, and its reload (`apply_install_set()` in `InstallOps.cpp`, through
  `PresetBundle::load_presets()`) shows each printer as the installed printers say, with the selected one, which is
  why the Klipper is selected again before it; the install selects the first nozzle it adds. That also hides a
  printer that the data folder's last selection showed when the app started.
- The Marlin's process (hidden presets, step 4). Std does not fit the Marlin, and the compatibility update picks the
  Marlin's process with Std's alias, "0.20mm Standard" (`PreferedProfileMatch` in `PresetBundle.cpp`). With
  `remember_printer_config` on, the switch first selects what the Marlin was last used with, and on a folder where it
  was never used it lands on the first of its processes, "0.08mm Extra Fine @MyMarlin", instead; the scenario turns
  the preference off for that reason as well.
- The filament a printer that is not the Klipper takes. PLA's `compatible_printers` is empty, so it fits every
  printer of the vendor but the ToolChanger, which has a Generic PLA of its own (the library leaves its generic
  filament out for a printer whose vendor has one of the same alias, `update_library_profile_excluded_from()` in
  `Preset.cpp`). Only another extruder switches to the ToolChanger, for the extruder a calibration test prints on; the
  other scenarios stay on printers with one extruder, so that the filament slots and extruder pages of a multi-extruder
  printer stay out of them.
- The compatible printers of a saved filament (saving, step 2). `Tab::save_preset()` sets the printer selected as
  `compatible_printers` when the preset saved has none, so that a filament made from a library one does not fit every
  printer by accident.
- An alias as a name (switching, row 7; plugin API, row 6). `"0.16mm Optimal"` is the alias of Opt and of the
  ToolChanger's 0.16mm Optimal. The tab's selector lists Opt by its full name, so the name matches no row and no
  hidden preset. The refusal is `preset_not_offered` with the `similar` names, since a compatible preset, Opt, carries
  the name (`not_offered()` in `src/slic3r/GUI/Operations/PresetOps.cpp`); `similar_preset_names()` ranks Opt first
  (both words matched, and compatible). Row 8's name, which no compatible preset carries, is `preset_incompatible`.
  The plugin API raises `ValueError` for both codes.
- Printer switches with an edited process (switching, rows 12 to 15). The preference `remember_printer_config`, on by
  default, makes a printer switch select again the process and filaments last used with the new printer
  (`PresetBundle::update_selections()`), which reloads the process from its saved values. Transfer keeps the edit
  through the process tab's cache, which is applied after that; Discard and closing the prompt lose it. The Klipper
  0.8 was last used with Std in hidden presets step 2 and row 10. Closing the prompt does not stop the switch at
  all: `Tab::select_preset()` cancels only when the dependent preset does not fit the new printer. That is the app's
  own code, as on `main`, left as it is: a prompt whose close does not cancel the switch is a bug for a fix of its
  own. Row 15 expects what the app does today.
- `preset_select` of the preset the tab edits, with unsaved changes (switching, row 5), asks the same question with
  Transfer offered, since a process can always take a transfer (`Tab::can_transfer_changes_to()`).
- After a project with project presets is replaced by a new one (inside the project, step 4), the process is expected
  to fall back to Std: `PresetBundle::reset_project_embedded_presets()` leaves no process selected and the
  compatibility update picks the printer's default. The replay read neither.
- A detached preset inside the project does not come back as one (inside the project, step 5). Loading a project skips
  each preset kept inside it that has no parent (`PresetCollection::load_project_embedded_presets()` in
  `src/libslic3r/Preset.cpp`, which logs "can not find parent"), so "MCP Detached" is not loaded. The process the
  project was saved with is then made from the project's own settings as an external preset named after the preset
  and the file, "MCP Detached(inside.3mf)" (`PresetCollection::load_external_preset()`), which is selected and listed
  as a user preset. That is the app's own code, as on `main` (commit 8c64808ae5), left as it is: a detached preset
  that does not survive its project is a bug for a fix of its own. Step 5 expects what the app does today.
- `object_add_primitive` with a shape alone asks for the size in the Primitive size dialog, as the shape's Speed Dial
  action does, so step 4 gives the cube a size.
- The process selected after deleting a selected preset with no parent (deleting, step 11) is the tab's next visible
  preset, which follows the collection's order, and the user processes earlier tests left on the folder are in that
  order: a run on a folder where one sorts after "MCP Delete Base" lands on it. The entry checks only that another
  process is selected.
- The 0.6 nozzle's process and filament (nozzle diameter, step 1). With `remember_printer_config` off, the switch keeps
  Std and PLA, which fit the 0.6 nozzle, whatever the 0.6 nozzle was last used with on this folder; going back to 0.4
  (step 2) keeps them too.
- The edits that must differ from the saved values (deleting, step 7; print, step 1). Std and Opt hold `wall_loops`
  3, so the child sets 2 and the print sets 4: an edit to 3 would leave no unsaved key and nothing to read in the
  G-code.
- The layer heights of the slice (print, step 2): the first layer is printed at the process's first layer height and
  the top one may be thinner to end at the cube's top, so both are left out of the 0.16 steps.
- How `gcode_config` writes a name (print, step 2): with quotes or without. The reader drops them, so the check holds
  either way.
- The towers' heights (temperature, step 2; other tests, row 1). `Plater::calib_temp()` keeps the blocks from
  `(500 - end) / 5 + 1` down to `(500 - start) / 5` blocks of 10 mm and drops what is left on the bed, so 252 and 232,
  rounded to 250 and 230, leave five blocks, 50 mm. The max volumetric structure is cut at `(end - start + 1) / step`
  mm, 22 mm. Both assume the model is taller than the cut.
- The project is dirty after a test (temperature, step 2), which step 7's refusal needs: the test's objects are
  added to a new project.
- The tower values in the G-code (temperature, step 3; other tests, rows 2 and 10). `interpolate_value_across_layers()`
  splits the layers into `(end - start) / step + 1` equal bands and clamps the top layer into the last, so with many
  more layers than bands every band appears once and the last is the end. The end G-code's `M104 S0` is dropped; the
  start G-code sets its temperatures before the first layer, which the reader skips. A Klipper printer's pressure
  advance is written as `SET_PRESSURE_ADVANCE ADVANCE=<k>` (`GCodeWriter.cpp`), not `M900`. The retraction tower's
  length is set from the layer's height (`start + floor(z - 0.4) * step`) on a tower cut at `1.4 + (end - start) /
  step` mm, so its last band is expected to be 1. A deretraction is expected to restore exactly the retracted length
  on the Klipper, whose `retract_restart_extra` is 0, and nothing else in the layers to have that shape.
- The first band of the retraction tower (other tests, row 10). The first layer change retracts by the printer's
  `retraction_length`, 0.8, before `GCode.cpp` writes the tower's first `; Calib_Retraction_tower:` line and sets its
  length, so the deretraction that ends that travel, `G1 E.8`, falls in the first band of length 0. That is the app's
  own G-code code, as on `main` (the line's code is from commits `40928a5d22`, `8a6573db19` and `0da24cd38b`, all on
  `main`), left as it is; the row expects the 0.8 there and no other deretraction in the bands of 0.
- The objects' order in `names` for the flow ratio tests (other tests, rows 6 to 9) is expected to be the order the
  shipped `resources/calib/filament_flow` model files hold them in.
- The VFA tower's layer height (other tests, rows 11 to 14), one row for each way `fit_vfa_speed()` in
  `src/libslic3r/calib.cpp` fits the end speed. PLA allows 12 mm³/s on the Direct Drive Standard extruder in use,
  the first of its six entries, so with the outer wall's line width the wall reaches about 168 mm/s at 0.2 mm. Up to
  that speed the tower keeps 0.2 mm (row 11, `Fits`). Above it, `auto_adjust` takes the largest layer height of the
  printer's compatible process profiles, up to 0.2 mm, that reaches the end speed (row 12, `ReducedLayerHeight`, 0.16
  mm for 200 mm/s); where none does, the smallest of them, 0.08 mm, with the end lowered to the largest multiple of
  the step it reaches (row 13, `LoweredEnd`, 380 in steps of 20); and without `auto_adjust` the end stays and a
  warning says where the blocks stop speeding up (row 14, `Clamped`). The candidates are the Klipper's processes
  (0.08 to 0.32 mm), so a change to those profiles moves rows 12 and 13. The VFA model's object is named after its
  file, "vfa.drc", as the others are.
- Discarding on the test (keep and discard, step 3) leaves slot 2 on "MCP Calib PLA" without its edit.

**Not checked:**
- User presets other tests left: every read of a list they join picks this test's own presets by name, and checks
  the others no further.
- The sidebar's settings search, which lists the same rows of the settings index as the Speed Dial: the Speed Dial's
  actions stand for it. Nor where a Speed Dial action on a numbered extruder page goes, the Extruder page with that
  extruder picked: no action is run.
- A printer of two to four extruders: the ToolChanger's five and the Klipper's one stand for more and fewer, and
  dropping the rows of the extruders a printer lacks is the same for any count.
- That a hidden printer no shown filament fits shows its model's default filaments (`show_default_filaments()` in
  `PresetOps.cpp`): every printer of the Custom vendor but the ToolChanger takes PLA (see **To settle**).
  `test_install_filaments`, on a Bambu Lab data folder of its own, selects such a printer, the A1 mini.
- Assigning filament presets to several slots, the sidebar's selector and the plugin API's filament editor: they belong
  to `ops_assign_filament_preset_one_slot` and `_multi_slot`, which another area's test takes the place of.
- A hidden printer selected through the plugin API, and the mixed-colour slot's refusal through both routes: checked in
  `test_operation_routes`.
- Installing and uninstalling printers and filaments: `test_install_printers` and `test_install_filaments`. Here the
  start installs and uninstalls a few without reading the replies.
- The Add filament action's message, which `test_objects` checks: keep and discard adds its second slot with
  `add_slot`, so that the slot's colour does not depend on the slots the app's run added before.
- That the Developer mode action turns the preference on and off, read through `preferences_get`: here a detached save
  is refused and then made around it, which shows it on, and each scenario's start reads it off.
- That `preset_export` with a path is held by a dialog titled Export Preset Bundle: checked in `test_person_forms`;
  here the dialog is driven to a written file and the file read.
- `preferences_set` and the Preferences dialog tab by tab: checked in `test_preferences`; this test changes one control
  in the dialog only, and sets the preferences it depends on with `preferences_set` without checking the reply.
- `preset_sync`: checked in `test_additions_refusals`.
- The undo snapshots of preset selections and imports, which `ops_import_presets` recorded through `ops_undo.py`: no
  tool lists the undo history, and a preset's change is not in it (`settings_set`'s description).
- The keys the spiral vase dialog changes besides the four named (reverting, step 16): which ones depend on the
  process's values. The selection reads them all with their values, against the reply's `changed`.
- The count of Generic PLA filaments (listing, step 1): it changes with the shipped profiles and with the user
  filaments earlier tests left, so the list is checked item by item instead.
- That no Template preset is listed, which `presets_and_plate_map` checked: `preset_list` leaves the Template vendor's
  presets out before any filter, so no read through it can find one, and the check could not fail.
- The wall count and filament cost of the slice beyond the G-code's settings block (print, step 2): counting walls
  needs the toolpaths, and the cost is the app's sum over the filament used.
- How the dialogs look: no screenshot is taken.
- The speeds the max volumetric and VFA towers step per layer: the G-code gives a move's feed rate, which the reader
  would have to tell from the brim's and travel moves by feature; the parameters and the presets they set are checked.
- The G-code of the input shaping tests beyond the slice's outcome: their shaper commands, band by band, would need a
  reader of their own.
- The process and printer keys a test's code sets beyond its row's "sets" list, and those two parts of the reply's
  `presets_changed` as whole lists. Each row's list holds the keys the test is built on: the swept setting, and the
  shells, walls, speeds, accelerations, jerk and cooling that would distort what it measures. The rest (VFA's
  `timelapse_type` and `detect_thin_wall`, input shaping's `spiral_mode_smooth` and `bottom_surface_pattern`, and the
  like) restate the function line by line, and which of them `presets_changed` lists depends on whether Std and the
  Klipper already hold the value. The filament's changes are fixed whole in every row.
- `presets_changed.filament.filament_slot` in the rows that print with slot 1, where the filament's name is checked
  instead; keep and discard step 6 checks it for slot 2.
- The extruder a manual filament map gives: `calib_filament_extruder()` reads the project's map, which MCP lists for
  reading only and the Filament grouping dialog sets for a plate, so another extruder takes the ToolChanger's
  automatic map, filament n on tool n. The rule itself is the dialogs' own, which the tool shares.
- The other tests that read the extruder's settings (the retraction tower's step, VFA's fit, the pressure advance
  pattern's line widths, the flow ratio tests' adjustments): another extruder checks max volumetric speed's tower,
  whose own settings follow the extruder's nozzle and are read back directly.
- The other filaments `preset_list` lists after keep and discard step 9: the filaments earlier tests installed or
  saved; the step reads PLA and "MCP Calib PLA" by name.
- A slice of the flow ratio tests (other tests, rows 6 to 9): `GCode.cpp` steps no value per layer for them (its
  per-layer calibration branches are the towers', the pressure advance tests', input shaping's and cornering's), so a
  slice would show only that objects with their own flow ratio slice, which is the slicer's and no calibration code.
- The Calibration menu's dialogs: `calibration_generate_test {test}` alone opens them, which `test_call_forms` does for
  the temperature test, with `silent: true` too, and `test_speed_dial` answers the retraction test's through its
  action; the other seven are opened by no test.

**Fixtures:** the test uses `tests/fixtures/heightmap.png` and `tests/fixtures/mcp_import_pla.json`, from which it
writes the preset file it imports, and writes its other files.

**Strength:** Strong — after every step it reads the selection back, each preset with its unsaved edits and their
values, and compares it whole with values written out here; it asserts each refusal by its code or its words, each
prompt by its title and buttons, and checks an exported preset through its file and the selected presets and an
unsaved edit through the G-code. Every calibration test's parameters, project, objects and filament changes are
compared exactly, and three towers by the values their G-code steps through; the process and printer keys beyond each
calibration row's list, and the G-code of the other calibration tests, are left out. The lists other tests add to are
read for its own presets alone.
