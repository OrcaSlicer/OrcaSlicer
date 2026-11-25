# Filaments and settings

The design of this area's sweep tests. The rules every entry follows and the grades are in
[TESTS.md](../TESTS.md).

Two tests. `test_filaments` is about the filament list: what a slot holds and what the project does with it (the
preset in a slot, the unsaved-changes prompt, the flushing volumes, the filaments of objects and parts, merging one
slot into another, the slot colours and the prime tower), and its mixed filaments (adding, changing and breaking one,
and its dialogs). `test_settings` is about where a setting applies (a preset, an object, a part, a height range) and
how the settings commands name those targets. They share no state: each sets the start it needs, so the sweep can
run them side by side, or one after the other in one app on one data folder. `test_filaments` also needs filament
presets the selectors do not offer yet, which it makes so itself before its first scenario. The filament list's scenarios and
its mixed filaments' are one test, on one launch, because they share their printer and their start; `test_settings`
reads no filament list and stays a test of its own.

A matrix of cases runs one case for each code path it reaches, and its scenario names the path each row stands for.
Where the routes into a slot meet in one function, a route is checked once where it enters and the branches behind
that function by one route.

## `test_filaments`
`test_filaments.py` · Python

**Tests:** Assigning a filament preset to a filament slot through each route a person or a plugin has: a pick in the
slot's selector in the sidebar, the selector's value set (`widget_set_value`), `preset_select`, and the plugin API's
`orca.host.preset.filament`. With one slot, where the assignment switches the filament tab, its unsaved-changes prompt
(offered, answered each way, answered ahead with `preset_changes`, and refused where Transfer is not offered). With
three slots, what the Auto-flush preference (`auto_calculate_flush`: All, Color, None) does to the flushing volumes when
a slot's preset or colour changes, the unsaved changes of the preset the tab edits, the plugin API's other forms, the
refusals and the hidden presets shown by name. Then multicolour printing: the filament of an object and of a part
through `settings_set`, one slot merged into another with `filament_delete`, the filament list's slot colours,
flushing volumes and multiplier through `filament_set_colour` and `filament_set_flushing`, and the project's plate
type and prime tower through `project_set_settings`, with what reaches the print and what the project file keeps.
Last, the mixed filaments of the filament list against the sidebar's mixed filament list: reading them with
`filament_list`; adding one with `filament_add_mixed` and changing one with `filament_edit_mixed`, each from its
arguments and through its dialog, where the gradient is set; a mixed slot having no preset or colour of its own (the
preset and settings tools refuse it); and what deleting or merging a filament that a mix mixes does to the mix, as the
sidebar does it: broken, and repaired by an edit.

Every scenario runs on the Generic Klipper Printer 0.4. Nothing they check is a Bambu Lab printer's own: their
filaments come from the Orca Filament Library, which fits every printer, and the colours, the flushing volumes, the
prompt, the plugin API and the mixed filaments work the same on any printer with one extruder and several filaments.
The project's own settings scenario sets the plate type, whose selector the sidebar shows only for a Bambu Lab printer
or one with `support_multi_bed_types`, which no Custom printer sets (without it the Klipper printer refuses the plate
type: "curr_bed_type: this printer has a single plate type"), so that scenario turns the printer's
`support_multi_bed_types` on for itself. The Klipper printer purges each filament change into the prime tower, where
the slice counts the purge as `tower_g` and leaves `flush_g` 0, so the flush settings are shown in the print through
`tower_g`.

**Starts from:** each scenario's own `start()`, and the filaments it shows by name hidden before its first scenario
(the presets' letters are given under Terms):
- `start(client, filaments=[B] * n)` for each scenario before the project's own settings, `n` being the slots it
  needs: a new project on `MyKlipper 0.4 nozzle` with `0.20mm Standard @MyKlipper` and `n` slots holding `B`.
  `presets()` is `{printer: "MyKlipper 0.4 nozzle", process: "0.20mm Standard @MyKlipper", filaments: [B, …]}`, one
  `B` per slot. The baseline's Auto-flush All, which the slot scenarios' volumes follow, is read back with
  `preferences_get` as `all`.
- `start(client, filaments=[B, M])` for the project's own settings, then `settings_set {type: "printer", values:
  {support_multi_bed_types: true}}`, which `settings_get` reads as "1": an unsaved edit of the printer preset, which
  turns on its plate types and which the next `start()` discards. `presets()` is `{printer: "MyKlipper 0.4 nozzle",
  process: "0.20mm Standard @MyKlipper", filaments: [B, M]}`.
- The mixed start, for each of the mixed filaments' scenarios: `start(client)`, whose `presets()` must be `{printer:
  "MyKlipper 0.4 nozzle", process: "0.20mm Standard @MyKlipper", filaments: [B]}`, then `add_slot` twice, with
  `#00C1AE` and `#F4E2C1`, the baseline's colours of slots 2 and 3, or with `#FF0000` and `#0000FF` in a scenario that
  reads a mix's colour, which then checks that `filament_list` gives slots 2 and 3 those two: `slots()` is `0,0,0`, and
  the three physical rows of `filament_list` have `preset` "Generic PLA @System" and `type` "PLA". `start()` deletes
  the mixed filaments an earlier test, run or scenario left before it sets one slot, so no scenario meets a mix it did
  not make. The mixed filaments' scenarios read the flushing volumes only for the matrix's size, which no preference
  changes.
- The six filaments it shows by name hidden from the selectors: before its first scenario, after a `start(client)` of
  its own (one slot of `B`, since a filament a slot holds cannot be uninstalled), `filament_install {names: [X, M, S,
  A, G, H]}`, then `filament_uninstall` of the same names, whose reply lists all six under `uninstalled` and has no
  `not_installed`. An uninstall that changes something shows every filament as the installed set says
  (`Ops::uninstall_filaments()`), so it hides one an earlier test showed by name for the app's session, which an
  uninstall of filaments not installed would leave shown, and takes out one an earlier test installed; the install
  makes it change something whatever was left. The hidden scenario's step 1 checks that the four it picks are hidden,
  and the refusals scenario's row 10 that `G` and `H` are.
- The baseline's Simple mode, read back once after that start as `app_state.mode` `simple`: the prompt lists the
  settings the mode shows, and `slow_down_layer_time` is one of them.

**Leaves:** what the last scenario, deleting or merging, leaves on the baseline its `start()` set: the Klipper printer
with `0.20mm Standard @MyKlipper`; one physical slot of `B` in `#00C1AE`, slot 2's colour of the mixed start, which
the deletes moved into slot 1, and two broken mixed filaments, which the sidebar hides beside one physical filament and
which the next test's `start()` replaces (it adds a slot, so that the mixes can be deleted, deletes them, and goes back
to one slot); and its project, unsaved, with the cubes `on_one` and `on_mix`. What an earlier scenario sets beyond
the baseline (the greys and the sentinel matrix, Auto-flush Color and None, the project scenario's colours,
multiplier 1.4, plate type and printer edit) the next scenario's `start()` puts back. `X`, `M`, `S`, `A`, `G`
and `H` not installed, each shown for the app's session since a scenario assigned it; and `filaments.3mf`,
`mixes.3mf` and `gradient.3mf`, saved in the test's temporary folder and deleted with it, at the top of the recent
projects, with that folder as the last one opened. It saves no preset (the mixed slot's refused `preset_save` saves
none), and the filaments it installs it uninstalls again. It restores nothing in a `finally`: every setting it changes
is one `start()` puts back.

A run again on the data folder a failed run left meets what that run left: its slots, mixes, presets and unsaved
changes, which `start()` replaces, deletes and discards, and filaments it showed or installed, which its first start
hides and uninstalls again. Its project files go to a new temporary folder, and it saves no preset, so nothing a
first run leaves stands in a second run's way.

**Terms:** filament presets of the Orca Filament Library, which fits every printer: `B` (Generic PLA @System, the
Klipper printer's default filament), `X` (FilAr PLA Blanco Calido, a PLA whose preset has the default colour
`#FFF8E7`), `M` (Generic PLA Matte @System, a PLA with no default colour), `S` (Bambu Support For PLA @System, a support
filament of type PLA), `A` (Bambu Support for ABS @System, a support filament of type ABS), `G` (Bambu PETG Basic
@System) and `H` (Bambu PETG HF @System). Only `X` has a default colour. Their `slow_down_layer_time`, from the
profiles: `B`, `X` and `M` 4, `S` 8, `A` 12, `G` 12, `H` 10. Every route resolves a name against the slot's selector,
which takes the name it shows ("FilAr PLA Blanco Calido", "Generic PLA Matte", "Bambu Support for ABS") or the
internal name; a row that gives a letter passes the internal name, and a row that quotes a name passes it as quoted.
The prompt is the dialog titled "Transfer or discard changes". Auto-flush is the preference `auto_calculate_flush`:
All (`all`), Color (`color change`), None (`disabled`).

The routes, for slot `n` and preset `P`:
- pick: `widget_click {target: {class: "PlaterPresetComboBox", index: n}}` (index 0 is the printer's selector), then
  `widget_click {target: {row: P}}` with the name the selector shows.
- tool: `preset_select {type: "filament", name: P, filament_slot: n}`, with `preset_changes` where the row gives one.
- api: `plugin("orca.host.preset.filament.begin_update(n, P).commit()")`; other forms as the rows give them.

**Reads:**
- `slots()` as in `test_objects`: the `mixed` of each `filament_list` row, one digit per slot.
- `state()` gives five fields, written `presets | tab | edits | colours | matrix`:
  - presets: each slot's preset, from `app_state` `presets.filaments`: `B X B`.
  - tab: the filament preset `preset_list {type: "filament", limit: 500}` marks `selected`, with `*` when it is also
    `modified`: `B*`.
  - edits: each slot's `slow_down_layer_time`, from `settings_get {type: "filament", filament_slot: n, keys:
    ["slow_down_layer_time"], include_meta: false}`, which reads the tab's unsaved value for a slot whose preset the tab
    edits: `9,4,9`.
  - colours: each `filament_list` row's `colour`, joined by `;`. `c3` is `#111111;#222222;#333333` and `cX` is
    `#111111;#FFF8E7;#333333`.
  - matrix: `flush_volumes_matrix` from `filament_list`, entries counted from 0 in the order it lists them (row
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
- `project()` gives the filament list's and the project's own settings, written `colours | multiplier | matrix | plate
  type | tower`: from `filament_list`, the rows' `colour` joined by `;`, `flush_multiplier` and `flush_volumes_matrix`;
  from `project_get_settings`, `curr_bed_type` and plate 0's `wipe_tower_x` and `wipe_tower_y`; and `plate_list`'s plate
  0 `curr_bed_type` and `wipe_tower`, which must agree with them.
- `grams()` gives, for each slot, `model_g`, `flush_g` and `tower_g`, and the plate's `filament_changes`, from the
  `summary` of `plate_slice_stats {plate: 0}` read after the slice.
- `mixes()` gives `filament_list`'s mixed rows, each written `slot = components @ ratios`, then `, bottom b top t` and
  `, per part` for a gradient, and `, broken` for a broken mix, the rows joined by `·`: `4 = 2,3 @ 50,50` is slot 4
  mixing slots 2 and 3 half and half; `4 = 2,3 @ 50,50, bottom 3 top 2, per part` the same with a gradient from slot 3
  at the bottom to slot 2 at the top, each part on its own; `3 = 0,1 @ 50,50, broken` a mix whose first component was
  deleted. A gradient whose `curve` is not `linear` fails every row: no step draws one. After every step of the mixed
  filaments' scenarios `slots()` and `mixes()` are compared with the row whole.
- `on(name)` gives the filament of the object named so, from `object_list`.

The data folder holds what other tests leave beside these presets: user filament presets, such as the "MCP …"
presets of `test_presets` and its calibration scenarios, and the filaments other tests install, which the selectors
and `preset_list` list too. No check reads those lists whole: the tab is the item `preset_list` marks `selected`, the
hidden checks look each preset up by its name, and every route names the preset it assigns.

A call that a dialog holds (the prompt, or a mixed filament's dialog) replies `blocked_by_dialog`. The test then
checks `dialogs()`, answers as the row says (with `dialog_answer`, a label or `cancel`, which closes the dialog, or by
the dialog's own controls), waits for the call's job with `app_wait {for: "job"}` and checks that the wait was met and
no dialog is open: the dialog opens once. In the slot scenarios every other call is followed by `app_wait {for:
"idle"}`, which must be met. A refusal is matched by its code where the operations layer gives one, otherwise by its
words, and is always followed by the scenario's full read-back.

Every scenario before the mixed filaments' starts with its `start()`, checks `presets()` against the start the
scenario gives, reads Auto-flush back as All, and checks `slots()`. Each mixed filaments' scenario starts with the
mixed start.

Each case of the slot scenarios starts from `arrange(presets, dirty)`, whose result is checked as the row's start: it
drops the tab's unsaved changes (`settings_set {type: "filament", filament_slot, reset_all: true}` on a slot holding the
preset the tab edits), puts `presets` in the slots with `preset_select {preset_changes: "discard"}`, switches the tab to
slot 1's preset with `settings_set {type: "filament", filament_slot: 1, reset_all: true}`, and sets the colours to
`#111111`, `#222222`, `#333333` (as many as there are slots) with `filament_set_colour`, slot by slot, then the matrix
to the sentinels with `filament_set_flushing`, the whole matrix replacing whatever the colours recalculated. With `dirty`
it then sets `slow_down_layer_time` 9 on slot 1's preset with `settings_set {type: "filament", filament_slot: 1}`.

**Scenario: hidden filaments, shown by name** (runs first)

One slot, from `arrange([B])`: `B | B | 4 | #111111 | 0`. `X`, `M`, `S` and `A` are hidden, as the start leaves them;
a pick needs them in the selectors, so the later scenarios' picks rely on this one's showing them.

| # | Step | State after | Also checked |
|---|---|---|---|
| 1 | `preset_list {type: "filament", query: P}` for each of "FilAr PLA Blanco Calido", "Generic PLA Matte", "Bambu Support For PLA" and "Bambu Support for ABS", then the same with `include_uninstalled: true` | `B \| B \| 4 \| #111111 \| 0` | the first lists none of them; the second's items include `X`, `M`, `S` and `A`, each with `installed: false` |
| 2 | tool "FilAr PLA Blanco Calido" | `X \| X \| 4 \| #FFF8E7 \| 0` | reply `selected` and `shown` `X` |
| 3 | tool "Generic PLA Matte" | `M \| M \| 4 \| #FFF8E7 \| 0` | reply `selected` and `shown` `M`; the colour stays, as `M` has no default colour |
| 4 | tool "Bambu Support For PLA" | `S \| S \| 8 \| #FFF8E7 \| 0` | reply `selected` and `shown` `S` |
| 5 | tool "Bambu Support for ABS" | `A \| A \| 12 \| #FFF8E7 \| 0` | reply `selected` and `shown` `A` |
| 6 | tool "Generic PLA" | `B \| B \| 4 \| #FFF8E7 \| 0` | reply `selected` `B` and no `shown` |
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
so the colour stays `#111111` on every row. Every row starts dirty, `B | B* | 9 | #111111 | 0`. The rows are what the
filament's type changes: the prompt without Transfer (`transfer_refusal()` in `Ops::assign_filament_preset()` and
the prompt's own buttons), the refused Transfer ahead, Discard ahead, and the cancel by each route, which with no
default colour leaves the slot's colour as it was. A clean assignment by each route, and Discard answered by the tool
or the api, run the code of the PLA scenario's rows 1 to 3, 10 and 13 with another preset, and are not repeated here.

| # | Step | State after | Also checked |
|---|---|---|---|
| 1 | pick `A`, answered Discard | `A \| A \| 12 \| #111111 \| 0` | `dialogs()` is the prompt alone, its buttons including Discard and Save and not Transfer |
| 2 | pick `A`, answered cancel | `B \| B* \| 9 \| #111111 \| 0` | the prompt as in 1 |
| 3 | tool `A`, `preset_changes: "transfer"` | as 2 | refused, `transfer_not_offered`, before any dialog opened |
| 4 | tool `A`, `preset_changes: "discard"` | as 1 | no dialog opened; reply `selected` `A`, `discarded` `{filament: {name: B, keys: [slow_down_layer_time]}}` |
| 5 | tool `A`, answered cancel | as 2 | the prompt as in 1; the job's result `selected` `B`, not refused |
| 6 | api `A`, answered cancel | as 2 | the prompt as in 1; raised `RuntimeError` "Switching the edited filament preset to "Bambu Support for ABS @System" for filament slot 1 was canceled; neither the slot assignment nor any setting was changed" |

**Scenario: one slot, the plugin API's forms that name no preset**

| # | Start | Step | State after | Also checked |
|---|---|---|---|---|
| 1 | clean | `begin_update(1).commit()` | `B \| B \| 4 \| #111111 \| 0` | commit `{target_name: B, selection_changed: false, slot_assignment_changed: false, shared_slots: [1], keys: [], dirty: false}` |
| 2 | clean | `update(1, "slow_down_layer_time", "8")` | `B \| B* \| 8 \| #111111 \| 0` | nothing raised |
| 3 | clean | `update("slow_down_layer_time", "8")` | as 2 | the tab's preset, which slot 1 holds |
| 4 | dirty | `begin_update(1).commit()` | `B \| B* \| 9 \| #111111 \| 0` | no dialog opened; commit as in 1 but `dirty: true` |
| 5 | dirty | `update(1, "slow_down_layer_time", "8")` | as 2 | no dialog opened |
| 6 | dirty | `update("slow_down_layer_time", "8")` | as 2 | |
| 7 | dirty | `begin_update(1, "Generic PLA").commit()`, the preset slot 1 holds | as 4 | no dialog opened; commit as in 4: nothing switched, nothing assigned |

**Scenario: three slots, what Auto-flush does to the flushing volumes**

`slots()` is `0,0,0`. Every case starts from `arrange([B, B, B])`: `B B B | B | 4,4,4 | c3 | S3`. All is the baseline's,
read back at the scenario's start; Color and None are each set with `preferences_set` and read back with
`preferences_get` before their rows, which follow it in the table.

The three routes into a slot meet in `Plater::priv::filament_preset_picked()`, where the preference is read and the
volumes are recalculated: the pick enters through `Plater::priv::on_select_preset()`, the tool and the api through
`Ops::assign_filament_preset()` and `Sidebar::select_filament_preset()`, which also runs the selector's
`item_picked()`, whose colour change the Color preference recalculates on. So row 1 runs by all three routes on slot 2,
each case showing that its route reaches the recalculation, and every other filament row by the tool alone, each
standing for one branch behind that function: a filament with a colour of its own (`X`), one without (`M`) and a
support filament (`S`), under each preference. The pick and the tool leave the tab on `B`; the api moves it to the
preset it names. The colour picker's change, `filament_set_colour {filament_slot: 2, colour: "#FFF8E7"}`, is the
other way into the recalculation, run once under each preference.

| # | Auto-flush | Step on slot 2 | State after | Also checked |
|---|---|---|---|---|
| 1 | All | pick, tool and api `X`, each from its own `arrange` | `B X B \| B \| 4,4,4 \| cX \| R2`, the tab `X` after the api | entries 1 and 7 (from the greys into the lighter `X`) are each greater than entries 3 and 5 (from `X` into them); the three routes give the same four entries |
| 2 | All | tool `M` | `B M B \| B \| 4,4,4 \| c3 \| R2` | |
| 3 | All | tool `S` | `B S B \| B \| 4,8,4 \| c3 \| Sup` | |
| 4 | All | the colour picker's change | `B B B \| B \| 4,4,4 \| cX \| R2` | the four entries are row 1's |
| 5 | Color | tool `X` | `B X B \| B \| 4,4,4 \| cX \| R2` | the colour changed, so the volumes are recalculated; the four entries are row 1's |
| 6 | Color | tool `M` | `B M B \| B \| 4,4,4 \| c3 \| S3` | no colour of its own, so nothing is recalculated |
| 7 | Color | tool `S` | `B S B \| B \| 4,8,4 \| c3 \| S3` | only All recalculates for a support filament |
| 8 | Color | the colour picker's change | `B B B \| B \| 4,4,4 \| cX \| R2` | the four entries are row 1's |
| 9 | None | tool `X` | `B X B \| B \| 4,4,4 \| cX \| S3` | the colour follows the preset; the volumes stay |
| 10 | None | tool `M` | `B M B \| B \| 4,4,4 \| c3 \| S3` | |
| 11 | None | tool `S` | `B S B \| B \| 4,8,4 \| c3 \| S3` | |
| 12 | None | the colour picker's change | `B B B \| B \| 4,4,4 \| cX \| S3` | |

Where `R2` is expected, the routes of row 1, the tool under Color and the colour changes give the same four entries:
an assignment recalculates as a colour change does, and on the Klipper printer the preset a slot holds does not change
what the calculator starts from (see To settle).

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
| 8 | api `begin_update(2).commit()`, answered cancel | `B M B \| B* \| 9,4,9 \| c3 \| S3` | the prompt as in 5; raised `RuntimeError` "Switching the edited filament preset to "Generic PLA Matte @System" for filament slot 2 was canceled; neither the slot assignment nor any setting was changed" |
| 9 | api `update(2, "slow_down_layer_time", "8")`, answered cancel | as 8 | the prompt as in 5; the same `RuntimeError` |

**Scenario: three slots, the selector's value and the plugin API's other forms**

No unsaved changes. Each row starts from `arrange` with the presets it names.

| # | Start | Step | State after | Also checked |
|---|---|---|---|---|
| 1 | `B B B` | `widget_set_value {target: {class: "PlaterPresetComboBox", index: 2}, value: "FilAr PLA Blanco Calido"}` | `B X B \| B \| 4,4,4 \| cX \| R2` | as a pick of the row |
| 2 | `B B B` | api `begin_update(2, X)`, not committed | `B X B \| X \| 4,4,4 \| cX \| R2` | the transaction `{target_name: X, selection_changed: true, slot_number: 2}`: the slot is assigned as the transaction is made |
| 3 | `B M B` | api `begin_update(2, "Generic PLA Matte").commit()` | `B M B \| M \| 4,4,4 \| c3 \| S3` | commit `{target_name: M, selection_changed: true, slot_assignment_changed: false, shared_slots: [2], keys: [], dirty: false}`: the tab switches and nothing is assigned or recalculated |
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
| 7 | tool, slot 2, "Generic PLA @MyToolChanger" | refused, `preset_incompatible`, starting ""Generic PLA @MyToolChanger" is not compatible with the selected printer.": a filament of the Custom vendor, which every data folder installs, for its MyToolChanger printers alone |
| 8 | api `begin_update(2, "Generic PLA @MyToolChanger").commit()` | raised `ValueError` ""Generic PLA @MyToolChanger" is not compatible with the selected printer." |
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
`model_g` above 0, slot 2 `model_g`, `flush_g` and `tower_g` 0, and `filament_changes` 0. The Klipper printer purges a
filament change into the prime tower, so the purges of the slices below are in `tower_g`, which no row here checks
(the project scenario does), and `flush_g` stays 0.

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
| 10 | `undo` | `cube 1 (1, 2) · cylinder 2` | `item(object 0, extruder)` as after 5; `item(object 0, wall_loops)` `{value: null, effective: "3", source: "global"}`, the `wall_loops` of `0.20mm Standard @MyKlipper`: the call was one undo step |
| 11 | `plate_slice` | as after 10 | outcome `ok`; `grams()` slot 2 `model_g` above step 6's: the cylinder prints with filament 2 too; slot 1 `model_g` above 0; `filament_changes` above 0 |

**Scenario: one slot merged into another**

From `start(client, filaments=[B, B, B])`: `slots()` is `0,0,0`. The colours `#FF0000`, `#00FF00` and `#0000FF` are set
on slots 1 to 3 with `filament_set_colour`, and the cubes `on_two`, 20 mm at (80, 128), and `on_three`, 20 mm at (176,
128), are added with `object_add_primitive` and put on filaments 2 and 3 with `settings_set {target: "object", object:
n, values: {extruder: m}}`. The start is checked: `filaments()` is `on_two 2 · on_three 3`, and the colours read
`#FF0000;#00FF00;#0000FF`. This is the merge of a physical slot into another, `Sidebar::delete_filament()` given the
slot to merge into, which moves the deleted slot's objects to that slot and the slots after it down by one; the mixed
filaments' scenarios merge into a mix.

| # | Step | `slots()` after | Also checked |
|---|---|---|---|
| 1 | `filament_delete {filament_slot: 2, merge_into: 1}` | `0,0` | reply `{filament_slot: 2, merged_into: 1, filament_slots: 2}`; `filaments()` `on_two 1 · on_three 2`; the colours `#FF0000;#0000FF`; `presets().filaments` `[B, B]`: the slot after the deleted one moved down, with its objects and its colour |

**Scenario: the slot colours, the flushing and the project's own settings, the print and the file**

From `start(client, filaments=[B, M])` and the printer's plate types turned on (see Starts from): `presets()` is
`{printer: "MyKlipper 0.4 nozzle", process: "0.20mm Standard @MyKlipper", filaments: [B, M]}`, `support_multi_bed_types`
reads "1" and `slots()` is `0,0`. The cube, its part and the cylinder of the scenario before are added the same way,
and the part and the cylinder given filament 2 with `settings_set`: `filaments()` is `cube 1 (1, 2) · cylinder 2`.
Then the colours `#111111;#222222` with `filament_set_colour`, the matrix `0,777,777,0` and the multiplier 1 with
`filament_set_flushing`, and the plate type "Cool Plate" and plate 0's prime tower at (60, 150) with
`project_set_settings`. The start is checked: `project()` is
`#111111;#222222 | 1 | 0,777,777,0 | Cool Plate | 60,150`, and `presets().filaments` is `[B, M]`. `project()` is read
after every step; a value it gives is written out in full in the table.

| # | Step | `project()` after | Also checked |
|---|---|---|---|
| 1 | `filament_list`, then `project_get_settings {keys: [curr_bed_type]}` | `#111111;#222222 \| 1 \| 0,777,777,0 \| Cool Plate \| 60,150` | both rows `mixed` false; `prime_volume_mode` "Default" and no `prime_volume_modes`: the Klipper printer has no purge mode to choose; `curr_bed_type`'s `meta.offered` holds "Cool Plate" and "Textured PEI Plate" |
| 2 | `filament_set_colour {filament_slot: 1, colour: "#ffffff"}` | `#FFFFFF;#222222 \| 1 \| 0,<a>,<b>,0 \| Cool Plate \| 60,150` | reply `{filament_slot: 1, colour: "#FFFFFF", changed: true}`: stored in upper case. `<a>` and `<b>` are worked out by the app, neither 777, and `<b>` (into white) greater than `<a>` (out of it) |
| 3 | `filament_set_flushing {flush_volumes_matrix: [0, 777, 777, 0]}`, `filament_set_colour {filament_slot: 2, colour: "#000000"}`, `filament_set_flushing {flush_multiplier: 0.7, flush_volumes_matrix: {index: 1, value: 400}}` and `project_set_settings {values: {curr_bed_type: "Textured PEI Plate"}}` | `#FFFFFF;#000000 \| 0.7 \| 0,400,<c>,0 \| Textured PEI Plate \| 60,150` | `<c>` neither 777 nor 400: the colour's recalculation replaced entry 2, and the entry given after it replaced entry 1 |
| 4 | `filament_set_flushing {flush_volumes_matrix: [0, 400, 300, 0]}` | `#FFFFFF;#000000 \| 0.7 \| 0,400,300,0 \| Textured PEI Plate \| 60,150` | reply `applied` holds `flush_volumes_matrix` |
| 5 | `project_set_settings` with `wipe_tower_x: {index: 0, value: 150}` and `wipe_tower_y: {index: 0, value: 180}` in one call | `#FFFFFF;#000000 \| 0.7 \| 0,400,300,0 \| Textured PEI Plate \| 150,180` | reply has no `note`: the tower fits there |
| 6 | `wipe_tower_x: {index: 0, value: 40}` | `#FFFFFF;#000000 \| 0.7 \| 0,400,300,0 \| Textured PEI Plate \| 40,180` | |
| 7 | `undo` | `#FFFFFF;#000000 \| 0.7 \| 0,400,300,0 \| Textured PEI Plate \| 150,180` | the move was one undo step of its own |
| 8 | `project_set_settings {values: {flush_multiplier: 0.5}}`, then `project_get_settings {keys: [filament_colour]}` | as after 7 | refused, saying "flush_multiplier" "belongs to the filament list" and naming `filament_set_flushing`; the read gives no item, `unknown_keys` `[filament_colour]` and `type_of` `{filament_colour: "filament_list"}` |
| 9 | `filament_set_flushing {flush_multiplier: 0.5, flush_volumes_matrix: {index: 0, value: 10}}` | as after 7 | refused, "flush_volumes_matrix[0] (nozzle 1, from slot 1 to slot 1) must be 0: no flush from a filament to itself"; the multiplier in the same call is not set either |
| 10 | `filament_set_flushing {prime_volume_mode: "Saving"}` | as after 7 | refused, "prime_volume_mode: this printer has no purge mode to choose"; `prime_volume_mode` reads "Default" |
| 11 | `filament_set_colour {filament_slot: 2, colour: "white"}` | as after 7 | refused, "Slot 2's colour "white" is not #RRGGBB" |
| 12 | `project_set_settings {values: {wipe_tower_x: {index: 0, value: 300}}}` | as after 7 | refused, "The prime tower of plate 0 would be off the plate" |
| 13 | `plate_slice` | as after 7 | outcome `ok`; `grams()` each slot's `model_g` and `tower_g` above 0 and `flush_g` 0, and `filament_changes` above 0 |
| 14 | `filament_set_flushing {flush_volumes_matrix: {index: 1, value: 200}}`, then `plate_slice` | `#FFFFFF;#000000 \| 0.7 \| 0,200,300,0 \| Textured PEI Plate \| 150,180` | `grams()`: slot 2's `tower_g` lower than after 13; each slot's `model_g`, to 0.01 g, and `filament_changes` as after 13 |
| 15 | `filament_set_flushing {flush_multiplier: 1.4}`, then `plate_slice` | `#FFFFFF;#000000 \| 1.4 \| 0,200,300,0 \| Textured PEI Plate \| 150,180` | `grams()`: each slot's `tower_g` higher than after 14 |
| 16 | `project_save` to a new file, `project_new {discard_changes: true, preset_changes: "discard"}`, `project_open` of the file | as after 15 | `filaments()` `cube 1 (1, 2) · cylinder 2`; `presets().filaments` `[B, M]`; `slots()` `0,0`; `support_multi_bed_types` "1": the project file keeps them, and the printer's settings with the plate type. The new project drops the printer's edit first, which `project_open` would otherwise ask about in a "Load project: unsaved changes" dialog |

The mixed filaments' scenarios follow, each from the mixed start (Starts from); `W` is the test's temporary
folder.

**Scenario: adding and reading**

Step 5 is the sidebar's Add itself, `filament_add` alone, where the new physical slot goes among the mixes; no check
reads the colour it gives, which depends on the slots the app's run added before.

| # | Step | `slots()` | `mixes()` | Also checked |
|---|---|---|---|---|
| 1 | `filament_add_mixed {components: [2, 3], ratios: [50, 50]}` | `0,0,0,1` | `4 = 2,3 @ 50,50` | reply `{added: true, filament_slot: 4}`; row 4 has a `#RRGGBB` `colour`, no `preset` and no `type`; rows 1 to 3 keep their preset; `app_state.presets.filaments` has 4 entries, one per slot, the mixed slot's too; `flush_volumes_matrix` holds 16 entries, 4 x 4 over every slot |
| 2 | the same call | `0,0,0,1` | as after 1 | reply `{added: false, filament_slot: 4}`: a mix the project has is not added again |
| 3 | `filament_add_mixed {components: [1, 2, 3], ratios: [20, 30, 50]}` | `0,0,0,1,1` | `4 = 2,3 @ 50,50 · 5 = 1,2,3 @ 20,30,50` | reply `{added: true, filament_slot: 5}` |
| 4 | `filament_add_mixed {components: [2, 3]}` | as after 3 | as after 3 | refused, `components_and_ratios` |
| 5 | `filament_add` | `0,0,0,0,1,1` | `5 = 2,3 @ 50,50 · 6 = 1,2,3 @ 20,30,50` | reply `filament_slot` 4: a physical slot goes before the mixes, which move up with their components as they were |
| 6 | a 20 mm cube `kept` added, `project_save` to `W/mixes.3mf`, `filament_delete {filament_slot: 6}`, then `project_open` of the file with `discard_changes` | as after 5 | as after 5 | after the delete `slots()` is `0,0,0,0,1`; the open brings slot 6 back: the project file keeps its mixes; `on("kept")` 1. The cube is there so that the open shows no warning, which the app gives for a project file that holds no object |

**Scenario: a mixed slot has no preset or colour of its own**

From the mixed start, `filament_add_mixed {components: [2, 3], ratios: [50, 50]}`: `slots()` `0,0,0,1`, `mixes()`
`4 = 2,3 @ 50,50`. Every step leaves both as they were.

| # | Step | Also checked |
|---|---|---|
| 1 | `settings_get {type: "filament", filament_slot: 4, keys: ["filament_cost"]}` | refused, `mixed_filament_slot` |
| 2 | `settings_set {type: "filament", filament_slot: 4, values: {filament_cost: 99}}` | refused, `mixed_filament_slot`; `settings_get {type: "filament", filament_slot: 2, compare: ":saved"}` lists nothing: the preset slots 1 to 3 hold has no unsaved change |
| 3 | `settings_list {type: "filament", filament_slot: 4}` | refused, `mixed_filament_slot` |
| 4 | `preset_save {type: "filament", filament_slot: 4, name: "MCP Mixed Slot"}` | refused, `mixed_filament_slot`; `preset_list {type: "filament", source: "user"}` names no "MCP Mixed Slot" |
| 5 | `preset_list {type: "filament", query: "Generic PLA"}` | the "Generic PLA @System" item's `filament_slots` `[1, 2, 3]`: the mixed slot does not use it |
| 6 | `filament_set_colour {filament_slot: 4, colour: "#00FF00"}` | refused, saying "Slot 4 is a mixed filament"; row 4's colour is not `#00FF00` |
| 7 | `filament_set_colour {filament_slot: 4, colour: c}`, `c` row 4's colour in lower case | reply `changed` false: its own colour is taken, in any case |
| 8 | `filament_set_flushing {flush_volumes_matrix: {index: 3, value: 50}}`, entry 3 being from slot 1 to slot 4 | refused, saying it "belongs to a mixed filament, which is never flushed"; entry 3 is not 50 |

**Scenario: changing a mix**

From the mixed start with slots 2 and 3 added in `#FF0000` and `#0000FF`, `filament_add_mixed {components: [2, 3],
ratios: [50, 50]}`, and a 20 mm cube `on_mix` added with `object_add_primitive` and put on filament 4 with `settings_set {target:
"object", object: 0, values: {extruder: 4}}`: `slots()` `0,0,0,1`, `mixes()` `4 = 2,3 @ 50,50`, `on("on_mix")` 4.
After every step `on("on_mix")` is 4: an edit keeps the objects on the mix.

| # | Step | `slots()` | `mixes()` | Also checked |
|---|---|---|---|---|
| 1 | `filament_edit_mixed {filament_slot: 4, components: [2, 3], ratios: [70, 30]}` | `0,0,0,1` | `4 = 2,3 @ 70,30` | reply `{filament_slot: 4, changed: true}`; row 4's colour has more red than blue: the blend follows the ratios |
| 2 | the same call | `0,0,0,1` | as after 1 | reply `changed` false |
| 3 | `... {ratios: [30, 70]}` with the same components | `0,0,0,1` | `4 = 2,3 @ 30,70` | row 4's colour has more blue than red |
| 4 | `... {components: [1, 2, 3], ratios: [20, 30, 50]}` | `0,0,0,1` | `4 = 1,2,3 @ 20,30,50` | the slot keeps its number |
| 5 | `filament_edit_mixed {filament_slot: 2, components: [2, 3], ratios: [50, 50]}` | as after 4 | as after 4 | refused, `not_mixed_filament` |
| 6 | `... {filament_slot: 9, ...}` with the same mix | as after 4 | as after 4 | refused, `no_such_slot` |
| 7 | `filament_edit_mixed {filament_slot: 4, components: [2, 3]}` | as after 4 | as after 4 | refused, `components_and_ratios` |
| 8 | `filament_edit_mixed {filament_slot: 4, components: [2, 2], ratios: [50, 50]}` | as after 4 | as after 4 | refused, `duplicate_component` |

**Scenario: a gradient, through the dialogs**

From the mixed start, `filament_add_mixed {components: [2, 3], ratios: [50, 50]}`: `slots()` `0,0,0,1`, `mixes()`
`4 = 2,3 @ 50,50`. The dialogs' controls are named by the app (`filament_1` to `filament_3`, `gradient`,
`gradient_direction`, `per_part_gradient`) and set with `widget_set_value {target: {name, within: <the dialog's
title>}}`; OK and Cancel are clicked with `widget_click {target: {label, within}}`.

| # | Step | `slots()` | `mixes()` | Also checked |
|---|---|---|---|---|
| 1 | `filament_edit_mixed {filament_slot: 4}` | `0,0,0,1` | as at the start | `blocked_by_dialog`; `dialogs()` one, titled Edit Mixed Filament, with OK and Cancel |
| 2 | in it, `gradient` true, `gradient_direction` 1, `per_part_gradient` true, then OK | `0,0,0,1` | `4 = 2,3 @ 50,50, bottom 3 top 2, per part` | the job's reply `{filament_slot: 4, changed: true}`; `gradient_direction`'s item 1 puts the second filament at the bottom |
| 3 | `filament_edit_mixed {filament_slot: 4, components: [3, 2], ratios: [50, 50]}` | `0,0,0,1` | `4 = 3,2 @ 50,50, bottom 2 top 3, per part` | a gradient stays for a mix of two, its direction with it |
| 4 | `... {components: [1, 2, 3], ratios: [20, 30, 50]}` | `0,0,0,1` | `4 = 1,2,3 @ 20,30,50` | a gradient goes for three |
| 5 | `filament_edit_mixed {filament_slot: 4}`, then Cancel | `0,0,0,1` | as after 4 | the job's reply `changed` false |
| 6 | `filament_add_mixed {}` | `0,0,0,1` | as after 4 | `blocked_by_dialog`; `dialogs()` one, titled Add Mixed Filament |
| 7 | in it, `gradient` true, then OK | `0,0,0,1,1` | `4 = 1,2,3 @ 20,30,50 · 5 = 1,2 @ 50,50, bottom 1 top 2` | the job's reply `{added: true, filament_slot: 5}`: the dialog starts from filaments 1 and 2 half and half, and item 0 of the direction, the first filament at the bottom |
| 8 | `filament_add_mixed {}`, then Cancel | `0,0,0,1,1` | as after 7 | the job's reply `{added: false}` |
| 9 | as row 6 of adding and reading, with `W/gradient.3mf` and `filament_delete {filament_slot: 5}` | as after 7 | as after 7 | after the delete `slots()` is `0,0,0,1`; the open brings the gradient back: the project file keeps it; `on("kept")` 1 |

**Scenario: deleting or merging a filament that a mix mixes**

From the mixed start, `filament_add_mixed {components: [1, 2], ratios: [50, 50]}` and `{components: [2, 3], ratios: [50,
50]}`, and two 20 mm cubes, `on_one` on filament 1 and `on_mix` on filament 4: `slots()` `0,0,0,1,1`, `mixes()`
`4 = 1,2 @ 50,50 · 5 = 2,3 @ 50,50`.

| # | Step | `slots()` | `mixes()` | Also checked |
|---|---|---|---|---|
| 1 | `filament_delete {filament_slot: 1, merge_into: 4}`, then `dialog_answer {button: "Cancel"}` | `0,0,0,1,1` | as at the start | `blocked_by_dialog`; `dialogs()` one, titled Warning, with OK and Cancel, its message holding "uses this physical filament"; the job refused, `merge_cancelled`; `on("on_one")` 1 |
| 2 | `filament_delete {filament_slot: 1, merge_into: 4, break_mix: false}` | as at the start | as at the start | refused, `mixed_uses_slot`, with no dialog |
| 3 | `filament_delete {filament_slot: 1, merge_into: 4, break_mix: true}` | `0,0,1,1` | `3 = 0,1 @ 50,50, broken · 4 = 1,2 @ 50,50` | reply `{filament_slot: 1, merged_into: 3, broken_mixed: [3]}`, with no dialog; `on("on_one")` and `on("on_mix")` 3: the objects went into the mix, which lost the filament merged into it |
| 4 | `plate_slice` | as after 3 | as after 3 | plate 0's `outcome` `error`, its `error` "A mixed filament has a deleted or mismatched component": an object prints with a broken mix |
| 5 | `filament_edit_mixed {filament_slot: 3, components: [1, 2], ratios: [50, 50]}` | `0,0,1,1` | `3 = 1,2 @ 50,50 · 4 = 1,2 @ 50,50` | reply `changed` true: an edit repairs a broken mix |
| 6 | `filament_delete {filament_slot: 2}` | `0,1,1` | `2 = 1,0 @ 50,50, broken · 3 = 1,0 @ 50,50, broken` | reply `broken_mixed` `[2, 3]`, with no dialog: the sidebar deletes a filament a mix mixes without asking |
| 7 | `filament_delete {filament_slot: 1}` | as after 6 | as after 6 | refused, `last_filament_slot`: one physical slot stays, whatever mixes there are |
| 8 | `filament_delete {filament_slot: 2}` | as after 6 | as after 6 | refused, `mixed_list_hidden`: with one physical filament the sidebar hides the mixed filament list and deletes nothing (`Sidebar::delete_filament()`) |

**To settle in review:**
- A cancelled prompt with one slot (one slot with `X`, rows 6 and 11) leaves the colour of the preset not chosen. The
  pick and the tool write the new preset's default colour before the tab asks (`PlaterPresetComboBox::item_picked()` runs
  `PresetComboBox::update_ams_color()` from `Sidebar::select_filament_preset()` or the selector's own handler); the
  cancel then runs `Tab::on_presets_changed()`, whose `Sidebar::update_presets()` puts the tab's preset back in a
  single slot, so the slot ends on `B` and the tool's reply says `selected` `B`, but nothing restores the colour. The
  api assigns only once the tab switched, so it leaves `#111111` (row 14). The rows expect what the code gives,
  `#FFF8E7` on `B`; this looks like a bug, and if the app is changed to restore the colour, rows 6 and 11 expect
  `#111111`.
- `R2` is checked by properties, since the app works out the volumes from the colours: each recalculated entry other
  than the sentinel and above 0, the four equal across routes and to a colour change, and for `X` the volumes into the
  lighter colour above those out of it. With the Klipper printer's `nozzle_flush_dataset` 0,
  `FlushVolCalculator::calc_flush_vol_rgb()` takes the flush predictor's value where it has one and its colour formula,
  at least 60, otherwise; both make a dark-to-light change cost far more than the reverse (on this build `X`'s four
  entries are 534, 85, 75 and 479).
- The colour change and the assignment of `X` give the same four entries. The calculator works out a change from the
  minimum of the filament it leaves (`get_min_flush_volumes()` in `Plater.cpp`, given to `FlushVolCalculator` by
  `Sidebar::auto_calc_flushing_volumes_internal()`): the nozzle volume less the filament the printer retracts at a cut.
  The Klipper printer sets neither (`nozzle_volume` 0, `enable_long_retraction_when_cut` 0), so the minimum is 0 for
  every slot, whichever preset it holds, and the colour change, with `B` still in slot 2, gives what the assignment of
  `X` gives. On a Bambu Lab printer `B`'s cut retraction would lower the colour change's entries out of slot 2.
- `Sup` is exact: 230 into a support filament and `max(<colour volume>, 700)` out of it
  (`Sidebar::auto_calc_flushing_volumes_internal()`); from `#222222` to `#111111` or `#333333` the colour volume, with the
  Klipper printer's minimum of 0, stays far under 700.
- The prompt's buttons are checked as "including" Transfer, Discard and Save (the dialog also lays out other labelled
  controls), and "not including" Transfer for `A`.
- The purges in the print (project, rows 13 to 15). The Klipper printer has `single_extruder_multi_material` and
  `purge_in_prime_tower` on, so the purge of a filament change is `flush_volumes_matrix[from][to]` times
  `flush_multiplier`, which the incoming filament extrudes into the prime tower (`Print::_make_wipe_tower()`,
  `WipeTower2`): the slice counts it in that slot's `tower_g` and leaves `flush_g` 0. `tower_g` also holds the tower's
  walls and sparse layers, which whichever filament is loaded prints and whose share moves with the purges, so the rows
  check which way a slot's `tower_g` moves, and only where its own purges change. Entry 1 at 200 instead of 400 halves
  the purge into slot 2 at each of the 25 changes from slot 1 (280 to 140 mm³ at 0.7): on this build slot 2's
  `tower_g` falls from 16.41 to 11.22 g, while slot 1's, whose purges stay, moves from 14.21 to 14.95 g and is not
  checked; the objects print as before (`model_g` 2.21 and 1.88 g, 50 filament changes in each slice). At 1.4 every
  purge doubles, and both slots' `tower_g` rise (to 22.20 and 15.29 g).
- The preference is set with `preferences_set`, not picked in the Preferences dialog; the dialog's
  choice control writes the same key (`PreferenceOps.cpp`), and `test_preferences` checks the dialog against
  `preferences_get`.
- Run time: about 70 assignment cases, seven slices and the mixed filaments' dialogs, 4 to 5 minutes on Windows with
  other tests running beside it, so `sweep.json`'s 1500 s leaves room for a loaded pool.
- What `plate_slice` does with an object on a broken mix (deleting or merging, row 4), measured: the call is not
  refused, and the slice ends in an error on the plate, with the notification "Mixed filament has invalid or
  mismatched components. Please re-edit affected entries.", which the row does not read.
- The Add Mixed Filament dialog's start (row 7 of the gradient scenario): filaments 1 and 2 half and half, measured on
  this build; the dialog may start from its first recommendation instead on another set of filaments.
- A mix left beside one physical filament (deleting or merging, row 8) is hidden in the sidebar and cannot be deleted
  there, which the tool follows; a person who deleted down to one filament has no way to see such a mix until they
  add a slot. That looks like a gap in the sidebar, open for the maintainer.
- Colours are compared by their channels, not their values: the blend is the app's (`blend_mixed_color()`), and only
  which way it moves with the ratios is the tool's behaviour.

**Not checked:**
- Adding and deleting slots: `test_objects` (its filament slots scenario). Assigning a preset to a mixed slot by the
  plugin API: `test_operation_routes`, which checks the code and the words.
- The rows the matrices leave out (one slot with `A`; Auto-flush by the pick and the api beyond `All` with `X`): each
  reaches the code of a row kept, as their scenarios say. A route that later takes a branch of its own behind
  `filament_preset_picked()` is caught only by the rows that run that route.
- The prompt's Save button: saving the edits as a new preset is `test_presets`'s.
- Whether an assignment takes an undo step: a preset change is not in the project's undo history (as `settings_set`
  says of a preset's), and the snapshots can be read only from the app's log.
- The values of `R2`, `<a>`, `<b>` and `<c>`, beyond the properties above.
- The flushing volumes after a hidden preset's assignment (refusals scenario, rows 11 and 12) beyond `R2`: what an
  assignment does to them is the Auto-flush scenario's.
- `filament_colour_type` and `filament_multi_colour`, which `update_ams_color()` writes beside `filament_colour`: no
  MCP tool lists them, and for a preset that is not an AMS slot's they only mirror `filament_colour` (an empty type,
  the colour itself).
- A text part made with the Text gizmo: the second part here is a cube from
  `part_add`. Both are model parts, which `settings_set` gives a filament the same way; the gizmo is not this area's.
- How the sidebar shows a slot: the colour swatch and the badge are not read, and no screenshot is taken.
- A printer with several extruders, where the filament grouping applies: `test_printers`.
- The exact list of plate types the Klipper printer offers with `support_multi_bed_types` on (all six on this build):
  the step checks two of them.
- Which layers print with filament 2: the slice's grams per slot show that filament 2 prints, not where.
- How much of a slot's `tower_g` is purge: the tower's walls and sparse layers are in it too, so the project scenario
  checks which way it moves, not by how much, and the objects scenario reads it only as 0 for slot 2 in the control.
- Which other presets the selectors and `preset_list` offer: the user presets and installed filaments other tests leave
  change those lists, so every check picks its presets by name.
- The install and uninstall that set the start, beyond the uninstall's reply naming each of the six and the hidden
  checks that follow it: `filament_install` and `filament_uninstall` are `test_install_filaments`'s.
- The gradient curve, which is drawn and changed only by a drag: every gradient here is the straight line.
- The dialogs' component selectors and ratio controls: the selectors are named, but each lists "Generic PLA" for every
  slot, and components and ratios have the silent form.
- What a mixed filament prints, its components layer by layer: no slice of a mix is read.
- A mix's colour beyond the channel comparison, and whether a component's colour change re-blends it.
- How the sidebar draws a broken mix: its row is not read on screen.
- The undo history: none of these is in it, as in the GUI, and nothing reads it here.
- The mixed filament list's Remove last and Remove all buttons: `test_objects` clicks Remove last mixed filament; Remove
  all asks first, and `filament_delete` deletes a mix one at a time.

**Fixtures:** none; the plugin API calls are the test's own code.

**Strength:** Strong — after every case it compares the slots, the tab, the unsaved values, the colours and the
flushing volumes whole with the state written out here, and after every step of the mixed filaments' scenarios
the slots and the mixes; it asserts the prompt and the dialogs, each reply and each refusal or raise exactly,
checks the volumes the app works out by stated properties, and shows the filaments and the flush settings in the
print, and the flush settings and the mixes in the project file.

## `test_settings`
`test_settings.py` · Python

**Tests:** The settings commands' targets. `settings_list` giving a type's settings whole, a saved preset's and a
filament slot's, and the settings an object and a region take; then `settings_set` and `settings_get` on an object, a
part, a modifier and a height range, named by index and taken from the selection, with each value's `effective` and
`source` in the order the slicer applies them; what the targets' settings do in the print; and the height ranges
replaced, the range that stays keeping its settings. It runs on the Generic Klipper Printer 0.4 with the process
0.20mm Standard @MyKlipper (`layer_height` 0.2, `wall_loops` 3, `sparse_infill_density` 15%, `top_shell_layers` 4):
nothing it checks is a Bambu Lab printer's own.

**Starts from:** every scenario's `start(client)`: a new project on `MyKlipper 0.4 nozzle` with `0.20mm Standard
@MyKlipper` and one slot holding `B` (Generic PLA @System), the printer's default filament. `presets()` is `{printer:
"MyKlipper 0.4 nozzle", process: "0.20mm Standard @MyKlipper", filaments: [B]}`, and `object_list` lists no object.
Nothing beyond the baseline reaches its checks: the settings commands read the presets and the project alike in every
mode, and no preference changes what they give.

**Leaves:** what the targets scenario leaves on the baseline its `start()` set: the Klipper printer with `0.20mm
Standard @MyKlipper` and one slot of `B`, and `settings.3mf` open as the project, with its cube, saved in the test's
temporary folder and deleted with it, in the recent projects, with that folder as the last one opened. It saves no
preset, installs nothing and sets no preference: `Generic PETG @System`, which the first scenario puts in slot 2, is
shown for the app's session only. The cooling slowdown the targets scenario turns off on the filament preset is an
unsaved change, reset at step 12 and, where a failed step left it, discarded by the next test's `start()`; so it
restores nothing in a `finally`.

Nothing it reads is a list another test adds to: the settings lists are the app's own, the objects a new project's and
the presets those `start()` selects. A run again on the data folder a failed run left meets only what `start()`
replaces, and its project file goes to a new temporary folder.

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

**Scenario: a type's settings, whole**

| # | Step | Checked |
|---|---|---|
| 1 | `listed(process)` | `total` at least 300, as many items as `total`, no `next_offset` and no item with `meta`; the items include `layer_height` "0.2", `wall_loops` "3", `sparse_infill_density` "15%" and `top_shell_layers` "4" |
| 2 | `listed(printer)` | `total` above 100, whole as in 1; `nozzle_diameter` "0.4", `printable_height` "250" |
| 3 | `listed(filament)` | `total` above 100, whole as in 1; `filament_type` "PLA", `slow_down_layer_time` "4" |
| 4 | `listed(process, include_meta: true)`, then with `offset` 100, 200 and on until a page has no `next_offset` | each page but the last holds 100 items and `next_offset` the next hundred; every item has `meta`; the pages' keys, in order, are step 1's |
| 5 | `listed(process, limit: 50)` | 50 items, `next_offset` 50, none with `meta`; they are step 1's first 50 |
| 6 | `listed(process, preset: "0.16mm Optimal @MyKlipper")` | `layer_height` "0.16"; `presets().process` is still 0.20mm Standard @MyKlipper: reading a preset does not select it |
| 7 | `add_slot` with `#00C1AE`, the baseline's colour of slot 2, then `preset_select {type: "filament", name: "Generic PETG", filament_slot: 2}` | `presets().filaments` `[B, Generic PETG @System]`; `listed(filament, filament_slot: 2)` gives `filament_type` "PETG" and `slow_down_layer_time` "8", `listed(filament, filament_slot: 1)` "PLA" and "4" |

**Scenario: what an object and a region take**

| # | Step | Checked |
|---|---|---|
| 1 | `listed(object)` and `listed(region)` | the object list holds more than 200 settings and the region list more than 100; every key of the object list is a process setting with the value `listed(process)` gives it; every key of the region list but `layer_height` is in the object list; both give `wall_loops` "3" and `sparse_infill_density` "15%"; the region list's `layer_height` is "0.2" with `targets` `["height_range"]`, and no other item of either list has `targets` |

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
| 12 | `plate_slice`, then `layers()` | as after 7 | outcome `ok`; every layer whose z is from 5.3 to 9.7 mm is 0.12 mm high, and every layer from 10.5 to 19.5 mm 0.2 mm, to 0.001 mm; the layer at z 3.0 mm has more `inner` than the layer nearest z 15 mm (six walls in the range, four on the object), and that one more than the control's layer at 15.0 mm (three walls); the layer nearest z 15 mm has more `sparse` than the control's at 15.0 mm (part 0's infill at 30%, not 15%). Then `settings_set {type: "filament", reset_all: true}`, and `slow_down_for_layer_cooling` reads "1" again, so that step 14's new project asks about no unsaved preset |
| 13 | `object_set_height_ranges {object: 0, ranges: [{min_z: 0, max_z: 5}, {min_z: 5, max_z: 12}]}` | `{extruder: 1, wall_loops: 4} · {sparse_infill_density: 30%} · {wall_loops: 5} · 0–5 {extruder: 0, layer_height: 0.2, wall_loops: 6}, 5–12 {extruder: 0, layer_height: 0.2}` | the range that stays keeps its settings; the new one takes the object's layer height and the Default filament |
| 14 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 13 | `object_list` part types `[part, modifier]` and `overrides` `["extruder", "wall_loops"]`: the project file keeps every target's settings |

**To settle in review:**
- The walls and the infill in the print (step 12) are checked as order, not amount. A wall loop more adds about a
  loop's printing time, so six walls print longer than four and four longer than three (on this build 2.77 s, 1.73 s
  and 1.17 s of inner wall), with the cube's 20 mm cross-section the same at 3 and 15 mm, and the modifier (7.5 to
  12.5 mm) and the top and bottom shells away from both layers; the control slice shows the two layers print alike
  without the targets' settings. The sparse infill's time follows its area times its density: at 15 mm the wall more takes
  about a tenth of the infill area and the density doubles, so the infill prints well over the control's (1.65 times on
  this build). Both comparisons hold only with the cooling slowdown off:
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

**Fixtures:** none.

**Strength:** Strong — it compares every target's settings whole after each step, asserts each value's effective value,
source and note exactly, checks the lists whole and paged against each other and known values, and shows the targets'
walls, infill and layer heights in the print against a control slice.
