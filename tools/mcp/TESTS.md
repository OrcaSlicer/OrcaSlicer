# MCP sweep tests

The design of the tests `run_sweep.py` runs. This file holds the rules every test follows, the grades, and the index
of all tests. Each test's entry is in its area's file under `test-entries/`: what the test checks, how it decides
pass or fail, what it leaves unchecked, and how far its verdict can be trusted. Change an entry first, before its
test is written or changed.

## Adding or changing a test

A test is a Python script, `test_<area>.py`, designed here first:

1. Write or change the test's entry in its area's file: what it tests, its scenarios with the state expected after
   each step, and what it leaves out on purpose and why. A test that fits no area starts a file of its own and a row
   in the table of areas.
2. Have the entry reviewed and agreed before any test code is written.
3. Write the test to match the entry and add it to `sweep.json`. When the test changes later, change its entry in
   the same commit.

Replays are not tests. `smoke_test.json` is kept as the example of the replay format, and `test_action_replay_hidden`
runs it as its input. The `tests/*.json` replays still in the sweep each leave it, with their entries, in the commit
that adds the Python test covering what they cover. No replay is added to the sweep.

### The read-back rule

A test decides by comparing the app's state with values its entry writes down before the test runs:

- **Check the start.** Read and check the state a scenario starts from, so that each step is measured from a known
  state: after `project_new`, `plate_list` lists one plate, current, with the name `""`.
- **Read each step back through another tool.** After a call, read the state it should leave with a tool other
  than the one that acted: `plate_list` after `plate_rename`. The call's own reply may be checked as well, never
  instead.
- **Check every value the scenario fixes.** After renaming one plate, check the number of plates and every plate's
  name, not only the renamed one, so that a change to the wrong plate fails. Leave out the fields the scenario does
  not fix, so that a field added to a reply breaks nothing.
- **Write expected values out.** An expected value comes from the entry, never from an earlier read in the same
  run: a "same as before" check passes when both reads are wrong.
- **A refusal is a result.** Assert that the call was refused with its code, or with its words where it has no
  code, then read the state and check it is the state before the call, written out again.
- **A wait that runs out of time fails.**
- **Exact unless the app computes it.** A position a call gives is checked exactly. A value the app works out,
  such as an arrangement or a layer count, gets a stated tolerance or a property to hold: no two objects overlap,
  every object is on its plate.
- **Check what a person keeps once through the file.** The reading tools share the app's model with the tools that
  change it, so they cannot see a value that is stored but lost on saving. For a property that belongs in the
  project or the print, add one check through the file: save and reopen the project, or read the G-code.

An entry is ready for review when every step has its expected state, and every value a step leaves that nothing
checks appears under **Not checked** with the reason. An entry is written before its test exists; the table of all
tests lists the ones the sweep runs.

### The shared check module

`checks.py` holds what the tests use to apply the rule, so that it is applied in one place:

- `Checks` collects named checks, prints `ok <name>` or `FAIL <name>: <detail>` for each, and gives the test's exit
  code. A failed check lets the test go on. `Checks.scenario(name, fn)` runs one scenario: an exception fails that
  scenario under its name, and the next scenario still runs.
- `refused(call, code=None, words=None)` passes only when the call is refused with exactly that code, or, for a
  refusal without one, with text that holds those words; it returns the refusal's text.
- `run(client, tool, **args)` makes a call and follows its job to the end, and fails at once, naming the dialog,
  when a dialog the test does not answer holds the call.
- `wait(client, condition, timeout_s)` fails when the wait runs out of time.
- Readers give the state a scenario checks in a fixed shape, such as `plates()`: a list of `{name, current, locked,
  objects}`, one per plate.

## How the sweep decides

**The runner.** `run_sweep.py` runs each entry of `sweep.json` in a folder of its own on a freshly launched
app, and a test passes when its process exits 0. A failed setup or launch, a run past its time limit (900 s
unless the entry sets `timeout_s`), or an error running the test fails it, and so does a test left without a
result. On macOS a new OrcaSlicer crash report fails the run. When a test ends, the runner's `stop` kills the app
it launched (`TerminateProcess` on Windows, `SIGTERM` elsewhere) and ignores how it exited, so a crash while the
app closes fails nothing on Windows or Linux; only the tests that close the app themselves see it. Tests that start
their own app (`"launch": "own"`) start it with their own environment, so on Linux they follow neither the X11
default nor `--headless`. No CI job runs the sweep: it runs when someone runs it.

**Replays.** A `.json` test runs with `orca_mcp.py run-replay`, which exits 0 when the replay passed. A step is ok
when its call returns no error, when a `script_run` step's script finished (so a fixture fails its step by
raising), and when every `expect` matcher holds. The first step that is not ok ends the replay as failed. What a
replay can catch follows from that:

- A step without `expect` checks only that the call returned no error.
- A step marked `optional` checks nothing: it passes whether the call succeeds or is refused, and whether its
  `expect` holds or not. A replay has no way to assert that a call is refused, or what a refusal says.
- Every matcher except `exists` fails when its pointer is missing, so a mistyped pointer fails the step. The
  exception is `{"exists": false}`, which a mistyped pointer passes.
- `gt`, `ge`, `lt` and `le` compare numbers only; `contains` is a substring of a string, a member of an array or
  a key of an object.
- Every run writes `result.json`, but nothing compares it with an earlier run's.

**Python tests.** A `.py` test is a script that makes its calls, prints `ok` or `FAIL` for each named check and
exits 1 when a check failed or an exception escaped. What it checks is whatever the script computes; the entries
name each check by the text it prints. A test written under the read-back rule makes its checks through
`checks.py`; the older tests each have a `check()` of their own.

## Strength

Each entry ends with one of three grades:

| Grade | Meaning |
|---|---|
| Strong | A regression in what the test exercises would fail it; values are checked exactly. |
| Partial | The main outcome is checked, but some of what it exercises is only run, or checked loosely. |
| Smoke | It passes as long as its calls succeed; it checks little of what they return or leave behind. |

## Areas

Each area's tests are designed in a file of their own under `test-entries/`. A test not yet in `sweep.json` has its
entry there before it is written, and the replays an area's tests take the place of keep their entries in that file
until they leave the sweep.

| Area | Python tests | Replays still in the sweep |
|---|---|---|
| [Plates](test-entries/plates.md) | [`test_plates`](test-entries/plates.md#test_plates) | — |
| [Objects](test-entries/objects.md) | [`test_objects`](test-entries/objects.md#test_objects), [`test_object_structure`](test-entries/objects.md#test_object_structure) | [`menu_items_objects`](test-entries/objects.md#menu_items_objects), [`menu_items_placement`](test-entries/objects.md#menu_items_placement), [`menu_items_structure`](test-entries/objects.md#menu_items_structure), [`objects_state`](test-entries/objects.md#objects_state), [`objects_order`](test-entries/objects.md#objects_order), [`objects_files`](test-entries/objects.md#objects_files), [`scene`](test-entries/objects.md#scene) |
| [Presets and calibration](test-entries/presets.md) | [`test_presets`](test-entries/presets.md#test_presets), [`test_calibration`](test-entries/presets.md#test_calibration) | [`presets`](test-entries/presets.md#presets), [`presets_and_plate_map`](test-entries/presets.md#presets_and_plate_map), [`ops_select_preset`](test-entries/presets.md#ops_select_preset), [`calibration`](test-entries/presets.md#calibration) |
| [Filaments and settings](test-entries/filaments_settings.md) | [`test_filaments`](test-entries/filaments_settings.md#test_filaments), [`test_settings`](test-entries/filaments_settings.md#test_settings) | [`ops_assign_filament_preset_one_slot`](test-entries/filaments_settings.md#ops_assign_filament_preset_one_slot), [`ops_assign_filament_preset_multi_slot`](test-entries/filaments_settings.md#ops_assign_filament_preset_multi_slot), [`multicolor`](test-entries/filaments_settings.md#multicolor), [`settings_targets`](test-entries/filaments_settings.md#settings_targets) |
| [Project files, imports and exports](test-entries/files.md) | [`test_files`](test-entries/files.md#test_files) | [`filaments_and_files`](test-entries/files.md#filaments_and_files), [`new_project_keeps_layer_gcode`](test-entries/files.md#new_project_keeps_layer_gcode), [`ops_open_project`](test-entries/files.md#ops_open_project) |
| [Slicing, layers and the Preview](test-entries/slice_preview.md) | [`test_slice_preview`](test-entries/slice_preview.md#test_slice_preview) | [`slice_stats`](test-entries/slice_preview.md#slice_stats), [`preview`](test-entries/slice_preview.md#preview), [`layers`](test-entries/slice_preview.md#layers), [`view_switches`](test-entries/slice_preview.md#view_switches) |
| [Gizmos and canvas input](test-entries/gizmos.md) | [`test_gizmos`](test-entries/gizmos.md#test_gizmos) | [`gizmos`](test-entries/gizmos.md#gizmos), [`dialogs_canvas_input`](test-entries/gizmos.md#dialogs_canvas_input) |
| [GUI automation tools](test-entries/gui_tools.md) | [`test_gui_tools`](test-entries/gui_tools.md#test_gui_tools), [`test_recording`](test-entries/gui_tools.md#test_recording), [`test_call_forms`](test-entries/gui_tools.md#test_call_forms) | [`context_menus`](test-entries/gui_tools.md#context_menus), [`rows`](test-entries/gui_tools.md#rows), [`web_dialogs`](test-entries/gui_tools.md#web_dialogs), [`recording`](test-entries/gui_tools.md#recording), [`agent_forms`](test-entries/gui_tools.md#agent_forms), [`actions_ask`](test-entries/gui_tools.md#actions_ask) |
| [Commands, preferences, installs and plugins](test-entries/commands.md) | [`test_every_action`](test-entries/commands.md#test_every_action), [`test_menu_items_refusals`](test-entries/commands.md#test_menu_items_refusals), [`test_additions_refusals`](test-entries/commands.md#test_additions_refusals), [`test_person_forms`](test-entries/commands.md#test_person_forms), [`test_preferences`](test-entries/commands.md#test_preferences), [`test_install_printers`](test-entries/commands.md#test_install_printers), [`test_install_filaments`](test-entries/commands.md#test_install_filaments), [`test_plugin_commands`](test-entries/commands.md#test_plugin_commands), [`test_plugin_tools`](test-entries/commands.md#test_plugin_tools), [`test_operation_routes`](test-entries/commands.md#test_operation_routes) | — |
| [Windows, views, printers and web pages](test-entries/windows_web.md) | [`test_printer_extruders`](test-entries/windows_web.md#test_printer_extruders), [`test_project_open_settings`](test-entries/windows_web.md#test_project_open_settings), [`test_sidebar_floating`](test-entries/windows_web.md#test_sidebar_floating), [`test_set_window`](test-entries/windows_web.md#test_set_window), [`test_set_view_preview`](test-entries/windows_web.md#test_set_view_preview), [`test_set_view_camera`](test-entries/windows_web.md#test_set_view_camera), [`test_speed_dial_open`](test-entries/windows_web.md#test_speed_dial_open), [`test_send_gcode`](test-entries/windows_web.md#test_send_gcode), [`test_web_ui`](test-entries/windows_web.md#test_web_ui), [`test_webview_modal`](test-entries/windows_web.md#test_webview_modal) | — |
| [Runtime, command line and harness](test-entries/runtime.md) | [`test_command_line`](test-entries/runtime.md#test_command_line), [`test_runtime_lifecycle`](test-entries/runtime.md#test_runtime_lifecycle), [`test_stdio_bridge`](test-entries/runtime.md#test_stdio_bridge), [`test_orca_mcp_attach`](test-entries/runtime.md#test_orca_mcp_attach), [`test_run_sweep`](test-entries/runtime.md#test_run_sweep), [`test_recovery_under_load`](test-entries/runtime.md#test_recovery_under_load), [`test_worker_admission`](test-entries/runtime.md#test_worker_admission), [`test_cancel_pending_slice`](test-entries/runtime.md#test_cancel_pending_slice), [`test_export_running_job`](test-entries/runtime.md#test_export_running_job), [`test_action_replay_hidden`](test-entries/runtime.md#test_action_replay_hidden), [`test_list_windows`](test-entries/runtime.md#test_list_windows), [`test_ref_locator_export`](test-entries/runtime.md#test_ref_locator_export) | [`smoke_test`](test-entries/runtime.md#smoke_test) |
| [Design tab](test-entries/design.md) | [`test_cad_replay`](test-entries/design.md#test_cad_replay), [`test_cad_slow_build`](test-entries/design.md#test_cad_slow_build), [`test_cad_failed_job`](test-entries/design.md#test_cad_failed_job), [`test_cad_examples`](test-entries/design.md#test_cad_examples), [`test_cad_click_edit`](test-entries/design.md#test_cad_click_edit), [`test_cad_commands`](test-entries/design.md#test_cad_commands), [`test_cad_sketching`](test-entries/design.md#test_cad_sketching), [`test_cad_context_menu`](test-entries/design.md#test_cad_context_menu) | [`cad_sketch_to_solid`](test-entries/design.md#cad_sketch_to_solid), [`cad_sketch_edit`](test-entries/design.md#cad_sketch_edit), [`cad_topology_targets`](test-entries/design.md#cad_topology_targets), [`cad_features`](test-entries/design.md#cad_features), [`cad_doc`](test-entries/design.md#cad_doc), [`cad_canvas`](test-entries/design.md#cad_canvas) |

## All tests

The 78 tests `sweep.json` lists: 46 Strong, 27 Partial, 5 Smoke.

| Test | Area | Kind | Strength |
|---|---|---|---|
| [`test_plates`](test-entries/plates.md#test_plates) | [Plates](test-entries/plates.md) | Python | Strong |
| [`menu_items_objects`](test-entries/objects.md#menu_items_objects) | [Objects](test-entries/objects.md) | replay | Strong |
| [`menu_items_placement`](test-entries/objects.md#menu_items_placement) | [Objects](test-entries/objects.md) | replay | Strong |
| [`menu_items_structure`](test-entries/objects.md#menu_items_structure) | [Objects](test-entries/objects.md) | replay | Strong |
| [`objects_state`](test-entries/objects.md#objects_state) | [Objects](test-entries/objects.md) | replay | Strong |
| [`objects_order`](test-entries/objects.md#objects_order) | [Objects](test-entries/objects.md) | replay | Strong |
| [`objects_files`](test-entries/objects.md#objects_files) | [Objects](test-entries/objects.md) | replay | Partial |
| [`scene`](test-entries/objects.md#scene) | [Objects](test-entries/objects.md) | replay | Partial |
| [`presets`](test-entries/presets.md#presets) | [Presets and calibration](test-entries/presets.md) | replay | Strong |
| [`presets_and_plate_map`](test-entries/presets.md#presets_and_plate_map) | [Presets and calibration](test-entries/presets.md) | replay | Strong |
| [`ops_select_preset`](test-entries/presets.md#ops_select_preset) | [Presets and calibration](test-entries/presets.md) | replay | Smoke |
| [`calibration`](test-entries/presets.md#calibration) | [Presets and calibration](test-entries/presets.md) | replay | Strong |
| [`ops_assign_filament_preset_one_slot`](test-entries/filaments_settings.md#ops_assign_filament_preset_one_slot) | [Filaments and settings](test-entries/filaments_settings.md) | replay | Smoke |
| [`ops_assign_filament_preset_multi_slot`](test-entries/filaments_settings.md#ops_assign_filament_preset_multi_slot) | [Filaments and settings](test-entries/filaments_settings.md) | replay | Smoke |
| [`multicolor`](test-entries/filaments_settings.md#multicolor) | [Filaments and settings](test-entries/filaments_settings.md) | replay | Partial |
| [`settings_targets`](test-entries/filaments_settings.md#settings_targets) | [Filaments and settings](test-entries/filaments_settings.md) | replay | Strong |
| [`filaments_and_files`](test-entries/files.md#filaments_and_files) | [Project files, imports and exports](test-entries/files.md) | replay | Partial |
| [`new_project_keeps_layer_gcode`](test-entries/files.md#new_project_keeps_layer_gcode) | [Project files, imports and exports](test-entries/files.md) | replay | Partial |
| [`ops_open_project`](test-entries/files.md#ops_open_project) | [Project files, imports and exports](test-entries/files.md) | replay | Partial |
| [`slice_stats`](test-entries/slice_preview.md#slice_stats) | [Slicing, layers and the Preview](test-entries/slice_preview.md) | replay | Strong |
| [`preview`](test-entries/slice_preview.md#preview) | [Slicing, layers and the Preview](test-entries/slice_preview.md) | replay | Strong |
| [`layers`](test-entries/slice_preview.md#layers) | [Slicing, layers and the Preview](test-entries/slice_preview.md) | replay | Strong |
| [`view_switches`](test-entries/slice_preview.md#view_switches) | [Slicing, layers and the Preview](test-entries/slice_preview.md) | replay | Strong |
| [`gizmos`](test-entries/gizmos.md#gizmos) | [Gizmos and canvas input](test-entries/gizmos.md) | replay | Partial |
| [`dialogs_canvas_input`](test-entries/gizmos.md#dialogs_canvas_input) | [Gizmos and canvas input](test-entries/gizmos.md) | replay | Partial |
| [`context_menus`](test-entries/gui_tools.md#context_menus) | [GUI automation tools](test-entries/gui_tools.md) | replay | Strong |
| [`rows`](test-entries/gui_tools.md#rows) | [GUI automation tools](test-entries/gui_tools.md) | replay | Partial |
| [`web_dialogs`](test-entries/gui_tools.md#web_dialogs) | [GUI automation tools](test-entries/gui_tools.md) | replay | Partial |
| [`recording`](test-entries/gui_tools.md#recording) | [GUI automation tools](test-entries/gui_tools.md) | replay | Partial |
| [`agent_forms`](test-entries/gui_tools.md#agent_forms) | [GUI automation tools](test-entries/gui_tools.md) | replay | Partial |
| [`actions_ask`](test-entries/gui_tools.md#actions_ask) | [GUI automation tools](test-entries/gui_tools.md) | replay | Partial |
| [`test_every_action`](test-entries/commands.md#test_every_action) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Partial |
| [`test_menu_items_refusals`](test-entries/commands.md#test_menu_items_refusals) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_additions_refusals`](test-entries/commands.md#test_additions_refusals) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_person_forms`](test-entries/commands.md#test_person_forms) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_preferences`](test-entries/commands.md#test_preferences) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_install_printers`](test-entries/commands.md#test_install_printers) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_install_filaments`](test-entries/commands.md#test_install_filaments) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_plugin_commands`](test-entries/commands.md#test_plugin_commands) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_plugin_tools`](test-entries/commands.md#test_plugin_tools) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Partial |
| [`test_operation_routes`](test-entries/commands.md#test_operation_routes) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Partial |
| [`test_printer_extruders`](test-entries/windows_web.md#test_printer_extruders) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_project_open_settings`](test-entries/windows_web.md#test_project_open_settings) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_sidebar_floating`](test-entries/windows_web.md#test_sidebar_floating) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_set_window`](test-entries/windows_web.md#test_set_window) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Partial |
| [`test_set_view_preview`](test-entries/windows_web.md#test_set_view_preview) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_set_view_camera`](test-entries/windows_web.md#test_set_view_camera) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Partial |
| [`test_speed_dial_open`](test-entries/windows_web.md#test_speed_dial_open) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Partial |
| [`test_send_gcode`](test-entries/windows_web.md#test_send_gcode) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_web_ui`](test-entries/windows_web.md#test_web_ui) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_webview_modal`](test-entries/windows_web.md#test_webview_modal) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Partial |
| [`test_command_line`](test-entries/runtime.md#test_command_line) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_runtime_lifecycle`](test-entries/runtime.md#test_runtime_lifecycle) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_stdio_bridge`](test-entries/runtime.md#test_stdio_bridge) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Partial |
| [`test_orca_mcp_attach`](test-entries/runtime.md#test_orca_mcp_attach) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_run_sweep`](test-entries/runtime.md#test_run_sweep) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_recovery_under_load`](test-entries/runtime.md#test_recovery_under_load) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Partial |
| [`test_worker_admission`](test-entries/runtime.md#test_worker_admission) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_cancel_pending_slice`](test-entries/runtime.md#test_cancel_pending_slice) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_export_running_job`](test-entries/runtime.md#test_export_running_job) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_action_replay_hidden`](test-entries/runtime.md#test_action_replay_hidden) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_list_windows`](test-entries/runtime.md#test_list_windows) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_ref_locator_export`](test-entries/runtime.md#test_ref_locator_export) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Partial |
| [`smoke_test`](test-entries/runtime.md#smoke_test) | [Runtime, command line and harness](test-entries/runtime.md) | replay | Smoke |
| [`test_cad_replay`](test-entries/design.md#test_cad_replay) | [Design tab](test-entries/design.md) | Python | Strong |
| [`test_cad_slow_build`](test-entries/design.md#test_cad_slow_build) | [Design tab](test-entries/design.md) | Python | Partial |
| [`test_cad_failed_job`](test-entries/design.md#test_cad_failed_job) | [Design tab](test-entries/design.md) | Python | Strong |
| [`test_cad_examples`](test-entries/design.md#test_cad_examples) | [Design tab](test-entries/design.md) | Python | Smoke |
| [`test_cad_click_edit`](test-entries/design.md#test_cad_click_edit) | [Design tab](test-entries/design.md) | Python | Strong |
| [`test_cad_commands`](test-entries/design.md#test_cad_commands) | [Design tab](test-entries/design.md) | Python | Partial |
| [`test_cad_sketching`](test-entries/design.md#test_cad_sketching) | [Design tab](test-entries/design.md) | Python | Strong |
| [`test_cad_context_menu`](test-entries/design.md#test_cad_context_menu) | [Design tab](test-entries/design.md) | Python | Strong |
| [`cad_sketch_to_solid`](test-entries/design.md#cad_sketch_to_solid) | [Design tab](test-entries/design.md) | replay | Strong |
| [`cad_sketch_edit`](test-entries/design.md#cad_sketch_edit) | [Design tab](test-entries/design.md) | replay | Strong |
| [`cad_topology_targets`](test-entries/design.md#cad_topology_targets) | [Design tab](test-entries/design.md) | replay | Strong |
| [`cad_features`](test-entries/design.md#cad_features) | [Design tab](test-entries/design.md) | replay | Partial |
| [`cad_doc`](test-entries/design.md#cad_doc) | [Design tab](test-entries/design.md) | replay | Strong |
| [`cad_canvas`](test-entries/design.md#cad_canvas) | [Design tab](test-entries/design.md) | replay | Partial |

## What the sweep does not catch

These patterns run across many tests, and each lets a real regression pass. The entries in the area files name where
each one applies.

**The operations replays record and do not check.** In the four `ops_*` replays, the `expect`s sit on the setup,
and the calls under test are `optional`, the ones that should succeed as well as the ones that should be refused.
Their `result.json` is a record, but nothing compares it with an earlier run's. `test_operation_routes` asserts a
handful of the states they record.

**`optional` accepts both outcomes.** A step that should be refused passes when it is accepted, and one that
should succeed passes when it is refused. A refusal is caught only when a later step happens to read the state it
would have changed, and many refusals have no such step. The Design replays come closest, since a refused call that
went through would usually shift a feature index or a count that the next step checks.

**A wait that runs out of time passes.** `app_wait` replies `satisfied: false`, or `status: "running"` for a job,
as a successful result when its time runs out. Most `app_wait` steps have no `expect`, including the ones meant to
show that a prompt opened, so they pause the replay for up to 30 s and check nothing.

**Fixtures return data and assert nothing.** The `tests/fixtures/*.py` helpers fail their step only when they raise
by accident. `ops_presets.preset_api` catches every exception and returns it as data, so no plugin API call in the
preset replays can fail.

**Replies are trusted instead of read back.** Several checks compare a tool's reply with the arguments it was
given, such as a window's size and position, the legend rows, or a dialog's size. Others pass whenever the call
returns, as `check(name, True)` after the call. Neither reads the state the call should have left.

**Slices are checked by their outcome.** Tests that set per-object, per-range or per-layer settings check that the
slice ended `ok`, and `layers` also checks the layer count. No test reads the G-code, so a setting that never
reaches the print passes.

**Screenshots and recordings are kept, not compared.** Nearly every GUI test saves pictures. Only
`test_set_view_preview` (two captures byte for byte) and `test_cad_commands` ("the screenshots differ") compare any.

**Absence is checked without a control.** Checks such as "no notice", "not in the journal", "the notification gets
no reply", "the broken plugin offers nothing" and a colour's text being absent pass just as well when the thing they
look for was renamed or never produced. No step first shows that it can be seen.

**A few tests wait a fixed time instead of synchronising.** `test_cancel_pending_slice` (0.1 s),
`test_recovery_under_load` (2 s and 1 s) and `test_cad_slow_build` on Windows (2 s) can pass on a slow machine
without reaching the state they claim to test.
