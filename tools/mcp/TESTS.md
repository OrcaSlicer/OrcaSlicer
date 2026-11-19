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
| [Objects](test-entries/objects.md) | [`test_objects`](test-entries/objects.md#test_objects), [`test_object_structure`](test-entries/objects.md#test_object_structure) | — |
| [Presets and calibration](test-entries/presets.md) | [`test_presets`](test-entries/presets.md#test_presets), [`test_calibration`](test-entries/presets.md#test_calibration) | — |
| [Filaments and settings](test-entries/filaments_settings.md) | [`test_filaments`](test-entries/filaments_settings.md#test_filaments), [`test_settings`](test-entries/filaments_settings.md#test_settings) | — |
| [Project files, imports and exports](test-entries/files.md) | [`test_files`](test-entries/files.md#test_files) | — |
| [Slicing, layers and the Preview](test-entries/slice_preview.md) | [`test_slice_preview`](test-entries/slice_preview.md#test_slice_preview) | — |
| [Gizmos and canvas input](test-entries/gizmos.md) | [`test_gizmos`](test-entries/gizmos.md#test_gizmos), [`test_gizmo_commands`](test-entries/gizmos.md#test_gizmo_commands) | — |
| [GUI automation tools](test-entries/gui_tools.md) | [`test_gui_tools`](test-entries/gui_tools.md#test_gui_tools), [`test_recording`](test-entries/gui_tools.md#test_recording), [`test_call_forms`](test-entries/gui_tools.md#test_call_forms) | — |
| [Commands, preferences, installs and plugins](test-entries/commands.md) | [`test_every_action`](test-entries/commands.md#test_every_action), [`test_menu_items_refusals`](test-entries/commands.md#test_menu_items_refusals), [`test_additions_refusals`](test-entries/commands.md#test_additions_refusals), [`test_person_forms`](test-entries/commands.md#test_person_forms), [`test_preferences`](test-entries/commands.md#test_preferences), [`test_install_printers`](test-entries/commands.md#test_install_printers), [`test_install_filaments`](test-entries/commands.md#test_install_filaments), [`test_plugin_commands`](test-entries/commands.md#test_plugin_commands), [`test_plugin_tools`](test-entries/commands.md#test_plugin_tools), [`test_operation_routes`](test-entries/commands.md#test_operation_routes) | — |
| [Windows, views, printers and web pages](test-entries/windows_web.md) | [`test_printer_extruders`](test-entries/windows_web.md#test_printer_extruders), [`test_project_open_settings`](test-entries/windows_web.md#test_project_open_settings), [`test_sidebar_floating`](test-entries/windows_web.md#test_sidebar_floating), [`test_set_window`](test-entries/windows_web.md#test_set_window), [`test_set_view_preview`](test-entries/windows_web.md#test_set_view_preview), [`test_set_view_camera`](test-entries/windows_web.md#test_set_view_camera), [`test_speed_dial_open`](test-entries/windows_web.md#test_speed_dial_open), [`test_send_gcode`](test-entries/windows_web.md#test_send_gcode), [`test_web_ui`](test-entries/windows_web.md#test_web_ui), [`test_webview_modal`](test-entries/windows_web.md#test_webview_modal) | — |
| [Runtime, command line and harness](test-entries/runtime.md) | [`test_command_line`](test-entries/runtime.md#test_command_line), [`test_runtime_lifecycle`](test-entries/runtime.md#test_runtime_lifecycle), [`test_stdio_bridge`](test-entries/runtime.md#test_stdio_bridge), [`test_orca_mcp_attach`](test-entries/runtime.md#test_orca_mcp_attach), [`test_run_sweep`](test-entries/runtime.md#test_run_sweep), [`test_recovery_under_load`](test-entries/runtime.md#test_recovery_under_load), [`test_worker_admission`](test-entries/runtime.md#test_worker_admission), [`test_cancel_pending_slice`](test-entries/runtime.md#test_cancel_pending_slice), [`test_export_running_job`](test-entries/runtime.md#test_export_running_job), [`test_action_replay_hidden`](test-entries/runtime.md#test_action_replay_hidden), [`test_list_windows`](test-entries/runtime.md#test_list_windows), [`test_ref_locator_export`](test-entries/runtime.md#test_ref_locator_export) | [`smoke_test`](test-entries/runtime.md#smoke_test) |
| [Design tab](test-entries/design.md) | [`test_cad_replay`](test-entries/design.md#test_cad_replay), [`test_cad_slow_build`](test-entries/design.md#test_cad_slow_build), [`test_cad_failed_job`](test-entries/design.md#test_cad_failed_job), [`test_cad_examples`](test-entries/design.md#test_cad_examples), [`test_cad_click_edit`](test-entries/design.md#test_cad_click_edit), [`test_cad_commands`](test-entries/design.md#test_cad_commands), [`test_cad_sketching`](test-entries/design.md#test_cad_sketching), [`test_cad_context_menu`](test-entries/design.md#test_cad_context_menu) | [`cad_sketch_to_solid`](test-entries/design.md#cad_sketch_to_solid), [`cad_sketch_edit`](test-entries/design.md#cad_sketch_edit), [`cad_topology_targets`](test-entries/design.md#cad_topology_targets), [`cad_features`](test-entries/design.md#cad_features), [`cad_doc`](test-entries/design.md#cad_doc), [`cad_canvas`](test-entries/design.md#cad_canvas) |

## All tests

The 61 tests `sweep.json` lists: 45 Strong, 14 Partial, 2 Smoke.

| Test | Area | Kind | Strength |
|---|---|---|---|
| [`test_plates`](test-entries/plates.md#test_plates) | [Plates](test-entries/plates.md) | Python | Strong |
| [`test_objects`](test-entries/objects.md#test_objects) | [Objects](test-entries/objects.md) | Python | Strong |
| [`test_object_structure`](test-entries/objects.md#test_object_structure) | [Objects](test-entries/objects.md) | Python | Strong |
| [`test_presets`](test-entries/presets.md#test_presets) | [Presets and calibration](test-entries/presets.md) | Python | Strong |
| [`test_calibration`](test-entries/presets.md#test_calibration) | [Presets and calibration](test-entries/presets.md) | Python | Strong |
| [`test_filaments`](test-entries/filaments_settings.md#test_filaments) | [Filaments and settings](test-entries/filaments_settings.md) | Python | Strong |
| [`test_settings`](test-entries/filaments_settings.md#test_settings) | [Filaments and settings](test-entries/filaments_settings.md) | Python | Strong |
| [`test_files`](test-entries/files.md#test_files) | [Project files, imports and exports](test-entries/files.md) | Python | Strong |
| [`test_slice_preview`](test-entries/slice_preview.md#test_slice_preview) | [Slicing, layers and the Preview](test-entries/slice_preview.md) | Python | Strong |
| [`test_gizmos`](test-entries/gizmos.md#test_gizmos) | [Gizmos and canvas input](test-entries/gizmos.md) | Python | Strong |
| [`test_gizmo_commands`](test-entries/gizmos.md#test_gizmo_commands) | [Gizmos and canvas input](test-entries/gizmos.md) | Python | Strong |
| [`test_gui_tools`](test-entries/gui_tools.md#test_gui_tools) | [GUI automation tools](test-entries/gui_tools.md) | Python | Strong |
| [`test_recording`](test-entries/gui_tools.md#test_recording) | [GUI automation tools](test-entries/gui_tools.md) | Python | Strong |
| [`test_call_forms`](test-entries/gui_tools.md#test_call_forms) | [GUI automation tools](test-entries/gui_tools.md) | Python | Strong |
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

These patterns remain, and each lets a real regression pass. The entries in the area files name where each one
applies.

**The Design tab's tests are not under the read-back rule.** The `cad_*` replays still in the sweep and the
`test_cad_*.py` tests are as they were. In the replays a step marked `optional` accepts both outcomes, so a step that
should be refused passes when it is accepted and one that should succeed passes when it is refused; a refusal is
caught only when a later step reads the state it would have changed. A replay's `app_wait` that runs out of time
replies `satisfied: false`, or a job still running, as a successful result, so it pauses the replay and checks
nothing.

**What the app works out is held to bounds.** Grams, times, adaptive layer profiles, arrangements, frame counts and
capture times follow the slicer's or the machine's work, so their checks are bounds or properties, and a change that
stays inside them passes.

**Pictures are compared only with each other.** Screenshots and recordings are checked by their size, their source,
or how they differ from another capture of the same run, never against a reference picture, so a view that draws
something wrong passes.

**A few steps still trust a fixed time.** `test_cancel_pending_slice` cancels 0.1 s after asking for the slice and
then checks that the cancel came before the slice started, so a slow machine fails it rather than passing it;
`test_recovery_under_load` gives its 32 half-sent connections 1 s to open and trusts that they filled the server's
connections.
