# Plates

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_plates`
`test_plates.py` · Python · takes the place of `plates.json`, `menu_items_plates.json` and `plate_icons.json`

**Tests:** The plates of the Prepare view: adding, duplicating, naming, locking, selecting and deleting plates, and
undoing that; what a lock keeps Arrange and Auto-Orient from doing; the settings a plate sets for itself; the
filament slots its filament orders count on; and the plate icons and plate menu, which do the same jobs from a
click. It runs on the harness's default printer, the Bambu Lab X1 Carbon 0.4.

**Reads:** `plates()` gives each plate as `{name, current, locked, objects}` from `plate_list`. `objects()` gives
each object as `{name, plate}` from `object_list`, and the objects outside the plates by name. `slots()` gives the
filament slots as `filament_is_mixed` from `project_get_settings`, one digit per slot (`"0,0,1"`: three slots, the
third a mixed filament). Every scenario starts with `project_new {discard_changes: true}` in the Prepare view and
checks that `plates()` is `"" 0 *`.

In the tables a plate is written as its name and its number of objects, with `*` on the current plate and `L` on a
locked one, and the plates are listed in order between `·`: `"" 1 *` · `"Copy" 1 L` is an unnamed current plate with
one object, then a locked plate named Copy with one object. After every step, `plates()` is compared with its row
whole.

**Scenario: a plate's life**

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 1 | `plate_delete` | `"" 0 *` | refused, `last_plate` |
| 2 | `plate_add` | `"" 0` · `"" 0 *` | the new plate is made current |
| 3 | `plate_add` | `"" 0` · `"" 0` · `"" 0 *` | |
| 4 | `plate_rename {plate: 1, name: "Left"}` | `"" 0` · `"Left" 0 *` · `"" 0` | naming a plate makes it current |
| 5 | `plate_lock {plate: 2, locked: true}` | `"" 0` · `"Left" 0 *` · `"" 0 L` | |
| 6 | `plate_delete {plate: 0}` | `"Left" 0 *` · `"" 0 L` | the plates after it move down; the current plate stays current |
| 7 | `undo` | `"" 0` · `"Left" 0 *` · `"" 0 L` | the delete is one undo step |
| 8 | `redo` | `"Left" 0 *` · `"" 0 L` | |
| 9 | `project_save` to a new file, `project_new`, `project_open` of the file | `"Left" 0 *` · `"" 0 L` | names and locks are kept in the project file |

**Scenario: by index, and on the current plate**

| # | Step | Plates after | Objects after |
|---|---|---|---|
| 1 | `object_add_primitive {shape: "cube", size: [20, 20, 20], name: "first"}` | `"" 1 *` | first on 0 |
| 2 | `plate_add` | `"" 1` · `"" 0 *` | |
| 3 | the same with `name: "second", plate: 1` | `"" 1` · `"" 1 *` | first on 0, second on 1 |
| 4 | `plate_select {plate: 0}` | `"" 1 *` · `"" 1` | |
| 5 | `plate_duplicate {plate: 1}` | `"" 1 *` · `"" 1` · `"" 1` | and second on 2: the new plate comes last, and plate 0 stays current |
| 6 | `plate_duplicate` | `"" 1 *` · `"" 1` · `"" 1` · `"" 1` | and first on 3 |
| 7 | `plate_rename {plate: 2, name: "Copy"}` | `"" 1` · `"" 1` · `"Copy" 1 *` · `"" 1` | |
| 8 | `plate_select {plate: 1}`, then `plate_rename {name: "Second"}` | `"" 1` · `"Second" 1 *` · `"Copy" 1` · `"" 1` | |
| 9 | `plate_rename {name: "Second"}` | as after 8 | |
| 10 | `undo` | `"" 1` · `"" 1 *` · `"Copy" 1` · `"" 1` | the unchanged name took no undo step, so undo takes back step 8's |
| 11 | `redo` | as after 8 | |
| 12 | `plate_lock {plate: 3, locked: true}` | `"" 1` · `"Second" 1 *` · `"Copy" 1` · `"" 1 L` | reply `changed: true` |
| 13 | `plate_lock {plate: 3}` | as after 8 | without `locked` it toggles |
| 14 | `plate_lock` | `"" 1` · `"Second" 1 * L` · `"Copy" 1` · `"" 1` | the current plate |
| 15 | `plate_lock {locked: true}` | as after 14 | reply `changed: false` |
| 16 | `undo` | as after 8 | the unchanged lock took no undo step, so undo takes back step 14's |
| 17 | `redo`, then `plate_lock {locked: false}` | as after 8 | |
| 18 | `plate_select {plate: 2}` | `"" 1` · `"Second" 1` · `"Copy" 1 *` · `"" 1` | |
| 19 | `plate_select` | as after 18 | without a plate the current plate stays |
| 20 | `plate_delete {plate: 1}` | `"" 1` · `"Copy" 1 *` · `"" 1` | first on 0, second on 1, first on 2; outside: second |
| 21 | `plate_delete` | `"" 1 *` · `"" 1` | first on 0, first on 1; outside: second, second. Deleting the current plate makes the plate before it current |
| 22 | `undo` | as after 20 | as after 20 |

**Scenario: duplicating a plate**

A copy must be the same objects at the same places on the new plate. A place is given as an offset from the plate's
front left corner (`origin` from `plate_list`), and objects are read from `object_list` as their name, plate,
`offset`, `rotation_deg`, `scale`, `bbox` and the `triangles` and `mesh_volume` of their part. Positions the app
works out from a plate's corner are checked to 0.001 mm.

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 1 | `object_add_primitive {shape: "cube", size: [20, 20, 20], name: "cube", position: corner + (40, 60)}`, then `object_transform {object: 0, rotate: [0, 0, 30], mode: "absolute"}` | `"" 1 *` | cube on plate 0, `offset` x and y at corner + (40, 60), resting on the bed (`bbox.min` z 0), `rotation_deg` `[0, 0, 30]`, `bbox.size` `[27.32, 27.32, 20]` to 0.01; its part has 12 triangles and `mesh_volume` 8000 |
| 2 | `object_add_primitive {shape: "cylinder", size: [20, 30], name: "cylinder", position: corner + (120, 80)}` | `"" 2 *` | cylinder on plate 0 at corner + (120, 80), resting on the bed, `bbox.size` `[20, 20, 30]` to 0.1, `mesh_volume` within 1 % of 9425 |
| 3 | `plate_duplicate {plate: 0}` | `"" 2 *` · `"" 2` | the reply's `new_plate` is 1. Plate 1 holds a cube and a cylinder, named so, at plate 1's corner + (40, 60) and + (120, 80), resting on the bed, each with its source's `rotation_deg`, `scale`, `bbox.size`, `triangles` and `mesh_volume`. The sources are as after 2; there are 4 objects, none outside the plates |
| 4 | `object_transform` of the cube's copy, `translate: [10, 0, 0]` | as after 3 | the copy is 10 mm further along x and the source cube has not moved: a copy is an object of its own, not another instance of its source |
| 5 | `undo`, twice | `"" 2 *` | the duplicate is one undo step: 2 objects, as after 2 |

**Scenario: what a lock does**

A lock keeps Arrange and Auto-Orient off a plate, and nothing else: the plate's objects can still be moved by hand.
Two cubes of 20 × 20 × 10 mm are turned onto their side with `object_transform {rotate: [0, 90, 0], mode:
"absolute"}`, so that each stands 20 mm tall (`bbox.size` `[10, 20, 20]`) and Auto-Orient would lay it flat. A is
added on plate 0 and B on plate 1, each with `position` 30 mm in from its plate's front left corner (the plate's
`origin` from `plate_list`). Objects are read from `object_list` as their plate, `offset`, `rotation_deg` and
`bbox.size`.

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 1 | A and B added and turned, then `plate_lock {plate: 0, locked: true}` | `"" 1 L` · `"" 1 *` | A at its corner, standing |
| 2 | `object_arrange` | `"" 1 L` · `"" 1 *` | A's `offset` and `rotation_deg` as after 1; B on plate 1; the reply lists plate 0 among the plates it skipped |
| 3 | `object_arrange {plates: 0}` | as after 2 | refused, saying plate 0 is locked; A as after 1 |
| 4 | `object_auto_orient {plates: [0]}` | as after 2 | refused, `plate_locked`; A still standing, `rotation_deg` `[0, 90, 0]` |
| 5 | `object_auto_orient {objects: [0, 1]}` | as after 2 | B lies flat (`bbox.size` z 10) and A still stands; the reply names B alone |
| 6 | `object_transform {object: 0, translate: [5, 0, 0]}` | as after 2 | A is 5 mm further along x: a lock does not stop a move by hand |
| 7 | `plate_lock {plate: 0, locked: false}`, then `object_auto_orient {objects: [0]}` | `"" 1` · `"" 1 *` | A lies flat: the call that left it standing turns it once the plate is unlocked |
| 8 | `object_arrange` | `"" 2` · `"" 0 *` | A and B both on plate 0, their squares apart and inside the plate; plate 1, empty now, is kept |

**Scenario: a plate's own settings**

It starts with two filament slots (`slots()` is `"0,0"`, made with `filament_add` and `filament_delete`), the project's
plate type set to Cool Plate with `project_set_settings`, and steps 1 to 3 of the by-index scenario: two plates with
a cube each, plate 1 current. A plate's settings are read from `plate_list` as `{curr_bed_type,
print_sequence, spiral_mode, first_layer_print_sequence, other_layers_print_sequence, overrides}`. In the table, a
plate whose settings are not named has the project's: `Cool Plate`, `by layer`, spiral off, both orders `null`, and
no `overrides`.

| # | Step | Settings after | Also checked |
|---|---|---|---|
| 1 | `plate_set_settings {plate: 1, values: {curr_bed_type: "Textured PEI Plate", print_sequence: "by object"}}` | plate 1: `Textured PEI Plate`, `by object`, overrides `[curr_bed_type, print_sequence]` | `plate_get_settings {plate: 1}` gives the same two values, `meta.offered` of `curr_bed_type` holds `Cool Plate`, and `spiral_mode` is `null` with effective `false` |
| 2 | `undo` | both plates the project's | |
| 3 | `redo` | as after 1 | |
| 4 | `plate_set_settings {plate: 1, values: {print_sequence: null}}` | plate 1: `Textured PEI Plate`, overrides `[curr_bed_type]` | the reply's effective `print_sequence` is `by layer` |
| 5 | `plate_set_settings {plate: 1, values: {curr_bed_type: "Glass Plate", print_sequence: "by object"}}` | as after 4 | refused, saying this printer does not offer `curr_bed_type "Glass Plate"`; the print sequence, which was valid, is not set either |
| 6 | `plate_set_settings {plate: 0, values: {first_layer_print_sequence: [1, 1]}}` | as after 4 | refused, saying a filament order lists every slot 1..2 once |
| 7 | `plate_set_settings {plate: 0, values: {other_layers_print_sequence: [{from: 2, order: [1, 2]}, {from: 5, to: 9, order: [2, 1]}]}}` | as after 4 | refused, saying the layer ranges overlap |
| 8 | `plate_set_settings {plate: 0, values: {skirt_start_angle: 90}}` | as after 4 | refused as a setting a plate does not take |
| 9 | `plate_lock {plate: 2, locked: true}` | as after 4, and `plates()` as before | refused, `no_such_plate` |
| 10 | `plate_set_settings {plate: 0, values: {first_layer_print_sequence: [2, 1], other_layers_print_sequence: [{from: 10, order: [1, 2]}, {from: 2, to: 9, order: [2, 1]}]}}` | plate 0: first `[2, 1]`, others `[{from: 2, to: 9, order: [2, 1]}, {from: 10, order: [1, 2]}]`, overrides `[first_layer_print_sequence, other_layers_print_sequence]` | the ranges come back sorted, and the open-ended one has no `to`; plate 0 is made current, as the Plate Settings dialog works on the current plate |
| 11 | `plate_set_settings {values: {first_layer_print_sequence: null, other_layers_print_sequence: null}}` | as after 4 | with no plate it sets the current plate, plate 0 |
| 12 | `plate_set_settings {plate: 0, values: {spiral_mode: true}}` | plate 0: spiral on, overrides `[spiral_mode]` | the spiral mode question was asked once, answered by an auto-answer rule whose use count goes from 0 to 1 |
| 13 | `plate_set_settings {plate: 0, values: {spiral_mode: null}}` | as after 4 | |
| 14 | `plate_slice {plates: 1}` | as after 4 | outcome `ok`; `plate_list` gives plate 1 `sliced: true` with no `layer_gcode`, and plate 0 `sliced: false` |
| 15 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 4 | a plate's own settings are kept in the project file |

**Scenario: filament slots**

| # | Step | `slots()` after | Also checked |
|---|---|---|---|
| 1 | `filament_delete` until it is refused | `"0"` | the last call refused, `last_filament_slot` |
| 2 | the Add filament action (`action_run`) | `"0,0"` | message `Added filament slot 2.` |
| 3 | the same | `"0,0,0"` | message `Added filament slot 3.` |
| 4 | `filament_delete {filament_slot: 4}` | `"0,0,0"` | refused, `no_such_slot` |
| 5 | `filament_delete {filament_slot: 2}` | `"0,0"` | message `Deleted filament slot 2.` |
| 6 | a click on Add Mixed Filament, answered OK | `"0,0,1"` | the dialog that opens is titled Add Mixed Filament |
| 7 | `filament_delete {filament_slot: 3}` | `"0,0,1"` | refused, `mixed_filament_slot` |
| 8 | `plate_set_settings {plate: 0, values: {first_layer_print_sequence: [2, 1]}}` | `"0,0,1"` | refused, saying it cannot be set while the project has mixed filaments; plate 0's order stays `null` |
| 9 | the Add filament action | `"0,0,0,1"` | message `Added filament slot 3.`: a new slot goes ahead of the mixed one |
| 10 | the Delete filament action | `"0,0,1"` | message `Deleted filament slot 3.` |
| 11 | a click on Remove last mixed filament | `"0,0"` | |

**Scenario: plate icons and the plate menu**

Each step is a `widget_click` on `{plate_icon, plate}` unless it says otherwise. The cubes are 20 mm and added with
`object_add_primitive` at a `position`; "on plate 1" means a cube's 20 mm square lies inside plate 1's rectangle
(`origin` and `size` from `plate_list`).

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 1 | `widget_list {query: "plate_icon"}` | `"" 0 *` | plate 0 shows exactly the icons `plate`, `orient`, `arrange`, `lock`, `settings` and `name` |
| 2 | `lock` on plate 0 | `"" 0 * L` | |
| 3 | `lock` on plate 0 | `"" 0 *` | |
| 4 | `settings` on plate 0, answered Cancel | `"" 0 *` | the dialog is titled Plate Settings; plate 0's settings are the project's afterwards |
| 5 | the Add Plate button of the 3D view's toolbar (`gltoolbar: "addplate"`) | `"" 0` · `"" 0 *` | |
| 6 | two cubes at plate 1's middle | `"" 0` · `"" 2 *` | both on plate 1 at the same `x` and `y` |
| 7 | `arrange` on plate 1 | `"" 0` · `"" 2 *` | the reply has `started` and `done`; the wait for idle is met; both cubes on plate 1 and their squares apart |
| 8 | a right click on `plate` of plate 1 | as after 7 | the menu lists `Edit Plate Name`, `Lock` and `Delete Plate` |
| 9 | the same, picking `Lock` | `"" 0` · `"" 2 * L` | the reply's `ran` is `Lock` |
| 10 | `lock` on plate 1 | `"" 0` · `"" 2 *` | |
| 11 | `move_to_front` on plate 1 | `"" 2 *` · `"" 0` | both cubes on plate 0 |
| 12 | `app_state` | as after 11 | no dialog open and no menu left open |

**To settle in review:**
- After undoing a delete, the plate that was current before the delete is current again (a plate's life, step 7;
  by index, step 22). Selecting a plate takes an undo snapshot of its own, so this is what undo is expected to give.
- The plate commands refuse with codes from the operations layer (`last_plate`, `no_such_plate`), but most of
  `plate_set_settings`'s refusals carry no code, so steps 5 to 8 of the settings scenario match their words. Giving
  them codes is a change to the app.
- The icons plate 0 shows on its own (icons, step 1): the replay this takes the place of counted at least seven
  nodes, one more than the six named here.
- An object with instances on two plates, which no step uses. A duplicate copies each object that has an instance on
  the source plate, with all of its instances, and moves the copy by the distance between the two plates, so the copy
  of an instance that stood on another plate lands beside that plate or on a third one. This is the app's own plate
  code (`PartPlateList::duplicate_plate()`), left as it is: a bug for a fix of its own.
- What a duplicate leaves out: the new plate gets the objects, but not the source plate's name, lock or own
  settings. The scenarios expect that (a copy of a named plate is unnamed).
- Arranging only a locked plate (lock, step 3): `object_arrange` refuses it with "Plate 0 is locked; unlock it first
  …" and no code, while Auto-Orient refuses a locked plate with `plate_locked`, so the step matches the words "Plate 0
  is locked". The refusal's advice says to select the plate before `plate_lock`, although `plate_lock` takes the
  plate; that is a bug in the MCP code (`McpToolsScene.cpp`, `object_arrange`), fixed with this test.
- Where the last arrange leaves things (lock, step 8): both cubes packed on plate 0 and plate 1 left empty, with
  plate 1 still current. An arrange never removes a plate, but which plate is current afterwards is to confirm.

**Not checked:**
- The refusals of a plate index the project lacks for each plate command, and Rename Plate's name dialog: they are
  checked in `test_menu_items_refusals`.
- How the plates and icons look: no screenshot is taken.
- The plate's `wipe_tower` position and `origin` and `size` themselves, beyond containing the arranged cubes.

## Replays this area replaces

Each leaves the sweep, with its entry, in the commit that adds the tests above.

### `plates`
`tests/plates.json` · replay, 69 steps, 35 expects, 13 optional

**Tests:** `plate_set_settings`, `plate_lock` and `plate_rename` as the Plate Settings dialog and plate icons do, read back with `plate_list` and `plate_get_settings`: a per-plate plate type and print sequence with undo/redo, first- and other-layer filament orders, spiral vase, an idempotent lock and a name; the `filament_add`/`filament_delete` commands; and a mixed filament whose slot cannot be deleted, ahead of which a new slot goes, and which blocks custom filament orders.

**Decides by:** Fails when a non-optional call errors or one of its 80 matchers misses.
- Slots: one slot after the optional deletes; `filament_add` messages "Added filament slot 2." and "3."; `filament_delete {filament_slot: 2}` message; `filament_is_mixed` = `"0,0"`.
- Start state: `plate_list` totals, object counts, empty overrides, `by layer`, unlocked, no layer G-code.
- Per-plate settings: applied and changed values; plate 1 `overrides` exactly `["curr_bed_type", "print_sequence"]`, plate 0 none; `plate_get_settings` value, effective and offered; undo empties overrides, redo restores `by object`; `null` gives effective `by layer`.
- Refusals: after five optional refused calls, plate 1 unlocked with overrides exactly `["curr_bed_type"]` and plate 0's empty.
- Filament orders: applied orders exact and sorted (open-ended range has no `to`), overrides exact, cleared with `null`.
- Spiral, lock, name, slice: exact values; locking twice reports `changed` true then false; plate 1 `sliced` true with no layer G-code.
- Mixed filament: dialog title exact; `filament_is_mixed` `"0,0,1"`; after the refused delete and order, plate 0's first-layer order is still null and a new slot gives `"0,0,0,1"`; delete and "Remove last mixed filament" return it to `"0,0"`.

**Not checked:**
- The refused `curr_bed_type: "Glass Plate"` + `by object` call: the read-back checks `overrides`, not plate 1's `curr_bed_type`, so accepting the bed type alone would pass.
- Redo is read back by `print_sequence` only, not `curr_bed_type`.
- The spiral-mode question is auto-answered; nothing confirms it was asked.
- `/items/0/wipe_tower` is `exists` only.

**Strength:** Strong — nearly every setting is read back exactly, and most refusals are followed by a read that would catch them.

### `menu_items_plates`
`tests/menu_items_plates.json` · replay, 37 steps, 58 expects, 0 optional

**Tests:** The plate commands by index and with no plate: `plate_duplicate`, `plate_rename` (which makes the plate current), `plate_lock`, `plate_select`, `plate_delete` (the deleted plate's object left outside), and undo of a delete.

**Decides by:** Fails when a call errors or an expect misses.
- Each reply's `plate`, `new_plate`, `current`, `name`, `locked`, `changed` and per-plate object counts are exact.
- `plate_list` after each: `total`, which plate is `current`, `name` (including an unnamed plate's ""), `locked`, object counts.
- `object_list`: duplicated plates hold "second" and "first"; after deleting plate 1 its object "second" is listed outside.
- Undo of the last delete gives `total` 3.

**Not checked:**
- The undo is checked by plate count only; the restored plate's object and name are not read.

**Strength:** Strong — every reply and the plate list are matched exactly after each command.

### `plate_icons`
`tests/plate_icons.json` · replay, 27 steps, 15 expects, 0 optional

**Tests:** `{plate_icon, plate}` locators doing what a click on the Prepare view's plate icon does: lock twice, Plate Settings opened and cancelled, a second plate's arrange icon spreading two stacked cubes, the plate's right-click menu listed and run, and move to front.

**Decides by:** Fails when a call errors or one of its 26 matchers misses.
- Listing: `widget_list {query: plate_icon}` `/nodes` ≥ 7.
- Lock: `/locked` true, then false.
- Settings: reply `/dialogs/0/title` contains `Plate`; `dialog_answer` must find "Plate Settings".
- Arrange: before, both cubes on plate 1 at Y offset 128.0; after the icon (`/started`, `/done` true, idle within 2 s), both still on plate 1 with Y offset ≠ 128.0.
- Menu: the listing contains `Edit Plate Name`; running `Lock` gives `/ran` = `Lock`, and the lock icon then reads `/locked` = false, showing the menu had locked it.
- Move to front: `/current` = 0; `app_state` current plate 0 of 2; both cubes on plate 0; no dialog open, no unseen menu.

**Not checked:**
- Arrange is checked only as "Y moved off 128"; the cubes could still overlap, and X is not read.
- The icon count is `ge 7`, not the known set of icons.
- Cancelling Plate Settings is not checked to leave the plate unchanged.

**Strength:** Strong — each icon's effect is read back from plate or object state.
