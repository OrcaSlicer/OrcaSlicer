# MCP harness

`orca_mcp.py` launches OrcaSlicer with its MCP server and talks to it, from a shell or from
Python. It uses only the standard library. `docs/HLSD/mcp-server.md` covers the server, and
`.claude/skills/orca-mcp/SKILL.md` covers the agent workflow.

## Commands

| Command | What it does |
|---|---|
| `launch [--xvfb] [--cad] [--printer "<model>@<nozzle>"] [--vendor V] [--filament F] [--datadir DIR] [--port N] [--bin PATH] [--print-config] [-- app args]` | Start OrcaSlicer with `--mcp`, wait until it is ready, print the session |
| `call TOOL ['<json args>'] [--wait]` | Call a tool and print its JSON result; `--wait` follows a returned job to its result |
| `tools` | List the tools, with the first line of each description |
| `snapshot ['<json args>']` | Print the `ui_snapshot` tree |
| `screenshot OUT [--target window\|canvas\|dialog]` | Save a full-size PNG, replacing `OUT` if it exists |
| `run-script FILE [--out DIR]` | Run a script in the launched instance and print the result; exit 0 when it passes |
| `stop` | Stop the app and its Xvfb, and remove the session |

`launch` creates a fresh temporary datadir unless you pass `--datadir`. It seeds the datadir with
an `OrcaSlicer.conf` that has a finished setup wizard and one printer and filament (Bambu Lab X1
Carbon 0.4 by default), with update checks and daily tips turned off. `--cad` turns the Design
(CAD) tab on, which `cad_call` needs: it sets `enable_cad_feature` in a fresh datadir's
`OrcaSlicer.conf`, and in a reused one too, since the app reads that setting only at startup. The token comes from
`ORCA_MCP_TOKEN` or is generated, and it reaches the app through the environment, not the command
line. On Linux, `launch` also sets `SSL_CERT_FILE`, so a fresh datadir doesn't prompt for a CA
bundle. On Windows, it gives the instance its own WebView2 profile in the datadir, apart from the one
your own OrcaSlicer uses, and finds `build-clang\src\Release\orca-slicer.exe` as well; run the
commands as `python tools/mcp/orca_mcp.py …`. On macOS, it finds the app bundle's binary under
`build/arm64`, `build/x86_64`, `build/universal` or `build`: the bundle the CMake target builds in
`src/Release`, or the one `build_release_macos.sh` assembles in `OrcaSlicer`. The app opens on the
desktop and takes focus while it starts, and needs no CA-bundle setting. `--xvfb` is Linux only. It
starts Xvfb on a free display between `:90`
and `:119`, with `--width`×`--height` (default 1600×1000), and sets `GDK_BACKEND=x11`. Xvfb runs with `-noreset`,
and its pointer is parked in the bottom-right corner. In the middle of the screen, the pointer would
raise tooltips over whatever opens there, and they would show in screenshots. The app's output goes to
`<datadir>/mcp-launch.log`. `--timeout` (default 120 s) limits how long `launch` waits for
`ready`.

## Session file

`launch` writes `{port, token, pid, datadir, xvfb_pid, display}` to `$ORCA_MCP_SESSION`, or to
`~/.cache/orca-mcp/session.json` if that is unset. The file is mode 0600, and every other command
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
`call_with_text()` also returns the text blocks after the JSON one, such as `ui_snapshot`'s tree.
`images()` also returns the PNG bytes of the image blocks. `call_raw()` returns the MCP result
unchanged.

## Smoke test

`smoke_test.json` is an end-to-end script that exercises the main paths against a fresh datadir:

- an auto-answered startup or unsaved-changes prompt;
- loading, a setting change, slicing and a G-code export;
- a dialog that an action opens and the script answers;
- screenshots of the dialog and the preview.

```bash
tools/mcp/orca_mcp.py launch --xvfb
tools/mcp/orca_mcp.py run-script tools/mcp/smoke_test.json --out /tmp/orca-smoke
tools/mcp/orca_mcp.py stop
```

Without the harness, `orca-slicer --datadir DIR --mcp-script tools/mcp/smoke_test.json
--mcp-script-out /tmp/orca-smoke` runs the script and exits. On Linux, set `SSL_CERT_FILE` (for
example to `/etc/ssl/certs/ca-certificates.crt`) as the harness does; otherwise a prompt for the CA
bundle blocks startup before the script can run. The exit code is 0 when it passes, 1
when a step fails and 2 when the script is invalid. The run leaves `result.json` and the
screenshots in the output directory. `tests/script_basic.json` is the smallest such check: load,
change a setting, slice and capture.

`tests/gizmos.json` drives every gizmo through the GUI tools, the way a person does: each one's icon,
its panel's fields, combos, toggles and links, and clicks or strokes at points of the model, then
checks what it left on the model (`list_objects`), with an undo and redo and a canvas screenshot per
gizmo. It uses the fixtures beside it (`tests/fixtures/star.svg`, `tests/fixtures/heightmap.png`).
Run it at a second window size through `run_script`'s `vars`:

```bash
tools/mcp/orca_mcp.py call run_script '{"path": "/abs/tools/mcp/tests/gizmos.json", "out_dir": "/tmp/gizmos-small", "vars": {"window_size": [1100, 760]}}' --wait
```

`tests/scene.json` checks object structure and placement: `transform` on an instance (the drop after a
scale, `size`, a move to another plate), on a part, what `list_objects` reports of them (bounding boxes
on the bed, filament, the scaled volume, a mirrored part and instance), the shape commands with a size
param, `arrange` on one plate and every plate, Fill bed, the Speed Dial no-op that `run_action`
reports as an error, `add_part` (shapes at a size and a position, a model file, one undo step,
and what it refuses), and the object structure commands (Change Type, Rename, number of instances,
individual objects, Assemble, Order By and its reversal, and what each refuses). It ends with all of
them on one scene: a block with a magnet pocket and a boss, sized, moved to a second plate, copied six
times and arranged there, split into objects, one mirrored, two assembled, ordered, the plate filled
and sliced.

`tests/layers.json` checks height range modifiers: `set_layer_ranges` with ranges given out of order,
their read-back in `list_objects`, one undo step (and none for the same ranges again), the layer count
of a slice with them, the refusals (an overlap, a layer height beyond the printer's, a missing filament,
an object setting, an empty range), a range in the second filament, a 3MF round trip, and `[]`. Then
variable layer height profiles on a Benchy with `set_layer_height_profile`: Adaptive called from the
Preview tab, its quality's effect on the point count, the layer count of a slice with it, Smooth as one
undo step, Reset and a Reset that changes nothing, the refusals (an argument of another action, the
assemble view), Smooth and Adaptive over height ranges, and a 3MF round trip. Last, layer G-codes on a
cube with `set_layer_gcode`: refused before a slice, a pause by layer and custom G-code by height called
from Prepare (it shows the Preview), the same code again changing nothing, a removal and an in-place
custom edit in one call, the refusals (a layer above the top, a code where another is, nothing to remove,
a missing filament, a missing plate, a plate changed since its slice that the Preview does not show), a
filament change, a 3MF round trip, and `clear`.

`tests/multicolor.json` assigns filaments to objects and parts through `set_settings`' object and
volume scopes, with undo and redo, and reads them back with `get_settings`' effective values. It then
sets the project's slot colours, flushing multiplier, one flushing volume and plate type through the
project scope (in one call, the colour first), checks the refusals that leave everything as it was
(the read-only `filament_is_mixed`, a bad colour, a purge mode the printer has no button for, a non-zero
diagonal, a tower off the plate), and moves the prime tower with an undo, before slicing.

`tests/presets.json` saves filament, process and printer presets with `save_preset` (a user preset,
one inside the project that a saved and reopened project keeps, a detached one, and the overwrite
rule), and drops unsaved edits with `set_settings {"reset_all": true}`. It reads presets that are not
selected and compares presets (`get_settings` `preset` and `compare`), writes one entry of a
per-variant value, and checks `set_settings`' `changed` after the spiral vase dialog. It reverts single
keys (`null`, `reset_to`), and answers the unsaved-changes prompts: `select_preset` without `changes`
still opens the prompt, `changes` transfers or discards without it, and `new_project` keeps modified
presets unless `changes` is `"discard"`. It ends by deleting the presets it made with `delete_preset`:
filaments the tab edits and does not, a base process and its child, and a printer once selected, with
the refusals for system presets, a preset another inherits from, unsaved changes and a printer that is
not selected. It exports a filament with the Export Preset Bundle dialog to its `out_dir` (`export_config`
with a directory), imports the archive again with `import_configs` and a path (kept while the preset
exists, imported once it is deleted) and imports `tests/fixtures/mcp_import_pla.json`. It changes a
preference in the Preferences dialog opened with `open_preferences` `control:auto_arrange`, by the
control's name, and reads it back with `get_settings {"scope": "preferences"}`. It starts from the
harness's default printer with one filament slot in Simple mode, deleting slots and selecting the mode
first, and reruns on the same datadir.

`tests/plates.json` sets a plate type and print sequence on one plate only (with undo and redo), the
first and other layers' filament orders, spiral vase, the lock (twice, which leaves it locked) and a
name through `set_settings`' plate scope, and reads them back with `list_plates` and `get_settings`.
It brings the filament list to two slots with the `filament_add` and `filament_delete` commands first,
and ends with a mixed filament, whose slot the delete command refuses and ahead of which a new slot
goes. It reruns on the same datadir.

`tests/calibration.json` makes every calibration test with `calibrate` (each pressure advance method,
a 2 x 2 pattern batch, the four flow ratio passes) and checks the objects it added, the parameters
it used and the presets it changed, slicing some of them. It checks the temperature rounding, the
refusals that leave the project as it was (modified presets without `changes`, an argument the test
does not take), `changes: "keep"` keeping an edit, and `new_project` dropping the test's settings. It
starts from the harness's default printer and reruns on the same datadir.

The Design-tab scripts drive `cad_call` and need an instance launched with `--cad`. Each starts
from a new project and checks what it builds by volume and by what `cad_call` reads back; steps
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
- `tests/cad_topology_targets.json` builds the bench's J wall hook from what `cad_call` reads back:
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
  verbs for the pick, and `ui_key` reaches the Design panel's key hook (Shift+E opens the Extrude
  card, Escape with no target closes it). Last, a change made only in the Design tab marks the
  project modified, and reopening the saved design-only project with `discard_changes` asks nothing
  and names the project after its file.

Two harness tests run on an instance launched with `--cad`. `test_cad_replay.py` makes calls as a
client, so they are journaled, exports the session with `export_script`, replays it into a new
project and compares the body's mass properties and the features. The session has an empty sketch
commit and a missed hole, which must fail and be left out, a legacy `fillet` answering ok false,
which stays in, and face and edge ids passed with their `generation`, which the journal drops.
`test_cad_slow_build.py` runs a long thread build (an M8 thread tapped 60 mm deep, about 8 s) and
checks that while it builds `get_state` answers, a window screenshot succeeds, `ui_snapshot` lists
the progress dialog and a screenshot of that dialog captures it, and another `cad_call` is refused.
`test_cad_failed_job.py` sweeps a coil whose every rebuild takes about 30 s (its turns sized from a
short calibration build), then a hole that misses it replies with a running job and fails after
the reply budget: an export while it runs leaves it out and names it in `left_out_running`, the
export after it has the rename made next and not the hole, and that script replays to the same coil.
It takes three to four minutes.

`tests/dialogs_canvas_input.json` checks that an auto-answered Transfer or Discard on the
unsaved-changes dialog does its work, that a toolbar item a selection just enabled takes a click at
once, and that a negative part, and a point seen through the bed from below, are click targets.

`tests/context_menus.json` right-clicks a cube, the empty bed and two selected objects, and clicks a
preset edit button that opens a menu: each menu is listed in the reply, or an item of it run with
`popup_item` (a modifier, a primitive, Delete), and none is ever shown, so on macOS the run finishes
with nobody pressing Escape.

`tests/rows.json` drives rows and popups: object list rows (plates, an object and its parts, Ctrl
to add a row, the Printable column, a double click), the object's and a part's menus run from their
rows, Compare presets' rows, the object settings table, the print button's popup and the plate type
drop-down list.

`tests/plate_icons.json` clicks the Prepare view's plate icons by `{plate_icon, plate}`: lock twice,
Plate Settings opened and cancelled, arrange of a second plate's two cubes (the reply waits for the
job), the plate menu from a right click, and move to front.

`tests/webview_modal.json` runs scripts in the web views of modal dialogs, the preset bundle dialog
and the setup wizard, by default and by locator, and closes the bundle dialog from its page's own
button. `test_webview_modal.py` checks what that script cannot: scripts are refused in the Flushing
volumes dialog and in the Home tab's web view behind a modal, and a page that closes its dialog
replies `closed` at once.

`tests/slice_stats.json` slices one cube, then two cubes in two colours, and checks the slice
reply's statistics: layers, max Z, filament changes, and each filament slot's model, flush and
prime tower grams.

`tests/preview.json` slices a cube and reads it back with `get_preview` (summary, feature and layer
tables, the object, the legend's state and colour ranges) and `get_state.preview` as `set_view`
changes the layer (by number and by height), the view type, legend rows and time estimate, then
reads the cube's G-code opened for viewing the same way. Run it on a fresh datadir: modified presets
left by an earlier script make opening the G-code ask about them. `test_set_view_preview.py` checks
what that script cannot: `set_view {view_type}` acts as the legend's combo (a feature type hidden by
a click on its row shows again on Line Type, and the pick reaches the app config), and the refusals
of the preview arguments.

`test_stdio_bridge.py [--bin PATH]` checks `orca-slicer --mcp-stdio` against the launched instance:
the handshake, notifications, tool calls in both protocol eras, and errors.
`test_recovery_under_load.py` fills the server's connections with 32 blocked `wait` calls and 32
idle or half-sent connections while a dialog is open, and checks that `dialog_respond` on a new
connection still closes it. `test_cancel_pending_slice.py` cancels a slice while it still waits for
the update a setting change scheduled, and checks that it never starts. `test_ref_locator_export.py`
uses ImGui refs after their items changed (the Text gizmo's font button, whose label shows the font,
and a Texture Displacement layer row renamed by its place when a layer before it goes) and checks
that the exported session replays. `test_worker_admission.py` starts a long arrange three ways (the
`arrange` tool, `run_action obj_arrange`, a click on the Arrange options panel's button) and checks
that changes, canvas keys and toolbar clicks are refused while it runs, reads answer, and `cancel`
stops it. `test_export_running_job.py` exports while a call's job runs past
its reply budget (left out, named in `left_out_running`), after it ends (an `await: false` step that
replays) and while a dialog holds its call (kept); it takes about two minutes.
`test_send_gcode.py` starts an OctoPrint-compatible stub host and sends a sliced cube to it with the
Speed Dial's `send_gcode`, the Send dialog answered with `ui_input` and `dialog_respond`: the command
is refused for a Bambu network printer and one without a print host, an upload the stub accepts
reads uploading at 99 until the stub answers and then completed in `get_state.uploads`, its exported
script uploads again, and one the stub refuses with HTTP 500 never reads completed and ends as an
error whose dialog `dialog_respond` closes.
