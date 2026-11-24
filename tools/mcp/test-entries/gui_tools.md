# GUI automation tools

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

Three tests:

- `test_gui_tools` drives the app's own menus, rows, popups and web dialogs with the generic GUI tools.
- `test_recording` makes and encodes recordings. What it can check depends on the machine's
  ffmpeg, and it says so in its output, so its verdict is kept apart from the others'.
- `test_call_forms` checks the forms a call takes: the names and values agents send, and a call with exactly an
  action's arguments doing what a person's pick does. Part of that needs the Design tab, which every app the sweep
  launches has.

Each runs in its job's app after the tests before it. Their `sweep.json` entries:

```json
"test_gui_tools": {"path": "test_gui_tools.py"},
"test_recording": {"path": "test_recording.py"},
"test_call_forms": {"path": "test_call_forms.py"}
```

## `test_gui_tools`
`test_gui_tools.py` · Python, 230 checks · in `sweep.json` as `{"path": "test_gui_tools.py"}`

**Tests:** The generic GUI tools on the app's own widgets, as an agent drives any of them: context menus and a
button's menu listed and run by path without being shown, the Edit menu's items, the rows of the object list, of
Compare presets and of the parameter table, the print button's popup, a drop-down list, and Orca's web dialogs (the
preset bundle dialog with the Export dialog above it, the colour picker over the filament picker, the setup wizard
and its pages' navigation, the Flushing volumes dialog), with the web view `webview_run_javascript` runs in while
they are open. Every menu item, row and page element the test runs or clicks is checked by what it does to the
project, the selection, the camera or the settings; a menu's listing is checked by the items it holds.

**Starts from:** `start(c)`, the Generic Klipper Printer, compared with `{printer: KLIPPER, process: KLIPPER_STD,
filaments: [GENERIC_PLA]}`, on which every scenario runs. The web dialogs scenario starts with `start(c,
filaments=["Bambu PLA Basic @System"])`, and the drop-down list with `start(c)` again and the printer's
`support_multi_bed_types` turned on (see there). Before the first scenario the test reads back the baseline's settings
its checks depend on: the window at 1600 × 900 (`app_state.window.size`), since `E`'s pixel and the canvas captures
follow the window's size; the 3D view's labels off (`app_state.view.labels`), which the menus scenario turns on and
off; and `use_printer_agents` false (`preferences_get`), under which a printer without a print host gives the print
button's popup its two items. Three scenarios set more of their start: the dialog's rows turn Compare presets'
transfer checkbox off, the drop-down list sets the plate type, and the web dialogs scenario takes the wizard card's
printer out (see each). The login region the web dialogs scenario reads is the one stored setting no tool can set
(`preferences_set` refuses `region`, since changing it signs out of the cloud account); no test changes it, so the
scenario checks it is `""`. A plugin an earlier test left adds web views of its own (`test_web_ui`'s fixture adds a
page among the main window's tabs, whose web view is made only when shown), so the web dialogs scenario's script
with no modal open reads the reply's URL to know it ran in the Home page. Measured with the fixture installed: the
Home page answers, and `widget_list` lists its web view alone.

**Leaves:** The drop-down list's project and printer: the Klipper printer with `support_multi_bed_types` on as an
unsaved edit, `0.20mm Standard @MyKlipper` and one `Generic PLA @System` slot; the project saved as
`OUT/plate_type.3mf` and changed since, its plate type `Cool Plate`, which the app also stores for the printer. The
next `start()` discards the edit and the project, and the sidebar then stores the printer's default plate type, `High
Temp Plate`, again. The auto-answer rules for "Uniform scale" and "per-object setting mode". The Generic Marlin Printer
not installed, when an earlier test had installed it. The print button's mode, `Export G-code file`, which the app
keeps only while Remember last print action is on, and Compare presets' transfer checkbox on, which the app keeps
until it closes. The recent projects and the last folder opened, naming the projects it saved in its own output
folder. What the baseline lists the next `start()` resets, and no test reads the rest as it finds it, so the test
restores nothing.

**Reads:** `objects()` gives each object from `object_list` as its name, the plate it is on, its parts, each part as
its name and `type` (`part`, `modifier`, `negative`, ...), and whether its first instance is printable. `selection()` gives
`app_state.selection` as `{objects, parts}`, or empty. `menu(reply)` gives a reply's `popup_menu` as `{items,
checked, disabled}`, an absent list read as `[]`. `rows(target)` parses `widget_list`'s tree into rows, each with
its label path, its value, and whether it is selected or checked. `dialogs()` gives the titles of
`app_state.dialogs`, `windows()` the titles `window_list` lists, `colours()` each `filament_list` row's `colour`,
joined by `;`, `project_file(path)` the JSON of a saved project's `Metadata/project_settings.config`, and
`canvas_png(name)` the file `window_screenshot {target: "canvas", include_image: false, save_path: OUT/<name>.png}`
writes. `OUT` is the test's output folder: the folder `--out` names, else a new temporary folder. Every scenario
starts with `project_new {discard_changes: true, preset_changes: "discard"}` in the Prepare view and the
auto-answer rules for "Uniform scale" and "per-object setting mode" (OK) registered, and checks that `dialogs()` is
`[]` and `plate_list` gives one unnamed plate, current.

In the tables an object is written as its name, a colon, and its parts as name and type joined by `+`: `Cube: Cube
part + Generic-Cylinder modifier`. "As after N" means the state written out at step N, compared whole again.

**Scenario: menus of the 3D view and of a button**

It starts with the one filament slot `start()` leaves, the cube of the Add Cube action (`action_run`) and the iso
view (`view_set {camera: "iso"}`). The start is checked: `objects()` is `Cube: Cube part`, printable;
`app_state.presets.filaments` has 1 entry; `app_state.view.labels` is false. `C` is `mouse_gesture {action: "click",
button: "right", at: {object: 0, point: [6, -6, 12.5]}}`, a point on the cube's top away from the diagonal its two
top triangles share: the Add Cube action's cube is a tenth of the bed's longer side, 25 mm on the Klipper printer's
250 mm bed, unless a person saved a cube size of their own in its dialog (`primitive_size`), which no test does. `E` is the same right click at the canvas pixel (1000, 150), empty space
above the bed. After each `C` and `E`, and after step 19's listing, the test waits for `app_wait {for: "idle"}`, so
that a check that a listing changed nothing comes after anything the call set off.

`OBJECT` is the set of menu items the object menu must list: `Add Part>Cube`, `Add Negative Part>Cylinder`, `Add
Modifier>Cylinder`, `Add Support Blocker>Cube`, `Add Support Enforcer>Cube`, `Delete`, `Printable` and `Auto Drop`.
`DEFAULT` is the empty space's whole menu, in order: `Add Primitive>Cube`, `Add Primitive>Cylinder`, `Add
Primitive>Sphere`, `Add Primitive>Cone`, `Add Primitive>Disc`, `Add Primitive>Torus`, `Add Primitive>Text`, `Add
Primitive>SVG`, the ten handy models `Add Handy models>Orca Cube`, `>OrcaSliced Combo`, `>Orca Badge`, `>Orca
Tolerance Test`, `>3DBenchy`, `>Cali Cat`, `>Autodesk FDM Test`, `>Voron Cube`, `>Stanford Bunny`, `>Orca String
Hell`, then `Add Models` and `Show Labels`.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | `C` | `Cube: Cube part`, printable | `menu()`'s items hold `OBJECT` and not `Add Primitive>Cube` or `Assemble`; `checked` is exactly `[Printable, Auto Drop]`; `dialogs()` `[]`: listing a menu runs nothing |
| 2 | `C` with `popup_item: "add modifier>cylinder"` | `Cube: Cube part + Generic-Cylinder modifier` | reply `ran` is `Add Modifier>Cylinder`: the path is matched ignoring case and named as the menu shows it |
| 3 | `action_run` Undo | `Cube: Cube part` | the menu item's command is one undo step |
| 4 | `action_run` Redo | as after 2 | |
| 5 | `widget_click {target: {menu: "Edit>Undo"}}` | `Cube: Cube part` | reply `clicked` is `Edit>Undo` (its label alone, though macOS shows the key after it) |
| 6 | `widget_click {target: {menu: "Edit>Redo"}}` | as after 2 | reply `clicked` is `Edit>Redo` |
| 7 | `C` with `popup_item: "Printable"` | as after 2, not printable | reply `ran` is `Printable` |
| 8 | `C` | as after 7 | `checked` is exactly `[Auto Drop]`: the listing reads the check item's state |
| 9 | `C` with `popup_item: "Printable"` | as after 2, printable | |
| 10 | `C` with `popup_item: "Add Modifier"` | as after 9 | refused, saying `Add Modifier` is a submenu; its details list `Add Modifier>Cylinder` |
| 11 | `C` with `popup_item: "Add Modifier>Pyramid"` | as after 9 | refused, saying the menu has no item `Add Modifier>Pyramid` |
| 12 | `object_select {none: true}`, then `E` | as after 9 | `selection()` empty after the select; the menu's items exactly `DEFAULT`, no `checked`, no `disabled` |
| 13 | `canvas_png("labels_off")`, then `E` with `popup_item: "Show Labels"`, then `canvas_png("labels_on")` | as after 9 | `app_state.view.labels` true; `labels_on.png` differs from `labels_off.png`: the 3D view draws the cube's label |
| 14 | `E` | as after 9 | `checked` exactly `[Show Labels]` |
| 15 | `E` with `popup_item: "Show Labels"`, then `canvas_png("labels_off_again")` | as after 9 | `app_state.view.labels` false; `labels_off_again.png` differs from `labels_on.png`: the label is gone again |
| 16 | `E` with `popup_item: "Add Primitive>Cylinder"` | as after 9, then `Cylinder: Cylinder part` | `app_state.objects` 2 |
| 17 | `widget_click {target: {label: "Add one filament"}}` | as after 16 | `app_state.presets.filaments` has 2 entries |
| 18 | `filament_set_colour {filament_slot: 1, colour: "#FF0000"}` and `{filament_slot: 2, colour: "#0000FF"}` | as after 16 | `colours()` `#FF0000;#0000FF` |
| 19 | `widget_click` of filament 1's `Click to edit preset` button, by the ref of the first button of that name `widget_list {target: "main"}` lists with a position and a size | as after 16 | items exactly `Edit`, `Merge with>Generic PLA`, `Decompose Color`, `Delete`, `Generic PLA` being the filament of slot 2 as the selector names it (Add one filament copies the last slot's, `PresetBundle::set_num_filaments`); `dialogs()` `[]`; `app_state.tab` `prepare` |
| 20 | the same with `popup_item: "Delete"` | as after 16 | reply `ran` `Delete`; `app_state.presets.filaments` has 1 entry and `colours()` is `#0000FF`: the slot whose button was clicked is the one deleted |
| 21 | `object_select {all: true}`, then `C` | as after 16 | `selection()` objects `[0, 1]` after the select; the items hold `Assemble`, `Align/Distribute>Align left (-X)` and `Delete`, and not `Add Part>Cube`: the menu is the multi-selection's, not one object's |
| 22 | `C` with `popup_item: "Delete"` | none | reply `ran` `Delete`: the multi-selection's Delete removes both objects; `app_state.objects` 0, `closed_menus` `[]`, `dialogs()` `[]` |

**Scenario: rows of the object list**

It starts with the Add Cube action's cube and the sidebar's Global/Objects switch on Objects (`widget_set_value
{target: {class: "SwitchButton", label: "Process"}, value: true}`). The start is checked: `objects()` is `Cube: Cube
part`, printable, on plate 0.

| # | Step | Checked after |
|---|---|---|
| 1 | `widget_list {target: "main", query: "row"}` | `rows()` holds the object list's `Plate 1` and, one level under it, `Cube` |
| 2 | `widget_click {target: {gltoolbar: "addplate"}}` | `plate_list` has 2 plates; `app_state.plates.current` 1 |
| 3 | `widget_click {target: {row: "Plate 1"}}` | reply `selected` `["Plate 1"]`; `app_state.plates.current` 0 |
| 4 | `widget_click {target: {row: "Plate 2"}}` | reply `selected` `["Plate 2"]`; current 1 |
| 5 | `widget_click {target: {row: "Plate 1"}}` | current 0 |
| 6 | `object_select {none: true}`, then `widget_click {target: {row: "Cube"}}` | `selection()` empty after the select; reply `selected` `["Plate 1>Cube"]`; `selection()` objects `[0]`, no parts |
| 7 | `view_set {look_at: [0, 0, 0], zoom: 1}` | `app_state.camera` `look_at` `[0, 0, 0]`, `zoom` 1 |
| 8 | `widget_click {target: {row: "Plate 1>Cube"}, double: true}` | reply `selected` `["Plate 1>Cube"]`; `app_state.camera.look_at` is the centre of the cube's `bbox` from `object_list` to 0.01 mm, and `zoom` above 1: a double click frames the object |
| 9 | `widget_click {target: {row: "Cube"}, button: "right"}` | `menu()`'s items hold `OBJECT`, `checked` exactly `[Printable, Auto Drop]`: the row's menu is the object's |
| 10 | the same with `popup_item: "Add Modifier>Cylinder"` | reply `ran` `Add Modifier>Cylinder`; `objects()` `Cube: Cube part + Generic-Cylinder modifier` |
| 11 | `widget_click {target: {row: "Cube>Generic-Cylinder"}}` | reply `selected` `["Plate 1>Cube>Generic-Cylinder"]`; `selection()` parts `[[0, 1]]` |
| 12 | `widget_click {target: {row: "Cube>Cube"}, modifiers: ["ctrl"]}` | reply `selected` `["Plate 1>Cube>Cube", "Plate 1>Cube>Generic-Cylinder"]`; `selection()` objects `[0]`, no parts: the two rows select every part of the cube, which the 3D view takes as the whole cube |
| 13 | `widget_click {target: {row: "Cube>Generic-Cylinder"}}` | `selection()` parts `[[0, 1]]`: a right click keeps a selection it is in, so the part is clicked alone first |
| 14 | the same with `button: "right", popup_item: "Change Type>Negative Part"` | reply `ran` `Change Type>Negative Part`; `objects()` `Cube: Cube part + Generic-Cylinder negative` |
| 15 | `widget_click {target: {row: "Plate 1>Cube", column: "Printable"}}` | `objects()` as after 14, not printable |
| 16 | `plate_slice` | plate 0's outcome `empty`, and `plate_list` gives it `sliced` false: the column did what Printable is for |
| 17 | `project_save {path: OUT/rows.3mf}`, `project_new`, `project_open` of the file | `objects()` is `[]` after the new project, then as after 15: the part's type and the printable flag are kept in the project file |
| 18 | `widget_click {target: {row: "Plate 1>Cube", column: "Printable"}}` | `objects()` as after 14, printable |
| 19 | `plate_slice` | plate 0's outcome `ok`; `plate_list` gives it `sliced` true |
| 20 | `app_state` | `dialogs` `[]`, `closed_menus` `[]` |

**Scenario: a dialog's rows, a table and a popup**

It starts with the Add Cube action's cube, the Objects switch on, and a cylinder modifier added from the cube row's
menu as in the rows scenario. The start is checked: `objects()` is `Cube: Cube part + Generic-Cylinder modifier`,
`app_state.presets.process` is `0.20mm Standard @MyKlipper`, and `windows()` holds no `Compare presets`. `SIDE` is
the labels of the main window's side buttons, as `widget_list {target: "main", query: "SideButton", include_menus:
false}` lists them: `Slice plate`, its unlabelled arrow, the print button, its unlabelled arrow. `TRANSFER` is the
locator `{label: "Transfer values from left to right", within: "Compare presets"}`, the window's transfer checkbox,
which the window shows once its two presets differ and keeps while the app runs, as the app makes the window once
and hides it on closing; each opening turns it off before reading the rows, so that they read as the window shows
them without it.

| # | Step | Checked after |
|---|---|---|
| 1 | `widget_click {target: {label: "Compare presets"}}` | `windows()` holds `Compare presets`; `dialogs()` `[]`, as it is modeless |
| 2 | `widget_set_value {target: {class: "PresetComboBox", within: "Compare presets", index: 5}, value: "0.12mm Fine @MyKlipper"}`, then `widget_set_value {target: TRANSFER, value: false}` | `rows()` of the window hold the header row `"0.20mm Standard @MyKlipper" vs "0.12mm Fine @MyKlipper"` and, under it, `Quality>Layer height>Layer height` with the value `0.2 \| 0.12`: both presets' values |
| 3 | `widget_click {target: {row: "Quality>Layer height>Layer height", within: "Compare presets"}}` | reply `clicked` holds that path; `rows()` read the row as selected |
| 4 | `keyboard_press {target: {class: "DiffPresetDialog"}, keys: "Escape"}` | `windows()` holds no `Compare presets`; `app_state.presets.process` is still `0.20mm Standard @MyKlipper` and `preset_list` marks no process preset modified: comparing changed nothing |
| 5 | steps 1 to 3 again, then `widget_set_value {target: TRANSFER, value: true}`; then `widget_set_value {value: false}` on each option row `rows()` reads checked, and `widget_set_value {target: {row: "Quality>Layer height>Layer height", within: "Compare presets"}, value: true}` | after the checkbox, `rows()` read every option row checked, as each row's toggle starts on; at the end, the layer height row alone of the option rows reads checked, and the bottom shell layers row (3 against 5) reads unchecked |
| 6 | once `widget_list` of the window lists the `Transfer` button enabled, waited up to 30 s, `widget_click {target: {label: "Transfer", within: "Compare presets"}}` | the wait is met; `windows()` holds no `Compare presets`; `app_state.presets.process` is `0.12mm Fine @MyKlipper` and `preset_list` marks it modified; `settings_get` of the process gives `layer_height` `"0.2"`, the left preset's value, and `bottom_shell_layers` `"5"`, the right preset's own: only the checked row was transferred |
| 7 | `object_select {objects: [0]}`, then `widget_click {target: {row: "Plate 1>Cube"}, button: "right", popup_item: "Edit in Parameter Table"}` | `selection()` objects `[0]`, no parts, after the select; the click replies `blocked_by_dialog`; `dialogs()` `["Object/Part Settings"]` |
| 8 | `widget_list {target: "dialog"}` | the table's rows are `Cube` twice (the object, then its part) and `Generic-Cylinder` once |
| 9 | `widget_click {target: {row: "Generic-Cylinder", within: "Object/Part Settings", column: "Wall loops"}}` | `selection()` parts `[[0, 1]]`: a cell's click selects its part, as the table tells the object list; the click's result has `column` `Wall loops`, read from its reply or, when it replies `blocked_by_dialog`, from its job once step 10 has closed the dialog |
| 10 | `dialog_answer {dialog: {title: "Object/Part Settings"}, button: "close"}` | reply `closed` true; the step 7 job ends without an error; `dialogs()` `[]` |
| 11 | `widget_click {target: {class: "SideButton", index: 3}}`, the print button's arrow | `widget_list {target: "popup"}` lists the popup and exactly its buttons `Print` and `Export G-code file`, in order |
| 12 | `widget_click {target: {class: "SideButton", label: "Print"}}` | reply `clicked` `Print`; `widget_list {target: "popup"}` lists no node; `SIDE` is `Slice plate`, none, `Print`, none: the print button shows the mode picked; `dialogs()` `[]` |
| 13 | step 11 again, then `widget_click {target: {class: "SideButton", label: "Export G-code file"}}` | the popup as at 11, then no popup node; `SIDE` is `Slice plate`, none, `Export G-code file`, none: each pick changes the button from the other's label, whichever it showed at the start; `dialogs()` `[]` |
| 14 | `app_state` | `dialogs` `[]`, `closed_menus` `[]` |

**Scenario: Orca's own web dialogs**

It starts with `start(c, filaments=["Bambu PLA Basic @System"])`, compared with `{printer: KLIPPER, process:
KLIPPER_STD, filaments: ["Bambu PLA Basic @System"]}`: the Orca Filament Library's Bambu PLA Basic, which suits any
printer. A filament whose name starts with `Bambu` opens Orca's own filament picker (`PresetComboBoxes.cpp`), which
finds the library's filament id `OFoiVqVM` under Bambu's `GFA00` (`FilamentColorCodeQuery::GetFilaInfoMap`). Slot 1's
colour is the `#26A69A` `start()` gives it. When `preset_list {type: "printer"}` offers `MyMarlin 0.4 nozzle`, the
Generic Marlin Printer whose wizard card step 25 ticks, the scenario takes it out first (`printer_uninstall {models:
[{vendor: "Custom", model: "Generic Marlin Printer"}]}`), so that the card starts unticked; no test installs it. The start is checked: `app_state.presets.filaments` is `["Bambu PLA Basic
@System"]`, `colours()` is `#26A69A`, `preferences_get` gives `region` as `""`, the selected printer is `MyKlipper
0.4 nozzle`, and `preset_list {type: "printer"}` offers no `MyMarlin 0.4 nozzle`. `HOME` is the ref of the first web
view `widget_list {target: "main", query: "webview"}` lists on the Home tab; `BUNDLE_VIEW` is the locator `{class:
"webview", within: "Preset Bundle"}`, the bundle dialog's web view; `HEX` is the locator `{web: "Hex color", within:
"Color Picker"}`; `WIZARD` is `within: "Setup Wizard"`. The Klipper printer offers Flushing volumes in its sidebar
once it has a second filament, as its single extruder multi-material is on.

| # | Step | Checked after |
|---|---|---|
| 1 | `view_set {tab: "home"}`, then `webview_run_javascript {code: "1 + 1"}` | `widget_list {target: "main", query: "webview"}` lists a web view, `HOME`; the script replies `result` 2 with a `url` holding `web/homepage/index.html`: with no modal open and no target, it runs in the one web view shown |
| 2 | `action_run` Preset Bundle | `blocked_by_dialog`; `dialogs()` `["Preset Bundle"]`; its page lists the elements named `#refresh_btn` and `#close_btn` and the text `Version` and `Update` of its top list's header |
| 3 | `webview_run_javascript {code: "1", target: HOME}` | refused with "modal dialog is open": a web view behind the modal takes no script |
| 4 | `webview_run_javascript {code: "document.title"}` | `result` `Preset Bundle`: with no target, the topmost modal's page |
| 5 | `webview_run_javascript {code: "!!document.getElementById('close_btn')", target: BUNDLE_VIEW}` | `result` true: the topmost modal's web view by locator |
| 6 | `webview_run_javascript {code: "SendWXMessage(JSON.stringify({command: 'export_page'}))"}`, the message the page's Export sends | `dialogs()` `["Preset Bundle", "Export Preset Bundle"]` within 30 s; then the test waits up to 30 s for `widget_list {target: "dialog", query: "webview"}` to list the Export dialog's web view at `ExportPresetDialog`, as it shows `about:blank` until its first navigation starts |
| 7 | `webview_run_javascript {code: "1", target: BUNDLE_VIEW}` | refused with "is under": a dialog under the topmost takes no script |
| 8 | `webview_run_javascript {code: "1"}` | the reply's `url` holds `ExportPresetDialog`: with no target, the topmost of the two |
| 9 | `dialog_answer {button: "close"}` | `dialogs()` `["Preset Bundle"]` within 30 s |
| 10 | `widget_click {target: {css: "#close_btn", within: "Preset Bundle"}}` | reply `closed` true with no `dialogs`, none being left under it: a page that closes its dialog is told by the close, where a click whose page does not answer is refused after the call's 10 s; the step 2 job ends without an error; `dialogs()` `[]` |
| 11 | `view_set {tab: "prepare"}`, then `widget_click {target: {label: "1", class: "wxBitmapButton"}}`, slot 1's colour | `blocked_by_dialog`; `dialogs()` `["Select Filament"]`; the dialog lists `#26A69A`, the slot's colour |
| 12 | `widget_click {target: {label: "+ More Colors", within: "Select Filament"}}` | `blocked_by_dialog`; `dialogs()` `["Select Filament", "Color Picker"]`, the picker on top |
| 13 | `widget_set_value {target: HEX, value: "FF0000"}` | reply `value` `FF0000FF`; `HEX`'s field lists `FF0000FF` |
| 14 | `mouse_gesture {target: {css: "#spectrum", within: "Color Picker"}, action: "drag", x: 20, y: 20, to: [200, 100], steps: 6}` | reply `done` `drag`; `HEX`'s field lists eight hex digits other than `FF0000FF`: the drag picked another colour |
| 15 | `widget_set_value {target: HEX, value: "3366CC"}` | reply `value` `3366CCFF`; `HEX`'s field lists `3366CCFF` |
| 16 | `widget_click {target: {web: "OK", within: "Color Picker"}}` | reply `closed` true and `dialogs` `["Select Filament"]`; the step 12 job ends without an error; the dialog lists `#3366CC` and no `#26A69A` |
| 17 | `dialog_answer {dialog: {title: "Select Filament"}, button: "cancel"}` | the step 11 job ends without an error; `dialogs()` `[]`; `colours()` `#26A69A`: a cancelled pick changes nothing |
| 18 | steps 11 (its click), 12, 15 and 16 again, then `dialog_answer {dialog: {title: "Select Filament"}, button: "ok"}` | `dialogs()` `[]`; `colours()` `#3366CC`: the colour the picker returned is slot 1's |
| 19 | `project_save {path: OUT/colour.3mf}` | `project_file()` of it gives `filament_colour` `["#3366CC"]` |
| 20 | `action_run` Setup Wizard | `blocked_by_dialog`; `dialogs()` `["Setup Wizard"]`; its page lists `Europe` within 30 s |
| 21 | `widget_click {target: {web: "Europe", WIZARD}}` | reply `checked` true; `widget_list` of the wizard lists `Europe` checked |
| 22 | `widget_click` by the ref `widget_list {target: "dialog"}` gives the page's `Next` button | reply `navigated` true with a `url` holding `/guide/21/`, the printer page |
| 23 | once `webview_run_javascript {code: "location.pathname"}` holds `/guide/21/`, waited up to 30 s, `widget_click` by the step 22 ref again | refused with "page changed": a ref from before the page navigated is stale |
| 24 | `webview_run_javascript {code: "location.reload(); 1"}` | reply `navigated` true: a reload at the same URL is told as a navigation |
| 25 | once the page lists the card, waited up to 30 s, `widget_click {target: {web: "Nozzle 0.4 Marlin", kind: "button", WIZARD}}`, the Generic Marlin Printer's card | reply `checked` true; `widget_list` of the wizard lists that card checked |
| 26 | `dialog_answer {dialog: {title: "Setup Wizard"}, button: "close"}` | reply `closed` true; the step 20 job ends; `dialogs()` `[]`; `region` still `""`, the selected printer still `MyKlipper 0.4 nozzle`, and `preset_list` still offers no `MyMarlin 0.4 nozzle`: closing the wizard keeps neither choice |
| 27 | `widget_click {target: {label: "Add one filament"}}`, then `widget_click {target: {label: "Flushing volumes"}}` | `dialogs()` `["Flushing volumes for filament change"]` within 30 s; then the test waits up to 30 s for the dialog's web view to be listed at `WipingDialog.html`, as at step 6 |
| 28 | `webview_run_javascript {code: "typeof document.title"}` | `result` `string`, and the reply's `url` holds `web/flush/WipingDialog.html`, the page the dialog loads |
| 29 | `dialog_answer {button: "close"}` | the step 27 click's job ends without an error; `dialogs()` `[]` |

**Scenario: a drop-down list**

The list is the plate type's, which the sidebar shows only for a Bambu Lab printer or one with
`support_multi_bed_types` (`Sidebar::update_all_preset_comboboxes`); without it the Klipper printer has one plate
type, its default `High Temp Plate`. The scenario runs last and starts with `start(c)`, compared with `{printer:
KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`; then `settings_set {type: "printer", values:
{support_multi_bed_types: "1"}}`, an unsaved edit of the printer that the next `start()` discards; the sidebar's
Global/Objects switch on Global, as `start()` leaves it, so that the sidebar shows one plate type list; and the plate
type set with `project_set_settings {values: {curr_bed_type: "Cool Plate"}}`, since the plate type the switch finds is
stored state (see **To settle**). The start is checked: the project's `curr_bed_type` is `Cool Plate`.

| # | Step | Checked after |
|---|---|---|
| 1 | `widget_click {target: {class: "ComboBox", label: "Smooth Cool Plate"}}` | the popup's rows are exactly, in order, `Smooth Cool Plate`, `Engineering Plate`, `Smooth High Temp Plate`, `Textured PEI Plate`, `Textured Cool Plate`, `Cool Plate (SuperTack)` |
| 2 | `widget_click {target: {row: "Textured PEI Plate"}}` | reply `clicked` `Textured PEI Plate`; the project's `curr_bed_type` `Textured PEI Plate`, and `plate_list` gives plate 0 that type |
| 3 | `project_save {path: OUT/plate_type.3mf}` | `project_file()` of it gives `curr_bed_type` `Textured PEI Plate`: the pick is kept in the project file |
| 4 | `widget_set_value {target: {class: "ComboBox", label: "Textured PEI Plate"}, value: "Smooth Cool Plate"}` | `curr_bed_type` `Cool Plate` |
| 5 | `app_state` | `dialogs` `[]`, `closed_menus` `[]` |

**To settle in review:**
- The object menu's `checked` for a new cube (menus, step 1; rows, step 9): `Printable` and `Auto Drop`, as an
  instance is printable and an object's `auto_drop` starts true (`Model.hpp`); Flush Options' check items show only
  when the plate holds two filaments. The object menu (`create_extra_object_menu()`) has no `Assemble`, which the
  multi-selection menu adds while only objects are selected (`can_merge_to_multipart_object()`).
- The empty space's menu `DEFAULT` (menus, step 12), from `create_default_menu()` and `handy_models()` in
  `GUI_Factories.cpp`, with labels off, as the start sets them. Writing it whole makes a new handy model a change to
  this entry.
- Filament 1's `Click to edit preset` button (menus, step 19) is the first of that name with a position and a size in
  `widget_list`. The printer's button of that name comes first in the listing but is hidden until the pointer is over
  its panel, and on Windows and macOS `{label: "Click to edit preset", index: 0}` still names it, as the locator's
  index counts it (its menu is `Edit preset` and `Add/Remove presets`); only on Linux is index 0 filament 1's. So the test names the button by the ref of the first one shown. Its menu is
  `create_filament_action_menu()`'s. The Klipper printer is a single-extruder multi-material printer
  (`single_extruder_multi_material` 1 in `fdm_klipper_common`), so the button opens that menu, and its `Merge with`
  and `Delete` items are there, as on a Bambu Lab printer (`Sidebar::should_show_SEMM_buttons`); `Merge with` names
  the other slot by its preset's alias, the name before ` @`.
- A call that opens a dialog the start's rules answer (the Add Cube action's size dialog, Add Modifier's tip) is
  followed to its end through that dialog, not judged by its first reply: under load the reply is at times
  `blocked_by_dialog` for a dialog whose rule has queued its press but not yet made it. The test passes with or
  without that; a reply that waits on a dialog no rule answers still fails the call after 60 s.
- Two part rows that select every part of the object (rows, step 12) make the 3D view's selection the whole object:
  `Selection::update_type()` switches to the instance mode when every part of the one instance is selected (on
  `main` since 1555904beff), and `app_state.selection` lists `parts` only for a selection of parts. So `selection()`
  reads objects `[0]` with no parts, while the reply's `selected` lists both rows.
- A plate whose only object is not printable slices to `empty` (rows, step 16): `classify_plates()` gives `empty` to a
  plate without printable instances. If the slice is refused instead, the step asserts that refusal's code.
- Compare presets (dialog rows, steps 2 to 4): the row value `0.2 | 0.12` (the cells after the label, joined) and the
  header row's label. Index 5 is the process preset's right-hand combo, the printer's and filament's coming first and
  the SLA material's being hidden. Escape closes the window: a wx dialog's Escape presses its `wxID_CANCEL` button,
  which `DiffPresetDialog` has (its `DialogButtons` Cancel), and that button's handler hides the dialog.
- Compare presets' Transfer (dialog rows, steps 5 and 6): the checkbox "Transfer values from left to right" shows the
  rows' toggle column and the Transfer button, which is enabled while a row is selected and the process preset has no
  unsaved changes, a state the dialog sets as the app idles (`wxEVT_UPDATE_UI`), so a click right after the row toggles
  can find the button still disabled, and the step waits for it; Transfer hides the dialog and the process tab's
  `transfer_options()` selects the right-hand preset and gives it the toggled rows' left-hand values
  (`MainFrame::make_diff_dialog()`, `Tab.cpp`). A row's toggle starts on (`ModelNode::m_toggle`) and is set as a row's
  checkbox, which `widget_set_value` takes. The two presets' values come from the Custom vendor's profiles:
  `layer_height` 0.2 against 0.12, `bottom_shell_layers` 3 (`fdm_process_common`) against 5 (`0.12mm Fine @MyKlipper`).
- The print button's popup (dialog rows, steps 11 to 13): a printer of another vendor than Bambu Lab, with
  `use_printer_agents` off, offers `Print` and `Export G-code file`, and `Export plate sliced file` only when the
  printer takes a G-code 3MF (`use_3mf`), which the Klipper printer does not (`MainFrame::available_print_actions`).
  The test picks `Print` and then `Export G-code file`, so that each pick changes the label the other left, whatever
  the button showed first; picking `Print` sends nothing, and the button is disabled until a slice.
- The plate type is kept per printer (drop-down list, start): every pick, and every plate type the sidebar sets,
  is stored in the app config under the printer (`Plater::priv::on_select_bed_type`, which
  `set_bed_type_accord_combox` runs too), and while the printer supports several plate types the sidebar takes the
  stored one when it shows the printer (`Sidebar::update_all_preset_comboboxes`). The Klipper printer's saved preset
  has `support_multi_bed_types` off, so the sidebar sets and stores its default, `High Temp Plate`, whenever it shows
  the printer: at launch, and when `start()` discards the switch. Turning the switch on then finds `High Temp Plate`,
  also after a run killed between steps 2 and 4, whose `Textured PEI Plate` the next launch overwrites (measured).
  The start still sets the plate type it needs rather than relying on that.
  `project_set_settings` sets the list as a pick does. In the Objects mode the sidebar can show the plate's own `Bed
  type` list too; on Global the list the locator names is the only one. With `support_multi_bed_types` on, the
  Klipper printer's list offers the six plate types step 1 reads, the ones `project_get_settings` gives as
  `curr_bed_type`'s `offered`.
- A cell's click in the parameter table selects its part in the object list (dialog rows, step 9), as the table's
  cell selection calls the object list's `select_items()`. The table lists an object's row and, for an object of
  more than one part, a row per part (`construct_object_configs()`). The table then builds the part's settings
  beside it, and when that outlasts the reply's time the click replies `blocked_by_dialog` and its job ends once the
  dialog closes: measured in Expert mode on the app's first build of those settings, and under load in Advanced
  mode. So the click's result is read at its end; the selection is read before the close.
- A double click frames at once (rows, step 8): the camera's target is set to the selection's box centre in the
  click's own handling (`Camera::zoom_to_box()`).
- The Select Filament dialog shows `#26A69A` as hex, not a colour name (web dialogs, step 11), as that colour has no
  Bambu colour code; and its OK stores the picked colour as `#3366CC`, in capitals (step 18).
- The login region reads `""` (web dialogs, start and step 26): a new data folder stores none, no tool can set it,
  and no test finishes the wizard, which is what stores one.
- The wizard card `Nozzle 0.4 Marlin` (web dialogs, step 25) is the Generic Marlin Printer's: the printer page names a
  Custom printer by the second word of its model and lists its nozzles (`CreatePrinterBlock` in
  `web/guide/21/common.js`); the Klipper printer's card, installed, reads `Nozzle 0.4 · 0.2 · 0.6 · 0.8 Klipper` and
  starts checked.
- The preset bundle page (web dialogs, step 2), from `PresetBundleDialog/index.html`: the Refresh button and the
  Close element by their ids, and the top list's column headings as text; its auto-update switch is commented out of
  the page, and its bundle lists, which hold what the data folder holds, are not read.
- The web view a script runs in (web dialogs, steps 1 to 9 and 28): `webview_run_javascript` without a target takes
  the topmost modal's web view, else the only one shown (`default_webview()` in `Tools/GuiTools.cpp`), and refuses a web
  view in a dialog under the topmost, or behind a modal, before it runs (`guard_page()`); no dialog is named there.
  The Flushing volumes dialog (steps 27 to 29) runs that same code as step 4; it stays as the one web dialog here
  whose web view the app makes with `wxWebView::New` (`WipingDialog`), where the bundle dialogs' come from
  `WebViewHostDialog`'s `WebView::CreateWebView`.
- The Export dialog's title, `Export Preset Bundle` (web dialogs, step 6), is the one
  `ExportPresetBundleDialog::create_webview()` sets; the Flushing volumes dialog's is `Flushing volumes for filament
  change` (`WipingDialog`).
- Two labels' captures differ (menus, steps 13 and 15): the object's label is drawn into the 3D view, which the
  canvas capture holds, so turning it on or off changes the picture's bytes.

**Not checked:**
- The refusals of a disabled menu item, an input that opened no menu, a bad column, a row without a checkbox and a
  locator that names two rows: unit tests in `tests/slic3rutils/test_mcp_results.cpp` cover them on built menus and
  lists.
- The object menu and the multi-selection menu whole: they hold items that depend on the object, the selection and
  the slots (Edit Text, cut info, Change Filament, Flush Options, Fix Model), so only the sets named are fixed.
- The menu items only listed, not run (the other add submenus, `Auto Drop`, the handy models, `Add Models`): the
  commands behind them are other tests' (`test_speed_dial` runs one handy model's action, and `object_add_primitive`
  is run throughout the objects tests).
- What the labels show: the canvas captures are compared with each other only, so a label drawn wrong but drawn
  passes.
- `closed_menus` is checked empty with no control: a call fills it only when it opens a second menu, which the unit
  test "a menu opened while a call acts on the UI is listed, run by its path or closed unseen, never shown" covers.
  Each `closed_menus` `[]` is of the menus closed since the scenario's start (`menus_left()`): the list keeps the
  app's run's last three, and an earlier test in the same app can leave one there.
- Editing a parameter table cell: `widget_set_value` sets no table cell, so the GUI tools cannot edit the table.
- Compare presets' transfer to presets of other types, or of more than one value.
- What the print button does in the mode picked: `test_files` exports the sliced file through `project_export`. The
  popup of a Bambu Lab printer (Print plate, Send, Export plate sliced file and the rest): only the Klipper printer's
  two items are checked.
- The plate type lists of other printers: only the Klipper printer's, with `support_multi_bed_types` on, is read.
- Finishing the wizard, which installs printers and filaments: the install tests own installs. What the reloaded
  printer page holds beyond the card step 25 ticks.
- The printers other tests installed: the printer list is read for the one printer the wizard's card names.
- Closing a dialog from its page by a script (`webview_run_javascript` clicking `#close_btn`): it takes the branch
  step 10's click takes. Both tools follow their page call with `observe_page()` (`Tools/GuiTools.cpp`), which replies
  `closed` once the dialog leaves the modal stack, so step 10 covers it.
- How soon the page's Close is told (web dialogs, step 10): the web view's destruction ends the click's wait for its
  page (`PageCalls` in `WebPages.cpp`), and without that the click is refused after its 10 s, so the reply's `closed`
  stands for the promptness and no time is measured; on Windows the click replies in 0.1 to 0.15 s.
- The waits for the Export and Flushing volumes dialogs' web views to leave `about:blank` (steps 6 and 27), for the
  printer page's URL (step 23) and for its card (step 25) have no check of their own: one that runs out its 30 s ends
  the scenario with a failure naming what it waited for.
- The filament step 27 adds: only that the Flushing volumes button opened its dialog.
- A script with no target and no modal while a plugin's page is shown: the Home tab is the one shown.
- The colour the spectrum drag picks: the page works it out from the canvas's layout, so only "another valid colour"
  is checked.
- How menus, rows and dialogs look: nothing is shown or captured.

**Strength:** Strong — every menu item, row and page element it runs is checked by its effect on the project, the
selection, the camera or the settings, and every script by the page it ran in or the refusal's words; only the
spectrum drag's colour is checked as another valid colour.

## `test_recording`
`test_recording.py` · Python, 112 checks with an ffmpeg that has libx265 and libx264 · in `sweep.json` as `{"path": "test_recording.py"}`

**Tests:** `recording_start`, `recording_stop` and `recording_encode`: a recording made while the GUI is driven, its
report and its file; a recording a nested `replay_run` starts and never stops, which the replay's end closes;
overlays and the time limit; and encoding asked for at the start, run as a job by the stop, deleting or keeping the
`.mkv`. The recordings are read back from their files, and every encode is checked when the machine has an ffmpeg
with the encoder it needs.

**Starts from:** `start(c)`, the Generic Klipper Printer, compared with `{printer: KLIPPER, process: KLIPPER_STD,
filaments: [GENERIC_PLA]}`: nothing it records is a Bambu Lab printer's own, and its G-code check reads this printer's
layer marks. Then it reads back the baseline's window size, 1600 × 900 (`app_state.window.size`), since the click at
(300, 200) that must land on empty space is placed in it; the sidebar is on Global, the baseline's, so it shows the
process fields the first scenario sets. The recordings' sizes are measured from captures of the same run, so they
follow whatever size the window has. Recordings go to the test's output folder `OUT`, and ffmpeg is found from the
environment, not the data folder: the app keeps no folder or encoder setting there that a check reads.

**Leaves:** The Klipper printer with `0.20mm Standard @MyKlipper` and, as an unsaved change, the layer height 0.16
the first scenario set, which each scenario's `project_new` keeps and the next `start()` discards; the encode
scenario's empty project; no recording running. Not the window, whose placement the app stores only when it closes
itself, and the runner ends it. The last folder opened, the handy models' folder of the import. Nothing else in the
data folder: its recordings, replay and G-code are in `OUT`.

**Reads:** `recording()` gives `app_state.recording`, or none. `mkv(path)` reads a Matroska file the way the app
writes it: its codec id, its video's pixel width and height, its default frame duration, its number of blocks
(frames) and its duration. `mp4(path)` reads an MP4's first sample entry (`hvc1` for H.265, `avc1` for H.264) and
its movie header's duration. `fitted(w, h)` is the video size the app makes of a `w` × `h` capture: scaled down so
its longer side is at most 1920, then each side rounded down to even (`video_size()`). `capture(target)` is the
`width` and `height` `window_screenshot {target, include_image: false}` reports. `ffmpeg()` finds ffmpeg as the app
does: `ORCA_FFMPEG` when set, else `ffmpeg` on `PATH`, else on macOS `/opt/homebrew/bin`, `/usr/local/bin` and
`/opt/local/bin`; then it lists the encoders that ffmpeg has (`-hide_banner -encoders`). Every scenario starts with
`project_new {discard_changes: true}` in the Prepare view, checks that `recording()` is none and `plate_list` gives
one unnamed plate, current, and records into the test's output folder `OUT`.

**Without ffmpeg.** What `ffmpeg()` finds decides each encode. A direct `recording_encode` with the encoder runs its
checks; with ffmpeg but not the encoder it must be refused, saying ffmpeg has no `libx265` (or `libx264`) encoder;
with no ffmpeg it must be refused with the reason the app gives: ffmpeg was not found, or, with `ORCA_FFMPEG` set to
something that is not a file, that `ORCA_FFMPEG` names it. An encode asked for at the start is refused at the start
only when ffmpeg is missing; with ffmpeg but not the encoder, the start and stop succeed and the encode job ends
with the encoder's error. A refusal or an error is checked like any other result, and the file checks the encode
would have had are printed as `SKIP <check>: <why>`, never passed silently. The test's last line names what it could
encode: `encodes checked: h265 yes, h264 no (ffmpeg has no libx264)`. The test's checks gain `skip(name, why)` for
this, in a subclass of `Checks` the test holds; see **To settle**.

**Scenario: a recording while the GUI is driven**

`MAIN` is `fitted(capture("main"))`, read before step 1.

| # | Step | Checked after |
|---|---|---|
| 1 | `recording_start {save_path: OUT/session.mkv, overwrite: true, captions: true, pointer: true}` | reply `path` `OUT/session.mkv`, `target` `main`, `fps` 10, `captions` true, `pointer` true, `max_seconds` 3600, `width` and `height` `MAIN`; `recording()` has that `path` and `target` `main` |
| 2 | `project_import` of `OrcaCube_v2.drc` | reply `added` 1; `object_list` total 1; `app_state.selection` objects `[0]`, as an import selects what it adds |
| 3 | `mouse_gesture {action: "click", x: 300, y: 200}` | `app_state.selection` empty: a click on empty space deselects |
| 4 | `mouse_gesture {action: "click", at: {object: 0, spot: "top"}}` | `app_state.selection` objects `[0]` |
| 5 | `widget_set_value {target: {option: "layer_height"}, value: "0.16"}`, a field of the sidebar on Global | `settings_get` of the process gives `layer_height` `"0.16"` |
| 6 | `keyboard_press {target: {option: "initial_layer_print_height"}, keys: "ctrl+a", text: "0.24"}`, then `keys: "Return"` | the first reply is exactly `{sent: "ctrl+a", to: "TextInput", typed: "0.24"}`; once `app_wait {for: "idle"}` holds, `initial_layer_print_height` is still `"0.2"`: typed keys reach ImGui fields and key handlers, not a native text field |
| 7 | `action_run` Preferences, then `dialog_answer {dialog: {title: "Preferences"}, button: "close"}` | the first replied `blocked_by_dialog` with `Preferences`; its job ends without an error; `app_state.dialogs` `[]` |
| 8 | `plate_slice`, then `project_export {path: OUT/session.gcode, overwrite: true}` | plate 0's outcome `ok`; the G-code's config block holds `; layer_height = 0.16` and `; initial_layer_print_height = 0.2`, and its first two distinct `;Z:` values are 0.2 and 0.36: the value set shaped the layers printed, and the keys typed did not |
| 9 | `recording_stop {silent: true}` | reply `ended` `stopped`, `path` `OUT/session.mkv`, `frames` and `written` at least 5, `failed` 0, `seconds` above 1, `capture_ms` above 0, `width` and `height` `MAIN`, `size_bytes` the file's size; `recording()` none. `mkv()`: codec `V_MJPEG`, `MAIN`, default frame duration 100 ms, `written` blocks or one more (the last frame held to the end), duration within 0.1 s of `seconds` |
| 10 | `recording_encode {save_path: OUT/session.mp4, overwrite: true, keep_raw_video_file: true}` | with libx265: reply `path` `OUT/session.mp4`, `source` `OUT/session.mkv`, `codec` `h265`, `encoder` `libx265`, `raw_video_file_deleted` false, `size_bytes` the file's size; `mp4()`: `hvc1`, duration within 0.3 s of step 9's `seconds`; `OUT/session.mkv` is still there. Without: refused as above; no `OUT/session.mp4`, and `OUT/session.mkv` is still there |

**Scenario: a recording a replay starts**

`CANVAS` is `fitted(capture("canvas"))`, read before step 1 in the Prepare view.

| # | Step | Checked after |
|---|---|---|
| 1 | `replay_run` of a replay whose steps are `recording_start {target: "canvas", fps: 5, save_path: OUT/inner.mkv, overwrite: true}` (with the expects `/target` `canvas` and `/fps` 5), `view_set {camera: "iso"}` and a `mouse_gesture` drag from (400, 300) to (500, 340) | reply `passed` true, no `unfinished_recording`; `recording()` none: the recording ended with the replay |
| 2 | `recording_stop {silent: true}` | reply `ended` `run_ended`, `path` `OUT/inner.mkv`, `written` at least 1, `failed` 0, `width` and `height` `CANVAS`; `mkv()`: `CANVAS`, default frame duration 200 ms (5 fps), `written` blocks or one more |
| 3 | `recording_stop {silent: true}` | refused, `No recording is running`: an ended recording is reported once; `recording()` none |
| 4 | `recording_encode {overwrite: true, codec: "h264"}` | with libx264: reply `path` `OUT/inner.mp4`, `source` `OUT/inner.mkv` (the session's last finished recording), `encoder` `libx264`, `raw_video_file_deleted` true; `OUT/inner.mkv` is gone and `mp4()` gives `avc1`. Without: refused as above; no `OUT/inner.mp4`, and `OUT/inner.mkv` is still there |
| 5 | `recording_start {save_path: OUT/inner.mkv, overwrite: true}` | it starts: reply `captions` false and `pointer` false, by default; `recording()` has `OUT/inner.mkv`: the replay's end released the file |
| 6 | `recording_stop {silent: true}` | `ended` `stopped`; `recording()` none |

**Scenario: overlays and the time limit**

| # | Step | Checked after |
|---|---|---|
| 1 | `recording_start {save_path: OUT/plain.mkv, overwrite: true, pointer: true}` | reply `captions` false, `pointer` true; `recording()` has `OUT/plain.mkv` |
| 2 | `recording_stop {silent: true}` | `ended` `stopped`; `recording()` none |
| 3 | `recording_start {save_path: OUT/plain.mkv, overwrite: true, captions: true}` | reply `captions` true, `pointer` false; `recording()` has `OUT/plain.mkv` |
| 4 | `recording_stop {silent: true}` | `ended` `stopped`; `recording()` none |
| 5 | `recording_start {save_path: OUT/short.mkv, overwrite: true, max_seconds: 2}` | reply `max_seconds` 2; `recording()` has `OUT/short.mkv` |
| 6 | wait up to 30 s for `recording()` to be none | the recording ended on its own; a wait that runs out fails |
| 7 | `recording_stop {silent: true}` | reply `ended` `max_seconds` and `seconds` at least 2: the limit is what ended it, once it was reached; `mkv()`'s duration within 0.1 s of `seconds` |
| 8 | `recording_start {save_path: OUT/wrong.avi}` | refused, saying `save_path must end with .mkv`; `recording()` none; no `OUT/wrong.avi` |

The default limit of an hour is step 1 of the first scenario (`max_seconds` 3600).

**Scenario: an encode asked for at the start**

| # | Step | Checked after |
|---|---|---|
| 1 | `recording_start {save_path: OUT/encoded.mkv, overwrite: true, encode: true, encode_preset: "compatible", crf: 30}` | reply `encode` is `{codec: h264, crf: 30, speed: medium, profile: high, keep_raw_video_file: false}`; `recording()` has `OUT/encoded.mkv`. With no ffmpeg: refused, saying `Cannot encode: ` and the reason above, `recording()` none and no `OUT/encoded.mkv`; the rest of the scenario is printed as `SKIP` |
| 2 | `recording_stop {silent: true}` | reply `path` `OUT/encoded.mkv`, `ended` `stopped`, and an `encode`: either `status` `running` with a `job`, or the encode's own result or error when it finished first; `recording()` none |
| 3 | `app_wait {for: "job", job}` when step 2 gave a job; otherwise step 2's `encode` is the result checked | with libx264: the result has `path` `OUT/encoded.mp4`, `encoder` `libx264`, `raw_video_file_deleted` true; `OUT/encoded.mkv` is gone, `mp4()` gives `avc1` and a duration within 0.3 s of step 2's `seconds`. With ffmpeg but not libx264: the result is an error saying ffmpeg has no `libx264` encoder; `OUT/encoded.mkv` is still there and there is no `OUT/encoded.mp4` |
| 4 | `recording_start {save_path: OUT/kept.mkv, overwrite: true, encode: true, encode_preset: "compatible", keep_raw_video_file: true}` | reply `encode.keep_raw_video_file` true; `recording()` has `OUT/kept.mkv` |
| 5 | `recording_stop {silent: true}` | an `encode`, a job or a result as in step 2; `recording()` none |
| 6 | `app_wait {for: "job", job}` when step 5 gave a job; otherwise step 5's `encode` is the result checked | with libx264: `raw_video_file_deleted` false; `OUT/kept.mkv` and `OUT/kept.mp4` both there, `mp4()` `avc1`. With ffmpeg but not libx264: the same error; `OUT/kept.mkv` is still there and there is no `OUT/kept.mp4` |

**To settle in review:**
- How a missing ffmpeg shows in the sweep. The proposal: `Checks.skip(name, why)` prints `SKIP <name>: <why>` and
  counts it, and `run_sweep.py` adds the count of a passing test's `SKIP` lines to its result line (`passed, 9
  skipped`), so a sweep on a machine without ffmpeg says so without reading the log. The other choice is an
  environment variable that turns the encode `SKIP`s into failures on a machine that must have ffmpeg. Either is a
  change to the harness, not to the app. Until it is settled, `skip()` lives in the test's own subclass of `Checks`,
  and a skipped check is not counted among the checks.
- Typed keys leave a setting's field as it was (first scenario, step 6): `keyboard_press` sends key events to the
  field's `TextInput`, which a native text control does not take as typing; its description says typing reaches
  ImGui fields and key handlers and sends a widget's text field to `widget_set_value` (`Tools/GuiTools.cpp`, this
  branch's d64e10f4dd). The step stays as the recording's typed caption, and step 8 shows that the first layer kept
  its 0.2 mm while the layer height set by `widget_set_value` took effect.
- A recording's first frame is the size `window_screenshot` captures of the same target (`MAIN`, `CANVAS`): both
  capture the window, or the 3D view, in physical pixels; the recorder then fits the capture with `video_size()`.
- The file holds `written` blocks or one more (first scenario, step 9): closing the sink writes the last frame again
  at the end time when the recording ran past it.
- A recording that reached its limit is reported by the next `recording_stop` with `ended` `max_seconds` (overlays,
  step 7), as `recording_stop`'s description says; the reason is set when the timer ends it, before the stop.
- The stop's `encode` (encode, steps 2 and 5) is nearly always `running` with a job, with or without the encoder: the
  stop starts the encode with no reply budget. `McpCore` checks for a finished job before the budget, so an encode
  that ended first comes back as its result or error, and the test takes either.
- The `seconds` an encode reports and the MP4's duration: within 0.3 s of the recording's, as the encode keeps the
  frames' timing; the tolerance is a proposal.
- The Klipper printer's G-code holds the config block after the moves and marks each layer with `;Z:<z>` (first
  scenario, step 8); `GCode.cpp` writes the block before the moves and `; Z_HEIGHT: <z>` for a Bambu Lab printer
  only. Its own layer-change G-code writes the height as `;<z>` without `Z:`, so the `;Z:` lines are the app's marks
  alone. `0.20mm Standard @MyKlipper`'s first layer is 0.2 mm (`fdm_process_common` of the Custom vendor).

**Not checked:**
- The frames' pixels: the captions and the pointer are drawn into MJPEG frames that the sweep's Python cannot decode
  without an image library. Their drawing is unit-tested in `tests/slic3rutils/test_mcp_recording.cpp` ("the caption
  is centred at the bottom with its spans in their roles' colours", "the pointer's ring leaves what it circles
  uncovered").
- The exact number of frames and the capture time: they depend on the machine's speed, so `frames`, `written` and
  `capture_ms` have lower bounds, and the file is checked to agree with the report.
- How soon after its limit a recording ends (overlays, step 7): the recorder's timer ends it at its first tick past
  the limit, which a loaded machine delays, so `seconds` has a lower bound only and the wait for the end lasts up to
  30 s; a recording that ran on past its limit but ended within those 30 s would pass.
- The person's forms: the Start Recording dialog, which `test_speed_dial`'s setup drives to point its folder at its
  output folder, and the Stop Recording action, which no test runs as an action.
- What `speed`, `profile` and `crf` do to the MP4: the ffmpeg command lines are unit-tested ("the ffmpeg command takes
  the speed and profile, and main10 encodes 10 bits").
- `job_cancel` of an encode, a write error and the size limit: the sink's unit tests cover write errors.
- A recording ending when a script ends or the server is turned off: only a replay's end is tested.

**Strength:** Strong — each recording's report is checked against its file, and each encode against its MP4, where an
ffmpeg with the encoder is present; frame counts and capture times have lower bounds, and the frames' pixels are left
to the unit tests.

## `test_call_forms`
`test_call_forms.py` · Python, 95 checks · in `sweep.json` as `{"path": "test_call_forms.py"}`

**Tests:** The forms a call takes. Names and value shapes agents send in place of the schema's (a lone path for
`paths`, `object` and a string index for `objects`, `action` and an action's short name for `id`, a camera direction
as `view_set`'s `canvas`, `key` for `keys`, `plate` as a string, legend rows in another case or with `_`, a gizmo by
its toolbar title in any case, `app_wait` with no condition, `project_export` without `format`), each shown by what
the call did; `settings_list` leaving meta out of a whole listing. And a call with exactly an action's arguments
doing what a person's pick does: `calibration_generate_test {test}` asking in the test's dialog where `silent` asks
nothing, View: Top turning the camera of the view shown, Deselect All closing the open gizmo too, Go to layer
through `action_run`, and `plate_slice {preview: true}`.

**Starts from:** `start(c)`, the Generic Klipper Printer, compared with `{printer: KLIPPER, process: KLIPPER_STD,
filaments: [GENERIC_PLA]}`: no form it checks is a Bambu Lab printer's own. It sets no other stored setting: no step
clicks the canvas at a pixel, reads a field's text or captures a picture, and the Preview's legend rows it reads are
listed whatever view type the Preview last showed, which the app keeps in the data folder.

**Leaves:** The Klipper printer with `0.20mm Standard @MyKlipper` and its filament unmodified, since the shown-view
scenario's opening `project_new` discards what the silent calibration changed. That scenario's project, its 20 mm cube
sliced and shown in the Preview, and the auto-answer rule for the restore prompt, which the next `start()` replaces
and clears. The last folder opened, the handy models' folder of the imports. It saves no project; the G-code and STL
it exports are in `OUT`.

**Reads:** `objects()` gives each object from `object_list` as its name, plate, first instance's `offset` and
`bbox`. `selection()` gives `app_state.selection` as `{objects, parts}`, or empty. `camera()` gives
`app_state.camera`'s `elevation`, `azimuth`, `look_at` and `zoom`. `legend()` gives `plate_slice_stats {include:
["view"]}`'s `view.legend`. `modified()` gives the printer, process and filament presets `preset_list` marks
modified. `stl(path)` reads a binary STL's triangle count and size. Every scenario starts with `project_new
{discard_changes: true, preset_changes: "discard"}` in the Prepare view, an auto-answer rule for the restore prompt
(No) registered, and checks that `objects()` is `[]`, `app_state.dialogs` `[]` and `plate_list` one unnamed plate,
current. `CUBE` is `resources/handy_models/OrcaCube_v2.drc`; `OUT` is the test's output folder.

**Scenario: names and values agents send**

| # | Step | Checked after |
|---|---|---|
| 1 | `project_import {paths: CUBE}`, a lone path | reply `added` 1; `objects()` names `[OrcaCube_v2.drc]` |
| 2 | `project_import {paths: [CUBE]}` | names `[OrcaCube_v2.drc, OrcaCube_v2.drc]` |
| 3 | `object_select {objects: [0]}` | `selection()` objects `[0]` |
| 4 | `object_select {object: "1"}` | `selection()` objects `[1]` |
| 5 | `object_rename {name: "renamed"}` | reply `ran` `object_rename`; names `[OrcaCube_v2.drc, renamed]`: the selected object was renamed |
| 6 | `action_run {action: "view_top"}` | reply `ran` `orca_command:view_top:orca`; `camera()` elevation 90 to 0.1° |
| 7 | `view_set {canvas: "front"}` | reply `camera` `front`; `camera()` elevation 0 and azimuth -90, to 0.1° |
| 8 | `object_transform {object: 0, position: [100, 110, 0]}`, the same for object 1 | both `offset`s x 100 and y 110 to 0.01 mm: the two objects overlap |
| 9 | `object_select {objects: [0]}`, then `keyboard_press {key: "Escape", target: {name: "OrcaSceneCanvas"}}` | `selection()` objects `[0]` after the select, then empty: the key reached the 3D view as `keys` |
| 10 | `action_run` Preferences | reply `blocked_by_dialog`; `app_state.dialogs` `["Preferences"]` |
| 11 | a second client calls `app_wait {}`; this client waits (up to 30 s) until `replay_export`'s `left_out_running` is `[{tool: "app_wait"}]`, waits one second, reads `replay_export` and `app_state` again, then calls `dialog_answer {dialog: {title: "Preferences"}, button: "close"}` | a second after the wait was first seen unfinished, `left_out_running` is still `[{tool: "app_wait"}]` and `app_state.dialogs` still `["Preferences"]`: with no condition the wait does not return while a dialog is open. After the answer, the wait replies `satisfied` true with `waited_ms` at least 1000, `replay_export` has no `left_out_running`, and `app_state.dialogs` is `[]`. The two replies' order is not checked |
| 12 | `object_select {objects: [0]}`, then `widget_click {target: {gizmo: "Support Painting"}}` | `selection()` objects `[0]` after the select; reply `gizmo` `FdmSupports`, `active` true; `app_state.gizmo.name` `FdmSupports` |
| 13 | `widget_click {target: {gizmo: "support painting"}}` | reply `active` false; `app_state.gizmo` null |
| 14 | `object_arrange {plate: "0"}` | both objects on plate 0, their `bbox` footprints apart and inside plate 0's rectangle (`origin` and `size` from `plate_list`) |
| 15 | `plate_slice {plate: "0"}` | reply `plates` is one plate, its `index` 0 and `outcome` `ok`; `plate_list` gives plate 0 `sliced` true |
| 16 | `view_set {tab: "preview", legend: {sparse_infill: false, Travel: false, "Prime tower": false, Pauses: false}}` | reply `legend` exactly `{"Sparse infill": false, "travel": false}`: rows this print lacks are left out, not refused; `legend()` gives `features["Sparse infill"]` false and `options["travel"]` false |
| 17 | `view_set {legend: {"Sparse infill": true, travel: true}}` | `legend()` gives both true |
| 18 | `view_set {tab: "prepare"}`, then `project_export {path: OUT/forms.gcode, overwrite: true}` | `app_state.tab` `prepare`; reply `export` `ok`; the file starts with `; HEADER_BLOCK_START`: the format came from the path |
| 19 | `project_export {path: OUT/forms.stl, overwrite: true}` | reply `objects` lists objects 0 and 1, by their `index`, and `bytes` is the file's size; `stl()`: its triangle count is the sum of the two objects' parts' `triangles` from `object_list`, and its size 84 + 50 × that count |
| 20 | `settings_list {type: "process", limit: 5}` | 5 items, each with a `value` and none with `meta` |
| 21 | `settings_get {type: "process", keys: ["wall_loops"]}` | `items[0].meta.type` `int` |

**Scenario: a test's dialog, asked or silent**

The start also checks that `app_state.project.name` is `Untitled` and `modified()` is empty.

| # | Step | Checked after |
|---|---|---|
| 1 | `calibration_generate_test {test: "temperature"}` | reply `status` `blocked_by_dialog`; `app_state.dialogs` `["Temperature calibration"]` |
| 2 | `dialog_answer {dialog: {title: "Temperature calibration"}, button: "cancel"}` | the step 1 job ends without an error; `app_state.dialogs` `[]`; `objects()` `[]`, `project.name` `Untitled`, `project.dirty` false and `modified()` empty: Cancel changes nothing |
| 3 | `calibration_generate_test {test: "temperature", silent: true, discard_changes: true, preset_changes: "discard"}` | reply `test` `temperature`, `names` `[temperature_tower.drc]`; `objects()` names `[temperature_tower.drc]`; `project.name` `Nozzle temperature test`; `app_state.dialogs` `[]`: silent asks nothing |

**Scenario: the shown view's camera and selection, and a slice shown in the Preview**

It starts with `CUBE` imported and sized to 20 mm (`object_transform {object: 0, size: [20, 20, 20]}`). The start
is checked: `objects()` is one object, on plate 0, `bbox.size` `[20, 20, 20]` to 0.01 mm.

| # | Step | Checked after |
|---|---|---|
| 1 | `view_set {tab: "prepare", camera: "front"}` | `app_state.tab` `prepare`; `camera()` elevation 0 to 0.1° |
| 2 | `view_set {tab: "design", camera: "front"}` | reply `camera_state.elevation` 0 to 0.1°; `app_state.tab` `design`, `camera()` elevation 0 |
| 3 | `action_run {id: "view_top"}` | reply `ran` `orca_command:view_top:orca` and `camera_state.elevation` 90 to 0.1°; `app_state.tab` `design`, `camera()` elevation 90 |
| 4 | `view_set {tab: "prepare"}` | `app_state.tab` `prepare`; `camera()` elevation 0: View: Top turned the Design tab's camera, the view shown, and not the Prepare view's |
| 5 | `object_select {objects: [0]}`, then `widget_click {target: {gizmo: "Support Painting"}}` | `selection()` objects `[0]`; `app_state.gizmo.name` `FdmSupports` |
| 6 | `action_run {id: "deselect_all"}` | reply `ran` `orca_command:deselect_all:orca`, `empty` true, `canvas` `3d`; `selection()` empty and `app_state.gizmo` null: Deselect All closes the open gizmo too |
| 7 | `plate_slice {preview: true}` | reply `status` `done`, plate 0's outcome `ok`; `app_state.tab` `preview`; `plate_list` gives plate 0 `sliced` true |
| 8 | `action_run {id: "go_to_layer", args: {percent: 50}}` | `plate_slice_stats {include: ["view"]}` gives `view.layer_count` 100 and `view.layers` `[1, 51]`: the top of the shown range moves to layer round(0.5 × 99) + 1 and its bottom stays |

**To settle in review:**
- Escape in the 3D view clears the selection (names, step 9): the canvas's key handling deselects all on Escape.
- Two clients' calls run at once (names, step 11): `app_wait` waits off the UI thread, so `dialog_answer` from
  another client goes through while it waits, as `test_web_ui`'s second client's calls do during a dialog's close.
  `app_wait` runs inline and makes no job, but it is journaled (`wait.journal`) from the moment `McpCore` admits it,
  so `replay_export` lists it under `left_out_running` until it replies (`Journal::to_replay()`); the Preferences
  action's call, which replied `blocked_by_dialog`, is kept in the replay and is not listed there. The journal is the
  app's, every client's calls in it, so the list is that one wait only while no call an earlier test made still runs:
  `start()` closes the dialogs such a call could wait on and waits for the jobs to end.
- An imported `.drc` keeps its extension in its object's name (names, step 1), as `temperature_tower.drc` does in
  the calibration replay.
- The Design tab's camera is its own (shown view, step 4): `test_set_view_camera` finds the Design zoom apart from
  the Prepare view's, so View: Top there leaves the Prepare view's elevation at 0.
- A new project is named `Untitled` (dialog, start and step 2), the name the Plater gives a project without a file.
- The temperature dialog's Cancel ends its job without an error (dialog, step 2); the reply's fields are not fixed.
- Deselect All closes the open gizmo (shown view, step 6), as `object_select`'s description says of `none`, which the
  action's arguments are.
- The 20 mm object slices to 100 layers at `0.20mm Standard @MyKlipper`'s 0.2 mm first and other layers, and Go to
  layer 50 % shows layers 1 to 51 (shown view, step 8): `show_layer_percent()` takes the top to `lround(0.5 × 99) + 1`.
- The exported G-code's first line is `; HEADER_BLOCK_START` (names, step 18): the Klipper printer has no file start
  G-code and no BTT thumbnail, either of which would come first (`GCode.cpp`).

**Not checked:**
- The normaliser's rules on their own (aliases, value forms, enum moves, the window pairs): unit tests in
  `tests/automation/test_mcp_protocol.cpp`. This test shows only that the forms work on the real schemas.
- Go to tab through `action_run`: `test_speed_dial` checks its effect and its journal step. The Slice and Preview
  action's effect, and Deselect All's clearing of the selection alone, are read by no test through the actions;
  `test_slice_preview` reads its slices made through `plate_slice`. Speed Dial picks of View: Top, Deselect All and
  Go to layer: `test_person_forms`.
- The silent calibration's parameters and preset changes: `test_presets`' calibration scenarios. A calibration
  dialog's OK: `test_speed_dial`'s `calib_retraction`.
- That a legend row a print lacks is never shown: no print here has a prime tower or a pause, so the reply's lack of
  `Prime tower` and `Pauses` has no control. An unknown row's refusal is `test_slice_preview`'s (a legend row clicked, step 11).
- Deselect All while the Design tab or the assemble view is shown.
- The G-code's content beyond its first line: names, step 18, checks only which format the path chose.

**Strength:** Strong — each form is shown by what the call did to the project, the view or the reply, not by the call
going through.
