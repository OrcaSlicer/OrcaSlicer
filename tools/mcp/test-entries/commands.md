# Commands, preferences, installs and plugins

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_every_action`
`test_every_action.py` · Python, 509 checks

**Tests:** Runs all 129 Speed Dial actions of `tests/data/speed_dial_actions.json` through `action_run`, each from a state rebuilt by calls (empty project, two cubes, sliced plates, an assembled object, a recording, and so on), and answers the action's dialog or file picker as its row says. For each action it reads back the action's effect, then checks that the reply names the action, that the journal keeps an `action_run` step for it with the arguments its command journals, and that `action_search` maps it to the command in `tests/data/speed_dial_action_calls.json`.

**Decides by:** Exits 1 when any printed `FAIL` check fails. A call error, or an uncaught exception from a row's lambda, also fails the test. Every action runs; none is skipped. What "passes" means depends on the row:
- Common to every row that runs (all but the 3 refused): `... its reply names the action it ran` (`ran` == `orca_command:<key>:orca`, skipped for `export_config`, whose dialog is cancelled). `... the journal keeps its action_run with the arguments its command journals` requires an `action_run` step of that id whose inner `args` equal one value exactly:
  - `{"percent": 50}` for `go_to_layer` and `{"tab": "preview"}` for `go_to_tab`, the values given for their input.
  - `{}`, no `args`, for every other action: its command journals nothing of its own, or, as for `obj_arrange` and `start_recording`, its action's fixed arguments are empty. The nine `calib_*` actions are among them: each opens its test's dialog (the person's form), and `calibration_generate_test` rewrites its journal entry (`ActionContext::set_journal_args`) only when it runs the test itself, with its parameters or `silent`.
  The test's docstring, which says every step carries the fixture's arguments, says this instead: an `action_run` step with the id alone replays the same, since the action supplies its fixed arguments.
  `... its action names the fixture's command` compares `action_search`'s `command` with the fixture's.
- Effect read back exactly, objects: `undo` (1 object), `redo` (2), `obj_delete` (only the other cube's id is left), `obj_delete_all` (none), `obj_mirror_x/y/z` (part centres move along that axis only, and instance `mirrored` is true), `obj_split_objects` (2 objects), `obj_split_parts` (1 object of 2 parts), `obj_center` (centre at 128,128 ±1 mm), `obj_drop` (min z 0 ±0.01), `obj_orient` (z size 20 ±0.5 after a 45° tilt), `obj_instances_up`/`obj_instances_down`/`obj_set_instances` (2/1/3 instances), `obj_instances_to_objects` (3 objects, 1 instance each), `obj_assemble` (1 object of 2 parts), the six `obj_type_*` (sorted part types), `obj_rename` (the typed name), the four `obj_order_by_*` (exact order), the ten `add_handy_*` (exact object names), `import_zip_archive` (1 object), `load_project` (2 objects, project path ends `load_me.3mf`), `new_project` (no objects).
- Effect read back exactly, plates and slicing: `slice_and_preview` (plate 0 sliced and tab `preview`), `slice_all` (2 plates, both sliced), `plate_add` (objects per plate `[2, 0]`), `plate_duplicate` (2 plates, the second holding 2 objects), `plate_delete` (1 plate, objects on `[0, None]`), `plate_rename` (the typed name), `plate_toggle_lock` (`locked` true), `plate_goto` (current 0), `go_to_layer` (top shown layer within 2 of half of `layer_count`).
- Effect read back exactly, app state: `mode_simple/advanced/expert` (`mode`), `go_to_tab` (`tab`), `select_all` (`[0, 1]`), `deselect_all` (`empty`), `filament_add`/`filament_delete` (2/1 slots), the eight camera `view_*` (elevation and azimuth ±2°; `view_top`/`view_bottom` check elevation only), `view_toggle_perspective` (`projection` differs), `start_recording` (recording, path in the output folder), `stop_recording` (no recording, and `stopped.mkv` is non-empty), `toggle_developer_mode` (`developer_mode` flipped), `install_local_plugin` (the fixture's name is in `plugin_list`), `refresh_plugins` (the side-loaded fixture's name is not in `plugin_list` before the refresh, which its `before()` reads, and is after).
- Effect read loosely: the nine `calib_*` (project `name` equals the test's name and at least 1 object), the six `add_primitive_<shape>` (1 object whose name contains the shape), `add_primitive_text` (Text gizmo open, 1 object), `obj_fit_volume` (x size > 200 mm), `obj_arrange` (both cubes on some plate), `view_fit_bed` (zoom smaller, `look_at` x within 5 mm of the bed centre), `reset_window_layout` (`sidebar_collapsed` false), `help_benchmark_3d_scene` (more than 1 object, a name containing "Orca", the benchmark window open), `toggle_run_script_panel` (3D view height differs), `help_tip_of_the_day` ("Daily Tips" ImGui window listed).
- File written, existence and size > 0 only: `export_gcode`, `export_stl`, `export_3mf`, `export_drc_single`, `export_toolpaths_obj`, `save_project`, `save_project_as`, `take_screenshot` (the reply's path is in the output folder). File count only: `export_stl_multi`, `export_drc_multi` (2 files each). Archive's G-code entries exactly: `export_sliced_file` (`plate_2` alone), `export_all_sliced_file` (`plate_1`, `plate_2`).
- Window or dialog opened, judged by title substring, then closed: `open_preferences`, `preset_bundle`, `export_config` (cancelled, and the call may error), `help_keyboard_shortcuts`, `help_setup_wizard`, `help_troubleshoot`, `help_network_test`, `help_about`, `help_migrate_filament_presets` (OK pressed), `open_action_replay`, `open_plugins`, `help_check_updates` (any dialog).
- File picker opened once (the auto-answer rule's `uses` == 1) and nothing loaded: `import_file`, `import_configs`, `add_primitive_svg`.
- Refused with an exact code, and no journal step: `send_gcode` (`no_print_host`), `sync_ams` (`no_printer`), `sync_presets` (`not_signed_in`). The seeded X1 Carbon prints through the Bambu network, where the `send_gcode` action opens the storage dialog, which messages a printer; so its row first makes the printer a print host with no address (`pre`: `settings_set {type: "printer", values: {bbl_use_printhost: true, host_type: "octoprint", print_host: ""}}`), and the action is refused before any dialog: `dialogs()` is empty and `app_state.uploads` is `[]` afterwards. Its `post` step, `settings_set {type: "printer", reset_all: true}`, discards those changes, after which `bbl_use_printhost` reads "0" and the printer preset has no unsaved changes, so every later row sees the X1 Carbon as it was.
- Hand-off to another program, only `ran` checked: `help_open_config_folder`, `open_wiki`, `open_youtube`, `install_plugin`.
- Setup: `every action has a row`, `every row is an action`, `every action has a call in the fixture`, `Start Recording's folder is the output folder`.

**Not checked:**
- 22 actions never show their effect: the 12 window rows only show that a dialog with that title opened. `help_migrate_filament_presets` presses OK without reading what was migrated. `help_check_updates` answers whatever dialog is topmost, because an empty title matches every dialog. The 3 picker rows never import anything. The 4 hand-off rows never observe the browser or file manager. The 3 refused rows read the code only, and `send_gcode`'s row, being refused before any dialog, never reaches the storage dialog it opens on the Bambu network, which no sweep test opens (the A2L check covers it by hand).
- Written files are never opened, except the two sliced archives (G-code entry names only). A truncated or wrong STL/3MF/DRC/G-code/OBJ, or a blank screenshot, passes.
- Loose effect checks where exact values are known: `calib_*` (only name and object count, not the generated test's objects), `add_primitive_*` (substring of the name), `obj_fit_volume` (> 200 mm), `obj_arrange` (on a plate; no positions, no overlap check), `view_fit_bed` (y of `look_at` unchecked), `reset_window_layout` (sidebar only), `toggle_run_script_panel` (any height change).
- `its action names the fixture's command` compares two static tables. It says nothing about what the action does.

**To settle in review:**
- That a calibration action's journal step has the id alone, not `{"test": "<name>"}`. The action runs its command's person form (`NativeCommands::with_person_form()`), which opens the test's dialog and journals nothing; only `calibration_generate_test`'s own run, with parameters or `silent`, calls `set_journal_args`, and `ActionContext` puts the action's fixed arguments in the step only in place of such a rewrite. Both are this branch's code (`McpToolsProject.cpp`, `McpPersonContext.hpp`, not on `main`). The step with the id alone replays the same, since the action supplies `test`.

**Strength:** Partial — about 70 actions have their effect read back exactly and about 23 loosely; 22 more only open a dialog, hand off, get refused or open a cancelled picker; and 12 export rows mostly check that a file exists. The journal step is pinned exactly for every action.

## `test_menu_items_refusals`
`test_menu_items_refusals.py` · Python, 126 checks

**Tests:** What the object and plate commands built from menu items refuse, called directly and picked from the Speed Dial (Space, search, click). Covers Set as Individual Objects, Select All with a gizmo open, target errors, Rename, Delete on the last solid part and on cut objects, Split, Change Type, Assemble on object-list rows, Mirror, Drop, Add Primitive, Auto-Orient on locked or unprintable objects, and the plate commands on a missing plate. For each case it also checks the state left behind.

**Decides by:** Exits 1 when any check fails; all checks print at the end, so an uncaught `McpError` from a setup call exits non-zero with nothing printed.
- Direct refusals: the error text holds a phrase and, where given, the details' `code` is exact (`gizmo_open`, `nothing_selected`, `one_object_only`, `wrong_selection`, `last_solid_part`, `cut_object`, `one_shell`, `auto_drop_question`, `no_such_plate` ×7, `no_objects_to_orient`, `not_printable` ×2, `not_renamed`).
- State after each refusal: `object_list` `total`, names, part ids and types, selection, `mirrored` flags, plate list (`name`, `locked`, `current`) and `rotation_deg`, all compared exactly with the state before.
- Picks: the expected notice text appears in `app_state.notifications`, or an auto-answer rule's `uses` rises by exactly 1 (Rename, last-solid-part message, cut question, Auto-Drop, Edit Plate Name). The state is then read back, for example Assemble on instance rows giving `[("other",1),("Assembly",2)]`.
- Notices two picks share: "Select two or more objects first." (Assemble with one object selected, then Assemble on every part row of an object) and "This acts on one object or part at a time: …" (Split to Objects on two instance rows, then Split to Parts on two part rows). Both refusals' notices are regular ones (`menu_info()` answers them as information), which fade about 10 s after they show, so the first can still be open at the second pick, and while it is open the same text pushed again only moves it to the end of the list (`NotificationManager::activate_existing`). So before the second pick of each pair, the test closes the earlier notice; one that has already faded leaves nothing to close. `widget_list` lists each notification as an `imgui_window` node for its ImGui window, `!!Ntfctn<id>`, with its text as a `text` node in it; the test finds the window whose text holds the notice's text and clicks its large invisible close button, which closes the notification in either theme. That button, labelled " ", and the close glyph beside it have labels the listing cleans to nothing and no `##` name, so `widget_list` lists neither and gives them no ref. The test reaches the invisible one with the locator `{imgui: "", window: "!!Ntfctn<id>", index: N - 1}`, which matches the window's unlabelled items in the order they are recorded; `render_close_button()` submits it last. N is the count the same locator without `index` is refused with ("N ImGui items match …; add \"index\""); it is at least 2, the glyph and the invisible button. A short refusal notice draws no minimize button (`m_multiline` with more than 3 lines only). "the earlier notice is closed" then waits up to 5 s, failing when the time runs out, for `app_state.notifications` to hold no notice with the text. After the second pick, its own check reads the text there again.
- Results that run: `object_delete {}` on two solid parts (the remaining part ids, and the reply's `parts` and `kept`), `object_split keep_z` (2 objects, upper piece above 20 mm), the Auto-Drop "No" pick (pieces dropped below 1 mm), Duplicate Plate (`ops_undo.taken()` count 1, and undo restores 1 plate and 1 object), rename to the same name (no snapshot).

**Not checked:**
- `a pick of Change Type with an object selected says to select a part`: notice text only; the object's part types are not read afterwards.
- `object_auto_orient names only what it turned, not an object on a locked plate`: reads the reply alone. Neither cube's rotation is read, so a turn of the locked-plate cube goes unseen.
- Refusals checked by text only, with no code: `Set as Individual Objects on an object of one instance ...`, `object_delete of an object the project lacks names its index`, `object_mirror with no axis is refused by its schema` (any error containing "axis").
- `and takes no undo snapshot` reads the info-level log through `ops_undo.py`. It would pass if snapshots stopped being logged; the Duplicate Plate count-1 check in the same run would catch that.

**To settle in review:**
- The notices the pairs share are regular notices, not error notices that never fade: `menu_info()` (`McpToolsScene.cpp`, from commits not on `main`) answers `too_few_objects` and `one_object_only` as information, as the menu items told them, since the person can select something else and pick again. So the first notice may fade before the second pick, and the close step then finds nothing; "the earlier notice is closed" still reads the text gone before the second pick, and that pick's own check reads it shown again.
- Reaching the close button through a refusal's count is roundabout. Naming the button in the app (`record_named_item` on the close button in `render_close_button()`) would let the test click it by name; that is a change to the app.

**Strength:** Strong — refusal codes and the state after each refusal are compared exactly, and each pick's notice is its own; only a few picks rest on notice text alone.

## `test_additions_refusals`
`test_additions_refusals.py` · Python, 64 checks

**Tests:** Refusals of the filament, preset, export, import and settings commands. Covers `preset_sync` and `filament_sync_ams` with no account or printer, `view_set` of a layer with no slice, per-object STL and toolpath exports of an empty project (called and picked), a ZIP entry the archive lacks, `layer_height` on a part or a height range, a missing height range, the sidebar while a bare G-code file is open, deleting the only or a mixed filament slot, the 64-slot limit, and an `object_order` that leaves an object out. It also checks that a height range's own settings show on its object-list row.

**Decides by:** Exits 1 when any check fails.
- Refusals: error text holds a phrase and the details' `code` is exact (`not_signed_in`, `no_printer` ×3, `no_layers`, `no_objects` ×2, `no_toolpaths`, `no_such_entry`, `last_filament_slot`, `mixed_uses_slot`, `mixed_filament_slot`, `too_many_filaments`, `order_misses_object`). Some are checked by text only (see below).
- Details: `entries` == `["cube.stl", "parts/block.stl"]`, and `height_ranges` == the two ranges.
- State left: `gcode_window` and `legend_collapsed` unchanged, export folder empty, `object_list.total` 0, part and range overrides unchanged, `sidebar_collapsed` unchanged, slot count 1/3/64.
- Picks: exact notice text in `notifications` ("The project has no objects.", "No more filament slots can be added.", or containing "The Preview shows no toolpaths"), and `dialogs` == `[]`.
- Height-range row: `widget_list` row shows "has settings" after `wall_loops` is set and not after it is nulled; the selected rows are unchanged.
- Object order: two 20 mm cubes added with `object_add_primitive {name: "first"}` and `{name: "second"}`. "two cubes, first and second" reads `object_list` plate 0's names as `["first", "second"]`. `object_order {plate: 0, order: [1]}` is refused as `order_misses_object`, naming object 0, and "and leaves the order as it was" reads the names as `["first", "second"]`. The control, "a full order is taken": `object_order {plate: 0, order: [1, 0]}` gives the names `["second", "first"]`.

**Not checked:**
- `and leaves the part's settings too` and `and the object's ranges stay` follow refused `settings_get` calls, which only read; they would catch a refused read that changed something.
- Refusals checked by text, with no code: `settings_set`/`settings_get` of `layer_height` on a part, a null `layer_height` on a range, a missing height range, filament settings with `target object`, and the sidebar `view_set`.
- `project_export ... as its action` (no path) checks the refusal only; whether a picker or file appeared is checked only for the folder variant and the picks.

**Strength:** Strong — refusal codes, details and the unchanged state are compared exactly, the order by names that tell the objects apart.

## `test_person_forms`
`test_person_forms.py` · Python, 61 checks

**Tests:** That a call with exactly an action's arguments behaves as a person's pick. `project_save {save_as}` and `project_export {format: gcode}` open their pickers, `project_save` with a path does not, and `preset_export` always opens its dialog. Speed Dial picks of View: Top, Deselect All and Go to layer run in place. `object_arrange`/`object_select {all}` and their `action_run` give the command's reply, journal step and replay, including in the assemble view and with Measure open.

**Decides by:** Exits 1 when any check fails.
- Pickers: the save/export auto-answer rule's `uses` rises by exactly 1 per call and not for the path save. The file exists for the path save, and no file has an mtime after the start in the project folder or `last_export_path`.
- `preset_export`: `status` `blocked_by_dialog` with a dialog titled "Export Preset Bundle", an error after cancel, the folder empty, and `silent` refused.
- `view_set` bad tab: the error starts with `No tab has the id "no_such_page".` and the payload contains `no_such_tab`.
- Picks: camera elevation ≥ 85, `tab` stays `preview`, the Prepare selection empty, Go to layer's top layer within 1 of the middle, and for each pick no new job under the command's tool name and no notice containing "Started" or "Finished".
- The control for those absences, last, after every pick that runs in place: a pick of Slice and Preview, opened and searched the same way on the same project, starts its slice as a job. "a pick that runs as a job is listed as one" reads `app_state.jobs` with a job of `tool` `plate_slice` numbered above the newest before the pick; "and shows its Started or Finished notice" reads `app_state.notifications` with a notice containing "Started" or "Finished" (`Started: <title>`, then `Finished: <title>`, from `PersonContext::submit()`).
- Arrange/select: `off_plate` == `[]`, no `ran` for direct calls, `ran` == the action id for `action_run`, no `level`, `outside` empty, `objects` == `[0, 1, 2]`. The journal step's `tool` and `args` are exact for `object_select` and `action_run` (and contain `spacing` for `object_arrange`). The exported replays give `passed` true with the state reproduced. The assemble-view `canvas` values and the `gizmo_open` refusal are exact.

**Not checked:**
- `a cancelled Save Project As is an error` and `a cancelled Export Preset Bundle is an error` accept any error; the message is not read.
- The cancelled `project_export {format: gcode}` reply is not read; the test only requires that it raises nothing.
- `preset_export refuses silent` passes on any error containing "silent", which a schema rejection of the argument would also give.
- `a pick of View: Top turns the camera` checks elevation only.
- `object_arrange {} replies with the settings in force ...` checks that a `settings` key exists, not its values.
- The control's notices fade after 10 s (a regular notice's time); it is read straight after the slice settles.

**Strength:** Strong — the picker counts, journal steps, replays and selections are compared exactly, and the "runs in place" absences have a control in the same run; a few error paths accept any error.

## `test_preferences`
`test_preferences.py` · Python, 65 checks

**Tests:** `preferences_get`/`preferences_set` against the Preferences dialog. The keys on each tab match the dialog's named controls; the MCP switch and port; a three-key write read back from the call, from `preferences_get` and from the reopened dialog; `restart_needed`; whole-call refusals. Also the backup timer following Auto backup, the downloads folder (call, dialog row, Browse while the dialog is open), Hide login side panel's live effect on the Home page, and preset sync / stealth mode with no account.

**Decides by:** Exits 1 when any check fails.
- Dialog parity, per tab: the named controls' set == the keys `preferences_get` lists, and each checkbox's and choice's shown value == `value`.
- Writes: `changed` holds exact before/after values, `preferences_get` and the dialog show the written values, `unchanged` == `["auto_arrange"]`, and `restart_needed` is true for `enable_multi_machine` only.
- Refusals: the text contains `preference_refused`, `no_such_choice`, `out_of_range` (with "240"), `not_a_preference`, `not_a_folder` or "modal dialog is open", and the good key stays unchanged.
- Backup timer: the periods logged by `set_interval` are exactly `[0]`, then `[on]`, then all 0.
- Downloads folder: stored value == folder, the dialog row label shows it, and the Browse pick shows and stores the browsed folder.
- Login panel: the computed `display` of `#LeftBoard` on the Home page, read with `webview_run_javascript`. "the Home page shows its login side panel" waits up to 20 s for `block` (the page shows the panel with jQuery's `show()`, `home.js`), then `none` within 10 s of writing true, then `block` again within 10 s of writing false. A page with no `#LeftBoard` reads `missing`, which fails each of the three.
- Sync/stealth: `changed` names the key, `preferences_get` and the dialog checkbox agree, and the stealth effect's log line appears.

**Not checked:**
- Tab parity compares values for checkboxes and choices only; number and text fields are compared only for `opengl_fps_cap` and `download_path`.
- `and writing <key> <value> starts no preset sync` looks for the absence of "start_sync_service" in the log, so it cannot fail if that log text changes. No control can show the line: a sync starts only for a signed-in account, which the sweep never has. The stealth-mode effect read from the same log lines shows that the log is read.
- After `a number outside its range is refused with the range`, nothing checks that the value stayed unchanged.

**Strength:** Strong — values are checked across three sources (reply, `preferences_get`, dialog), and the login panel by its exact display; only a few negative log reads are weak.

## `test_install_printers`
`test_install_printers.py` · Python, 31 checks

**Tests:** `printer_list_models`, `printer_install` and `printer_uninstall` on a fresh folder seeded with the X1 Carbon 0.4, across a restart. Covers the wizard cache being built, install and refusal (no such model or nozzle), persistence, uninstall of the selected printer and of the last one, `preset_changes: transfer` with unsaved process and printer edits, the Printer Selection dialog's Confirm with the Velo+ ticked in its web page, and the filaments the reload of an install installs or swaps.

**Decides by:** Exits 1 when any check fails; a launch failure is a failed `the app starts` check.
- Models: `installed_nozzles` exact (`["0.4"]` for the X1C and the Velo+ after install, `[]` before), `nozzles` include the expected sizes, and the cache file appears.
- Install/uninstall: `added`/`removed` == the preset name, and `selected` matches `preset_list`'s selected. The printer set is unchanged after refusals (`no_such_model` with `similar`, `no_such_nozzle` with `nozzles`, `last_printer` with its text, `unsaved_preset_changes`).
- The Velo+'s filaments: the reload installs a printer's default filaments where no installed one fits it, and no filament the X1 Carbon's folder holds fits the Velo+. "the reload installs the Velo+'s default filament": `filaments_installed` is not empty, equals the installed filaments `preset_list` lists after the install minus those before (`installed_filaments()`, every filament whatever it fits), and holds `Goofoo ABS @System`, the Velo+ model's `default_materials` (`Goofoo/machine/Goofoo Velo+.json`), which `PresetBundle::load_installed_filaments()` installs; `filaments_uninstalled` is absent. "and offers them for it": each name is in `preset_list {type: filament, printer: <Velo+ preset>}`.
- Transfer: `added` is the X1C's other 3 nozzles, `layer_height` stays 0.12, the printer edit (240) moves to the newly selected preset while that preset's saved value is 250, and `modified` lists both keys.
- Dialog: the tick's `ModelCheckBoxSelected` class must be set before Accept. After Confirm, every Velo+ nozzle is installed, the new presets equal the printer selector's new names, the X1C item is unchanged, and the selected printer is a Velo+ preset.
- Restart: the Velo+ preset is still listed.
- A third vendor, with `Generic ABS @System` installed first: the Voxelab Aquila X2's install. "printer_install's filaments_installed is what preset_list shows installed": `filaments_installed` (absent read as empty) equals the installed filaments after minus those before. "and swaps no filament": `filaments_uninstalled` is absent, nothing installed before is gone from `preset_list`, and `Generic ABS @System` is still installed. A reload swaps a @System filament only for a vendor whose bundle lists default filaments of its own (the vendor's `default_filaments`, which `PresetBundle::apply_vendor_config()` reads). Only an INI bundle fills that list (`VendorProfile` reads it from the INI's `default_filaments` section); the shipped JSON profiles never do, as `printer_install`'s description and `apply_install_set()` say, so none is swapped. A machine profile's `default_filament_profile` is another key, which the swap does not read.

**Not checked:**
- No shipped profile can make the reload swap a @System filament, so the swap that `filaments_uninstalled` reports is never shown; the Voxelab install pins that nothing is swapped. Showing a swap would need an INI vendor bundle as a fixture.
- `printer_list_models lists more than one vendor` only requires > 1 vendor and > 100 models.
- A fixed 2 s sleep after each stop stands in for waiting until the app exits before the relaunch on the same folder.

**To settle in review:**
- That the Velo+'s install installs `Goofoo ABS @System`. The evidence: the Velo+ model's `default_materials` is `Goofoo ABS @System`, which `load_installed_filaments()` installs for a printer that no installed filament fits, and the folder holds only the X1 Carbon's filaments, which the BBL profiles limit to Bambu Lab printers. If a filament the seed installs does fit the Velo+, nothing is installed and the check fails, which would show the test's premise wrong.

**Strength:** Strong — installs, removals, refusals, transfer, the dialog's Confirm and the filaments the reloads install are read back exactly.

## `test_install_filaments`
`test_install_filaments.py` · Python, 23 checks

**Tests:** `filament_install` and `filament_uninstall` on a fresh folder (X1 Carbon, Bambu PLA Basic in slot 1), across a restart. Covers install by the selector's short name, refusal of unknown names, the slot selector offering the new preset, reinstall reporting `installed_already`, re-installing a second printer's default filaments when an uninstall leaves it none, a merely shown printer (A1 mini) getting none of its defaults, and refusal to uninstall the slot's filament.

**Decides by:** Exits 1 when any check fails.
- Install: `installed` contains the candidate preset, `preset_list` offers it, every installed name has that `alias`, `preset_select` puts it in slot 1 with no `shown`, a second install gives `installed` == `[]`, and `no_such_filament` comes with `similar`.
- Shown printer: the printer is not installed and nothing fits it; selecting it adds shown filaments; `filament_install` gives `installed` == `[X1C_ONLY]` with no `filaments_installed`; the installed set == before plus that one; and after the restart none of the shown filaments are installed.
- Defaults given back: `filaments_installed` in the uninstall reply is non-empty and == the Velo+'s fitting filaments minus slot ones. "what the reply calls uninstalled is exactly what stays uninstalled": the reply's `uninstalled` (always present) equals the names asked minus those `filaments_installed` gives back, and none of them is installed afterwards (`filament_uninstall` lists under `uninstalled` only the presets left uninstalled). After the restart the Velo+'s fitting set is unchanged.
- Uninstall: the preset leaves `preset_list` and reads `installed: false`; `filament_in_use` comes with the text "Filament slot 1 uses Bambu PLA Basic @BBL X1C.", and the filament stays installed.

**Not checked:**
- The filament exercised is the first not-installed Bambu Lab preset with an alias that `preset_list` returns, so which filament is tested depends on the shipped profiles.
- `after a restart the filaments offered for the second printer are what the uninstall replied` compares against `preset_list` before the restart, not against the reply.
- A fixed 2 s sleep after each stop.

**Strength:** Strong — installs, re-installs, uninstalls and refusals are read back exactly from the replies and from `preset_list` before and after a restart.

## `test_plugin_commands`
`test_plugin_commands.py` · Python, 37 checks (38 when the first uninstall fails)

**Tests:** The plugin commands on the fixture plugins in a fresh folder: `plugin_list`, `plugin_install_local` (again, and with `overwrite`), `plugin_enable` (plugin and capability, by key and by name), `plugin_reload`, `plugin_uninstall`, and `plugin_refresh` finding side-loaded plugins. Also a person's pick of Refresh Plugins (returns at once, notifies, rescans once), and the Plugins window's rows matching `plugin_list`.

**Decides by:** Exits 1 when any check fails.
- Install: `level` == "success", the row is `installed` with `source` "local", a reinstall gives `plugin_installed` with "is already installed", overwrite asks nothing, and no dialog is open.
- Fields: "plugin_list lists the plugins with their fields" reads the installed fixture's row, which the install check has just found: it has the fields `key`, `name`, `version`, `installed`, `enabled`, `status`, `source`, `capabilities` and `error`. Before the install, every row there is (none, in a fresh folder) is held to the same fields.
- Enable: `enabled` and `status` "Activated", capability lists, `changed` true. Refusal codes are exact: `plugin_not_enabled`, `empty_capability`, `no_such_capability` (with `capabilities`), `no_such_plugin` (with `similar`).
- Reload: `reloaded` == key, `reinstalled` false, stays enabled.
- Uninstall: `uninstalled` == key, `unsubscribed` false, the row and the file are gone, and a second uninstall gives `no_such_plugin`.
- Refresh: the Speed Dial pick replies within 10 s, without a job, then "Plugins refreshed." appears within 60 s, and the log has exactly 1 "Rescanning plugins..." line since the pick. `plugin_refresh` replies without a job and lists the side-loaded plugin, which was absent before.
- Window: `{key: enabled}` read from the page == `plugin_list`'s.

**Not checked:**
- `plugin_uninstall` is retried once, after a 2 s sleep, when the first try fails with "Failed to delete plugin folder", on any platform. A regression that makes that first removal always fail passes if the retry succeeds.
- `call()` presses Yes on any dialog a call blocks on, so an unexpected question during install, enable or reload is accepted silently.
- `and with clear_cache` checks only that the plugin stays enabled; the cache is not observed.
- The window comparison reads only each row's key and switch, not names, status or capabilities.

**Strength:** Strong — each command's reply and the resulting `plugin_list`, files and window rows are compared exactly.

## `test_plugin_tools`
`test_plugin_tools.py` · Python, 36 checks

**Tests:** A plugin's side of the MCP tools, with `mcp_tools_fixture` side-loaded (enabled, 5 capabilities, thread permission) and `mcp_tools_broken`, whose `get_default_config` raises after it registered `measure`. Covers the fixture's `measure` operation through `tool_search`/`tool_call`, its scripts calling tools from the Script worker and a thread, setting an auto-answer rule owned by the plugin, being refused `replay_export`, keeping a progress-dialog handle, and receiving NewProject events. Also covers several `tool_search` queries, unloading via the Plugins dialog (operation and rule withdrawn), and a `--run` launch without MCP where the rule script still runs.

**Decides by:** Exits 1 when any check fails, or 1 at once if a launch fails.
- Operation: `group` "Plugins", `description` starting "Offered by a plugin.", `tool_call` result == `{"twice": 4}`, absent from `tools/list`, and withdrawn within 15 s of the untick.
- Fixture scripts (outcome JSON read from the plugin's storage folder): `call` and `thread` == `{"ok": true, "result": true}`, `rule` == `{"ok": true, "result": "plugin"}` (both with MCP and under `--run`), `export` gives `ok` false with "agent's session", and a separate script's `handle.update(5)` returns true.
- Progress dialog: "the plugin opens a progress dialog and keeps its handle": the `handle` script's outcome has `ok` true, and `window_list` lists a window titled `MCP tools fixture`, the title the script gives `create_progress_dialog`.
- `tool_search`: `listed`/`input_schema`/`summary` per kind of item, the `brim_width` setting, the first Python item `orca.host.TriangleMesh.face_normals`, group "objects" containing `object_arrange`, an unknown group refused naming "Objects", "?" gives `total` 0, and no "cad" section.
- Design commands: "the Design tab is on (launch --cad)" reads `app_state.design_tab` true, and then `tool_search`'s search for "extrude" finds `extrude` in the group `Design`, unlisted, with its schema.
- A menu item's command: with Prepare shown, "tool_call runs a menu item's command" calls `script_toggle_panel` and reads the 3D view's height in `widget_list` (its `canvas "OrcaSceneCanvas"` line) less than before the call, as the Run Script panel docks under it; "and a second call hides it" reads it greater than after the first call.
- Events and journal: `new_projects.txt` gains exactly 1 line per `project_new` and per `new_project` action. Before the `rule` script runs, the agent calls `dialog_auto_answer {rules: []}` itself (it clears only the rules MCP clients set). "a plugin's calls are not in the agent's journal": `replay_export`'s steps hold exactly one `dialog_auto_answer` step, the agent's, with `rules` `[]`; the plugin's call of the same tool is not there.
- The plugin whose load failed: "the plugin whose load failed is listed with its error" reads `plugin_list`'s row for `mcp_tools_broken` with an `error` holding "this fixture's load fails here on purpose" (a row's `error` is the plugin's own, `PluginOps`). "and offers none of its operations": `tool_search {tool: "plugin.mcp_tools_broken.measure"}` is refused with text starting "Unknown tool: plugin.mcp_tools_broken.measure".
- Rule lifetime: the rule's match is in `auto_answer_rules` while the plugin is loaded and gone within 15 s of the untick.

**Not checked:**
- `tool_search describes the plugin's operation` checks only that the text holds `"a"`, the quoted name of the operation's argument. `tool_search's search finds the Speed Dial's other actions` accepts any action title containing "fixture".
- The progress dialog's gauge: `handle.update(5)` returns true, but the gauge is not read.
- The `--run` launch's exit code is ignored, and only the rule script runs there. A fixed 3 s sleep after the stop stands in for waiting on the first app to exit. The fixed port 13692 can collide with another instance.

**Strength:** Partial — the fixture's operation, script outcomes, events, journal, failed load and unload are each checked exactly, with a control for the journal's absence, but two `tool_search` matchers are loose and the `--run` launch's exit code is not read.

## `test_operation_routes`
`test_operation_routes.py` · Python, 22 checks

**Tests:** That the routes into one operation agree: the Go to layer action and `view_set` asked for a layer of a Preview with none; `preset_select` and the plugin API given a mixed-colour filament slot; and the plugin API showing and selecting a hidden printer. It also checks single-route states: `project_save` to a `.gcode.3mf` name, exports of an empty project, and `object_arrange` from the assemble view and with the Move gizmo open.

**Decides by:** Exits 1 if any named check fails or a call outside `answer()` raises. The three conditional checks run only after a guard check that itself fails when they cannot run, so there is no silent skip.
- Empty project: "project_save refuses a .gcode.3mf name" (code `sliced_file_name`), the text contains `sliced_3mf`, and no file. The `.3mf` export is written; `stl` and `sliced_3mf` are refused with `no_objects` and write no file.
- Layers: the action and `view_set {layer: 1}` both refuse with `no_layers`, and "in the same words (layer)" compares the texts exactly.
- Arrange from the assemble view: no refusal and `off_plate` eq `[]`, the offsets changed, and the selection's `canvas` is `3d` afterwards.
- Arrange with the Move gizmo: both cubes are first moved to one spot with `object_transform {object: i, position: [128, 128, 10]}`, and "the cubes are back on one spot" reads both instance offsets from `object_list` as x 128 and y 128. Then the gizmo opens (`active`), `object_arrange` is not refused, and "object_arrange arranges with the Move gizmo open" reads `object_list` again: both cubes are on plate 0 and their 20 mm squares are apart, their offsets differing by at least 20 mm in x or in y. A gizmo is still open afterwards.
- Mixed slot: `filament_is_mixed` holds a 1; `preset_select` refuses with `mixed_filament_slot`; the plugin API raises `ValueError` with a message equal to `preset_select`'s text.
- Hidden printer: a printer listed with `installed: false` exists; after the plugin API's `begin_update(name)`, nothing raised and `app_state.presets.printer` equals it.

**Not checked:**
- "and the arrangement moved a cube" (from the assemble view) passes on any change to the offsets, not that the cubes no longer overlap.
- "and the gizmo stays open" accepts any gizmo, not Move.
- The hidden printer runs the plugin API only; `preset_select` of a hidden printer is not compared. No pair is checked to "accept the same ones" (the README's claim): there is no sliced-Preview case for the action against `view_set`.
- What a refusal leaves, beyond "no file": the tab after a refused layer, the slot's preset after the mixed-slot refusal.
- It is the only test that compares the routes into an operation. The G-code export, a model or preset import, a new or saved project, a plate selected or locked, an instance count, a slice and `printer_send`'s refusals are compared across routes nowhere.

**Strength:** Partial — codes and words are compared exactly where it checks, and both arranges must move what overlaps, but the assemble-view arrange is checked only as "moved", and it compares the routes in only a handful of states.
