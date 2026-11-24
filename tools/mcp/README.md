# MCP harness

`orca_mcp.py` launches OrcaSlicer with its MCP server and talks to it, from a shell or from
Python. It uses only the standard library. `docs/HLSD/mcp-server.md` covers the server, and
`docs/HLSD/mcp-tools.md` its commands.

## Commands

| Command | What it does |
|---|---|
| `launch [--headless [kwin\|mutter] \| --xvfb] [--cad] [--unrestricted-scripts] [--printer "<model>@<nozzle>"] [--vendor V] [--filament F] [--datadir DIR] [--port N] [--bin PATH] [--print-config] [-- app args]` | Start OrcaSlicer with `--mcp`, wait until it is ready, print the session |
| `call TOOL ['<json args>'] [--wait]` | Call a tool and print its JSON result; `--wait` follows a returned job to its result |
| `tools` | List the tools, with the first line of each description |
| `widgets ['<json args>']` | Print the `widget_list` tree |
| `screenshot OUT [--target main\|canvas\|dialog]` | Save a full-size PNG, replacing `OUT` if it exists |
| `run-replay FILE [--out DIR]` | Run a replay in the launched instance and print the result; exit 0 when it passes |
| `stop` | Stop the app and its private display, and remove the session |

`launch` creates a fresh temporary datadir unless you pass `--datadir`. It seeds the datadir with
an `OrcaSlicer.conf` that has a finished setup wizard and one printer and filament (by default the
Custom vendor's Generic Klipper Printer 0.4 nozzle with Generic PLA, so a first start copies no other vendor's
profiles; `--vendor V --printer "<model>@<nozzle>"` seeds another, its preset named as the vendor's profiles name
it, with `--filament` or the model's first default filament), with update checks and daily tips turned off, and with the Orca Cloud login kept
in a token file in the datadir (`use_encrypted_token_file`) instead of the system's secret store, so the
instance starts signed out and never reads your login: on macOS that read would stop a newly built app at
a keychain prompt. A seeded datadir's main window opens 1600×900 at the top left of the screen's work area
(on macOS below the menu bar), so it fits on a 1920×1080 screen, instances started side by side stay on
screen, and the tests' views and screenshots all have one size; the tests that size the window ask for
the same 1600×900. `--cad` starts the app with
`--cad`, which turns the Design (CAD) tab on for that run, and with it its commands, and
`--unrestricted-scripts` starts it with `--unrestricted-scripts`, which lets Python scripts
(`script_run`) run unrestricted for that run. Neither writes the datadir's `OrcaSlicer.conf`, and a later
launch without them has the settings the datadir holds. The token comes from
`ORCA_MCP_TOKEN` or is generated, and it reaches the app through the environment, not the command
line. On Linux, `launch` also sets `SSL_CERT_FILE`, so a fresh datadir doesn't prompt for a CA
bundle, and if that prompt does come up anyway, as when the file it names is gone, `launch` answers it
with Yes, as it waits for the app to be ready: an unattended launch has no person to answer it. It answers No
to the prompt to restore unsaved items, which opens when the last app on the datadir was ended with a project
unsaved, so a launch always starts on an empty project. On
Windows, it gives the instance its own WebView2 profile in the datadir, apart from the one
your own OrcaSlicer uses, and finds `build-clang\src\Release\orca-slicer.exe` as well; run the
commands as `python tools/mcp/orca_mcp.py …`. On macOS, it finds the app bundle's binary under
`build/arm64`, `build/x86_64`, `build/universal` or `build`: the bundle the CMake target builds in
`src/Release`, or the one `build_release_macos.sh` assembles in `OrcaSlicer`. The app opens on the
desktop and takes focus while it starts, and needs no CA-bundle setting.

On Linux, `--headless` and `--xvfb` keep the app off your desktop, on a private screen of
`--width`×`--height` (default 1600×1000), and run it as an X11 client (`GDK_BACKEND=x11`). Use
`--headless` on a machine with a desktop. It starts a headless Wayland compositor on a session bus of
its own and runs the app on that compositor's XWayland. The app draws with the GPU's OpenGL driver,
as on your desktop, and the compositor is a window manager, so focus, stacking and popup lists
behave as they do there; dialogs show in screenshots and recordings, and the private bus keeps the
D-Bus services the app starts, such as the desktop portals, out of your session. It takes KWin
(`kwin_wayland --virtual`), else mutter (`mutter --headless`; test required: not yet run on a GNOME
machine, so check the window, input, screenshots and the OpenGL driver the app's log reports before
relying on it); `--headless kwin` or `--headless mutter` names one. The compositor's output goes to
`<datadir>/mcp-display.log`. When neither is installed or starts, `--headless` says so and falls
back to Xvfb. `--xvfb` is for a machine without a desktop, such as a CI runner or a server. It
starts Xvfb on a free display between `:90` and `:119`, one of its own even when other launches
start at the same moment, as a sweep's jobs do. Xvfb draws OpenGL in software, and runs Openbox
as its window manager when Openbox is installed, as Chromium's tests do under Xvfb. Without a window
manager, a popup list, such as a combo box's drop-down or the Print button's menu, closes as soon as
it opens, and `launch` says so. Xvfb runs with `-noreset`, and its pointer is parked in the bottom-right
corner. In the middle of the screen, the pointer would raise tooltips over whatever opens there, and
they would show in screenshots. The app's output goes to `<datadir>/mcp-launch.log`. `--timeout`
(default 120 s) limits how long `launch` waits for `ready`.

## Session file

`launch` writes `{port, token, pid, datadir, display_server, display_server_pid, display_env,
display}` to `$ORCA_MCP_SESSION`, or to `~/.cache/orca-mcp/session.json` if that is unset.
`display_server` is `kwin`, `mutter`, `xvfb` or null (your desktop), and `display_env` holds the
variables the app was started with to reach it (null for one removed), which `app_env()` applies to
another process. The file is mode 0600, and every other command
reads it. To run several instances at once, give each its own `ORCA_MCP_SESSION` and `--port`.

## From Python

```python
import os, sys
sys.path.insert(0, "tools/mcp")
from orca_mcp import OrcaMcp, McpError

orca = OrcaMcp(port=13619, token=os.environ["ORCA_MCP_TOKEN"])
orca.call("project_import", paths=["/abs/path/part.stl"])
result = orca.await_job(orca.call("plate_slice"))    # follows a job envelope to the end
orca.call("app_wait", **{"for": "idle"})             # "for" is a Python keyword
info, pngs = orca.images("window_screenshot", target="canvas")
```

`call()` returns the tool's JSON result and raises `McpError` when the tool reports an error. The server
rounds a reply's numbers for an agent to read (4 decimals, or 3 significant digits where that keeps more);
`orca.exact_numbers = True` asks every later call for them whole, for a check that computes with them.
`call_with_text()` also returns the text blocks after the JSON one, such as `widget_list`'s tree.
`images()` also returns the PNG bytes of the image blocks. `call_raw()` returns the MCP result
unchanged. With `ORCA_MCP_CALL_LOG` set to a file, every call appends a JSON line with its tool, its
time in milliseconds and whether it succeeded, which shows where a test spends its time.

To drive an OrcaSlicer that is already running with the MCP server on (`--mcp`, or "MCP server"
under AI features in Preferences), attach to it through its datadir instead of a port and token:

```python
orca = OrcaMcp.from_datadir()                        # the datadir the app uses without --datadir
orca = OrcaMcp.from_datadir("/abs/datadir")          # an app started with --datadir /abs/datadir
design = orca.call_command("describe_design")             # a Design command (the Design tab on)
orca.call_command("helix", radius=10, pitch=3, height=30)
```

`from_datadir()` reads the port and token from `<datadir>/mcp/server.json`, which the server writes
when it starts. Without a datadir it takes `default_datadir()`: `%APPDATA%\OrcaSlicer` on Windows,
`~/Library/Application Support/OrcaSlicer` on macOS, `$XDG_CONFIG_HOME/OrcaSlicer` (or
`~/.config/OrcaSlicer`) on Linux. A portable install keeps its datadir in `data_dir` beside the
binary (beside the `.app` on macOS) and is found only when that datadir is given, or the binary as
`from_datadir(exe=...)`; a symlink to the binary is resolved on Linux, as the app resolves its own,
and taken as it is on Windows, as the app takes it. On macOS it is taken as it is, as the stdio
bridge (`McpStdioBridge.cpp`) takes it; whether the app itself resolves a symlinked binary there is
unverified, which matters only for a symlinked binary. When the file is missing, or a ping to the
port it names fails (nothing listens there, or another server answers), `from_datadir()` raises
`McpError` saying so, with the failure as its cause.

`call_command(tool, **args)` calls `tool_call {tool, args}`, follows a running job to its end and
returns the command's reply. A call whose first
reply says a dialog blocks it fails at once, naming the dialog, rather than waiting on it. A call that
first replied `running` and opens a dialog later is waited on up to the timeout (600 s), since `app_wait`
reports only that its job is still running. Every `McpError` it raises starts with the command's name
(`"extrude: ..."`) and has the original error as its cause. `tool_list {group: "Design"}` lists the
Design commands with what each does, and `tool_describe {tool}` gives one's manual, its arguments and examples among it.

## The skill's list of actions

`gen_skill_reference.py <skills-checkout> [--check]` writes
`skills/orcaslicer-mcp-skills/references/actions.md` in a checkout of the orcaslicer-mcp-skills repository:
the commands' Speed Dial actions section by section, from the command reference
`tests/data/command_catalog.json` (`docs/HLSD/mcp-server.md`, "Making a feature reachable"), and before them
what the reference says of particular commands: the Design commands with one form, which needs its value,
and the commands of actions the tool list does not show. Outside the Design tab a section is a Speed Dial
section (an action's `group`, or the `action_group` of a command's own action), and each action has a row
with its key, its title, the command and arguments it runs (`project_export {"format": "gcode"}` for Export
G-code) and its description. In the Design section a command has a row of its own when it has an action of
its own (`has_own_action`), and each Design menu item folded into a command follows it as a row with the
command's key, the item's title and the arguments that open its tool (`sketch_pattern` with
`{"type": "linear"}` for Linear array). It needs no running app. With `--check` it writes
nothing, prints the difference and exits 1 when the file is not what it would write. When a command
changed, regenerate the reference first: run `slic3rutils_tests "[Reference]"` with
`ORCA_UPDATE_COMMAND_REFERENCE=1` in the environment.

## Smoke test

`smoke_test.json` is an end-to-end replay that exercises the main paths, on a fresh datadir or one earlier tests
used, since it selects its own presets:

- an auto-answered startup or unsaved-changes prompt;
- loading, a setting change, slicing and a G-code export;
- a dialog that an action opens and the replay answers;
- screenshots of the dialog and the preview.

```bash
tools/mcp/orca_mcp.py launch --headless
tools/mcp/orca_mcp.py run-replay tools/mcp/smoke_test.json --out /tmp/orca-smoke
tools/mcp/orca_mcp.py stop
```

Without the harness, `orca-slicer --datadir DIR --run tools/mcp/smoke_test.json
--outputdir /tmp/orca-smoke` runs the replay and exits. On Linux, set `SSL_CERT_FILE` (for
example to `/etc/ssl/certs/ca-certificates.crt`) as the harness does; otherwise a prompt for the CA
bundle blocks startup before the replay can run. The exit code is 0 when it passes, 1
when a step fails and 2 when the replay is invalid, cannot be read or is neither a `.json` nor a `.py`
file. The run leaves `result.json` and the screenshots in the `--outputdir` folder, or without it in a
new `orca_action_replay/replay-<timestamp>` folder in the temporary folder (`%TEMP%` on Windows,
`<tmp>/orcaslicer_<uid>` elsewhere). Files that a step or a script saves without a path go to the
`--outputdir` folder too, or without it to `<datadir>/automation/out`. A `--run` launch is never handed to a window that is already open, whatever
the single-instance preference says, and it is refused with an action or `--downward-check`, which start
no GUI.

`test_runtime_lifecycle.py [--bin PATH] [CHECK ...]` checks how the app's automation behaves while the app closes,
and while its server is switched off and on during a call, starting the built app as its own child on fresh data
folders. The app is closed with the File menu's Quit while a job runs, while a recording runs, and while a call no
stop interrupts is still running, where the app keeps the runtime rather than free it under the call and logs that
a call did not return; a `--run` with the endpoint never opened exits on its own; the server, switched off and
on in Preferences during a call, serves on the port it had; and with the server switched off and nothing else
driving the GUI, the log says the GUI instruments went off. It prints one line per check and takes about a minute
and a half:
the run that outlives its reply budget runs with the budget capped at 4 s (`ORCA_MCP_REPLY_BUDGET_MS`).

`test_plugin_tools.py` checks a plugin's calls into the tools in the running app: it side-loads
`tests/plugins/mcp_tools_fixture` into a fresh data folder and launches twice: over MCP, with the Design tab
on (its operation is in the group Plugins, listed by `tool_list`, described by `tool_describe`, run by
`tool_call` and out of `tools/list`; a menu item's command is listed by `tool_list` with what it does and run
by `tool_call`; `group` takes a group's name in any case, a group whose commands are all listed gives a note,
and the Design commands are listed under their headings; calls from its Script worker
and a thread it starts reach the tools, an auto-answer rule it sets is the plugin's, `replay_export` is
refused, nothing it calls is journaled, a script that finds its progress dialog handle can use it,
`tests/plugins/mcp_tools_broken`, whose load fails, offers none of its operations, and unticking the
plugin in the Plugins dialog withdraws its operation and ends its rule); and without MCP,
where a `--run` runs the plugin's script and its rule is still the plugin's. Run it as
`python tools/mcp/test_plugin_tools.py`. The
Run Script panel's `.py` files are not driven from here: the panel is hidden from automation and opens its
files through a file picker, so how it reads and saves them is covered by the unit tests of
`decode_python_source()` and `encode_python_source()`.

A `.py` file given to `--run` runs as a Python script (`script_run`) instead, exiting 0 when it
finishes and 1 when it raises, is cancelled or runs out of time; its traceback goes to the log. A
script's own file access is held to OrcaSlicer's folders, and it starts no programs or threads and reaches
no network, unless scripts run unrestricted: start the app with `--unrestricted-scripts` for one run (as
`orca_mcp.py launch --unrestricted-scripts` does), or a person turns the setting on in Preferences. No tool
can turn either on.

A `.py` file runs as Python runs it (`python file.py`), with the whole plugin API: `__name__` is
`"__main__"`, `__file__` and `sys.argv` name it, the modules beside it import (afresh each run, so an edit
shows), and the packages its PEP 723 block lists are installed first. It may open windows and progress
dialogs, which close when the run ends, and its threads and pool tasks call the tools too. The run ends once
its code and the non-daemon threads and pool tasks it started are done:

```python
# /// script
# dependencies = ["numpy"]
# ///
import queue, threading
import orca
import helper                                    # helper.py beside this file

def main():
    progress = orca.host.ui.create_progress_dialog("Measuring", "Starting", 3)
    for step in range(3):
        progress.update(step + 1, helper.describe(step))
    # A window's callbacks run on the UI thread and call no tools; they hand the work to the script.
    pages = queue.Queue()
    window = orca.host.ui.create_window("<button onclick='window.orca.postMessage({go: 1})'>Go</button>",
                                        on_message=pages.put)
    pages.get()                                  # the person pressed Go
    worker = threading.Thread(target=lambda: print(orca.host.tools.call("app_state")["ready"]))
    worker.start()                               # waited for before the run ends

if __name__ == "__main__":
    main()
```

A window that stays open is closed with the run, so a script that wants the person to work in it waits, as
the `pages.get()` above does. While a modal it opened is up, tool calls that change the project are refused,
so interactive scripts use modeless windows. `docs/HLSD/mcp-server.md` ("Python scripts") has the rules for
overlapping runs, imports, pools, `atexit` and Stop.

`test_gizmos.py` drives every gizmo through the GUI tools, the way a person does: each one's icon, its panel's
fields, combos, toggles and links, and clicks or strokes at points of the model. It reads each gizmo's work back from
the model it leaves (placements, painted facets, texture layers, text and SVG parts, brim ears), checks its undo
before any redo, and reads what a person keeps again from a saved and reopened project. It also checks the canvas
input the gizmos rely on (a press held across gestures, a toolbar item a selection has just enabled, points on a
negative part and seen from below the bed) and that auto-answer rules run the preset prompt's Transfer and Discard.
It also drives the commands that open a gizmo on a named target (`object_paint`, `object_measure`,
`object_simplify`, `part_boolean`, `object_cut`, `object_lay_on_face`, `object_set_brim_ears`): each opens its gizmo
and the same call again leaves it open, a refused opening leaves the model and the selection as they were, and a
call with acting arguments closes the open gizmo and does the job, read back through `object_list`, the undo log and
a saved and reopened project. It uses the fixtures beside it (`tests/fixtures/star.svg`,
`tests/fixtures/heightmap.png`), and scenarios named on its command line run alone.

`test_printers.py` checks `printer_status` and the job commands with no printer connected, reading `printer_status`
first and calling no job command where a printer is connected, and a printer's extruders on printers it installs:
the Bambu Lab H2D's two, the A1 mini's nozzle card and the Eryone ER20's extruder count.

`test_objects.py` checks the plates and the object commands, each called by index and, where its menu item acts on
the selection, on the selection, in three parts run in one app. The plates: plates added, duplicated, named, locked,
selected and deleted by index and on the current plate, with undo; what a lock keeps Arrange and Auto-Orient from
doing; a plate's own settings through `plate_set_settings`; the filament slots its filament orders count on; and the
plate icons and the plate menu. Where objects stand and how many there are: shapes added at a size, a place, a plate
and a name; instances moved, turned, scaled and sent to another plate, and what Auto Drop does to a move; parts moved
and scaled; the selection; Mirror, Drop to Bed, Center, Scale to Fit and Auto-Orient; the number of instances and Set
as Individual Objects; Arrange, Fill bed and Clone; and instances left out of the print. What objects are made of and
how the list orders them: renaming and deleting, Split and Assemble, parts of every type, Change Type, Order By and an
order given, Fix Model, Reload from disk, Replace 3D file and the keep_painting preference. After each step it reads
the plates and objects back with `plate_list` and `object_list`, through the readers in `checks.py`, and compares
them with the state its entry in `test-entries/objects.md` writes out; a closing scenario saves and reopens one
project holding what each part keeps.

`test_slice_preview.py` checks slicing and what a slice leaves: what `plate_slice` and the Slice and Preview and
Slice All Plates actions slice (no objects, a plate that fails validation, two plates); the statistics of a slice in
one colour and in two; the Preview's tables, legend and layers as `plate_slice_stats`, `view_set`, the Go to layer
action and `app_state` give them, and a G-code file opened for viewing; height range modifiers, variable layer
heights and a plate's layer G-codes, each read in the G-code and kept by a 3MF; every command that takes a plate
index; `view_set`'s display switches, sidebar, legend and G-code window; a legend row clicked, canvas captures right after
a layer or view type change, and `view_set`'s refusals of the Preview's fields; and `app_state`'s account, network plug-in
and configuration folder. It slices about thirty times.

`test_settings.py` checks `settings_list` (each type whole, a saved preset's and a slot's values, what an object and
a region take) and the settings commands' targets: a value set on an object, a part, a modifier and a height range,
by index and from the selection, read back with the value it prints with and where that comes from, shown to reach
the print, and ranges replaced keeping the settings of the one that stays.
`test_sidebar_floating.py` launches its own app twice, the second time with the sidebar floating in the saved window
layout, and checks that `view_set`'s `sidebar_width` is refused there.

`test_recording.py` records while it drives the GUI (clicks on the 3D view, a setting typed, the Preferences dialog
opened and closed, a slice) and reads the recording back from its file. A nested replay starts a recording it never
stops, which the replay's end closes. Short recordings check that a recording has no overlays unless `captions` or
`pointer` turns one on, that one started with a path and no `max_seconds` runs under an hour's limit, and that one
started with `encode` is encoded as a job once `recording_stop` reports it, the encode deleting its `.mkv` unless
`keep_raw_video_file` keeps it. The encodes need an ffmpeg with libx265 and libx264 (`ORCA_FFMPEG`, or `ffmpeg` on
`PATH`); the checks that need one are skipped without it. The recordings are kept in `--out` (a new temporary
folder without it): look at the videos, or at frames `ffmpeg -i session.mkv -vf fps=1 frame%02d.png` extracts, for
the caption, the pointer and the dialog.

`test_filaments.py` assigns filament presets to slots through each route a person or a plugin has: a pick in the
sidebar's selector, its value set with `widget_set_value`, `preset_select` and the plugin API. On one slot it answers
the filament tab's unsaved-changes prompt every way; on three it checks what the Auto-flush preference does to the
flushing volumes when a slot's preset or colour changes. Then it assigns filaments to objects and parts with
`settings_set`, merges one slot into another with `filament_delete`, sets the slot colours and flushing volumes with
`filament_set_colour` and `filament_set_flushing` and the plate type and prime tower with `project_set_settings`, and
checks what reaches the print and what the project file keeps. Last it adds, changes and breaks mixed filaments with
`filament_add_mixed`, `filament_edit_mixed` and `filament_delete`, from their arguments and through their dialogs.

`test_presets.py` checks the presets of the three types: selecting one by the name its selector shows, hidden ones
included; `preset_list`'s filters; what a setting is and where it lives (`settings_get`, `settings_list`); saving,
overwriting, reverting and deleting presets; the unsaved-changes prompt of a preset switch and of a new project,
answered ahead with `preset_changes` or in the dialog; the same selections through the plugin API; a preset kept
inside the project; the Export Preset Bundle dialog and `preset_import` (with `tests/fixtures/mcp_import_pla.json`);
a printer variant picked by its nozzle; a preference changed in the Preferences dialog; and that the presets
selected are what the G-code is sliced with. Then it makes every calibration test with `calibration_generate_test`
and checks the project it makes, the parameters it uses, the presets and objects it changes, the warnings for values
it rounds, and the refusals that leave the project and presets as they were; `preset_changes` "keep" and "discard";
and, in the exported G-code, that the temperature, pressure advance and retraction towers change their value band by
band. It runs in an app and on a data folder earlier tests used, as the sweep runs it: its start installs and
uninstalls again the printers it needs hidden, which reloads the presets and hides a printer an earlier test showed.

`test_operation_routes.py` drives an operation of the operations layer (`docs/HLSD/mcp-server.md`, "The operations
layer") through the routes that reach it, and checks that a command, a Speed Dial action that runs it and the plugin
API's binding refuse the same states with the same code and words, and accept the
same ones: a layer asked of a Preview with none (the Go to layer action and `view_set`), a mixed-colour
filament slot, and a hidden printer selected through the plugin API. It also checks what a command refuses
or does in those states alone: a project saved under a `.gcode.3mf` name, the exports of a project with no
objects, and an arrangement from the assemble view and with a gizmo open. Launch the app from a folder that holds no `.py` files, since the app's working directory is on Python's module path and a file there can
shadow a standard module that `script_run` needs.

The Design-tab replays drive the Design commands through `tool_call` and need an instance launched with
`--cad`; `test_cad_replays.py` runs each with `replay_run` from a new project. Each checks what it builds by volume and by what the reading
commands give back, under each reply's `result`; steps
marked `optional` are calls that must be refused and change nothing.

- `tests/cad_sketch_to_solid.json` takes sketches to solids: an L-bracket profile on XZ through the
  modeling origin with its inside corner filleted in the sketch (after an empty commit that must
  fail and leave the sketch open), a shelf-pin jig plate whose five holes are in its sketch, a lip
  sketched on the plate's top face and joined to it, one loop of the plate's sketch extruded alone,
  and a ring revolved about its sketch's centerline.
- `tests/cad_sketch_edit.json` drives the sketch tools: the bench's hex standoff from a polygon 8 mm
  across the flats, a trim and an extend on crossing lines, and a plate with a chamfered and a
  rounded corner, two slots (one an array copy turned upright), an elliptic hole and four holes
  arrayed about a point, constrained from picks and from named points, with a refused constraint
  rolled back and one deleted.
- `tests/cad_topology_targets.json` builds the bench's J wall hook from what the reading commands give back:
  its inside corners found by convexity, M4 countersinks seated on the front face on the axes of
  pilot holes, the cones' kind, apex side and radii from `body_describe`, the hole heights and the
  plate's thickness from `measure`, a datum plane midway through the plate and one 70 mm up with a
  sketch on it, a refused tangent plane, and the back face pushed out 2 mm.
- `tests/cad_features.json` threads, sweeps, lofts and splits: the bench's M5 hex standoff tapped on
  its bore (M5 inferred from the bore) and again on the XY plane, each within the bench's volume
  bounds; a spring swept along a helix in round and square wire; a square-to-circle loft between XY
  and a datum plane 40 mm up; the loft split by that datum and its lower half cut again by XY turned
  over.
- `tests/cad_doc.json` reads and edits features as their cards do: a variable bound to an extrude,
  then removed, and literal edits with `feature_edit`. Then it commits the design to the plate,
  recommits over the first object and undoes that in one step, exports STEP and reads it back into a
  new design, and commits two bodies both ways. Last it walks the Design history (`undo` / `redo` of
  an edit and of a rename), suppresses, moves and deletes features, names, hides and colours bodies,
  commits only the shown one, and saves and reopens the project to check that a body's name and
  colour were kept.
- `tests/cad_canvas.json` drives the Design view as a canvas: `view_set` shows the Design tab and
  points its camera (and refuses Prepare's and Preview's fields there), canvas and widget
  screenshots capture the view, a click at its centre picks the block and a right click lists the
  verbs for the pick, and `keyboard_press` reaches the Design panel's key hook (Shift+E opens the Extrude
  card, Escape with no target closes it). Last, a change made only in the Design tab marks the
  project modified, and reopening the saved design-only project with `discard_changes` asks nothing
  and names the project after its file.

`test_cad_replays.py` runs on an instance launched with `--cad`, in parts, and before each part closes what the one
before left open, calls `start()` and sets the Design view's switches: the six replays above, then `round_trip`, `slow_build` and `examples`, or the
parts named on its command line. `round_trip` makes calls as a client, so they are journaled, exports the session
with `replay_export`, replays it into a new project and compares the body's mass properties and the features. The
session has an empty sketch commit, a missed hole and a fillet the kernel refuses, which must fail and be left out, a
shell thicker than the plate answering ok false, which stays in, and face and edge ids passed with their
`generation`, which the journal drops. `slow_build` runs a long thread build (an M8 thread tapped 250 mm deep, which
takes several seconds) and checks that while it builds `app_state` answers, a window screenshot succeeds,
`widget_list` lists the progress dialog and a screenshot of that dialog captures it, and another Design command is
refused. `examples` runs the examples `tool_describe {tool}` gives for every Design command, and for `undo` and
`redo`, each on a fresh design, and fails one whose call fails or whose result has `ok` false, an error, or is the
person's form.
`test_cad_failed_job.py`, on a `--cad` instance too, sweeps a coil whose every rebuild outlasts the reply budget (its
turns sized from the session's first build), then a hole that misses it replies with a running job and fails after
the reply budget: an export while it runs leaves it out and names it in `left_out_running`, the
export after it has the rename made next and not the hole, and that replay runs to the same coil.
It takes four to eight minutes, or under a minute with the reply budget capped at 4 s by
`ORCA_MCP_REPLY_BUDGET_MS=4000` for the app and the test, as the sweep runs it.

`test_cad_commands.py` checks what the Design commands' arguments do where the Design panel's own functions
answer, which the unit tests, on a bare document, do not reach. Each rung starts from an empty design, calls
commands with their arguments and grades what `describe_design`, `feature_describe`, `sketch_describe` and
`body_mass_properties` read back: the `operation` and `type` words and the suppressed feature; each of six
shapes drawn once with its tool on the canvas and once with `sketch_draw`, which must agree in entities,
constraints, loops and degrees of freedom; `sketch_edit` and its replace, cancel and refusals;
`sketch_project` with and without an open sketch, `sketch_text`, `sketch_insert_svg` and Scale artwork;
`feature_edit`'s `refs` and a card's picks while it edits a feature; `view_set`'s Design options;
`select_geometry`, `confirm_tool`, `cancel_tool` and `body_place`; the Boolean card a call opens; and a
pick of a folded item's action from the Speed Dial, and its replay. Its docstring lists the rungs.

Two more harness tests on a `--cad` instance drive the Design tab the way a person does. Every action goes
through the UI tools (`view_set`, `widget_click`, `mouse_gesture`, `keyboard_press`, `widget_set_value`,
`dialog_answer` and `dialog_auto_answer`), and the Design commands only read the result back (`sketch_describe`,
`describe_design`, `feature_describe`), save for one call in the scale rung; the right-click menu's layout they
grade against is read from `scripts/CAD/tool_atlas.json`, the Design table the app's own is generated from,
for the selection `describe_design` names. Buttons and rows are found by label, and canvas positions come from
a calibration that maps the open sketch's plane to canvas pixels, so nothing in them is tuned to a window
size, screen scaling or theme, though the sketching test's relations rung needs a canvas of about 940 × 600 click
pixels and stops when it is smaller. A map fitted from five probe points is reused, once one probe confirms it,
while the canvas size and the camera stay the ones it was fitted on. The sketch tool's pick budgets and badge layout are in the canvas's GL pixels,
which `cad_gui.py` turns into click pixels by the canvas scale that `view_set`'s `camera_state` gives (`zoom`
over `click_px_per_mm`). What they share is in `cad_gui.py` beside them. `test_cad_sketching.py` draws each
sketch tool's primitive with clicks on the canvas and types its exact values into the canvas's value field,
drives the edit operations, presses each constraint button on shapes drawn for it in one sketch, which it commits
and opens again to read every relation back, undoes inside a sketch and across its commit, saves the project and opens it again through the File menu, and draws on top of a sketch holding 1204
entities. That last rung, `scale`, loads them with one `sketch_draw` command while the rectangle tool is armed:
the one call in these tests that changes the sketch outside the UI, on purpose, since a bulk add made while a
creation tool is armed must not be read as a drawn gesture that swallows the next key and click.
`test_cad_context_menu.py` checks that the right-click offer names what was clicked and lists exactly the
verbs the Design table says apply to it, and drives from the menu every sketch verb that has no key, a Text
typed into the Text dialog and an SVG import (`tests/fixtures/rect_40x20.svg`) among them, and checks that the
value fields a drawing opens take digits typed with no click into them and commit on Return: the rounded
rectangle's three, and a rectangle's width label clicked in Select.

```bash
tools/mcp/orca_mcp.py launch --cad
tools/mcp/test_cad_sketching.py
tools/mcp/test_cad_context_menu.py
tools/mcp/test_cad_context_menu.py curves label
tools/mcp/test_cad_commands.py words select
```

Each prints `ok` or `FAIL` for every check and exits 1 if any failed. Rungs named on the command line
run alone, in that order (each test's docstring lists them); with none named, every rung runs.
`test_cad_sketching.py selfcheck-recovery` runs only when named: it fails on purpose with the mouse
button held, the offer asked for and a tool armed, to show that the rungs after a failed one start
clean. The tests find widgets, menu paths and verbs by their English names, so they need an English
UI: on any other language, or with the Design tab off, they exit 2 before the first check and say how
to fix it. The app logs each picker a rule
answers in `<datadir>/log/debug_*.log.0`, with the reason when it cancels one a rule gave a path. MCP
input reaches the app through wxWidgets, not the OS, so a focus problem that only a window manager
causes is out of the tests' reach.

`test_gui_tools.py` drives the generic GUI tools on the app's own widgets: context menus and a button's menu listed
in the reply, or an item of them run with `popup_item`, none ever shown, so on macOS the run finishes with nobody
pressing Escape; the Edit menu's Undo and Redo by the paths `Edit>Undo` and `Edit>Redo`, which name them on every
platform; the rows of the object list, Compare presets and the parameter table; the print button's popup and a
drop-down list; and Orca's web dialogs. Each item, row or page element it runs is checked by what it does to the
project, the selection, the camera or the settings. On Linux run it with `--headless`, or with `--xvfb` where Openbox
is installed: under Xvfb with no window manager those popups close as soon as they open.

Web pages are driven like widgets: `widget_list` lists each shown page's elements under its web view
with `d` refs, and `widget_click`, `widget_set_value`, `keyboard_press`, `mouse_gesture` and `dialog_answer` act on them, by
ref or by `{web: "Next", kind, css, within, index}`. `test_gui_tools.py` drives Orca's own web
dialogs that way alone: the preset bundle dialog, the colour picker behind a filament swatch (its hex
field, and a drag on its spectrum) and the setup wizard's first steps and its pages' navigation, and the web view a script runs in while those
dialogs (and the Flushing volumes dialog) are open. `test_web_ui.py` covers every
kind of page control, action and what it changed on its page, refusal, locator, timeout and close on
the pages of a fixture plugin, in a modal dialog, a modeless window, a dock panel and the Plugins
dialog's configuration frame, a token in a page's URL shown redacted, and the permission prompt the
plugin's thread opens, answered with `dialog_answer` and with a rule registered while it is open, and the
plugin's page among the main window's tabs, which the Go to tab action shows by its id.
Side-load the plugin into the datadir first:

```bash
tools/mcp/test_web_ui.py --install /tmp/orca-mcp-test
tools/mcp/orca_mcp.py launch --datadir /tmp/orca-mcp-test
tools/mcp/test_web_ui.py
```

`test_set_window.py` checks `window_list` and `window_set`: the main window listed first, the Plugins window (which
doesn't block the app) by a ref that `window_screenshot` and `widget_list` read alone and `keyboard_press` Escape
closes, Preferences last as a modal dialog, Action Replay closed by Escape, and `window_set` on the main window (sized
twice, moved, placed over its
display's work area, maximized, minimized and restored), on the Plugins window by its ref and on
Preferences by its `{label}` locator. On Wayland it checks
that a window screenshot is the size the reply gives and skips positions, which Wayland neither reports
nor takes. On X11 it reads the X server's stacking order through libX11 to check that the main window,
restored and raised while Preferences is open, stays under it.

`test_speed_dial_open.py` goes from Home to Prepare, the Design tab (launched with `--cad`) and Preview by
their tabs, and checks that the page's 3D view then holds the keyboard focus and that Space opens the Speed
Dial, which Escape closes. A pointer entering a view also gives it the focus, so the tab switch itself is
proved only where no pointer rests on the view: on a native Wayland display, or a desktop with the pointer
elsewhere. Where KWin runs the display, it also checks through KWin's D-Bus that KWin draws no title bar on
the dial.

`test_set_view_camera.py`, on an instance launched with `--cad`, checks `view_set`'s `look_at` and `zoom`
on Prepare, Preview and the Design tab, and `orbit` on Prepare and the Design tab, against the reply's
`camera_state` and `app_state.camera`, including straight after `project_new`, whose fit runs at the
next frame, and a fit on Prepare after the window was sized on the Home tab.

`test_call_forms.py`, on an instance launched with `--cad`, calls commands the way agents often write them: a lone
path for `paths`, `object` for `objects` with the index as a string, an action by its short name, `action` for
`id`, a camera direction as `view_set`'s `canvas`, which is read as its `camera`, `key` for `keys`, `app_wait` with
no condition, a gizmo by its toolbar title, `plate` as a string, legend rows in another case or with `_` for a space,
and `project_export` with the format taken from the path, each shown by what the call did. It also checks that a
type's whole `settings_list` listing leaves meta out.

`test_stdio_bridge.py [--bin PATH]` checks `orca-slicer --mcp-stdio` against the launched instance:
the handshake, notifications, tool calls in both protocol eras, and errors. With no instance
answering, it checks that the bridge answers the handshake and lists the tools of the app it would start
without starting OrcaSlicer, and that a tool call then starts OrcaSlicer on the bridge's port (the
test stops that app).
`test_runtime_calls.py` checks how the runtime takes calls, in five parts on one app: `dialog_answer` still closes
a dialog while 32 blocked `app_wait` calls and 32 idle or half-sent connections fill the server; a slice cancelled
while it waits for the update a setting change scheduled never starts; the Run Script panel stays out of the GUI
tools' reach while `replay_run` still runs `smoke_test.json` and a nested inline replay; ImGui refs still find their
items after the items' labels change, and the exported session replays; and a long arrange started three ways
refuses changes, canvas keys and toolbar clicks, answers reads, and stops on `job_cancel`.
`test_export_running_job.py` exports while a call's job runs past
its reply budget (left out, named in `left_out_running`), after it ends (an `await: false` step that
replays) and while a dialog holds its call (kept); it takes about a minute, or about 40 s with the reply budget
capped at 20 s by `ORCA_MCP_REPLY_BUDGET_MS=20000` for the app, as the sweep runs it.
`test_send_gcode.py` starts an OctoPrint-compatible stub host, gives the Klipper printer `start()` selects an OctoPrint
print host with the printer agents off, as the baseline leaves them, and sends a sliced cube to the stub with
`printer_send`'s Speed Dial action, `send_gcode`, the Send dialog answered with `widget_set_value` and `dialog_answer`.
The sends and `printer_test_host` are refused with no print host address; the host test answers through the stub and
fails on a port nothing listens on; `print_start` is refused before slicing, Print all and Send all are refused as the
Print button offers neither, and once sliced `print_start` and `printer_send {plates: 0}` open the upload dialog, which
the test cancels. An upload the stub accepts reads uploading at 99 until the stub answers and then completed in
`app_state.uploads`, its exported replay uploads again, and one the stub refuses with HTTP 500 never reads completed
and ends as an error whose dialog `dialog_answer` closes.

`test_call_forms.py` and `test_person_forms.py` check a call with exactly an action's arguments, which may ask
as a person's pick of the action does: `calibration_generate_test {test}` asks in the test's dialog and with
`silent` runs with its defaults, View: Top turns the camera of the view shown, Deselect All closes the open gizmo
too, Go to layer takes its value through `action_run`, and `plate_slice {preview: true}` slices and shows the
Preview tab (`test_call_forms.py`); `project_save {save_as: true}` and `project_export {format: gcode}` open their file
pickers while a path writes without one, `preset_export` always opens its dialog, `object_arrange {}` and
`object_select {all: true}` (and `action_run` of their actions) reply as their commands do, the selection in the
assemble view's too, and the Speed Dial's own picks of a view action, Deselect All, Go to layer, Auto-Arrange and
Select All Objects run in place (the script). `test_project_open_settings.py`
launches its own app, whose Preferences load a project's geometry only, and checks that `project_open` loads
the settings a project was saved with all the same.

The commands that do a menu item's job take their targets by index, and `test_objects.py` calls them so and reads
each back. `test_menu_items_refusals.py`
checks that each object command made from menu items refuses with its own reason, and `test_additions_refusals.py`
the filament, preset, export and import commands'. `test_files.py` checks the project's files: a new project and its
question, opening, saving, importing models and every export read back from its file. `test_preferences.py` checks
`preferences_get` and `preferences_set` against the Preferences dialog, tab by tab. Some launch their own app,
on a fresh data folder and printers the sweep does not seed: `test_install_printers.py` and
`test_install_filaments.py` (installs and uninstalls, read back after a restart), `test_plugin_commands.py`
(the plugin commands on the fixture plugins of `tests/plugins`, against the Plugins window)
and `test_sidebar_floating.py` (`view_set`'s
`sidebar_width` refused while the sidebar floats).

`test_speed_dial.py` checks that `action_list` lists each of the 130 Speed Dial actions of
`tests/data/speed_dial_actions.json` with the command `tests/data/speed_dial_action_calls.json` gives it, and runs
seven of them through `action_run`, one for each way an action runs (a calibration dialog, a handy model, a window,
the rear view, Go to tab with its value, a screenshot and a refused Sync Presets), each from a state set up afresh,
reading back its effect, the reply's `ran` and the step `replay_export` keeps.

## The sweep

`run_sweep.py` runs every test above, the `test_*.py` files, on several jobs at
once: `-j N` (or `--jobs N`) runs N jobs, as a build's `-j` does, and defaults to half the cores, up to six. It
exits 0 when all of them pass and no OrcaSlicer crash report appeared (macOS). `sweep.json` lists each test with
what it needs, and a test file it does not list stops the run before anything starts, so a new test is never
skipped by accident.

A job keeps one datadir and one app for all its tests, `job-<n>/datadir` under `--out` (by default a new
`orca-sweep-<time>` folder in the temporary folder), as you keep the OrcaSlicer you opened that morning: the first
launch seeds the datadir, and the app, launched with `--cad` and `--unrestricted-scripts`, runs the job's tests one
after another, each finding the app and the datadir as the tests before left them, so each test sets the start it
needs (see `TESTS.md`, "Sharing an app and a data folder"). Before each test the runner checks that the app answers
an MCP call, and launches it again on the datadir when it does not (after a crash, or a test that closed it); when
`sweep.json` marks the test `fresh`; when the test has a `setup` step, which runs with no app on the datadir; and
when the test's `env` differs from the one the app was launched with. A test that runs past its time limit has the
app stopped, so the next test gets a new one, and a job's app is stopped when the job has no tests left. Before a
launch the runner waits for the last app's processes, its WebView2 ones included, to end. The app's session file
(`ORCA_MCP_SESSION`) is `job-<n>/session.json`, and each launch serves on a port of its own (from 14100 up, or from
`--first-port`, so that two sweeps can run side by side). Each test also runs in a folder of its own under `--out`,
with its own temporary folder (`TMPDIR`, `TMP`, `TEMP`) and empty working folder, and leaves `test.log` there. The
job's app runs in `job-<n>/app-cwd`, with `job-<n>/app-tmp` as its temporary folder, since it outlives the test that
launched it, whose folder goes when that test passes. The longest tests start first, by each one's last passing
time in `--history` (`~/.cache/orca-mcp/sweep-times.json`), and tests that share a lock never run at the same time.
`--shuffle SEED` orders them at random from `SEED` instead and names the seed on the first line, so that an order
that fails can be run again with the same seed; `--shuffle` alone picks a seed.

The serial tests, the ones `sweep.json` marks `exclusive`, run after the rest, one at a time with nothing else
running, on a job of their own with an app of its own. A test that fails while others run runs again in that phase,
alone, in a new app on the datadir it failed on: one that passes then counts as passed and shows as `passed alone`.
A test that runs out of time is interrupted, as Ctrl-C would, so its own cleanup closes the apps it started (on
Windows its process tree is ended), and a test the runner could not run is recorded as an error, so a run that
leaves a test without a result never exits 0.

A passing run's folder is deleted as soon as it passes. A failed run's folder is kept, with a copy of the app's
log from the job's datadir, also when the test then passes alone, so the log says why it failed beside the others;
the datadir of a job one of whose tests failed is kept too, the other datadirs are deleted at the end, and
`--keep` keeps everything. `results.jsonl` in `--out` records every run with its job, its phase (`pool`, `serial` or
`alone`), the tests that ran before it on that job's datadir, so a failure can be traced to what they left, and
`launch_seconds` when the run launched the job's app. `--only` and `--skip` pick tests by name, and `--list` prints
the order, the serial phase with why each test is in it, and exits; with `--shuffle` it names the seed first.

On Linux the runner opens each app on your desktop as an X11 client (`GDK_BACKEND=x11`), so a sweep
leaves its windows on your screen while it runs, and a window manager un-minimizes a window on request
and reports its position. `--wayland` runs them as native Wayland clients instead, as a Wayland session
does: the compositor then owns positions, minimization and activation, so a check that needs a window
moved, or a minimized one brought back, is reported SKIP and does not fail its test. `--headless` runs
them on a private compositor instead, as `orca_mcp.py launch --headless` does, which is what a machine
without a desktop needs. `--xvfb` runs each job's app on a private Xvfb display of its own, with
Openbox as its window manager, as `orca_mcp.py launch --xvfb` does. OpenGL is drawn in software, so
`--headless` is the better pick wherever KWin is installed; without Openbox, the tests that open a
popup list, `test_gui_tools.py` and `test_filaments.py`, fail under `--xvfb`. With either `--headless`
or `--xvfb`, a test that starts its own apps (`launch: own`) gets a private display of its own for the
length of the test, which those apps inherit whether they start with `orca_mcp.py launch` or run the
binary, so no window opens on your desktop. Each flag is Linux-only, and only one of the three can be
given. Without any of them the run needs an X display, and `--wayland` needs a Wayland session;
`--list` needs none.

CI runs the sweep on every Build all run (`.github/workflows/mcp_sweep.yml`): it downloads the run's
x86_64 Linux AppImage, extracts it, and runs `run_sweep.py --xvfb --bin squashfs-root/AppRun` with
Openbox and ffmpeg installed. A failed run uploads its `--out` folder as the `mcp-sweep-linux-x86_64`
artifact.

An entry's keys:

- `path`: the test, relative to `tools/mcp`.
- `launch`: `app` (the default; the test runs in the job's app), `fresh` (the runner launches a new app for the
  test, which the tests after it then run in), `own` (the test starts its own apps on datadirs of its own, and
  the job's app is left as it is) or `none` (no app).
- `setup`: arguments for a run of the test's own file before the test; for a test in the job's app it runs with
  the app stopped, and a new app is launched after it. `args`: the test's arguments. Both fill in `{bin}` and
  `{datadir}`.
- `env`: variables for the test and, for a test in the job's app, for the launch: a test whose `env` differs from
  the one the job's app was launched with gets a new app launched with it.
- `exclusive`, `locks`, and `why`, which each of them needs: what the test shares with others. An exclusive test
  runs in the serial phase.
- `timeout_s`: how long the test may run (900 by default).

`test_run_sweep.py` checks the runner without the app, on a stand-in sweep of scripts that launch nothing and one
of scripts that run in a stand-in app, which shows when the runner launches and stops a job's app, and that
`sweep.json` lists every test there is.

`TESTS.md` holds the rules the sweep's tests follow and the index of all of them, and `test-entries/` an entry for
each test, by area: what it tests, how it decides pass or fail, and what it leaves unchecked. A new test starts as
an entry there, reviewed before the test is written.
