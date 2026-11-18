# Filaments and settings

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

Two tests, not one. `test_filaments` is about what a filament slot holds and what the project does with it (the
preset in a slot, the unsaved-changes prompt, the flushing volumes, the filaments of objects and parts, the project's
colours and prime tower); `test_settings` is about where a setting applies (a preset, an object, a part, a height
range) and how the settings commands name those targets. They share no state, and each runs on a fresh app of its own,
so the sweep can run them side by side; `test_filaments` also needs presets that nothing has shown yet, which a fresh
app gives it.

## `test_filaments`
`test_filaments.py` · Python · takes the place of `ops_assign_filament_preset_one_slot.json`,
`ops_assign_filament_preset_multi_slot.json` and `multicolor.json`

**Tests:** Assigning a filament preset to a filament slot through each route a person or a plugin has: a pick in the
slot's selector in the sidebar, the selector's value set (`widget_set_value`), `preset_select`, and the plugin API's
`orca.host.preset.filament`. With one slot, where the assignment switches the filament tab, its unsaved-changes prompt
(offered, answered each way, answered ahead with `preset_changes`, and refused where Transfer is not offered). With
three slots, what the Auto-flush preference (`auto_calculate_flush`: All, Color, None) does to the flushing volumes when
a slot's preset or colour changes, the unsaved changes of the preset the tab edits, the plugin API's other forms, the
refusals and the hidden presets shown by name. Then multicolour printing: the filament of an object and of a part
through `settings_set`, and the project's own settings through `project_set_settings` (slot colours, flushing volumes
and multiplier, plate type, prime tower), with what reaches the print and what the project file keeps. It runs on the
harness's default printer, the Bambu Lab X1 Carbon 0.4, on the sweep's fresh app and data folder.

**Terms:** filament presets `B` (Bambu PLA Basic @BBL X1C), `X` (FilAr PLA Blanco Calido, a PLA whose preset has the
default colour `#FFF8E7`), `M` (Bambu PLA Matte @BBL X1C, a PLA with no default colour), `S` (Bambu Support For PLA @BBL
X1C, a support filament of type PLA), `A` (Bambu Support for ABS @BBL X1C, a support filament of type ABS), `G` (Bambu
PETG Basic @BBL X1C) and `H` (Bambu PETG HF @BBL X1C). Only `X` has a default colour. Their `slow_down_layer_time`, from
the profiles: `B`, `X` and `M` 4, `S` 8, `A` 12, `G` 12, `H` 10. Every route resolves a name against the slot's selector,
which takes the name it shows ("FilAr PLA Blanco Calido", "Bambu Support for ABS") or the internal name; a row that
gives a letter passes the internal name, and a row that quotes a name passes it as quoted. The prompt is the dialog
titled "Transfer or discard changes". Auto-flush is the preference `auto_calculate_flush`: All (`all`), Color (`color
change`), None (`disabled`).

The routes, for slot `n` and preset `P`:
- pick: `widget_click {target: {class: "PlaterPresetComboBox", index: n}}` (index 0 is the printer's selector), then
  `widget_click {target: {row: P}}` with the name the selector shows.
- tool: `preset_select {type: "filament", name: P, filament_slot: n}`, with `preset_changes` where the row gives one.
- api: `plugin("orca.host.preset.filament.begin_update(n, P).commit()")`; other forms as the rows give them.

**Reads:**
- `slots()` as in `test_plates`: `filament_is_mixed` from `project_get_settings`, one digit per slot.
- `state()` gives five fields, written `presets | tab | edits | colours | matrix`:
  - presets: each slot's preset, from `app_state` `presets.filaments`: `B X B`.
  - tab: the filament preset `preset_list {type: "filament", limit: 500}` marks `selected`, with `*` when it is also
    `modified`: `B*`.
  - edits: each slot's `slow_down_layer_time`, from `settings_get {type: "filament", filament_slot: n, keys:
    ["slow_down_layer_time"], include_meta: false}`, which reads the tab's unsaved value for a slot whose preset the tab
    edits: `9,4,9`.
  - colours: `filament_colour` from `project_get_settings`. `c3` is `#111111;#222222;#333333` and `cX` is
    `#111111;#FFF8E7;#333333`.
  - matrix: `flush_volumes_matrix` from `project_get_settings`, entries counted from 0 in the order it lists them (row
    = from, column = to). `S3` is the sentinel `0,777,777,777,0,777,777,777,0`. `R2` is slot 2's row and column worked
    out by the app: entries 1, 3, 5 and 7 each above 0 and other than 777, entries 2 and 6 777 and the diagonal 0.
    `Sup` is `0,230,777,700,0,700,777,230,0`: a support filament in slot 2, 230 into it (`g_flush_volume_to_support`)
    and 700 out of it (`g_min_flush_volume_from_support`, above what the greys' colours need).
  After every step of the slot scenarios `state()` is compared with the state its row gives, whole, and
  `app_state.dialogs` is empty unless the row says the prompt is open.
- `dialogs()` gives the open dialogs from `app_state.dialogs` as `{title, buttons}`, a button by its label.
- `plugin(code)` runs `code` with `script_run` and gives what it returns: a transaction's `{target_name,
  selection_changed, slot_number}`, a commit's `{target_name, selection_changed, slot_assignment_changed, shared_slots,
  keys, dirty}`, or, when the plugin API raises, `{raised: {type, message}}`. It asserts what `ops_presets.preset_api`
  only reported.
- `filaments()` gives each object as its filament and its parts' filaments from `object_list`, written `cube 1 (1, 2)
  · cylinder 2`: the cube on filament 1, its part 0 on 1 and its part 1 on 2, the cylinder on 2.
- `item(target, key)` gives one item of `settings_get {target, object, part, keys: [key], include_meta: false}` as
  `{value, effective, source, parts}` and the reply's `note`; a field the reply leaves out is absent.
- `project()` gives the project's own settings, written `colours | multiplier | matrix | plate type | tower`: from
  `project_get_settings`, `filament_colour`, `flush_multiplier`, `flush_volumes_matrix`, `curr_bed_type`, and plate 0's
  `wipe_tower_x` and `wipe_tower_y`; and `plate_list`'s plate 0 `curr_bed_type` and `wipe_tower`, which must agree with
  them.
- `grams()` gives, for each slot, `model_g` and `flush_g`, and the plate's `filament_changes`, from the `summary` of
  `plate_slice_stats {plate: 0}` read after the slice.

A call that the prompt holds replies `blocked_by_dialog`. The test then checks `dialogs()`, answers with
`dialog_answer` (a label, or `cancel`, which closes the prompt), waits for the call's job with `app_wait {for: "job"}`
and checks that the wait was met and no dialog is open: the prompt opens once. Every other call is followed by
`app_wait {for: "idle"}`, which must be met. A refusal is matched by its code where the operations layer gives one,
otherwise by its words, and is always followed by the scenario's full read-back.

The test turns on Simple mode once, with the `mode_simple` action, and checks `app_state.mode` is `simple`: the prompt
lists the settings the mode shows, and `slow_down_layer_time` is one of them. Every scenario starts with `project_new
{discard_changes: true, preset_changes: "discard"}` in the Prepare view; sets Auto-flush to All with `preferences_set`
and checks `preferences_get` reads `all`, since a preference outlives the scenario that sets it; sets the number of slots
with `filament_delete` until it is refused and `filament_add`; and checks `slots()`.

Each case of the slot scenarios starts from `arrange(presets, dirty)`, whose result is checked as the row's start: it
drops the tab's unsaved changes (`settings_set {type: "filament", filament_slot, reset_all: true}` on a slot holding the
preset the tab edits), puts `presets` in the slots with `preset_select {preset_changes: "discard"}`, switches the tab to
slot 1's preset with `settings_set {type: "filament", filament_slot: 1, reset_all: true}`, and sets the colours to
`#111111`, `#222222`, `#333333` (as many as there are slots) and the matrix to the sentinels with one
`project_set_settings` call, the colours first and the whole matrix replacing whatever they recalculated. With `dirty`
it then sets `slow_down_layer_time` 9 on slot 1's preset with `settings_set {type: "filament", filament_slot: 1}`.

**Scenario: hidden filaments, shown by name** (runs first)

One slot, from `arrange([B])`: `B | B | 4 | #111111 | 0`. `X`, `M`, `S` and `A` are hidden on a fresh data folder; a
pick needs them in the selectors.

| # | Step | State after | Also checked |
|---|---|---|---|
| 1 | `preset_list {type: "filament", query: P}` for each of "FilAr PLA Blanco Calido", "Bambu PLA Matte", "Bambu Support For PLA" and "Bambu Support for ABS", then the same with `include_uninstalled: true` | `B \| B \| 4 \| #111111 \| 0` | the first lists none of them; the second's items include `X`, `M`, `S` and `A`, each with `installed: false` |
| 2 | tool "FilAr PLA Blanco Calido" | `X \| X \| 4 \| #FFF8E7 \| 0` | reply `selected` and `shown` `X` |
| 3 | tool "Bambu PLA Matte" | `M \| M \| 4 \| #FFF8E7 \| 0` | reply `selected` and `shown` `M`; the colour stays, as `M` has no default colour |
| 4 | tool "Bambu Support For PLA" | `S \| S \| 8 \| #FFF8E7 \| 0` | reply `selected` and `shown` `S` |
| 5 | tool "Bambu Support for ABS" | `A \| A \| 12 \| #FFF8E7 \| 0` | reply `selected` and `shown` `A` |
| 6 | tool "Bambu PLA Basic" | `B \| B \| 4 \| #FFF8E7 \| 0` | reply `selected` `B` and no `shown` |
| 7 | `preset_list {type: "filament", query: P}` for each of the four | as after 6 | each lists its preset, with no `installed` field: a preset a call showed stays shown |

**Scenario: one slot, a PLA with a default colour**

Every row starts from `arrange([B])`, with `dirty` where the Start column says so: clean is `B | B | 4 | #111111 | 0`,
dirty is `B | B* | 9 | #111111 | 0`. The matrix of one slot has nothing to recalculate, so it stays `0`.

| # | Start | Step | State after | Also checked |
|---|---|---|---|---|
| 1 | clean | pick `X` | `X \| X \| 4 \| #FFF8E7 \| 0` | no dialog opened |
| 2 | clean | tool `X` | as 1 | reply `selected` `X`, and no `shown`, `discarded` or `modified` |
| 3 | clean | api `X` | as 1 | nothing raised; commit `{target_name: X, selection_changed: true, slot_assignment_changed: true, shared_slots: [1], keys: [], dirty: false}` |
| 4 | dirty | pick `X`, answered Transfer | `X \| X* \| 9 \| #FFF8E7 \| 0` | the click replies `blocked_by_dialog`; `dialogs()` is the prompt alone, its buttons including Transfer, Discard and Save |
| 5 | dirty | pick `X`, answered Discard | `X \| X \| 4 \| #FFF8E7 \| 0` | the prompt as in 4 |
| 6 | dirty | pick `X`, answered cancel | `B \| B* \| 9 \| #FFF8E7 \| 0` | the prompt as in 4; the slot and the tab are as at the start, but the colour is `X`'s (see To settle) |
| 7 | dirty | tool `X`, `preset_changes: "transfer"` | as 4 | no dialog opened; reply `selected` `X`, `modified` `{filament: {name: X, keys: [slow_down_layer_time]}}`, no `discarded` |
| 8 | dirty | tool `X`, `preset_changes: "discard"` | as 5 | no dialog opened; reply `selected` `X`, `discarded` `{filament: {name: B, keys: [slow_down_layer_time]}}`, no `modified` |
| 9 | dirty | tool `X`, answered Transfer | as 4 | the call replies `blocked_by_dialog`; the prompt as in 4; the job's result as 7's reply |
| 10 | dirty | tool `X`, answered Discard | as 5 | the prompt as in 4; the job's result as 8's reply |
| 11 | dirty | tool `X`, answered cancel | as 6 | the prompt as in 4; the job's result is not refused: `selected` `B`, `modified` `{filament: {name: B, keys: [slow_down_layer_time]}}`, no `discarded` |
| 12 | dirty | api `X`, answered Transfer | as 4 | the prompt as in 4; commit as in 3 but `dirty: true` |
| 13 | dirty | api `X`, answered Discard | as 5 | the prompt as in 4; commit as in 3 |
| 14 | dirty | api `X`, answered cancel | `B \| B* \| 9 \| #111111 \| 0` | the prompt as in 4; raised `RuntimeError` "Switching the edited filament preset to "FilAr PLA Blanco Calido" for filament slot 1 was canceled; neither the slot assignment nor any setting was changed": the api assigns only after the tab switched, so nothing changed |

**Scenario: one slot, a support filament of another type**

As the scenario before, with `A`, a filament of type ABS, so the prompt offers no Transfer. `A` has no default colour,
so the colour stays `#111111` on every row.

| # | Start | Step | State after | Also checked |
|---|---|---|---|---|
| 1 | clean | pick `A` | `A \| A \| 12 \| #111111 \| 0` | no dialog opened |
| 2 | clean | tool `A` | as 1 | reply `selected` `A` |
| 3 | clean | api `A` | as 1 | commit `{target_name: A, selection_changed: true, slot_assignment_changed: true, shared_slots: [1], keys: [], dirty: false}` |
| 4 | dirty | pick `A`, answered Discard | as 1 | `dialogs()` is the prompt alone, its buttons including Discard and Save and not Transfer |
| 5 | dirty | pick `A`, answered cancel | `B \| B* \| 9 \| #111111 \| 0` | the prompt as in 4 |
| 6 | dirty | tool `A`, `preset_changes: "transfer"` | as 5 | refused, `transfer_not_offered`, before any dialog opened |
| 7 | dirty | tool `A`, `preset_changes: "discard"` | as 1 | reply `discarded` `{filament: {name: B, keys: [slow_down_layer_time]}}` |
| 8 | dirty | tool `A`, answered Discard | as 1 | the prompt as in 4; the job's result as 7's reply |
| 9 | dirty | tool `A`, answered cancel | as 5 | the prompt as in 4; the job's result `selected` `B`, not refused |
| 10 | dirty | api `A`, answered Discard | as 1 | the prompt as in 4; commit as in 3 |
| 11 | dirty | api `A`, answered cancel | as 5 | raised `RuntimeError` "Switching the edited filament preset to "Bambu Support for ABS @BBL X1C" for filament slot 1 was canceled; neither the slot assignment nor any setting was changed" |

**Scenario: one slot, the plugin API's forms that name no preset**

| # | Start | Step | State after | Also checked |
|---|---|---|---|---|
| 1 | clean | `begin_update(1).commit()` | `B \| B \| 4 \| #111111 \| 0` | commit `{target_name: B, selection_changed: false, slot_assignment_changed: false, shared_slots: [1], keys: [], dirty: false}` |
| 2 | clean | `update(1, "slow_down_layer_time", "8")` | `B \| B* \| 8 \| #111111 \| 0` | nothing raised |
| 3 | clean | `update("slow_down_layer_time", "8")` | as 2 | the tab's preset, which slot 1 holds |
| 4 | dirty | `begin_update(1).commit()` | `B \| B* \| 9 \| #111111 \| 0` | no dialog opened; commit as in 1 but `dirty: true` |
| 5 | dirty | `update(1, "slow_down_layer_time", "8")` | as 2 | no dialog opened |
| 6 | dirty | `update("slow_down_layer_time", "8")` | as 2 | |
| 7 | dirty | `begin_update(1, "Bambu PLA Basic").commit()`, the preset slot 1 holds | as 4 | no dialog opened; commit as in 4: nothing switched, nothing assigned |

**Scenario: three slots, what Auto-flush does to the flushing volumes**

`slots()` is `0,0,0`. Every case starts from `arrange([B, B, B])`: `B B B | B | 4,4,4 | c3 | S3`. Each preference is set
with `preferences_set` and read back with `preferences_get` before its cases. Each filament row is run by three routes
on slot 2: the pick and the tool leave the tab on `B`, the api moves it to the preset it names.

| Auto-flush | Into slot 2 | State after (pick, tool) | Tab after api | Also checked |
|---|---|---|---|---|
| All | `X` | `B X B \| B \| 4,4,4 \| cX \| R2` | `X` | entries 1 and 7 (from the greys into the lighter `X`) are each greater than entries 3 and 5 (from `X` into them) |
| All | `M` | `B M B \| B \| 4,4,4 \| c3 \| R2` | `M` | |
| All | `S` | `B S B \| B \| 4,8,4 \| c3 \| Sup` | `S` | |
| Color | `X` | `B X B \| B \| 4,4,4 \| cX \| R2` | `X` | the colour changed, so the volumes are recalculated |
| Color | `M` | `B M B \| B \| 4,4,4 \| c3 \| S3` | `M` | no colour of its own, so nothing is recalculated |
| Color | `S` | `B S B \| B \| 4,8,4 \| c3 \| S3` | `S` | only All recalculates for a support filament |
| None | `X` | `B X B \| B \| 4,4,4 \| cX \| S3` | `X` | the colour follows the preset; the volumes stay |
| None | `M` | `B M B \| B \| 4,4,4 \| c3 \| S3` | `M` | |
| None | `S` | `B S B \| B \| 4,8,4 \| c3 \| S3` | `S` | |

After the filament rows of each preference, one more case from the same start: `project_set_settings {values:
{filament_colour: {index: 1, value: "#FFF8E7"}}}`, the colour picker's change. Under All and Color it leaves `B B B | B
| 4,4,4 | cX | R2`; under None `B B B | B | 4,4,4 | cX | S3`. Where `R2` is expected, the three routes and the colour
change give the same four entries under one preference, and Color gives what All gave for `X`: an assignment
recalculates as a colour change does.

**Scenario: three slots, unsaved changes in the preset the tab edits**

Rows 1 to 7 start from `arrange([B, B, B], dirty)`: `B B B | B* | 9,9,9 | c3 | S3`, every slot holding the preset the
tab edits. Rows 8 and 9 start from `arrange([B, M, B], dirty)`: `B M B | B* | 9,4,9 | c3 | S3`.

| # | Step | State after | Also checked |
|---|---|---|---|
| 1 | pick `X` | `B X B \| B* \| 9,4,9 \| cX \| R2` | no dialog opened: with several slots a pick does not switch the tab, and its changes stay |
| 2 | tool `X` | as 1 | reply `selected` `X`, `modified` `{filament: {name: B, keys: [slow_down_layer_time]}}`, no `discarded` |
| 3 | tool `X`, `preset_changes: "transfer"` | as 1 | not refused; the reply as in 2 |
| 4 | tool `X`, `preset_changes: "discard"` | as 1 | the reply as in 2: nothing is discarded |
| 5 | api `X`, answered Transfer | `B X B \| X* \| 4,9,4 \| cX \| R2` | the call replies `blocked_by_dialog`, the prompt's buttons including Transfer and Discard; commit `{target_name: X, selection_changed: true, slot_assignment_changed: true, shared_slots: [2], keys: [], dirty: true}` |
| 6 | api `X`, answered Discard | `B X B \| X \| 4,4,4 \| cX \| R2` | the prompt as in 5; commit as in 5 but `dirty: false` |
| 7 | api `X`, answered cancel | `B B B \| B* \| 9,9,9 \| c3 \| S3` | the prompt as in 5; raised `RuntimeError` "Switching the edited filament preset to "FilAr PLA Blanco Calido" for filament slot 2 was canceled; neither the slot assignment nor any setting was changed" |
| 8 | api `begin_update(2).commit()`, answered cancel | `B M B \| B* \| 9,4,9 \| c3 \| S3` | the prompt as in 5; raised `RuntimeError` "Switching the edited filament preset to "Bambu PLA Matte @BBL X1C" for filament slot 2 was canceled; neither the slot assignment nor any setting was changed" |
| 9 | api `update(2, "slow_down_layer_time", "8")`, answered cancel | as 8 | the prompt as in 5; the same `RuntimeError` |

**Scenario: three slots, the selector's value and the plugin API's other forms**

No unsaved changes. Each row starts from `arrange` with the presets it names.

| # | Start | Step | State after | Also checked |
|---|---|---|---|---|
| 1 | `B B B` | `widget_set_value {target: {class: "PlaterPresetComboBox", index: 2}, value: "FilAr PLA Blanco Calido"}` | `B X B \| B \| 4,4,4 \| cX \| R2` | as a pick of the row |
| 2 | `B B B` | api `begin_update(2, X)`, not committed | `B X B \| X \| 4,4,4 \| cX \| R2` | the transaction `{target_name: X, selection_changed: true, slot_number: 2}`: the slot is assigned as the transaction is made |
| 3 | `B M B` | api `begin_update(2, "Bambu PLA Matte").commit()` | `B M B \| M \| 4,4,4 \| c3 \| S3` | commit `{target_name: M, selection_changed: true, slot_assignment_changed: false, shared_slots: [2], keys: [], dirty: false}`: the tab switches and nothing is assigned or recalculated |
| 4 | `B M B` | api `begin_update(2).commit()` | as 3 | commit as in 3 |
| 5 | `B M B` | api `t = begin_update(2); t.set("slow_down_layer_time", "8"); t.commit()` | `B M B \| M* \| 4,8,4 \| c3 \| S3` | commit `{target_name: M, selection_changed: true, slot_assignment_changed: false, shared_slots: [2], keys: [slow_down_layer_time], dirty: true}` |
| 6 | `B M B` | api `update(2, "slow_down_layer_time", "8")` | as 5 | nothing raised |
| 7 | `B M B` | api `update("slow_down_layer_time", "8")` | `B M B \| B* \| 8,4,8 \| c3 \| S3` | the preset the tab edits, which slots 1 and 3 share |

**Scenario: three slots, refusals and hidden presets**

Every row starts from `arrange([B, B, B])`, and every refused row leaves `B B B | B | 4,4,4 | c3 | S3`.

| # | Step | Checked |
|---|---|---|
| 1 | tool, slot 5, "FilAr PLA Blanco Calido" | refused, `no_such_slot`, "Filament slot 5 does not exist (the slots are 1..3)" |
| 2 | api `begin_update(5, X).commit()` | raised `IndexError` "Filament slot 5 is out of range; slot numbers are 1-based and this printer has 3 filament slot(s)" |
| 3 | api `begin_update(0, X).commit()` | raised `IndexError` "Filament slot 0 is out of range; slot numbers are 1-based and this printer has 3 filament slot(s)" |
| 4 | api `update(5, "slow_down_layer_time", "8")` | raised `IndexError`, as in 2 |
| 5 | tool, slot 2, "No Such Filament" | refused, `preset_not_offered`, starting "The selector of filament slot 2 does not offer "No Such Filament"." |
| 6 | api `begin_update(2, "No Such Filament").commit()` | raised `ValueError` "The selector of filament slot 2 does not offer "No Such Filament"." |
| 7 | tool, slot 2, "Bambu PLA Basic @BBL A1" | refused, `preset_incompatible`, starting ""Bambu PLA Basic @BBL A1" is not compatible with the selected printer." |
| 8 | api `begin_update(2, "Bambu PLA Basic @BBL A1").commit()` | raised `ValueError` ""Bambu PLA Basic @BBL A1" is not compatible with the selected printer." |
| 9 | api `begin_update("FilAr PLA Blanco Calido")`, with no slot | raised `ValueError` starting "begin_update on the filament editor takes (slot_number, preset_name="")" |
| 10 | `preset_list {type: "filament", query: "Bambu PETG"}`, then with `include_uninstalled: true` | the first lists neither `G` nor `H`; the second lists both with `installed: false`; `state()` as at the start |
| 11 | tool, slot 2, "Bambu PETG Basic" | `state()` is `B G B \| B \| 4,12,4 \| c3 \| R2`; reply `selected` and `shown` `G`; `preset_list {type: "filament", query: "Bambu PETG Basic"}` lists `G` |
| 12 | api `begin_update(2, H).commit()` | `state()` is `B H B \| H \| 4,10,4 \| c3 \| R2`; nothing raised; commit `slot_assignment_changed: true`; `preset_list {type: "filament", query: "Bambu PETG HF"}` lists `H` |

**Scenario: filaments of objects and parts**

`slots()` is `0,0`, both slots `B`. A cube "cube" of 20 × 20 × 10 mm at (100, 100) and a cylinder "cylinder" of 20 × 10
mm at (160, 100) are added with `object_add_primitive`, then a cube part of 5 mm with `part_add {object: 0, type:
"part", shape: "cube", size: [5, 5, 5]}`. The start is checked: `filaments()` is `cube 1 (1, 1) · cylinder 1`;
`item(object 1, extruder)` is `{value: "1", effective: "1", source: "object"}`, as an object added this way is given
filament 1 (`ObjectList::load_mesh_object()`); `item(object 0, extruder)` is `{value: "1", effective: "1", source:
"object", parts: [{part: 1, value: "1"}]}`, as a part added to it is given its object's filament
(`ObjectList::load_generic_subobject()`); and a first slice, the control, gives outcome `ok` and `grams()` slot 1
`model_g` above 0, slot 2 `model_g` 0 and `flush_g` 0, and `filament_changes` 0.

| # | Step | `filaments()` after | Also checked |
|---|---|---|---|
| 1 | `settings_set {target: "object", object: 1, values: {extruder: 2}}` | `cube 1 (1, 1) · cylinder 2` | reply `applied.extruder` "2" and `objects[0].index` 1; `item(object 1, extruder)` `{value: "2", effective: "2", source: "object"}`; `item(object 0, extruder)` as at the start |
| 2 | `undo` | `cube 1 (1, 1) · cylinder 1` | `item(object 1, extruder)` `{value: "1", effective: "1", source: "object"}` |
| 3 | `redo` | `cube 1 (1, 1) · cylinder 2` | `item(object 1, extruder)` as after 1 |
| 4 | `settings_set {target: "part", object: 1, part: 0, values: {extruder: 1}}` | `cube 1 (1, 1) · cylinder 1` | reply `applied.extruder` "1" and `note` "A single-part object's filament is the object's; it was set there."; `item(object 1, extruder)` `{value: "1", effective: "1", source: "object"}` |
| 5 | `settings_set {target: "part", object: 0, part: 1, values: {extruder: 2}}` | `cube 1 (1, 2) · cylinder 1` | reply `applied.extruder` "2", `parts` `[{object: 0, part: 1}]` and no `note`; `item(part 0/1, extruder)` `{value: "2", effective: "2", source: "part"}` with `note` "A part's value (source "part", or in parts) applies only inside that part's volume."; `item(object 0, extruder)` `{value: "1", effective: "1", source: "object", parts: [{part: 1, value: "2"}]}` with the same note |
| 6 | `plate_slice` | as after 5 | outcome `ok`; `grams()` slot 2 `model_g` above 0: the cube's part alone is on filament 2, and the control printed nothing with it |
| 7 | `settings_set {target: "part", object: 0, part: 1, values: {extruder: null}}` | `cube 1 (1, 1) · cylinder 1` | reply `applied.extruder` "1"; `item(part 0/1, extruder)` `{value: "1", effective: "1", source: "part"}`: a part's Default writes its object's filament into the part (`ObjectList::set_extruder_for_items()`) |
| 8 | `settings_set {target: "part", object: 0, part: 1, values: {extruder: 2}}`, then `settings_set {target: "object", object: 1, values: {extruder: 2}}` | `cube 1 (1, 2) · cylinder 2` | `item(part 0/1, extruder)` as after 5; `item(object 1, extruder)` as after 1 |
| 9 | `settings_set {target: "object", object: 0, values: {extruder: 2, wall_loops: 5}}` | `cube 2 (2, 2) · cylinder 2` | reply `applied` `{extruder: "2", wall_loops: "5"}`; `item(object 0, extruder)` `{value: "2", effective: "2", source: "object"}` with no `parts`: the object's filament clears its parts'; `item(object 0, wall_loops)` `{value: "5", effective: "5", source: "object"}` |
| 10 | `undo` | `cube 1 (1, 2) · cylinder 2` | `item(object 0, extruder)` as after 5; `item(object 0, wall_loops)` `{value: null, effective: "2", source: "global"}`: the call was one undo step |
| 11 | `plate_slice` | as after 10 | outcome `ok`; `grams()` slot 2 `model_g` above step 6's: the cylinder prints with filament 2 too; slot 1 `model_g` above 0; `filament_changes` above 0 |

**Scenario: the project's own settings, the print and the file**

`slots()` is `0,0`, slot 1 `B` and slot 2 `M`, set with `preset_select`. The cube, its part and the cylinder of the
scenario before are added the same way, and the part and the cylinder given filament 2 with `settings_set`:
`filaments()` is `cube 1 (1, 2) · cylinder 2`. Then, with `project_set_settings`, the colours `#111111;#222222`, the
matrix `0,777,777,0`, the multiplier 1, the plate type "Cool Plate" and plate 0's prime tower at (60, 150). The start is
checked: `project()` is `#111111;#222222 | 1 | 0,777,777,0 | Cool Plate | 60,150`, and `app_state` `presets.filaments`
is `[B, M]`. `project()` is read after every step; a value it gives is written out in full in the table.

| # | Step | `project()` after | Also checked |
|---|---|---|---|
| 1 | `project_get_settings {keys: [filament_is_mixed, curr_bed_type, prime_volume_mode]}` | `#111111;#222222 \| 1 \| 0,777,777,0 \| Cool Plate \| 60,150` | `filament_is_mixed` "0,0" with `meta.read_only` true; `curr_bed_type`'s `meta.offered` holds "Cool Plate" and "Textured PEI Plate"; `prime_volume_mode` "Default" with `meta.offered` empty: the X1C has no purge mode to choose |
| 2 | `project_set_settings {values: {filament_colour: {index: 0, value: "#ffffff"}}}` | `#FFFFFF;#222222 \| 1 \| 0,<a>,<b>,0 \| Cool Plate \| 60,150` | reply `applied.filament_colour` "#FFFFFF;#222222": stored in upper case. `<a>` and `<b>` are worked out by the app, neither 777, and `<b>` (into white) greater than `<a>` (out of it) |
| 3 | `flush_volumes_matrix: [0, 777, 777, 0]`, then one call with `filament_colour: {index: 1, value: "#000000"}`, `flush_multiplier: 0.7`, `flush_volumes_matrix: {index: 1, value: 400}` and `curr_bed_type: "Textured PEI Plate"` | `#FFFFFF;#000000 \| 0.7 \| 0,400,<c>,0 \| Textured PEI Plate \| 60,150` | `<c>` neither 777 nor 400: the colour went first, its recalculation replaced entry 2, and the entry given replaced entry 1 |
| 4 | `flush_volumes_matrix: [0, 400, 300, 0]` | `#FFFFFF;#000000 \| 0.7 \| 0,400,300,0 \| Textured PEI Plate \| 60,150` | |
| 5 | `wipe_tower_x: {index: 0, value: 150}` and `wipe_tower_y: {index: 0, value: 180}` in one call | `#FFFFFF;#000000 \| 0.7 \| 0,400,300,0 \| Textured PEI Plate \| 150,180` | reply has no `note`: the tower fits there |
| 6 | `wipe_tower_x: {index: 0, value: 40}` | `#FFFFFF;#000000 \| 0.7 \| 0,400,300,0 \| Textured PEI Plate \| 40,180` | |
| 7 | `undo` | `#FFFFFF;#000000 \| 0.7 \| 0,400,300,0 \| Textured PEI Plate \| 150,180` | the move was one undo step of its own |
| 8 | `filament_is_mixed: "1"` | as after 7 | refused, "filament_is_mixed is read-only: mixed filaments are made in the sidebar's mixed filament list"; `slots()` `0,0` |
| 9 | `filament_colour: {index: 1, value: "white"}` with `flush_multiplier: 0.5` in one call | as after 7 | refused, "Slot 2's colour "white" is not #RRGGBB"; the multiplier in the same call is not set either |
| 10 | `prime_volume_mode: "Saving"` | as after 7 | refused, "prime_volume_mode: this printer has no purge mode to choose"; `prime_volume_mode` reads "Default" |
| 11 | `flush_volumes_matrix: {index: 0, value: 10}` | as after 7 | refused, "flush_volumes_matrix[0] (nozzle 1, from slot 1 to slot 1) must be 0: no flush from a filament to itself" |
| 12 | `wipe_tower_x: {index: 0, value: 300}` | as after 7 | refused, "The prime tower of plate 0 would be off the plate" |
| 13 | `plate_slice` | as after 7 | outcome `ok`; `grams()` each slot's `model_g` and `flush_g` above 0, and `filament_changes` above 0 |
| 14 | `flush_volumes_matrix: {index: 1, value: 200}`, then `plate_slice` | `#FFFFFF;#000000 \| 0.7 \| 0,200,300,0 \| Textured PEI Plate \| 150,180` | `grams()`: slot 2's `flush_g` lower than after 13; slot 1's `flush_g` and `filament_changes` as after 13, `flush_g` to 0.01 g |
| 15 | `flush_multiplier: 1.4`, then `plate_slice` | `#FFFFFF;#000000 \| 1.4 \| 0,200,300,0 \| Textured PEI Plate \| 150,180` | `grams()`: each slot's `flush_g` higher than after 14 |
| 16 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 15 | `filaments()` `cube 1 (1, 2) · cylinder 2`; `app_state` `presets.filaments` `[B, M]`; `slots()` `0,0`: the project file keeps them |

**To settle in review:**
- A cancelled prompt with one slot (one slot, rows 6 and 11) leaves the colour of the preset not chosen. The pick and
  the tool write the new preset's default colour before the tab asks (`PlaterPresetComboBox::item_picked()` runs
  `PresetComboBox::update_ams_color()` from `Sidebar::select_filament_preset()` or the selector's own handler); the
  cancel then runs `Tab::on_presets_changed()`, whose `Sidebar::update_presets()` puts the tab's preset back in a
  single slot, so the slot ends on `B` and the tool's reply says `selected` `B`, but nothing restores the colour. The
  api assigns only once the tab switched, so it leaves `#111111` (row 14). The rows expect what the code gives,
  `#FFF8E7` on `B`; this looks like a bug, and if the app is changed to restore the colour, rows 6 and 11 expect
  `#111111`.
- `R2` is checked by properties, since the app works out the volumes from the colours: each recalculated entry other
  than the sentinel and above 0, the four equal across routes and to a colour change, and for `X` the volumes into the
  lighter colour above those out of it. With the X1C's `nozzle_flush_dataset` 0, `FlushVolCalculator::calc_flush_vol_rgb()`
  takes the flush predictor's value where it has one and its colour formula otherwise; both make a dark-to-light change
  cost far more than the reverse.
- `Sup` is exact: 230 into a support filament and `max(<colour volume>, 700)` out of it
  (`Sidebar::auto_calc_flushing_volumes_internal()`); from `#222222` to `#111111` or `#333333` the colour volume plus the
  calculator's minimum (the X1C's 107 mm³ nozzle volume, less the cut retraction where it applies) stays far under 700.
- The prompt's buttons are checked as "including" Transfer, Discard and Save (the dialog also lays out other labelled
  controls), and "not including" Transfer for `A`.
- The flush grams in the print (project, rows 13 to 15). The purge of a filament change is `flush_volumes_matrix[from][to]`
  times `flush_multiplier` (`GCode.cpp`, at least `g_min_purge_volume`, 100 mm³; the X1C's `grab_length` is 0), extruded
  between `; FLUSH_START` and `; FLUSH_END`. The processor charges the first nozzle volume of each purge, 107 mm³ on the
  X1C, to the outgoing filament and the rest to the incoming one (`GCodeProcessor.cpp`, the remaining volume reset at
  each tool change). Entry 1 at 400 or 200 times 0.7 (280 or 140 mm³) is above 107 either way, so a change from slot 1
  to slot 2 charges slot 1 107 mm³ in both slices and slot 2 the rest, and the changes from slot 2 to slot 1 are the
  same: slot 1's `flush_g` stays to 0.01 g while slot 2's falls. At 1.4 both entries grow and so do both slots' grams.
- The preference is set with `preferences_set`, where the replays picked it in the Preferences dialog; the dialog's
  choice control writes the same key (`PreferenceOps.cpp`), and `test_preferences` checks the dialog against
  `preferences_get`.
- Run time: about 60 assignment cases and six slices; the entry may need a `timeout_s` above 900 in `sweep.json`.

**Not checked:**
- Adding and deleting slots, and the mixed filament's rules: `test_plates` (its filament slots scenario). Assigning a
  preset to a mixed slot, by the tool and the plugin API: `test_operation_routes`, which checks the code and the
  words; the state that refusal leaves is not read there either.
- The prompt's Save button: saving the edits as a new preset is `test_presets`'s.
- Whether an assignment takes an undo step: a preset change is not in the project's undo history (as `settings_set`
  says of a preset's), and the snapshots can be read only from the app's log.
- The values of `R2`, `<a>`, `<b>` and `<c>`, beyond the properties above.
- The flushing volumes after a hidden preset's assignment (refusals scenario, rows 11 and 12) beyond `R2`: what an
  assignment does to them is the Auto-flush scenario's.
- `filament_colour_type` and `filament_multi_colour`, which `update_ams_color()` writes beside `filament_colour`: no
  MCP tool lists them, and for a preset that is not an AMS slot's they only mirror `filament_colour` (an empty type,
  the colour itself).
- A text part made with the Text gizmo, which `multicolor.json` gave filament 2: the second part here is a cube from
  `part_add`. Both are model parts, which `settings_set` gives a filament the same way; the gizmo is not this area's.
- How the sidebar shows a slot: the colour swatch and the badge are not read, and no screenshot is taken.
- A printer with several extruders, where the filament grouping applies: `test_printer_extruders`.
- The exact list of plate types the X1C offers: the step checks two of them.
- Which layers print with filament 2: the slice's grams per slot show that filament 2 prints, not where.

**Fixtures:** none; the plugin API calls are the test's own code.

## `test_settings`
`test_settings.py` · Python · takes the place of `settings_targets.json` and its fixture `tests/fixtures/settings_targets.py`

**Tests:** The settings commands' targets. `settings_list` giving a type's settings whole, a saved preset's and a
filament slot's, and the settings an object and a region take; then `settings_set` and `settings_get` on an object, a
part, a modifier and a height range, named by index and taken from the selection, with each value's `effective` and
`source` in the order the slicer applies them; what the targets' settings do in the print; and the height ranges
replaced, the range that stays keeping its settings. It runs on the harness's default printer, the Bambu Lab X1 Carbon
0.4, with the process 0.20mm Standard @BBL X1C (`layer_height` 0.2, `wall_loops` 2, `sparse_infill_density` 15%,
`top_shell_layers` 5).

**Reads:**
- `listed(type, **args)` gives `settings_list`'s reply: `total`, `next_offset` where given, and the items as `{key:
  value}` in order, with whether each has `meta`.
- `item(target, key, ...)` gives one item of `settings_get {target, keys: [key], include_meta: false}` whole: `value`,
  `effective`, `source`, and `parts`, `height_ranges`, `object_effective` and `object_source` where given; and the
  reply's `note` and the target it names (`objects`, `parts`, `height_range`).
- `overrides(target, ...)` gives `settings_get` with the target and no keys as `{key: value}`: what the target sets, a
  filament of 0 (Default) not counting.
- `ranges()` gives object 0's `layer_config_ranges` from `object_list`, as `[{min_z, max_z, values}]`, every key of
  `values` included: a range made by `object_set_height_ranges` carries `extruder` "0" (Default) and the object's
  `layer_height` (`ObjectList::get_default_layer_config()`).
- `targets()` gives the targets' settings, written `O · P0 · P1 · ranges`: `overrides(object 0)`, `overrides(part
  0/0)`, `overrides(part 0/1)` and `ranges()`, a range as `min–max {values}`. Values are written without their quotes.
  It is compared whole after every step of the targets scenario.
- `layers()` gives `plate_slice_stats {include: ["layer_features"], limit: 1000}`'s layers as `{z, height, inner,
  sparse}`, `inner` and `sparse` being the layer's "Inner wall" and "Sparse infill" seconds.

Every scenario starts with `project_new {discard_changes: true, preset_changes: "discard"}` in the Prepare view, deletes
filament slots with `filament_delete` until it is refused, puts `B` (Bambu PLA Basic @BBL X1C) in slot 1 with
`preset_select`, and checks that `app_state.presets` is the X1C, 0.20mm Standard @BBL X1C and `[B]`, and that
`object_list` lists no object: a new project keeps the slots of the project before it.

**Scenario: a type's settings, whole**

| # | Step | Checked |
|---|---|---|
| 1 | `listed(process)` | `total` at least 300, as many items as `total`, no `next_offset` and no item with `meta`; the items include `layer_height` "0.2", `wall_loops` "2", `sparse_infill_density` "15%" and `top_shell_layers` "5" |
| 2 | `listed(printer)` | `total` above 100, whole as in 1; `nozzle_diameter` "0.4", `printable_height` "250" |
| 3 | `listed(filament)` | `total` above 100, whole as in 1; `filament_type` "PLA", `slow_down_layer_time` "4" |
| 4 | `listed(process, include_meta: true)`, then with `offset` 100, 200 and on until a page has no `next_offset` | each page but the last holds 100 items and `next_offset` the next hundred; every item has `meta`; the pages' keys, in order, are step 1's |
| 5 | `listed(process, limit: 50)` | 50 items, `next_offset` 50, none with `meta`; they are step 1's first 50 |
| 6 | `listed(process, preset: "0.16mm High Quality @BBL X1C")` | `layer_height` "0.16"; `app_state.presets.process` is still 0.20mm Standard @BBL X1C: reading a preset does not select it |
| 7 | `filament_add`, then `preset_select {type: "filament", name: "Bambu PETG Basic", filament_slot: 2}` | `app_state.presets.filaments` `[B, Bambu PETG Basic @BBL X1C]`; `listed(filament, filament_slot: 2)` gives `filament_type` "PETG" and `slow_down_layer_time` "12", `listed(filament, filament_slot: 1)` "PLA" and "4" |

**Scenario: what an object and a region take**

| # | Step | Checked |
|---|---|---|
| 1 | `listed(object)` and `listed(region)` | the object list holds more than 200 settings and the region list more than 100; every key of the object list is a process setting with the value `listed(process)` gives it; every key of the region list but `layer_height` is in the object list; both give `wall_loops` "2" and `sparse_infill_density` "15%"; the region list's `layer_height` is "0.2" with `targets` `["height_range"]`, and no other item of either list has `targets` |

**Scenario: targets and precedence**

A cube "cube" of 20 × 20 × 20 mm at the middle of plate 0 (`origin` plus half `size` from `plate_list`), a 5 mm
modifier cube at its centre with `part_add {object: 0, type: "modifier", shape: "cube", size: [5, 5, 5], position: [<x>,
<y>, 10]}`, so it spans 7.5 to 12.5 mm, and the ranges 0 to 5 and 5 to 10 mm with `object_set_height_ranges`. The start
is checked: `object_list` gives the cube with part types `[part, modifier]` and `overrides` `["extruder"]`; `targets()`
is `{extruder: 1} · {} · {} · 0–5 {extruder: 0, layer_height: 0.2}, 5–10 {extruder: 0, layer_height: 0.2}` (an object
added this way is given filament 1, and a modifier's filament 0 does not count). Then the filament's cooling slowdown
is turned off for the scenario's slices, `settings_set {type: "filament", values: {slow_down_for_layer_cooling:
false}}`, and `settings_get {type: "filament", keys: ["slow_down_for_layer_cooling"]}` reads "0": a layer then prints
at its features' own speeds, so their seconds follow their lengths. A first slice is the control: outcome `ok`, every
layer of `layers()` 0.2 mm high to 0.001 mm, and the layers at z 3.0 and 15.0 mm with the same `inner` to 2 %.

| # | Step | `targets()` after | Also checked |
|---|---|---|---|
| 1 | `settings_set {target: "object", object: 0, values: {wall_loops: 4}}` | `{extruder: 1, wall_loops: 4} · {} · {} · 0–5 {extruder: 0, layer_height: 0.2}, 5–10 {extruder: 0, layer_height: 0.2}` | reply `applied` `{wall_loops: "4"}`, `objects[0].index` 0; `object_list` `overrides` `["extruder", "wall_loops"]` |
| 2 | `settings_set {target: "part", object: 0, part: 1, values: {wall_loops: 5}}` | `{extruder: 1, wall_loops: 4} · {} · {wall_loops: 5} · 0–5 {extruder: 0, layer_height: 0.2}, 5–10 {extruder: 0, layer_height: 0.2}` | reply `applied` `{wall_loops: "5"}`, `parts` `[{object: 0, part: 1}]` |
| 3 | `settings_set {target: "height_range", object: 0, min_z: 0, max_z: 5, values: {wall_loops: 6}}` | `{extruder: 1, wall_loops: 4} · {} · {wall_loops: 5} · 0–5 {extruder: 0, layer_height: 0.2, wall_loops: 6}, 5–10 {extruder: 0, layer_height: 0.2}` | reply `applied` `{wall_loops: "6"}`, `objects[0].index` 0, `height_range` `{min_z: 0, max_z: 5}`; `overrides(range 0–5)` `{layer_height: "0.2", wall_loops: "6"}` |
| 4 | `settings_set {target: "height_range", object: 0, min_z: 5, max_z: 10, values: {layer_height: 0.12}}` | `{extruder: 1, wall_loops: 4} · {} · {wall_loops: 5} · 0–5 {extruder: 0, layer_height: 0.2, wall_loops: 6}, 5–10 {extruder: 0, layer_height: 0.12}` | reply `applied` `{layer_height: "0.12"}` |
| 5 | `object_select {parts: [[0, 1]]}`, then `settings_get {target: "part"}` | as after 4 | the reply names `parts` `[{object: 0, part: 1}]`; its items are `wall_loops` "5" alone |
| 6 | `settings_get {target: "part", keys: ["wall_loops"]}`, the part still selected | as after 4 | `{value: "5", effective: "5", source: "part"}`, no `height_ranges`: a modifier that sets its own value keeps it inside a range; `note` "A part's value (source "part", or in parts) applies only inside that part's volume." |
| 7 | `settings_set {target: "part", object: 0, part: 0, values: {sparse_infill_density: "30%"}}` | `{extruder: 1, wall_loops: 4} · {sparse_infill_density: 30%} · {wall_loops: 5} · 0–5 {extruder: 0, layer_height: 0.2, wall_loops: 6}, 5–10 {extruder: 0, layer_height: 0.12}` | reply `applied` `{sparse_infill_density: "30%"}` |
| 8 | `item(part 0/1, sparse_infill_density)` | as after 7 | `{value: null, effective: null, source: "overlapped_part", object_effective: "15%", object_source: "global", parts: [{part: 0, value: "30%"}]}`; `note` "A part's value (source "part", or in parts) applies only inside that part's volume. Inside a modifier, mesh or painted, that sets none of a key (source "overlapped_part"), the value of the part it overlaps applies: that part's own in parts, else object_effective; which parts it overlaps is not worked out." |
| 9 | `item(object 0, wall_loops)` | as after 7 | `{value: "4", effective: "4", source: "object", parts: [{part: 1, value: "5"}], height_ranges: [{min_z: 0, max_z: 5, value: "6"}]}`; `note` "A part's value (source "part", or in parts) applies only inside that part's volume. A height range's value (source "height_range", or in height_ranges) applies only between its min_z and max_z." |
| 10 | `settings_get {target: "height_range", object: 0, min_z: 0, max_z: 5, keys: ["wall_loops", "sparse_infill_density"], include_meta: false}` | as after 7 | `wall_loops` `{value: "6", effective: "6", source: "height_range", parts: [{part: 1, value: "5"}]}`; `sparse_infill_density` `{value: null, effective: "15%", source: "global", parts: [{part: 0, value: "30%"}]}`; `note` as in 9 |
| 11 | `object_select {objects: [0]}`, then `settings_get {target: "height_range", min_z: 5, max_z: 10, keys: ["layer_height"], include_meta: false}` | as after 7 | the reply names `objects[0].index` 0 and `height_range` `{min_z: 5, max_z: 10}`; the item `{value: "0.12", effective: "0.12", source: "height_range"}`, and `note` "A height range's value (source "height_range", or in height_ranges) applies only between its min_z and max_z.": the selected object stands for the range's object |
| 12 | `plate_slice`, then `layers()` | as after 7 | outcome `ok`; every layer whose z is from 5.3 to 9.7 mm is 0.12 mm high, and every layer from 10.5 to 19.5 mm 0.2 mm, to 0.001 mm; the layer at z 3.0 mm has more `inner` than the layer nearest z 15 mm (six walls in the range, four on the object), and that one more than the control's layer at 15.0 mm (two walls); the layer nearest z 15 mm has more `sparse` than the control's at 15.0 mm (part 0's infill at 30%, not 15%). Then `settings_set {type: "filament", reset_all: true}`, and `slow_down_for_layer_cooling` reads "1" again, so that step 14's new project asks about no unsaved preset |
| 13 | `object_set_height_ranges {object: 0, ranges: [{min_z: 0, max_z: 5}, {min_z: 5, max_z: 12}]}` | `{extruder: 1, wall_loops: 4} · {sparse_infill_density: 30%} · {wall_loops: 5} · 0–5 {extruder: 0, layer_height: 0.2, wall_loops: 6}, 5–12 {extruder: 0, layer_height: 0.2}` | the range that stays keeps its settings; the new one takes the object's layer height and the Default filament |
| 14 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 13 | `object_list` part types `[part, modifier]` and `overrides` `["extruder", "wall_loops"]`: the project file keeps every target's settings |

**To settle in review:**
- The walls and the infill in the print (step 12) are checked as order, not amount. A wall loop more adds about a
  loop's printing time, so six walls print longer than four and four longer than two, with the cube's 20 mm
  cross-section the same at 3 and 15 mm, and the modifier (7.5 to 12.5 mm) and the top and bottom shells away from both
  layers; the control slice shows the two layers print alike without the targets' settings. The sparse infill's time
  follows its area times its density: at 15 mm the two more walls take about a fifth of the infill area and the density
  doubles, so the infill prints about 1.6 times as long. Both comparisons hold only with the cooling slowdown off:
  `CoolingBuffer` otherwise stretches a layer shorter than `slow_down_layer_time` (4 s for `B`) across its features, so a
  feature's seconds would depend on the rest of the layer.
- The layer heights (step 12) are checked away from the range boundaries: where a range starts or ends, the slicer may
  give one or two layers a height between the two, so the windows leave out 0.3 mm on either side.
- An object added with `object_add_primitive` carries `extruder` 1 from the start (`ObjectList::load_mesh_object()`), so
  it is among the object's overrides throughout.
- The list totals stay lower bounds: the count follows the settings the app has, which grows; the lists are checked to
  be whole, paged consistently, and to hold known keys with their values.

**Not checked:**
- Refusals of a key a target does not take, a missing height range and a null `layer_height` on a range:
  `test_additions_refusals`.
- A setting's meta, searches, comparisons and per-variant values: `test_presets`.
- The values of the object and region lists beyond the few known ones: they are compared with `listed(process)`, the
  same process read through another view, so a value wrong in both views passes.
- The modifier's `wall_loops` in the print: inside the infill a modifier with settings of its own becomes a region of
  its own, walled on both sides, so the print shows the region rather than its wall count.
- Which object list keys are object-only: the scenario checks the lists against the process and each other, and a few
  known keys.

**Fixtures:** none; `tests/fixtures/settings_targets.py` leaves with the replay.

## Replays this area replaces

Each leaves the sweep, with its entry, in the commit that adds the tests above.

### `ops_assign_filament_preset_one_slot`
`tests/ops_assign_filament_preset_one_slot.json` · replay, 258 steps, 90 expects, 69 optional

**Tests:** Assigning a filament preset to the only slot, where the assignment also switches the filament tab and its unsaved-changes prompt applies. Each route (sidebar, `preset_select`, plugin API `begin_update(1, name)`) for a PLA (Transfer offered) and a support filament of another type (no Transfer), without and with unsaved changes, with each `preset_changes` value and none, and the prompt answered each way. Then the plugin API forms that name no preset.

**Decides by:** Fails when a non-optional call errors, a fixture raises, or an expect misses. Every expect sits on setup.
- Case setup (`arrange_filaments`, 32 steps): no dialog open, `flush_volumes_matrix` eq `0`, and ` (modified)` on the filament tab in dirty cases.
- Setup: one slot; `auto_calculate_flush` reads `all`.
- Implicit: non-optional calls must succeed without a prompt: the clean sidebar picks, `preset_select` with no `preset_changes` on a clean tab, `transfer` and `discard` for the PLA, `discard` for the support filament. The `await: false` calls in the dirty cases must not be refused at once.

**Not checked:**
- No `probe()` after a call is asserted: whether the slot changed, the tab switched, the edits were transferred or dropped, or the colour settings were written.
- Whether the prompt opens and offers Transfer for the PLA but not for the support filament: `app_wait {for: dialog}` cannot fail, and the answers are optional. `preset_changes: "transfer"` for the support filament is optional, so neither its refusal nor its acceptance is checked.
- The plugin API route cannot fail (`preset_api` catches exceptions), including the `update`/`begin_update` forms without a name.
- `undo_taken` is never asserted.

**Strength:** Smoke — the tool and sidebar routes must succeed without a prompt, but nothing a call leaves is asserted.

### `ops_assign_filament_preset_multi_slot`
`tests/ops_assign_filament_preset_multi_slot.json` · replay, 303 steps, 110 expects, 32 optional

**Tests:** Assigning a filament preset to slot 2 of three through the sidebar selector (row click, and once `widget_set_value`), `preset_select` and the plugin API's `filament.begin_update(2, name)`, under each Auto-flush preference (All, Color, None), for a PLA with a default colour, one without, and a support filament. Then the plugin API's other forms, unsaved changes in the filament the tab edits, the refusals (no such slot, no such name, an incompatible preset, a mixed-colour slot) and a hidden preset.

**Decides by:** Fails when a non-optional call errors, a fixture raises, or an expect misses. Every expect sits on setup; none is on what a call under test left.
- Case setup (`arrange_filaments`, 46 steps): no dialog open (`/result/dialogs` empty), `flush_volumes_matrix` equal to the sentinels `0,777,777,777,0,777,777,777,0`, and in the dirty cases a filament tab name ending ` (modified)`. Because no dialog may be open, a prompt the previous case left open fails the next case.
- Preferences: `auto_calculate_flush` reads `all`, `color change`, `disabled` after each pick in the Preferences dialog.
- Setup: three slots (`/presets/filaments/2` exists, `/3` does not). The Add Mixed Filament dialog opens (title eq), `filament_is_mixed` becomes `0,0,0,1` and goes back to `0,0,0`.
- Implicit: the non-optional `preset_select` calls and sidebar row clicks must not be refused and must not open a prompt, since an awaited call held by a dialog times out after 120 s and fails. That includes the hidden `Bambu PETG Basic` through `preset_select`.

**Not checked:**
- The `probe()` after each call has no expect. The slot mapping, the edited filament preset, the three colour settings, the project's colours and the flushing volumes are only recorded, so whether the flush matrix is recalculated under All, Color or None (the reason for the three passes) is never asserted.
- Every plugin API call goes through `ops_presets.preset_api`, which catches any exception and returns it as data. That route cannot fail any case, including the assignments that should succeed and the hidden `Bambu PETG HF`.
- The refusals (slots 5 and 0, `No Such Filament`, `Bambu PLA Basic @BBL A1`, the mixed slot, `begin_update` without a slot) are optional `preset_select` steps or caught exceptions. Neither the refusal nor its code or text is asserted.
- The unsaved-changes prompt: `app_wait {for: dialog}` returns `satisfied: false` on timeout without failing, and the Transfer/Discard/cancel `dialog_answer` steps and "second prompt" waits are optional. Whether a prompt opens, and which buttons it offers, is not checked.
- `undo_taken` is never asserted.
- `arrange_filaments` does not assert the slots it set (`/result/slots`).

**Strength:** Smoke — the tool and sidebar routes must succeed without a prompt, but nothing a call leaves is asserted.

### `multicolor`
`tests/multicolor.json` · replay, 56 steps, 26 expects, 5 optional

**Tests:** Filaments assigned to objects and parts through `settings_set`'s object and part targets, read back with `settings_get`'s effective value and source, with undo and redo; then the project's slot colours, flush multiplier, a flush-matrix entry, plate type and prime tower position through `project_set_settings`, refusals that should change nothing, and a slice.

**Decides by:** Fails when a non-optional call errors or one of its 55 matchers misses.
- Object filament: `/applied/extruder` = `"2"`; effective `"2"` with source `object` on object 1, `"1"` on object 0; undo gives `"1"`, redo `"2"`.
- Parts: part 0 of a one-part object sets the object (`"1"`, with a `/note`); a text part takes `"2"` with source `part` and is listed under the object's `/items/0/parts/0` (part 1, `"2"`); `null` resets it to the object's `"1"`.
- One undo step: after `extruder` and `wall_loops` in one call and an undo, `extruder` effective `"1"`, the part's `"2"` back, `wall_loops` value null with source `global`.
- Project settings: meta (`read_only`, `offered`), `filament_colour` `^#FFFFFF;#000000`, `flush_multiplier` `"0.7"`, matrix `^0,400,` and bed type, all read back with `project_get_settings`; after four refused calls the colour, multiplier, matrix and `filament_is_mixed` still match.
- Prime tower: x/y set to 150/180, then x to 40, undo back to 150/180.
- Slice: `/status` = `done`, outcome `ok`.

**Not checked:**
- The refused `prime_volume_mode: "Saving"` and `wipe_tower_x: 300` calls are optional and never read back; if either were accepted the test still passes.
- The refused colour `"white"` targets slot 0, which is already `#FFFFFF`; only the multiplier in the same call shows the call was refused.
- The slice is checked only for `ok`; nothing shows the cylinder and the text part print with filament 2.
- `Add one filament` is checked only by `/presets/filaments/1` existing, which a datadir with several slots satisfies without the click.
- `/note` matchers are `exists`; `filaments.png` is saved and never compared.

**Strength:** Partial — filament targets and project settings are read back exactly, but two refusals and the slice's use of filament 2 are unchecked.

### `settings_targets`
`tests/settings_targets.json` · replay, 39 steps, 83 expects, 0 optional

**Tests:** `settings_list` returning a type whole, a saved preset's and a slot's values, and what an object and a region take; `settings_set`/`settings_get` on targets object, part and height range by index and from the selection, with `effective` and `source` in the slicer's precedence; ranges replaced keeping the one that stays.

**Decides by:** Fails when a call errors, a fixture raises, or an expect misses.
- Whole lists (`listing()`): process ≥ 300, printer and filament > 100 settings, items equal to total, no next page; with meta 100 a page and `limit` 50 pages without meta.
- Values (`value_of()`): `layer_height` "0.2", "0.16" in the named saved preset, slot 2 "PETG", slot 1 "PLA".
- `nested()`: the object list has > 200 keys, all process keys with the process's values; the region list > 100 keys, all object keys except `layer_height`, whose targets are exactly ["height_range"].
- Per target: `settings_set` replies `applied` and name the object, part or range; `object_list` reads the range values "6" and "0.12"; `overrides()` lists each target's own overrides (4, 5, 6; the range also has `layer_height`, no `extruder`).
- Precedence: the selected part reads "5" from source "part"; the modifier over a part that sets `sparse_infill_density` reads source "overlapped_part" with the part's "30%" and a note; the object reads effective "4" with the part's "5" and range's "6"; the range reads effective "6" from "height_range" and the density null from "global"; the selected object stands for the range target ("0.12").
- Replacing ranges keeps the lower range's "6", gives the new upper range `max_z` 12 at "0.2" with no `wall_loops`; the plate slices "ok".
- Fixture `settings_targets.py` asserts nothing; it returns totals, values and key lists computed from the tools' replies.

**Not checked:**
- The slice is checked only as "ok"; no printed result shows the object, part or range `wall_loops`.
- `nested()` compares two `settings_list` views with each other, not with known values, so a fault shared by both passes.
- Loose: list totals as lower bounds; the range's `layer_height` override and `object_effective` checked with `exists` where the values are known; `settings_get {target: part}` without keys checks only that an item has a key.

**Strength:** Strong — target naming, values, effective values and sources are matched exactly for every target.
