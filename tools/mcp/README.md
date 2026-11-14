# MCP harness

`orca_mcp.py` launches OrcaSlicer with its MCP server and talks to it, from a shell or from
Python. It uses only the standard library. `docs/HLSD/mcp-server.md` covers the server.

## Commands

| Command | What it does |
|---|---|
| `launch [--headless [kwin\|mutter] \| --xvfb] [--cad] [--unrestricted-scripts] [--printer "<model>@<nozzle>"] [--vendor V] [--filament F] [--datadir DIR] [--port N] [--bin PATH] [--print-config] [-- app args]` | Start OrcaSlicer with `--mcp`, wait until it is ready, print the session |
| `call TOOL ['<json args>'] [--wait]` | Call a tool and print its JSON result; `--wait` follows a returned job to its result |
| `tools` | List the tools, with the first line of each description |
| `widgets ['<json args>']` | Print the `list_widgets` tree |
| `screenshot OUT [--target main\|canvas\|dialog]` | Save a full-size PNG, replacing `OUT` if it exists |
| `run-replay FILE [--out DIR]` | Run a replay in the launched instance and print the result; exit 0 when it passes |
| `stop` | Stop the app and its private display, and remove the session |

`launch` creates a fresh temporary datadir unless you pass `--datadir`. It seeds the datadir with
an `OrcaSlicer.conf` that has a finished setup wizard and one printer and filament (Bambu Lab X1
Carbon 0.4 by default), with update checks and daily tips turned off, and with the Orca Cloud login kept
in a token file in the datadir (`use_encrypted_token_file`) instead of the system's secret store, so the
instance starts signed out and never reads your login: on macOS that read would stop a newly built app at
a keychain prompt. `--cad` turns the Design
(CAD) tab on, and with it its commands: it sets `enable_cad_feature` in a fresh datadir's
`OrcaSlicer.conf`, and in a reused one too, since the app creates the Design tab and its commands only at
startup, when that setting is on.
`--unrestricted-scripts` lets Python scripts (`run_script`) run unrestricted the same way, by setting
`mcp_python_unrestricted`, since no tool can turn that on. The token comes from
`ORCA_MCP_TOKEN` or is generated, and it reaches the app through the environment, not the command
line. On Linux, `launch` also sets `SSL_CERT_FILE`, so a fresh datadir doesn't prompt for a CA
bundle. On Windows, it gives the instance its own WebView2 profile in the datadir, apart from the one
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
starts Xvfb on a free display between `:90` and `:119`, which draws OpenGL in software and runs no
window manager, so a popup list, such as a combo box's drop-down or the Print button's menu, closes
as soon as it opens. Xvfb runs with `-noreset`, and its pointer is parked in the bottom-right
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
orca.call("open_files", paths=["/abs/path/part.stl"])
result = orca.await_job(orca.call("slice"))          # follows a job envelope to the end
orca.call("wait", **{"for": "idle"})                 # "for" is a Python keyword
info, pngs = orca.images("screenshot", target="canvas")
```

`call()` returns the tool's JSON result and raises `McpError` when the tool reports an error.
`call_with_text()` also returns the text blocks after the JSON one, such as `list_widgets`'s tree.
`images()` also returns the PNG bytes of the image blocks. `call_raw()` returns the MCP result
unchanged.

To drive an OrcaSlicer that is already running with the MCP server on (`--mcp`, or "MCP server"
under AI features in Preferences), attach to it through its datadir instead of a port and token:

```python
orca = OrcaMcp.from_datadir()                        # the datadir the app uses without --datadir
orca = OrcaMcp.from_datadir("/abs/datadir")          # an app started with --datadir /abs/datadir
scene = orca.call_command("describe_scene")["result"]   # a Design command (the Design tab on)
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

`call_command(tool, **args)` calls `call_tool {tool, args}`, follows a running job to its end and
returns the command's whole reply, which for a Design command is `{"result": ...}`. A call whose first
reply says a dialog blocks it fails at once, naming the dialog, rather than waiting on it. A call that
first replied `running` and opens a dialog later is waited on up to the timeout (600 s), since `wait`
reports only that its job is still running. Every `McpError` it raises starts with the command's name
(`"extrude: ..."`) and has the original error as its cause. `search_tools {group: "Design"}` names the
Design commands, and `search_tools {tool}` gives one's arguments and examples.

## The skill's list of commands

`gen_skill_reference.py <skills-checkout> [--check]` writes
`skills/orcaslicer-mcp-skills/references/actions.md` in a checkout of the orcaslicer-mcp-skills repository:
the Speed Dial's commands section by section, each with its key, title, arguments and description, from
the command reference `tests/data/command_catalog.json` (`docs/HLSD/mcp-server.md`, "Making a feature
reachable"), and before them what the reference says of particular commands: those with one form, which
needs its value, and the typed tools that are tiles too. It needs no running app. With `--check` it writes
nothing, prints the difference and exits 1 when the file is not what it would write. When a command
changed, regenerate the reference first: run `slic3rutils_tests "[Reference]"` with
`ORCA_UPDATE_COMMAND_REFERENCE=1` in the environment.

## Smoke test

`smoke_test.json` is an end-to-end replay that exercises the main paths against a fresh datadir:

- an auto-answered startup or unsaved-changes prompt;
- loading, a setting change, slicing and a G-code export;
- a dialog that an action opens and the replay answers;
- screenshots of the dialog and the preview.

```bash
tools/mcp/orca_mcp.py launch --headless
tools/mcp/orca_mcp.py run-replay tools/mcp/smoke_test.json --out /tmp/orca-smoke
tools/mcp/orca_mcp.py stop
```

Without the harness, `orca-slicer --datadir DIR --mcp-run tools/mcp/smoke_test.json
--mcp-run-out /tmp/orca-smoke` runs the replay and exits. On Linux, set `SSL_CERT_FILE` (for
example to `/etc/ssl/certs/ca-certificates.crt`) as the harness does; otherwise a prompt for the CA
bundle blocks startup before the replay can run. The exit code is 0 when it passes, 1
when a step fails and 2 when the replay is invalid. The run leaves `result.json` and the
screenshots in the output directory. `tests/replay_basic.json` is the smallest such check: load,
change a setting, slice and capture.

`test_plugin_tools.py` checks a plugin's calls into the tools in the running app: it side-loads
`tests/plugins/mcp_tools_fixture` into a fresh data folder and launches twice: over MCP, with the Design tab
on (its operation is reached through `search_tools` and `call_tool` and stays out of `tools/list`, a Speed
Dial command is found among `search_tools`' commands with its schema and not again among its actions, a
Design command is found there too, in no section of its own, calls from its Script worker
and a thread it starts reach the tools, an auto-answer rule it sets is the plugin's, `export_replay` is
refused, nothing it calls is journaled, a script that finds its progress dialog handle can use it,
`tests/plugins/mcp_tools_broken`, whose load fails, offers none of its operations, and unticking the
plugin in the Plugins dialog withdraws its operation and ends its rule); and without MCP,
where a `--mcp-run` runs the plugin's script and its rule is still the plugin's. Run it as
`python tools/mcp/test_plugin_tools.py`. The
Run Script panel's `.py` files are not driven from here: the panel is hidden from automation and opens its
files through a file picker, so how it reads and saves them is covered by the unit tests of
`decode_python_source()` and `encode_python_source()`.

A `.py` file given to `--mcp-run` runs as a Python script (`run_script`) instead, exiting 0 when it
finishes and 1 when it raises, is cancelled or runs out of time; its traceback goes to the log. A
script's own file access is held to OrcaSlicer's folders, and it starts no programs or threads and reaches
no network, until the setting lets scripts run unrestricted: set `"mcp_python_unrestricted": "true"` in the
`app` section of the datadir's `OrcaSlicer.conf` before the launch, as
`orca_mcp.py launch --unrestricted-scripts` does, since no tool can turn it on.

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
    worker = threading.Thread(target=lambda: print(orca.host.tools.call("get_app_state")["ready"]))
    worker.start()                               # waited for before the run ends

if __name__ == "__main__":
    main()
```

A window that stays open is closed with the run, so a script that wants the person to work in it waits, as
the `pages.get()` above does. While a modal it opened is up, tool calls that change the project are refused,
so interactive scripts use modeless windows. `docs/HLSD/mcp-server.md` ("Python scripts") has the rules for
overlapping runs, imports, pools, `atexit` and Stop.

`tests/gizmos.json` drives every gizmo through the GUI tools, the way a person does: each one's icon,
its panel's fields, combos, toggles and links, and clicks or strokes at points of the model, then
checks what it left on the model (`list_objects`), with an undo and redo and a canvas screenshot per
gizmo. It uses the fixtures beside it (`tests/fixtures/star.svg`, `tests/fixtures/heightmap.png`).
Run it at a second window size through `run_replay`'s `vars`:

```bash
tools/mcp/orca_mcp.py call run_replay '{"path": "/abs/tools/mcp/tests/gizmos.json", "out_dir": "/tmp/gizmos-small", "vars": {"window_size": [1100, 760]}}' --wait
```

`tests/scene.json` checks object structure and placement: `transform` on an instance (the drop after a
scale, `size`, a move to another plate), on a part, what `list_objects` reports of them (bounding boxes
on the bed, filament, the scaled volume, a mirrored part and instance), the selection as `select_objects` and
`get_app_state` report it (parts, whole instances, one instance of two), the shape commands given a size
(`run_action` with `args` `{"size": [...]}`), `arrange` on one plate and every plate, Fill bed, the Speed
Dial no-op that `run_action` reports as an error, `add_part` (shapes at a size and a position, a model
file, one undo step, and what it refuses), and the object structure commands (Change Type, Rename,
number of instances, individual objects, Assemble, Order By and its reversal, and what each refuses). It
ends with all of them on one scene: a block with a magnet pocket and a boss, sized, moved to a second
plate, copied six times and arranged there, split into objects, one mirrored, two assembled, ordered,
the plate filled and sliced.

`tests/layers.json` checks height range modifiers: `set_height_ranges` with ranges given out of order,
their read-back in `list_objects`, one undo step (and none for the same ranges again), the layer count
of a slice with them, the refusals (an overlap, a layer height beyond the printer's, a missing filament,
an object setting, an empty range), a range in the second filament, a 3MF round trip, and `[]`. Then
variable layer height profiles on a Benchy with `set_variable_layer_height`: Adaptive called from the
Preview tab, its quality's effect on the point count, the layer count of a slice with it, Smooth as one
undo step, Reset and a Reset that changes nothing, the refusals (an argument of another action, the
assemble view), Smooth and Adaptive over height ranges, and a 3MF round trip. Last, layer G-codes on a
cube with `set_custom_gcode_at_layer`: refused before a slice, a pause by layer and custom G-code by height called
from Prepare (it shows the Preview), the same code again changing nothing, a removal and an in-place
custom edit in one call, the refusals (a layer above the top, a code where another is, nothing to remove,
a missing filament, a missing plate, a plate changed since its slice that the Preview does not show), a
filament change, a 3MF round trip, and `clear`.

`tests/recording.json` records while it drives the GUI: clicks on the 3D view, a setting typed, the
Preferences dialog opened and closed, a slice. It stops the recording (frames, length, size) and encodes
it to H.265, then has a nested replay start a recording of the 3D view that it never stops, checks the
replay's end closed it, and encodes that to H.264. Short recordings check that a recording has no overlays
unless `captions` or `pointer` turns one on, that one started with a path and no `max_seconds` runs under an
hour's limit, and that one started with `encode` is encoded as a job once `stop_recording` reports it. Its
`new_project` and `stop_recording` calls pass `silent: true`: with no arguments they would do what a
person's pick does, asking and stopping without a report. The encodes need an ffmpeg with libx265 and libx264
(`ORCA_FFMPEG`, or `ffmpeg` on `PATH`) and are optional without one. Look at the videos, or at frames
`ffmpeg -i session.mkv -vf fps=1 frame%02d.png` extracts, for the caption, the pointer and the dialog.

`tests/multicolor.json` assigns filaments to objects and parts through `set_settings`' object and
part scopes, with undo and redo, and reads them back with `get_settings`' effective values. It then
sets the project's slot colours, flushing multiplier, one flushing volume and plate type through the
project scope (in one call, the colour first), checks the refusals that leave everything as it was
(the read-only `filament_is_mixed`, a bad colour, a purge mode the printer has no button for, a non-zero
diagonal, a tower off the plate), and moves the prime tower with an undo, before slicing.

`tests/presets.json` saves filament, process and printer presets with `save_preset` (a user preset,
one inside the project that a saved and reopened project keeps, a detached one, and the overwrite
rule), and drops unsaved edits with `set_settings {"reset_all": true}`. It selects a hidden filament,
one the setup wizard has not added, by name. It reads presets that are not selected and compares presets (`get_settings` `preset` and `compare`), writes one entry of a
per-variant value, and checks `set_settings`' `changed` after the spiral vase dialog. It reads what
settings do from `get_settings`' meta (`description`, `where`): by key, for one group, and found by
the words of their descriptions, in one scope and in all three. Keys named without a scope are read
from the scope that has each one, and a key named in the wrong scope is listed with the scope that has
it (`scope_of`) and refused by `set_settings`. It reverts single keys (`null`, `reset_to`), and answers the unsaved-changes prompts:
`select_preset` without `preset_changes` still opens the prompt, `preset_changes` transfers or discards without it,
and `new_project` keeps modified presets unless `preset_changes` is `"discard"`. It ends by deleting the
presets it made with `delete_preset`: filaments the tab edits and does not, a base process and its
child, and a printer once selected, with the refusals for system presets, a preset another inherits
from, unsaved changes and a printer that is not selected. It exports a filament with the Export
Preset Bundle dialog to its `out_dir` (`export_config` with a directory), imports the archive again
with `import_configs` and a path (kept while the preset exists, imported once it is deleted) and
imports `tests/fixtures/mcp_import_pla.json`. It changes a preference in the Preferences dialog
opened with `open_preferences` `behaviour:auto_arrange`, by the control's name, and reads it back with
`get_settings {"scope": "preferences"}`. It starts from the harness's default printer with one
filament slot in Simple mode, deleting slots and selecting the mode first, and reruns on the same
datadir.

`tests/plates.json` sets a plate type and print sequence on one plate only (with undo and redo), the
first and other layers' filament orders, spiral vase, the lock (twice, which leaves it locked) and a
name through `set_settings`' plate scope, and reads them back with `list_plates` and `get_settings`.
It brings the filament list to two slots with the `filament_add` and `filament_delete` commands first,
and ends with a mixed filament, whose slot the delete command refuses and ahead of which a new slot
goes. It reruns on the same datadir.

`tests/calibration.json` makes every calibration test with `generate_calibration_test` (each pressure advance method,
a 2 x 2 pattern batch, the four flow ratio passes) and checks the objects it added, the parameters
it used and the presets it changed, slicing some of them. It checks the temperature rounding, the
refusals that leave the project as it was (modified presets without `preset_changes`, an argument the test
does not take), `preset_changes: "keep"` keeping an edit, and `new_project` dropping the test's settings. It
starts from the harness's default printer and reruns on the same datadir.

`tests/ops_*.json` drive each operation of the operations layer (`docs/HLSD/mcp-server.md`, "The
operations layer") through every route that reaches it: the Speed Dial command run with `run_action`,
the typed tool and, for presets, a pick in the sidebar and the plugin API called from a `run_script`
step. One replay per operation, named after it: G-code, sliced 3MF, model 3MF and STL exports, a model
import, a project opened, started anew and saved, a plate selected and locked, arrange, slice, the
Preview's layer range, a printer or process preset selected (`ops_select_preset`), a filament assigned
to a slot on a printer with one slot and with several (`ops_assign_filament_preset_one_slot`,
`_multi_slot`), presets imported, an object's instance count, and the refusals of `send_gcode`, which
never sets a print host, so nothing is sent. Each records what a call leaves: its reply or its
refusal's text, the state read back after it, and the undo snapshots it took, which
`tests/fixtures/ops_undo.py` reads from the app's log since no tool lists the undo history. Their
`expect`s are on the setup and on what is read back. A replay cannot assert a refusal's text, so a
call that a route refuses is `optional`, and `result.json` is the record of those texts, to compare
between two builds.
The preset replays share `tests/fixtures/ops_presets.py`, which sets the slots, colours and flushing
volumes to known values before each case, and each needs a freshly launched app, since a hidden
preset that `select_preset` shows stays shown until a restart. The replays whose commands write files
need a fresh `--out`, since those commands write without `overwrite`. Launch the app from a folder that
holds no `.py` files, since the app's working directory is on Python's module path and a file there can
shadow a standard module that `run_script` needs.

`test_operation_routes.py` asserts what those replays only record: that a command, the tool that does
the same thing and the plugin API's binding refuse the same states with the same code and words, and
accept the same ones. It covers a project saved under a `.gcode.3mf` name, the exports of a project
with no objects, a layer asked of a Preview with none, an arrangement from the assemble view and with
a gizmo open, a mixed-colour filament slot, and a hidden printer selected through the plugin API. Run
it on a freshly launched app, for the reasons above and because the Preview keeps the last slice's
layers.

The Design-tab replays drive the Design commands through `call_tool` and need an instance launched with
`--cad`. Each starts from a new project and checks what it builds by volume and by what the reading
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
  pilot holes, the cones' kind, apex side and radii from `query_topology`, the hole heights and the
  plate's thickness from `measure`, a datum plane midway through the plate and one 70 mm up with a
  sketch on it, a refused tangent plane, and the back face pushed out 2 mm.
- `tests/cad_features.json` threads, sweeps, lofts and splits: the bench's M5 hex standoff tapped on
  its bore (M5 inferred from the bore) and again on the XY plane, each within the bench's volume
  bounds; a spring swept along a helix in round and square wire; a square-to-circle loft between XY
  and a datum plane 40 mm up; the loft split by that datum and its lower half cut again by XY turned
  over.
- `tests/cad_doc.json` reads and edits features as their cards do: a variable bound to an extrude,
  then removed, and literal edits with `set_feature`. Then it commits the design to the plate,
  recommits over the first object and undoes that in one step, exports STEP and reads it back into a
  new design, and commits two bodies both ways. Last it walks the Design history (`undo` / `redo` of
  an edit and of a rename), hides, moves and deletes features, names, hides and colours bodies,
  commits only the shown one, and saves and reopens the project to check that a body's name and
  colour were kept.
- `tests/cad_canvas.json` drives the Design view as a canvas: `set_view` shows the Design tab and
  points its camera (and refuses Prepare's and Preview's fields there), canvas and widget
  screenshots capture the view, a click at its centre picks the block and a right click lists the
  verbs for the pick, and `press_keys` reaches the Design panel's key hook (Shift+E opens the Extrude
  card, Escape with no target closes it). Last, a change made only in the Design tab marks the
  project modified, and reopening the saved design-only project with `discard_changes` asks nothing
  and names the project after its file.

Three harness tests run on an instance launched with `--cad`. `test_cad_replay.py` makes calls as a
client, so they are journaled, exports the session with `export_replay`, replays it into a new
project and compares the body's mass properties and the features. The session has an empty sketch
commit and a missed hole, which must fail and be left out, a legacy `fillet` answering ok false,
which stays in, and face and edge ids passed with their `generation`, which the journal drops.
`test_cad_slow_build.py` runs a long thread build (an M8 thread tapped 60 mm deep, about 8 s) and
checks that while it builds `get_app_state` answers, a window screenshot succeeds, `list_widgets` lists
the progress dialog and a screenshot of that dialog captures it, and another Design command is refused.
`test_cad_failed_job.py` sweeps a coil whose every rebuild takes about 30 s (its turns sized from a
short calibration build), then a hole that misses it replies with a running job and fails after
the reply budget: an export while it runs leaves it out and names it in `left_out_running`, the
export after it has the rename made next and not the hole, and that replay runs to the same coil.
It takes three to four minutes. `test_cad_examples.py` runs the examples `search_tools {tool}` gives for
every Design command, and for `undo` and `redo`, each on a fresh design, and fails one whose call fails or
whose result has `ok` false, an error, or is the person's form; `--tool NAME` runs one command's.

Three more harness tests on a `--cad` instance drive the Design tab the way a person does. Every action goes
through the UI tools (`set_view`, `click`, `mouse_gesture`, `press_keys`, `set_widget_value`, `answer_dialog`
and `auto_answer_dialogs`), and the Design commands only read the result back (`sketch_describe`,
`describe_scene`, `describe_feature`), save for one call in the scale rung; the right-click menu's layout they
grade against is read from `scripts/CAD/tool_atlas.json`, the Design table the app's own is generated from,
for the selection `describe_scene` names. Buttons and rows are found by label, and canvas positions come from
a calibration that maps the open sketch's plane to canvas pixels, so nothing in them is tuned to a window
size, screen scaling or theme. The sketch tool's pick budgets and badge layout are in the canvas's GL pixels,
which `cad_gui.py` turns into click pixels by the canvas scale that `set_view`'s `camera_state` gives (`zoom`
over `click_px_per_mm`). What they share is in `cad_gui.py` beside them. `test_cad_sketching.py` draws each
sketch tool's primitive with clicks on the canvas and types its exact values into the canvas's value field,
drives the edit operations and each constraint button on drawn geometry, undoes inside a sketch and across its
commit, saves the project and opens it again through the File menu, and draws on top of a sketch holding 1204
entities. That last rung, `scale`, loads them with one `sketch_add` command while the rectangle tool is armed:
the one call in these tests that changes the sketch outside the UI, on purpose, since a bulk add made while a
creation tool is armed must not be read as a drawn gesture that swallows the next key and click.
`test_cad_context_menu.py` checks that the right-click offer names what was clicked and lists exactly the
verbs the Design table says apply to it, and drives from the menu every sketch verb that has no key, a Text
typed into the Text dialog and an SVG import (`tests/fixtures/rect_40x20.svg`) among them.
`test_cad_click_edit.py` checks that each value field a drawing tool opens takes digits typed with no click
into it and that Return commits them, for the line, rectangle, circle, slot, polygon, ellipse and arc tools
armed by their keys, the rounded rectangle armed from the offer, and a rectangle's width label clicked in
Select.

```bash
tools/mcp/orca_mcp.py launch --cad
tools/mcp/test_cad_sketching.py
tools/mcp/test_cad_context_menu.py
tools/mcp/test_cad_click_edit.py rect label
```

Each prints `ok` or `FAIL` for every check and exits 1 if any failed. Rungs named on the command line
run alone, in that order (each test's docstring lists them); with none named, every rung runs.
`test_cad_sketching.py selfcheck-recovery` runs only when named: it fails on purpose with the mouse
button held, the offer asked for and a tool armed, to show that the rungs after a failed one start
clean. The tests find widgets, menu paths and verbs by their English names, so they need an English
UI: on any other language, or with the Design tab off, they exit 2 before the first check and say how
to fix it. On Linux the steps that need a file picker answered with a path, the sketching test's
`roundtrip` and the context-menu test's SVG import, print `skip`, since a GTK picker can't take a
path from MCP (`docs/HLSD/mcp-server.md`, "Dialogs and web views"). The app logs each picker a rule
answers in `<datadir>/log/debug_*.log.0`, with the reason when it cancels one a rule gave a path. MCP
input reaches the app through wxWidgets, not the OS, so a focus problem that only a window manager
causes is out of the tests' reach.

`tests/dialogs_canvas_input.json` checks that an auto-answered Transfer or Discard on the
unsaved-changes dialog does its work, that a toolbar item a selection just enabled takes a click at
once, and that a negative part, and a point seen through the bed from below, are click targets.

`tests/context_menus.json` right-clicks a cube, the empty bed and two selected objects, and clicks a
preset edit button that opens a menu: each menu is listed in the reply, or an item of it run with
`popup_item` (a modifier, a primitive, Delete), and none is ever shown, so on macOS the run finishes
with nobody pressing Escape. It also picks the Edit menu's Undo and Redo by the paths `Edit>Undo` and
`Edit>Redo`, which name them on every platform, though macOS shows their keys after them.

`tests/rows.json` drives rows and popups: object list rows (plates, an object and its parts, Ctrl
to add a row, the Printable column, a double click), the object's and a part's menus run from their
rows, Compare presets' rows, the object settings table, the print button's popup and the plate type
drop-down list. On Linux run it with `--headless`: under Xvfb those popups close as soon as they open.

`tests/plate_icons.json` clicks the Prepare view's plate icons by `{plate_icon, plate}`: lock twice,
Plate Settings opened and cancelled, arrange of a second plate's two cubes (the reply waits for the
job), the plate menu from a right click, and move to front.

`tests/webview_modal.json` runs JavaScript in the web views of modal dialogs, the preset bundle dialog
and the setup wizard, by default and by locator, and closes the bundle dialog from its page's own
button. `test_webview_modal.py` checks what that replay cannot: `run_javascript` is refused in the Home tab's
web view behind a modal and in a dialog under the topmost, the Flushing volumes dialog's page takes
them as any topmost dialog's does, and a page that closes its dialog replies `closed` at once.

Web pages are driven like widgets: `list_widgets` lists each shown page's elements under its web view
with `d` refs, and `click`, `set_widget_value`, `press_keys`, `mouse_gesture` and `answer_dialog` act on them, by
ref or by `{web: "Next", kind, css, within, index}`. `tests/web_dialogs.json` drives Orca's own web
dialogs that way alone: the preset bundle dialog, the colour picker behind a filament swatch (its hex
field, and a drag on its spectrum) and the setup wizard's first steps. `test_web_ui.py` covers every
kind of page control, action and what it changed on its page, refusal, locator, timeout and close on
the pages of a fixture plugin, in a modal dialog, a modeless window, a dock panel and the Plugins
dialog's configuration frame, a token in a page's URL shown redacted, and the permission prompt the
plugin's thread opens, answered with `answer_dialog` and with a rule registered while it is open.
Side-load the plugin into the datadir first:

```bash
tools/mcp/test_web_ui.py --install /tmp/orca-mcp-test
tools/mcp/orca_mcp.py launch --datadir /tmp/orca-mcp-test
tools/mcp/test_web_ui.py
```

`test_list_windows.py` checks `list_windows`: the main window first, the Plugins window (which doesn't
block the app) with a ref that `screenshot` captures alone, `list_widgets` reads alone and `press_keys` Escape
closes, Preferences last as a modal dialog, and Action Replay listed as hidden, without a ref.

`test_set_window.py` checks `set_window` on the main window (sized twice, moved, placed over its
display's work area, maximized, minimized and restored), on the Plugins window by its ref and on
Preferences by its `{label}` locator. On Wayland it checks
that a window screenshot is the size the reply gives and skips positions, which Wayland neither reports
nor takes. On X11 it reads the X server's stacking order through libX11 to check that the main window,
restored and raised while Preferences is open, stays under it.

`test_action_replay_hidden.py` opens Action Replay
and checks that the GUI tools don't see it: `list_widgets` lists no window for it, `run_javascript`
without a target behaves as before it opened, and a key sent without a target never reaches it. It
then shows the Run Script panel on
Prepare and checks the same for it, with no more web views in `list_widgets` than before, a key sent
without a target reaching the 3D view, a window screenshot, and an agent's `run_script` finishing
while it is shown; `run_replay` still runs the smoke test and a nested inline replay.

`test_speed_dial_open.py` goes from Home to Prepare, the Design tab (launched with `--cad`) and Preview by
their tabs, and checks that the page's 3D view then holds the keyboard focus and that Space opens the Speed
Dial, which Escape closes. A pointer entering a view also gives it the focus, so the tab switch itself is
proved only where no pointer rests on the view: on a native Wayland display, or a desktop with the pointer
elsewhere. Where KWin runs the display, it also checks through KWin's D-Bus that KWin draws no title bar on
the dial.

`tests/slice_stats.json` slices one cube, then two cubes in two colours, and checks the slice
reply's statistics: layers, max Z, filament changes, and each filament slot's model, flush and
prime tower grams.

`tests/preview.json` slices a cube and reads it back with `get_slice_stats` (summary, feature and layer
tables, the object, the legend's state and colour ranges) and `get_app_state.preview` as `set_view`
changes the layer (by number and by height), the view type, legend rows and time estimate, then
reads the cube's G-code opened for viewing the same way. Run it on a fresh datadir: modified presets
left by an earlier replay make opening the G-code ask about them. `test_set_view_preview.py` checks
what that replay cannot: `set_view {view_type}` acts as the legend's combo (a feature type hidden by
a click on its row shows again on Line Type, and the pick reaches the app config), a canvas
screenshot straight after a layer or view type change shows the new layers and the legend laid out
(it matches the capture after it), and the refusals of the preview arguments.

`test_set_view_camera.py`, on an instance launched with `--cad`, checks `set_view`'s `look_at` and `zoom`
on Prepare, Preview and the Design tab, and `orbit` on Prepare and the Design tab, against the reply's
`camera_state` and `get_app_state.camera`, including straight after `new_project`, whose fit runs at the
next frame, and a fit on Prepare after the window was sized on the Home tab.

`tests/agent_forms.json` calls tools the way agents often write them: a lone path for `paths`,
`object` for `objects` with the index as a string, an action by the key inside its id, `action` for
`id`, a camera direction as `set_view`'s `canvas`, which is read as its `camera`, `transform` to a `position`, `key` for `keys`,
`wait` with no condition, a gizmo by its toolbar title, `plate` for `plates`, legend rows in another
case or with `_` for a space, and `export` with the format taken from the path. It also checks that a
preset scope's whole `get_settings` listing leaves meta out.

`test_stdio_bridge.py [--bin PATH]` checks `orca-slicer --mcp-stdio` against the launched instance:
the handshake, notifications, tool calls in both protocol eras, and errors. With no instance
answering, it checks that the bridge answers the handshake and lists the launched instance's tools
without starting OrcaSlicer, and that a tool call then starts OrcaSlicer on the bridge's port (the
test stops that app).
`test_recovery_under_load.py` fills the server's connections with 32 blocked `wait` calls and 32
idle or half-sent connections while a dialog is open, and checks that `answer_dialog` on a new
connection still closes it. `test_cancel_pending_slice.py` cancels a slice while it still waits for
the update a setting change scheduled, and checks that it never starts. `test_ref_locator_export.py`
uses ImGui refs after their items changed (the Text gizmo's font button, whose label shows the font,
and a Texture Displacement layer row renamed by its place when a layer before it goes) and checks
that the exported session replays. `test_worker_admission.py` starts a long arrange three ways (the
`arrange` tool, `run_action obj_arrange`, a click on the Arrange options panel's button) and checks
that changes, canvas keys and toolbar clicks are refused while it runs, reads answer, and `cancel_job`
stops it. `test_export_running_job.py` exports while a call's job runs past
its reply budget (left out, named in `left_out_running`), after it ends (an `await: false` step that
replays) and while a dialog holds its call (kept); it takes about two minutes.
`test_send_gcode.py` starts an OctoPrint-compatible stub host and sends a sliced cube to it with the
Speed Dial's `send_gcode`, the Send dialog answered with `set_widget_value` and `answer_dialog`: the command
is refused for a Bambu network printer and one without a print host, an upload the stub accepts
reads uploading at 99 until the stub answers and then completed in `get_app_state.uploads`, its exported
replay uploads again, and one the stub refuses with HTTP 500 never reads completed and ends as an
error whose dialog `answer_dialog` closes.
