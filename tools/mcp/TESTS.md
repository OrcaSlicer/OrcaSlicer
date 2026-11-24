# MCP sweep tests

The design of the tests `run_sweep.py` runs. This file holds the rules every test follows, the grades, and the index
of all tests. Each test's entry is in its area's file under `test-entries/`: what the test checks, how it decides
pass or fail, what it leaves unchecked, and how far its verdict can be trusted. Change an entry first, before its
test is written or changed.

## Adding or changing a test

A test is a Python script, `test_<area>.py`, designed here first:

1. Write or change the test's entry in its area's file: what it tests, the state it starts from and what it leaves
   in the data folder, its scenarios with the state expected after each step, and what it leaves out on purpose and
   why. A test that fits no area starts a file of its own and a row in the table of areas.
2. Have the entry reviewed and agreed before any test code is written.
3. Write the test to match the entry and add it to `sweep.json`. When the test changes later, change its entry in
   the same commit.

Replays are not tests, and `run_sweep.py` runs none: it takes only the `test_*.py` files. `smoke_test.json` is kept
as the example of the replay format, and `test_runtime_calls` runs it as its input. `test_cad_replays` runs the Design
tab's `tests/cad_*.json` replays the same way, each with `replay_run`, until Python tests under the read-back rule
take their place. No replay is added.

### The read-back rule

A test decides by comparing the app's state with values its entry writes down before the test runs:

- **Start from the baseline, then check it.** The app and the data folder a test runs on were used by the tests
  before it (see [Sharing an app and a data folder](#sharing-an-app-and-a-data-folder)), so a test trusts nothing it
  finds: `start()` puts the app in [the baseline](#the-baseline) with the test's printer, process and filament
  slots, and the test sets anything else its checks depend on beyond the baseline. Then it reads that state and
  checks it, so that each step is measured from a known state: after `start()`, `plate_list` lists one plate,
  current, with the name `""`.
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
- **Wait for a condition, never for a time.** A test that waits for the app checks the state it expects with
  `until()` until it holds or a deadline passes, and fails when the deadline passes; it never pauses a fixed time
  and then assumes the state changed. A deadline is generous, 30 s unless the work takes longer (a slice, an
  install, a launch), because it costs a passing run nothing: the wait ends as soon as the state holds. A check
  that something does not happen first waits until the app reports itself idle (`app_wait {for: idle}`) and then
  checks once; where the app gives no such signal, it samples the state throughout a window its entry states. A
  check never bounds how long the app takes, except with a deadline long enough that only a hang fails it: it
  checks the property the time stood for instead, such as which of two things came first.
- **Exact unless the app computes it.** A position a call gives is checked exactly. A value the app works out,
  such as an arrangement or a layer count, gets a stated tolerance or a property to hold: no two objects overlap,
  every object is on its plate.
- **Check what a person keeps once through the file.** The reading tools share the app's model with the tools that
  change it, so they cannot see a value that is stored but lost on saving. For a property that belongs in the
  project or the print, add one check through the file: save and reopen the project, or read the G-code.

An entry is ready for review when every step has its expected state, and every value a step leaves that nothing
checks appears under **Not checked** with the reason. An entry is written before its test exists; the table of all
tests lists the ones the sweep runs.

### Sharing an app and a data folder

The sweep runs the tests of one job in one app on one data folder, one after another, as a person works through a
day in the OrcaSlicer they opened that morning. The runner launches the job's app once, with `--cad` and
`--unrestricted-scripts`, so that every test has the Design tab and Python scripts without the permission checks,
and launches it again only when it is gone or the next test needs a new one (see [How the sweep
decides](#how-the-sweep-decides)). The folder keeps what each test leaves: user presets, preferences, recent
projects, installed printers, plugins, scripts and each printer's remembered presets, filament slots and plate type.
The app keeps, until it closes, the dialogs and windows left open, the auto-answer rules, a recording, the window's
size and the colour the next new filament slot takes. Which tests ran before a test changes from sweep to sweep,
since the runner orders them by time, or at random with `--shuffle`. Three rules keep a verdict independent of that
order:

- **Start from the baseline.** `start()` resets every setting [the baseline](#the-baseline) lists and closes what
  the app's run keeps open, so that every test starts from the same state whatever ran before it. An entry's
  **Starts from** names the `start()` the test makes and only what it sets beyond the baseline, such as a preference
  at another value. A setting a test's checks depend on that another test may change joins the baseline, rather
  than being set by each test that reads it. A test that needs a printer the seed lacks installs it there; the
  folder keeps it for the tests after.
- **Check what the test owns.** Where a check reads a list that other tests add to, such as the user presets, the
  recent projects, the plugins or the menu items a plugin adds, it compares the items this test made or the app
  ships, picked from the read by name, and the items an earlier test left are outside it. What a test saves under a
  name gets a name no other test uses. A whole-list check stays whole where nothing another test leaves can reach
  the list, as with the plates and objects of a new project.
- **Leave what a person would keep.** A test does not take back what it made: what it leaves is what the next test
  meets, as a person's next hour would, and `start()` resets what the baseline lists. An entry's **Leaves** lists
  it. A test restores at its end, in a `finally`, only a setting `start()` cannot reset, such as the language, which
  the app applies only when it starts again, and its entry says so.

**The default printer.** A data folder `orca_mcp.py launch` seeds has one printer, the Generic Klipper Printer of
the Custom vendor with its 0.4 nozzle: `MyKlipper 0.4 nozzle`, `0.20mm Standard @MyKlipper` and `Generic PLA
@System`. The app then installs only the two vendors it always needs, Custom and the Orca Filament Library, so a
new folder's first start copies about 1,500 profile files instead of 4,000. A test that exercises a Bambu Lab
printer's own behaviour, such as Send over the Bambu network, starts on the Bambu Lab X1 Carbon 0.4 nozzle, which
`start()` installs on the first test of a job that needs it. `test_printers` installs the printers it is about, the
Bambu Lab H2D 0.4 nozzle and the Eryone ER20 0.4 nozzle, on its job's folder with `printer_install`.

### The baseline

The state `start()` puts the app in before every test. Each value is the one a new data folder has, so that a test
on its job's first launch and one on a folder twenty tests have used start alike; the two exceptions are named
below the table. `start()` reads each setting, changes only those that differ, and reads them back.

| Setting | Value | Set with |
|---|---|---|
| Dialogs and windows | none open but the main window | each dialog `app_state.dialogs` lists answered Cancel or closed; each other window `window_list` lists closed |
| Jobs | none running | `until()` on `app_state.jobs` |
| Recording | none | `recording_stop {silent: true}` |
| Auto-answer rules | none | `dialog_auto_answer {rules: []}` |
| Project | a new one, its changes and the presets' unsaved changes discarded, unchanged once the rows below are set | `project_new`, again after them when they marked it changed |
| Printer, process, filament slots | the test's: by default the default printer's, with one slot | `preset_select`, `filament_add`, `filament_delete`; mixed slots deleted |
| Slot colours | slot n takes `SLOT_COLOURS[n-1]`: `#26A69A`, `#00C1AE`, `#F4E2C1`, `#ED1C24`, … | `filament_set_colour` |
| Flushing | multiplier 0.3, prime volume mode Default where the printer offers one, Auto-flush on All (a preference: the volumes follow the colours) | `filament_set_flushing` |
| Plate type | a new folder's for the printer: High Temp Plate on the Klipper printer, Cool Plate on the X1 Carbon | `project_set_settings {values: {curr_bed_type: ...}}` |
| Preferences | a new folder's value for every key `preferences_get` lists, except those it marks refused | one `preferences_set` with the keys that differ |
| Mode | Simple | `settings_set_mode {mode: "simple"}` |
| Display switches | labels, overhangs, outline and wireframe off; gridlines, axes, the navigator and the G-code window on; perspective | `view_set` |
| Window layout | the default: the sidebar 390 px wide and unfolded | `view_set {layout: "default"}` |
| Preview | the legend unfolded, section view off | `view_set {legend_collapsed: false, section: {on: false}}` |
| Sidebar | Global, not Objects; the Run Script panel hidden | the switch's `widget_set_value`; the panel's toggle |
| Tab, canvas and camera | Prepare, 3D, the default view | `view_set {tab: "prepare", canvas: "3d", camera: "default"}` |
| Main window | 1600 × 900, neither maximized nor minimized | `window_set {state: "normal", size: [1600, 900]}` |

The exceptions: the tab is Prepare, not the Home a new app opens on, because every test works in Prepare or goes to
a tab of its own, and Home is a web page; and the main window is 1600 × 900, not the seed's size, because tests that
measure the canvas need a size every machine's screen holds. Slot 1's colour is a new folder's; the next slots'
are the ones a new app gives its first added slots, in that order.

A new folder's preferences are read, not written down here, since their defaults differ between systems and
machines (the download folder is the user's): `orca_mcp.py launch` records them in the folder's
`mcp-baseline.json` when it seeds the folder, and `start()` finds that file through `app_state`'s
`config_folder`. On a folder `launch` did not seed, `start()` leaves the preferences as it finds them. The keys
`preferences_get` marks refused stay out: `preferences_set` refuses them, among them `language` and `region`
(`CadGui.start()` stops when the app is not in English), the MCP server's switch and port, and the file
associations, which are the system's.

Left to the tests that use them: the Design tab's switches, which `cad_gui.reset_switches()` sets in `CadGui.start()`
and in each part of `test_cad_replays`, and its camera, which `CadGui.start()` sets; the Preview's colour scheme and legend rows, which only a sliced plate shows, so a test that reads them sets
them; the arrange options, which `object_arrange` stores from its arguments, so a test that arranges passes them;
and what a test reads only as it finds it or owns by name: the Text gizmo's styles, the Design Text dialog's font,
the Commit to Plate mode, the remembered folders, the printer-agent machines, Speed Dial's counts and the recording
folder. Two lists of the app's run are left too, since no tool clears them: the notifications, which a regular one
leaves on its own about 10 s after it shows, so a test that reads them waits for those an earlier test's actions
left to go; and `app_state`'s `closed_menus`, of which a test reads those closed since its start with
`menus_left()`.

### The shared check module

`checks.py` holds what the tests use to apply the rule, so that it is applied in one place:

- `Checks` collects named checks, prints `ok <name>` or `FAIL <name>: <detail>` for each, and gives the test's exit
  code. A failed check lets the test go on. `Checks.scenario(name, fn)` runs one scenario: an exception fails that
  scenario under its name, and the next scenario still runs.
- `refused(call, code=None, words=None)` passes only when the call is refused with exactly that code, or, for a
  refusal without one, with text that holds those words; it returns the refusal's text.
- `run(client, tool, **args)` makes a call and follows its job to the end, and fails at once, naming the dialog,
  when a dialog the test does not answer holds the call.
- `wait(client, condition, timeout_s)` makes one `app_wait` and fails when it runs out of time.
- `until(fn, what, timeout_s=30, interval_s=0.5)` is how a test waits for anything else: it calls `fn` at once and
  then every `interval_s` until `fn` returns something true, and returns that; when `timeout_s` passes it raises,
  naming `what` and `fn`'s last return, so that a failure says what never happened and what was seen instead. The
  Design tab's helpers in `cad_gui.py` check every 0.05 s, since a click there takes effect within a frame or two.
- `start(client, printer=KLIPPER, process=None, filaments=None, colours=None)` puts the app in [the
  baseline](#the-baseline) with `printer` selected, installed first with `printer_install` when the data folder
  lacks it, then `process` (default: the printer's default process), then one filament slot per name in `filaments`
  (default: the printer's default filament), each slot coloured from `colours` (default: `SLOT_COLOURS`). Any mixed
  filament slot an earlier test left goes first, after adding a slot where one physical slot is left, beside which
  the sidebar deletes no mix, and each switch discards unsaved preset changes. It returns `presets()`, which the
  test compares with the start its entry writes out. `KLIPPER`, `KLIPPER_STD` and `GENERIC_PLA` name the default
  printer's three presets; `X1C`, `X1C_STD` and `BAMBU_PLA` the Bambu Lab X1 Carbon 0.4 nozzle's.
- `add_slot(client, colour)` adds a filament slot as the sidebar's Add does and gives it `colour`, read back. A
  scenario adds slots with it, never with `filament_add` alone: the colour `filament_add` gives is the next of a
  palette the app's run cycles through, so it depends on how many slots the run added before, and a slot's purge,
  and so the prime tower's size, follows its colour.
- `dial_pick(client, title, canvas=SCENE_CANVAS)` opens the Speed Dial with Space and picks the action titled
  `title` as a person's click does, once the dial shows and its page answers; `speed_dial(client)` is the dial as
  `window_list` lists it.
- `menus_left(state, since)` is the part of `app_state`'s `closed_menus` closed after `since`: the list keeps the
  app's run's last three menus, an earlier test's among them, so a test checks the ones its own calls left.
- Readers give the state a scenario checks in a fixed shape, such as `plates()`: a list of `{name, current, locked,
  objects}`, one per plate, and `presets()`: `{printer, process, filaments}`, the selected presets' names with one
  filament per slot.

## How the sweep decides

**The runner.** `run_sweep.py` runs the entries of `sweep.json` on `-j` jobs at once. Each job has one data folder
and one app for all its tests: the first launch seeds the folder with the default printer, and the app, launched
with `--cad --unrestricted-scripts`, runs the job's tests one after another. Before each test the runner checks
that the app answers and launches it again on the folder when it does not: after a crash, after a test that closed
it, or after the runner killed it for a test that ran past its time limit. A test whose checks need a newly
started app has `"launch": "fresh"` in `sweep.json`, and the runner launches the app again before it; a test that
starts apps of its own has `"launch": "own"` and runs them on folders of its own. Each test has a folder of its own
for its session file, temporary files and working folder. The job's app has its temporary files and working folder
in the job's folder instead, since it outlives the test that launched it, whose folder goes when that test passes.
The longest tests, by their last passing time in the history file, start first, and tests that hold the same lock
never overlap. `--shuffle SEED` orders them at random instead and prints the seed, so that an order that fails can
be run again.

The serial tests, those `sweep.json` marks `exclusive`, run after the rest, one at a time with nothing else
running, on a job of their own. A test that fails while other tests run is run again in that phase, alone, in a new
app on the data folder it failed on, so that a failure that follows from what an earlier test left in the folder
fails again, and one that follows from the load passes. A test that passes alone counts as passed, and the summary
lists it as `passed alone`.

A test passes when its process exits 0. A failed setup or launch, a run past its time limit (900 s unless the
entry sets `timeout_s`), or an error running the test fails it, and so does a test left without a result. On macOS
a new OrcaSlicer crash report fails the run. A test during which the app dies fails, since its calls go unanswered,
and the next test on the job starts a new app. When a job's last test ends, the runner's `stop` ends its app
(`TerminateProcess` on Windows, `SIGTERM` elsewhere) and waits until the app's processes, its WebView2 ones
included, are gone, so that two apps never hold one folder; it ignores how the app exited, so a crash while the app
closes fails nothing on Windows or Linux, and only the tests that close the app themselves see it.

A passing run's folder is deleted as soon as it passes. A failed run's folder is kept with a copy of the app's
log from the job's data folder, also when the test then passes alone, and so is the data folder of a job one of
whose tests failed; the other data folders are deleted at the end, and `--keep` keeps everything. `results.jsonl` records each run of each test with
its job and the tests that ran before it on that job's data folder, so that a failure can be traced to what they
left. Tests that start their own app (`"launch": "own"`) start it on data folders of their own, with their own
environment, so on Linux they don't follow the X11 default. With `--headless` or `--xvfb` the runner starts a private
display for each such test, which its apps inherit through that environment, so they stay off the desktop too. No CI
job runs the sweep: it runs when someone runs it.

**Replays a test runs.** A test that runs a replay with `replay_run` reads its `passed`, which is true when every
step was ok. A step is ok
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
entry there before it is written.

| Area | Python tests |
|---|---|
| [Plates and objects](test-entries/objects.md) | [`test_objects`](test-entries/objects.md#test_objects) |
| [Presets and calibration](test-entries/presets.md) | [`test_presets`](test-entries/presets.md#test_presets) |
| [Filaments and settings](test-entries/filaments_settings.md) | [`test_filaments`](test-entries/filaments_settings.md#test_filaments), [`test_settings`](test-entries/filaments_settings.md#test_settings) |
| [Project files, imports and exports](test-entries/files.md) | [`test_files`](test-entries/files.md#test_files) |
| [Slicing, layers and the Preview](test-entries/slice_preview.md) | [`test_slice_preview`](test-entries/slice_preview.md#test_slice_preview) |
| [Gizmos and canvas input](test-entries/gizmos.md) | [`test_gizmos`](test-entries/gizmos.md#test_gizmos) |
| [GUI automation tools](test-entries/gui_tools.md) | [`test_gui_tools`](test-entries/gui_tools.md#test_gui_tools), [`test_recording`](test-entries/gui_tools.md#test_recording), [`test_call_forms`](test-entries/gui_tools.md#test_call_forms) |
| [Commands, preferences, installs and plugins](test-entries/commands.md) | [`test_speed_dial`](test-entries/commands.md#test_speed_dial), [`test_menu_items_refusals`](test-entries/commands.md#test_menu_items_refusals), [`test_additions_refusals`](test-entries/commands.md#test_additions_refusals), [`test_person_forms`](test-entries/commands.md#test_person_forms), [`test_preferences`](test-entries/commands.md#test_preferences), [`test_install_printers`](test-entries/commands.md#test_install_printers), [`test_install_filaments`](test-entries/commands.md#test_install_filaments), [`test_plugin_commands`](test-entries/commands.md#test_plugin_commands), [`test_plugin_tools`](test-entries/commands.md#test_plugin_tools), [`test_operation_routes`](test-entries/commands.md#test_operation_routes) |
| [Windows, views, printers and web pages](test-entries/windows_web.md) | [`test_printers`](test-entries/windows_web.md#test_printers), [`test_project_open_settings`](test-entries/windows_web.md#test_project_open_settings), [`test_sidebar_floating`](test-entries/windows_web.md#test_sidebar_floating), [`test_set_window`](test-entries/windows_web.md#test_set_window), [`test_set_view_camera`](test-entries/windows_web.md#test_set_view_camera), [`test_speed_dial_open`](test-entries/windows_web.md#test_speed_dial_open), [`test_send_gcode`](test-entries/windows_web.md#test_send_gcode), [`test_web_ui`](test-entries/windows_web.md#test_web_ui) |
| [Runtime, command line and harness](test-entries/runtime.md) | [`test_runtime_lifecycle`](test-entries/runtime.md#test_runtime_lifecycle), [`test_stdio_bridge`](test-entries/runtime.md#test_stdio_bridge), [`test_orca_mcp_attach`](test-entries/runtime.md#test_orca_mcp_attach), [`test_run_sweep`](test-entries/runtime.md#test_run_sweep), [`test_runtime_calls`](test-entries/runtime.md#test_runtime_calls), [`test_export_running_job`](test-entries/runtime.md#test_export_running_job) |
| [Design tab](test-entries/design.md) | [`test_cad_replays`](test-entries/design.md#test_cad_replays), [`test_cad_failed_job`](test-entries/design.md#test_cad_failed_job), [`test_cad_commands`](test-entries/design.md#test_cad_commands), [`test_cad_sketching`](test-entries/design.md#test_cad_sketching), [`test_cad_context_menu`](test-entries/design.md#test_cad_context_menu) |

## All tests

The 39 tests `sweep.json` lists: 29 Strong, 10 Partial.

| Test | Area | Kind | Strength |
|---|---|---|---|
| [`test_objects`](test-entries/objects.md#test_objects) | [Plates and objects](test-entries/objects.md) | Python | Strong |
| [`test_presets`](test-entries/presets.md#test_presets) | [Presets and calibration](test-entries/presets.md) | Python | Strong |
| [`test_filaments`](test-entries/filaments_settings.md#test_filaments) | [Filaments and settings](test-entries/filaments_settings.md) | Python | Strong |
| [`test_settings`](test-entries/filaments_settings.md#test_settings) | [Filaments and settings](test-entries/filaments_settings.md) | Python | Strong |
| [`test_files`](test-entries/files.md#test_files) | [Project files, imports and exports](test-entries/files.md) | Python | Strong |
| [`test_slice_preview`](test-entries/slice_preview.md#test_slice_preview) | [Slicing, layers and the Preview](test-entries/slice_preview.md) | Python | Strong |
| [`test_gizmos`](test-entries/gizmos.md#test_gizmos) | [Gizmos and canvas input](test-entries/gizmos.md) | Python | Strong |
| [`test_gui_tools`](test-entries/gui_tools.md#test_gui_tools) | [GUI automation tools](test-entries/gui_tools.md) | Python | Strong |
| [`test_recording`](test-entries/gui_tools.md#test_recording) | [GUI automation tools](test-entries/gui_tools.md) | Python | Strong |
| [`test_call_forms`](test-entries/gui_tools.md#test_call_forms) | [GUI automation tools](test-entries/gui_tools.md) | Python | Strong |
| [`test_speed_dial`](test-entries/commands.md#test_speed_dial) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Partial |
| [`test_menu_items_refusals`](test-entries/commands.md#test_menu_items_refusals) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_additions_refusals`](test-entries/commands.md#test_additions_refusals) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_person_forms`](test-entries/commands.md#test_person_forms) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_preferences`](test-entries/commands.md#test_preferences) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_install_printers`](test-entries/commands.md#test_install_printers) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_install_filaments`](test-entries/commands.md#test_install_filaments) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_plugin_commands`](test-entries/commands.md#test_plugin_commands) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Strong |
| [`test_plugin_tools`](test-entries/commands.md#test_plugin_tools) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Partial |
| [`test_operation_routes`](test-entries/commands.md#test_operation_routes) | [Commands, preferences, installs and plugins](test-entries/commands.md) | Python | Partial |
| [`test_printers`](test-entries/windows_web.md#test_printers) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_project_open_settings`](test-entries/windows_web.md#test_project_open_settings) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_sidebar_floating`](test-entries/windows_web.md#test_sidebar_floating) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_set_window`](test-entries/windows_web.md#test_set_window) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Partial |
| [`test_set_view_camera`](test-entries/windows_web.md#test_set_view_camera) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Partial |
| [`test_speed_dial_open`](test-entries/windows_web.md#test_speed_dial_open) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Partial |
| [`test_send_gcode`](test-entries/windows_web.md#test_send_gcode) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_web_ui`](test-entries/windows_web.md#test_web_ui) | [Windows, views, printers and web pages](test-entries/windows_web.md) | Python | Strong |
| [`test_runtime_lifecycle`](test-entries/runtime.md#test_runtime_lifecycle) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_stdio_bridge`](test-entries/runtime.md#test_stdio_bridge) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Partial |
| [`test_orca_mcp_attach`](test-entries/runtime.md#test_orca_mcp_attach) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_run_sweep`](test-entries/runtime.md#test_run_sweep) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_runtime_calls`](test-entries/runtime.md#test_runtime_calls) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Partial |
| [`test_export_running_job`](test-entries/runtime.md#test_export_running_job) | [Runtime, command line and harness](test-entries/runtime.md) | Python | Strong |
| [`test_cad_replays`](test-entries/design.md#test_cad_replays) | [Design tab](test-entries/design.md) | Python | Partial |
| [`test_cad_failed_job`](test-entries/design.md#test_cad_failed_job) | [Design tab](test-entries/design.md) | Python | Strong |
| [`test_cad_commands`](test-entries/design.md#test_cad_commands) | [Design tab](test-entries/design.md) | Python | Partial |
| [`test_cad_sketching`](test-entries/design.md#test_cad_sketching) | [Design tab](test-entries/design.md) | Python | Strong |
| [`test_cad_context_menu`](test-entries/design.md#test_cad_context_menu) | [Design tab](test-entries/design.md) | Python | Strong |

## What the sweep does not catch

These patterns remain, and each lets a real regression pass. The entries in the area files name where each one
applies.

**The Design tab's tests are not under the read-back rule.** The `cad_*` replays `test_cad_replays` runs and the
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

**A few steps still trust a fixed time, where no tool shows the state to wait for.** `test_runtime_calls` gives its
32 half-sent connections 1 s and the Run Script panel's page 1 s, since no tool sees either; it cancels a slice as
soon as `app_state` lists its job, inside the slice's half-second wait, a race a slow machine fails rather than
passes. `test_plugin_tools`' `--run` replay waits 10 s for a dialog that never opens, so that the plugin script it
starts can record its outcome: a replay has no step that waits for a file.
