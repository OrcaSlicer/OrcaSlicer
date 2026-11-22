# Project files, imports and exports

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_files`
`test_files.py` · Python, 472 checks · in `sweep.json` as `{"path": "test_files.py", "timeout_s": 1200}`

**Tests:** What the project's files do: a new project and the question New Project asks about unsaved changes; opening
a project file and saving one under a name; importing models (STL, a project's `.3mf`, a ZIP archive, a STEP file and
files that hold no model); and exporting the objects as STL and DRC, the model as a 3MF, the plate's G-code and the
sliced file. Every written file is read back: the test opens it itself (a G-code's text, a sliced file's entries, a
folder's file names and sizes), or the app reads it (an exported mesh imported into an empty project, a saved project
opened again). It runs on the default printer, the Generic Klipper Printer 0.4, with its 0.20mm Standard @MyKlipper
process (`wall_loops` 3, `top_shell_layers` 4, layers of 0.2 mm from the first, `exclude_object` on) and one Generic
PLA slot. Nothing it checks needs a Bambu Lab printer's own behaviour.

**Starts from:** `start(client)`, whose `presets()` must be `{printer: "MyKlipper 0.4 nozzle", process: "0.20mm
Standard @MyKlipper", filaments: ["Generic PLA @System"]}`. It sets nothing beyond the baseline. It reads back with
`preferences_get` the preferences its checks depend on, each at the baseline's value, a new folder's:
`project_load_behaviour` `ask_when_relevant` (an open loads the project's settings and asks nothing about them),
`max_recent_count` 18 and `recent_models` false (the recent projects), `enable_step_mesh_setting` true (the STEP options
dialog) and `auto_slice_after_change` false (no plate slices by itself); and `drc_bits`, which "drc_bits is 0,
lossless" checks is 0, under which the DRC rows read their meshes back exactly. The Preferences dialog's field takes 8
to 30 only, so a baseline with a lossy depth fails that check by name, ahead of the rows. If the user filament presets
list "MCP Files PLA", which only a failed run of this test can have left (importing models, step 10, is the regression
that makes it), the test deletes it with `preset_delete` and checks it is no longer listed. `W` is new each run, so
nothing a failed first run left in its own `W` reaches a rerun.

**Leaves:** In the app's presets, the Klipper printer with the one slot of Generic PLA that `start()` set, in the
baseline's `#26A69A`. In the project, the last scenario's Benchy, unsaved, with its last `wall_loops` an unsaved edit
of the process. In the recent projects, at the top, the projects it saves or opens in `W`: `kept.3mf`, `open_src.3mf`,
`clean.3mf`, `dirty.3mf`, `presets.3mf`, `empty.3mf`, `saved.3mf` and `import_src.3mf`, which name files that are gone
once the test passes and `W` is deleted. The folders the app remembers, all set to `W`: `app.last_export_path`, where
the G-code picker starts, and the `recent` section's `last_opened_folder` and `settings_folder`. No user preset: every
other preset edit is discarded by a new project, and the preset file it imports as a model loads none. It restores
nothing in a `finally`.

**Files:** The test writes into a folder of its own, `W` (`tempfile.mkdtemp()`): the STL cubes and ZIP archives it
imports, written with `zipfile` and an ASCII STL writer of its own, and every file the app exports or saves. It reads
`tests/data/named_single_body.step` (a tetrahedron with 10 mm legs: 4 triangles, 166.667 mm³),
`resources/handy_models/3DBenchy.drc` and `tools/mcp/tests/fixtures/heightmap.png`. It copies
`tests/data/test_stl/ASCII/20mmbox-LF.stl` (a 20 mm box, 12 triangles) into `W` as `W/20mmbox-LF.stl`, so that the
path the recent-projects check looks for is this run's alone. It writes a filament preset file of its own,
`W/mcp_files_pla.json`: `{name: "MCP Files PLA", filament_settings_id: ["MCP Files PLA"], inherits: "Generic PLA
@System", from: "User", filament_cost: ["24"], version: "2.8.0.16"}`, a preset the Klipper printer takes
(`preset_import` of the file imports it, measured). The name is no other test's:
`tools/mcp/tests/fixtures/mcp_import_pla.json` ("MCP Import PLA", inheriting "Generic PLA @System") is the fixture
`test_presets` writes "MCP Imported PLA" from. It does not use
`tests/fixtures/filaments_and_files.py`, which `test_additions_refusals.py` writes its archive with.

**Reads:** `plates()` and `slots()` as `test_objects` reads them. New readers in `checks.py`:
- `project()`: `app_state.project`, whose fields are `name`, `path` and `dirty`, as `{name, file, dirty}`: `file` is
  `path` normalised (`os.path.normcase` of `os.path.abspath`), or none where `path` is `""`.
- `meshes()`: each object from `object_list` as `{name, offset, size, filament, parts}`: its first instance's `offset`
  x and y, its `bbox.size`, its `filament`, and each part as `{id, type, triangles, mesh_volume}`.
- `values(keys, type="process")`: `settings_get {type, keys}` as `{key: value}`.
- `unsaved(type)`: the keys `settings_get {type, compare: ":saved"}` lists, sorted: what differs from the preset as it
  is stored.
- `recent()`: the files of the Recent Projects actions, read from each action's `description` ("Open the recent
  project <path>.") in `action_list {section: "Recent Projects"}`, normalised as `project()` normalises. The list holds
  what earlier tests on the data folder opened and saved, so a row only asks whether it holds or lacks one of `W`'s
  files, which no other test and no earlier run names.
- `undo_steps()`: the names of the undo steps logged since its previous call, from the lines
  `Undo / Redo snapshot taken: <name>` of the app's log (`app_log`'s `file`), as `fixtures/ops_undo.py` reads them. A
  row that checks it calls it once just before its step, which sets the mark, and checks what the call after the step
  gives.
- `dialogs()`: `app_state.dialogs` as `{title, message}`.
- `sliced_flags()`: `plate_list`'s `sliced` of each plate, in order.
- `layer_gcode()`: plate 0's `layer_gcode` from `plate_list`, each code as `{z, type}`.
- `gcode(path)`: what a G-code file holds, read as text: `layers`, the number on its `; total layer number: N` line;
  `labels`, how many `EXCLUDE_OBJECT_DEFINE` lines it has, one per object instance the Klipper process's
  `exclude_object` names to the printer (a Bambu Lab printer's G-code lists them on a `; model label id:` line instead,
  which the Klipper printer's lacks); and `config`, the `; key = value` lines between `; CONFIG_BLOCK_START` and
  `; CONFIG_BLOCK_END` as `{key: value}`, a block the Klipper printer's G-code writes after its moves.
- `sliced(path)`: the names of a `.gcode.3mf`'s G-code entries (`Metadata/plate_<n>.gcode`), sorted, and the `gcode()`
  of each.
- `reimport(path)`: `project_new {discard_changes: true}`, then `project_import {paths: [path]}`; it gives the
  import's reply and then `meshes()`.
- `answer_all(expected)`: answers the dialogs a call opens, each OK as it comes topmost, waiting with `app_wait {for:
  "dialog"}` (a wait that runs out fails) until it has answered one dialog for each `{title, message words}` in
  `expected`, in whatever order they come; it checks that no other dialog opened and that `dialogs()` is then empty.

**Start of a scenario:** `project_new {discard_changes: true, preset_changes: "discard"}`, then `view_set {tab:
"prepare"}` (the new-project and open scenarios show `"home"` instead), then `app_wait {for: "idle"}`, which must be
satisfied. The start is checked: `project()` is `Untitled · none · clean`, `meshes()` is empty, `plates()` is
`"" 0 *`, and `dialogs()` is empty. Presets, preferences and recent projects persist from one scenario to the next;
`start()` is made once, before the first.

**Notation:** A project is written `name · file · clean` or `dirty` (`project()`), `none` for no file, and a file in
`W` by its name. Objects are listed by name, in `object_list`'s order, between brackets: `[small, big]`. A mesh is
written `name t/v [x, y, z]`: its object's name, its one part's `triangles` and `mesh_volume` (mm³, rounded to 0.001
as `object_list` rounds it), and its `bbox.size` to 0.001 mm. A part alone is written `type t/v`. Cubes are added with
`object_add_primitive {shape: "cube", size: [s, s, s], name, position: [x, y, 0]}`, written "cube `name` s at
(x, y)"; plate 0 covers 0 to 250 mm in x and y, and `offset` x and y equal the position given, to 0.001 after a
round trip through a file. A refusal is checked with `refused()` by its code, or by its words where it has none, and
every refused call is followed by the read-back of its row. "No file" means the path does not exist afterwards; the
control for each is the scenario where the same export or save writes its file. A plate slice is checked through its
reply (`outcome`, `layers`) and then `sliced_flags()`.

**Scenario: a new project**

Each call is made from the Home tab (`view_set {tab: "home"}` first), so that `app_state.tab` shows the tab it leaves.
New Project (the action) is `action_run {id: "new_project"}`.

| # | Step | Project after | Also checked |
|---|---|---|---|
| 1 | New Project | `Untitled · none · clean` | the reply is not `blocked_by_dialog` and its `ran` is `orca_command:new_project:orca`; `dialogs()` empty; tab `prepare`; `undo_steps()` `["Reset Project", "New Project"]` |
| 2 | `project_new {silent: true}` | `Untitled · none · clean` | reply `objects` 0, `names` `[]`, `plates` 1, no `discarded` or `modified`; tab `home`; `undo_steps()` `["Reset Project", "New Project"]` |
| 3 | cube `box` 20 at (128, 128), then `project_new {silent: true}` | `Untitled · none · dirty` | refused, `unsaved_changes`; `[box]` at (128, 128); tab `home` |
| 4 | `project_new {discard_changes: true}` | `Untitled · none · clean` | reply `objects` 0; `[]`; tab `home` |
| 5 | `settings_set {values: {top_shell_layers: 7}}` | `Untitled · none · dirty` | `values([top_shell_layers])` `7`; `unsaved("process")` `[top_shell_layers]` |
| 6 | New Project | as after 5 | reply `blocked_by_dialog`; `dialogs()` is one dialog, titled `Creating a new project: unsaved changes`: a project with no objects asks nothing about saving it |
| 7 | `dialog_answer` Discard; the job is followed to its end | `Untitled · none · clean` | `top_shell_layers` `4`; `unsaved("process")` `[]`; `dialogs()` empty; tab `prepare` |
| 8 | `settings_set {values: {top_shell_layers: 7}}`, New Project, `dialog_answer` Transfer, the job followed to its end | `Untitled · none · clean` | the dialog of 6 opened; `top_shell_layers` `7`, `unsaved("process")` `[top_shell_layers]`: the change is carried into the new project; tab `prepare` |
| 9 | `project_new {discard_changes: true, preset_changes: "discard"}` | `Untitled · none · clean` | reply `discarded` `{process: {name: "0.20mm Standard @MyKlipper", keys: ["top_shell_layers"]}}` and no `modified`; `top_shell_layers` `4`; `unsaved("process")` `[]`; tab `home` |
| 10 | `settings_set {values: {top_shell_layers: 7}}`, then `project_new {silent: true}` | `Untitled · none · dirty` | refused, `unsaved_changes`; `top_shell_layers` `7`; tab `home` |
| 11 | `project_new {discard_changes: true}` | `Untitled · none · clean` | reply `modified` `{process: {name: "0.20mm Standard @MyKlipper", keys: ["top_shell_layers"]}}` and no `discarded`; `top_shell_layers` `7`, `unsaved("process")` `[top_shell_layers]`: the presets' changes carry over by default |

**Scenario: New Project's question keeps the layer G-code**

The project is saved before the pause and the move, so only the question's Save can have written them to its file.

| # | Step | Project after | Also checked |
|---|---|---|---|
| 1 | cube `kept` 20 at (100, 100), `project_save {path: W/kept.3mf}` | `kept · kept.3mf · clean` | reply `path` the file and `bytes` its size on disk |
| 2 | `plate_slice` | as after 1 | outcome `ok`, `layers` 100; `sliced_flags()` `[true]` |
| 3 | `plate_set_custom_gcode {add: [{layer: 26, type: "pause"}]}` | `kept · kept.3mf · dirty` | reply `layer_gcode` `[{z: 5.2, layer: 26, type: "pause"}]`; `layer_gcode()` `[{z: 5.2, type: pause}]`; `sliced_flags()` `[false]`: a layer code invalidates the slice |
| 4 | `object_transform {object: 0, translate: [5, 0, 0]}` | `kept · kept.3mf · dirty` | `kept` at (105, 100) |
| 5 | New Project, `dialog_answer` Cancel, the job followed to its end | `kept · kept.3mf · dirty` | the dialog was titled `Orca Slicer - Save` and its message holds "The current project has unsaved changes"; `[kept]` at (105, 100); `layer_gcode()` `[{z: 5.2, type: pause}]`; `dialogs()` empty |
| 6 | New Project, `dialog_answer` Yes, the job followed to its end | `Untitled · none · clean` | the dialog was `Orca Slicer - Save`, and no other opened; `[]`; `layer_gcode()` `[]`; `plates()` `"" 0 *`; tab `prepare` |
| 7 | `project_open {path: W/kept.3mf}` | `kept · kept.3mf · clean` | `[kept]` at (105, 100); `layer_gcode()` `[{z: 5.2, type: pause}]`: the Save wrote the pause and the move |

**Scenario: opening a project**

The project opened is made first and saved: cube `source` 20 at (100, 100) with `settings_set {values: {wall_loops:
5}}`, `project_save {path: W/open_src.3mf}`. The test then copies the file to `W/clean.3mf`, `W/dirty.3mf` and
`W/presets.3mf`, which no call names before its case opens it, so each open shows in `recent()`. Each case starts with
`project_new {discard_changes: true, preset_changes: "discard"}` and `view_set {tab: "home"}`. "Opened" in a row
means: the reply (or the job's reply) is `{opened: "project", objects: 1, names: ["source"], plates: 1}`; `[source]`
at (100, 100); `values([wall_loops, top_shell_layers])` `5`, `4`; `unsaved("process")` `[wall_loops]`; tab
`prepare`; `dialogs()` empty.

| # | Step | Project after | Also checked |
|---|---|---|---|
| 1 | `recent()` | `Untitled · none · clean` | `recent()` holds `open_src.3mf` and none of the three copies |
| 2 | `project_open {path: W/clean.3mf}` | `clean · clean.3mf · clean` | opened; `recent()` holds `clean.3mf`; `undo_steps()` `["Reset Project", "Load Project"]` |
| 3 | (new case) cylinder `cyl` `[20, 10]`, then `project_open {path: W/dirty.3mf}` | `Untitled · none · dirty` | refused, `unsaved_changes`; `[cyl]`; `dialogs()` empty; tab `home`; `recent()` lacks `dirty.3mf` |
| 4 | the same with `discard_changes: true` | `dirty · dirty.3mf · clean` | opened; `recent()` holds `dirty.3mf` |
| 5 | (new case) `settings_set {values: {top_shell_layers: 7}}`, then `project_open {path: W/presets.3mf}` | `Untitled · none · dirty` | refused, `unsaved_changes`; `[]`; `top_shell_layers` `7`; `dialogs()` empty |
| 6 | the same with `discard_changes: true` | `Untitled · none · clean` | reply `blocked_by_dialog`; `dialogs()` is one dialog titled `Load project: unsaved changes`; read while it is open: `[]`, `top_shell_layers` `7`, `unsaved("process")` `[top_shell_layers]`. The call has dropped the project's changes with a silent new project, which leaves the presets' changes and reads clean, and the open then asks about the presets' |
| 7 | `dialog_answer` Discard; the job is followed to its end | `presets · presets.3mf · clean` | opened: the file's `top_shell_layers` 4, the unsaved 7 gone; `recent()` holds `presets.3mf` |

**Scenario: saving a project under a name**

Each save's `undo_steps()` is `[]`: a save takes no undo step.

| # | Step | Project after | Also checked |
|---|---|---|---|
| 1 | `project_save {path: W/empty.3mf}` | `empty · empty.3mf · clean` | reply `path` the file and `bytes` its size on disk; `recent()` holds `empty.3mf`: a project with no objects is saved |
| 2 | (new project) `project_open {path: W/empty.3mf}` | `"" · none · clean` | the reply is `blocked_by_dialog`; `answer_all([Warning "The file does not contain any geometry data."])`; the job's reply `{opened: "project", objects: 0, names: [], plates: 1}`; `plates()` `"" 0 *` |
| 3 | (new project) cube `box` 20 at (100, 100), `project_save {path: W/saved.3mf}` | `saved · saved.3mf · clean` | reply `path` and `bytes` as in 1; `recent()` holds `saved.3mf` |
| 4 | `object_transform {object: 0, translate: [5, 0, 0]}` | `saved · saved.3mf · dirty` | `box` at (105, 100) |
| 5 | `project_save {path: W/saved.gcode.3mf}` | `saved · saved.3mf · dirty` | refused, `sliced_file_name`; no file; `[box]` at (105, 100) |
| 6 | `project_save {path: W/missing/saved.3mf}` | `saved · saved.3mf · dirty` | refused, with the words "directory does not exist"; `W/missing` does not exist; `[box]` at (105, 100) |
| 7 | `project_save {path: W/saved.3mf}` | `saved · saved.3mf · dirty` | refused, with the words "file exists (pass overwrite: true to replace it)"; `[box]` at (105, 100) |
| 8 | `project_open {path: W/saved.3mf, discard_changes: true}` | `saved · saved.3mf · clean` | `[box]` at (100, 100): the refused saves wrote nothing |
| 9 | `object_transform {object: 0, translate: [5, 0, 0]}`, then `project_save {path: W/saved.3mf, overwrite: true}` | `saved · saved.3mf · clean` | reply `bytes` the file's size; `[box]` at (105, 100) |
| 10 | (new project) `project_open {path: W/saved.3mf}` | `saved · saved.3mf · clean` | `[box]` at (105, 100): the file was replaced |

**Scenario: importing models**

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | `project_import {paths: [W/20mmbox-LF.stl]}` | `20mmbox-LF.stl 12/8000 [20, 20, 20]` | reply `{opened: "models", added: 1, objects: 1, names: ["20mmbox-LF.stl"], plates: 1}`; `project()` `20mmbox-LF · none · dirty`: an untitled project takes the name of the first file imported, as File > Import names it; `undo_steps()` `["Import Objects: 20mmbox-LF.stl"]`; `unsaved("process")` `[]`; `recent()` lacks `W/20mmbox-LF.stl` (the `recent_models` preference is off) |
| 2 | `undo` | `[]` | the import is one undo step |
| 3 | (new project) cylinder `cyl` `[20, 10]`, `settings_set {values: {top_shell_layers: 7}}`, then the import of 1 | `[cyl, 20mmbox-LF.stl]`, the box as in 1 | reply `added` 1; `project()` `20mmbox-LF · none · dirty`; `top_shell_layers` `7`; `unsaved("process")` `[top_shell_layers]` |
| 4 | `undo` | `[cyl]` | `top_shell_layers` `7`; `unsaved("process")` `[top_shell_layers]` |
| 5 | (new project) cube `source` 20 at (100, 100), `settings_set {values: {wall_loops: 5}}`, `project_save {path: W/import_src.3mf}`; (new project) cylinder `cyl` `[20, 10]`, `settings_set {values: {top_shell_layers: 7}}` | `[cyl]` | `values([wall_loops, top_shell_layers])` `3`, `7` |
| 6 | `project_import {paths: [W/import_src.3mf]}` | `[cyl, source]`, `source 12/8000 [20, 20, 20]` | reply not `blocked_by_dialog`, `added` 1; `dialogs()` empty; `values([wall_loops, top_shell_layers])` `3`, `7`: a project's `.3mf` adds its objects and none of its settings; `unsaved` of process `[top_shell_layers]`, of filament and printer `[]`; `project()` `import_src · none · dirty`; `undo_steps()` `["Import Objects: import_src.3mf"]` |
| 7 | `undo` | `[cyl]` | `values([wall_loops, top_shell_layers])` `3`, `7`; `unsaved("process")` `[top_shell_layers]` |
| 8 | (new project) cube `box` 20 at (128, 128); `project_import {paths: [W/missing.stl]}` | `[box]` | refused, with the words "file not found"; `undo_steps()` `[]` |
| 9 | `project_import {paths: [heightmap.png]}` | `[box]` | the reply is `blocked_by_dialog`; `answer_all([Warning "The file does not contain any geometry data.", Orca Slicer error "Unknown file format"])`; the job ends refused, `nothing_loaded`; `undo_steps()` `["Import Objects: heightmap.png"]`; `project()` `Untitled · none · dirty`: an import that adds nothing names nothing |
| 10 | `project_import {paths: [W/mcp_files_pla.json]}` | `[box]` | as 9, with `Import Objects: mcp_files_pla.json`; `preset_list {type: "filament", source: "user"}` lists no "MCP Files PLA" and `app_state.presets.filaments` is `["Generic PLA @System"]`: a preset file is no model and loads no preset |

A file the loader cannot read opens two dialogs. `load_files()` catches the reader's "Unknown file format: input file
must have .stl, .obj, or .amf(.xml) extension." and shows it with `show_error()`, which defers the `Orca Slicer error`
dialog with `CallAfter`; then, having loaded nothing, it shows the `Warning` "The file does not contain any geometry
data." at once, inside the call. So the call replies `blocked_by_dialog`, and the error dialog opens over the warning
or after it, as the event loop runs it: `answer_all()` takes either order.

**Scenario: ZIP archives**

The import unpacks the entries into the system's temporary folder and names each object after its file there, adding a
number when the folder has one of that name (`Plater::preview_zip_archive()`), which a file left by another run could
cause. So the entries' file names start with a tag the run makes, `T`: the base name of `W`, which `mkdtemp()` makes
unique. The test writes `W/cubes.zip` with, in this order, the entries `parts/T_large.stl` (a 20 mm cube),
`parts/T_middle.stl` (15 mm), `T_small.stl` (10 mm) and `readme.txt` (no model), and `W/not_a.zip`, a text file.
Objects come in the archive's order.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | `project_import {paths: [W/cubes.zip], entries: ["T_small.stl", "parts/T_large.stl"]}` | `T_large.stl 12/8000 [20, 20, 20]`, `T_small.stl 12/1000 [10, 10, 10]` | reply `added` 2, `archives` `[{path: W/cubes.zip, entries: ["T_small.stl", "parts/T_large.stl"]}]`; the objects in the archive's order, not the order `entries` names them; `undo_steps()` `["Import Objects: cubes.zip"]`; `dialogs()` empty |
| 2 | `undo` | `[]` | one undo step |
| 3 | (new project) `project_import {paths: [W/cubes.zip]}` | `T_large.stl 12/8000 [20, 20, 20]`, `T_middle.stl 12/3375 [15, 15, 15]`, `T_small.stl 12/1000 [10, 10, 10]` | reply `added` 3, `archives[0].entries` `["parts/T_large.stl", "parts/T_middle.stl", "T_small.stl"]`: every model file, and not the text file |
| 4 | `project_import {paths: [W/not_a.zip]}` | as after 3 | refused, `unreadable_archive`; `undo_steps()` `[]` |
| 5 | (new project) cube `inside_zip` 10 at (128, 128), `project_export {format: "3mf", path: W/project.3mf}`; the test puts it alone into `W/project.zip` as `project.3mf` | `[inside_zip]` | |
| 6 | (new project) cubes `first` 20 at (80, 128) and `second` 30 at (176, 128); `project_import {paths: [W/project.zip]}` | `[first, second, inside_zip]`, `inside_zip 12/1000 [10, 10, 10]` | reply `added` 1, `archives[0].entries` `["project.3mf"]`; `dialogs()` empty; `project()` `Untitled · none · dirty`: the project's objects join those there, its file does not become the project's, and an archive names no project, as Import ZIP Archive names none; `undo_steps()` `["Import Objects: project.zip"]` |

**Scenario: a STEP file's mesh options**

Each call with the STEP file replies `blocked_by_dialog` with `dialogs` one dialog titled `STEP file import
parameters`; the test answers it OK and follows the call's job to its end, and `dialogs()` is then empty.

| # | Step | Parts after (object 0) | Also checked |
|---|---|---|---|
| 1 | `project_import {paths: [named_single_body.step]}` | `part 4/166.667` | the job's reply `added` 1; 1 object; `project()` `named_single_body · none · dirty` |
| 2 | `part_add {object: 0, type: "part", path: named_single_body.step}` | `part 4/166.667`, `part 4/166.667` | part 0's `id` as after 1 |
| 3 | `part_add {object: 0, type: "part", shape: "cube", size: [5, 5, 5]}` | `part 4/166.667`, `part 4/166.667`, `part 12/125` | no dialog; parts 0 and 1 keep their ids |
| 4 | `part_replace {object: 0, part: 2, path: named_single_body.step}` | `part 4/166.667`, `part 4/166.667`, `part 4/166.667` | parts 0 and 1 keep their ids; part 2's `id` is the `part_id` the reply gives and not the cube's: the cube's mesh was replaced by the file's; 1 object |

**Scenario: exporting meshes**

It starts with cube `small` 20 at (100, 100) and cube `big` 30 at (160, 100): together they span 90 to 175 in x, 85 to
115 in y and 0 to 30 in z. A binary STL is 84 bytes and 50 a triangle: 684 bytes for a cube, 1284 for two. The test
creates the folder `W/each` before it exports into it. Each export's `undo_steps()` is `[]`, and `project()` stays
`Untitled · none · dirty`.

| # | Step | Written | Also checked |
|---|---|---|---|
| 1 | `project_export {format: "stl", path: W/small.stl, objects: [0]}` | `small.stl`, 684 bytes | reply `objects` `[{index: 0, name: "small"}]`, `bytes` 684 |
| 2 | `project_export {format: "stl", path: W/both.stl}` | `both.stl`, 1284 bytes | reply `objects` `[{index: 0, name: "small"}, {index: 1, name: "big"}]`, `bytes` 1284 |
| 3 | `project_export {format: "stl", path: W/each, per_object: true}` | `each` holds exactly `big.stl` and `small.stl`, 684 bytes each | reply `objects` as in 2, `files` `[W/each/small.stl, W/each/big.stl]`, no `bytes` |
| 4 | the same again | `each` holds exactly `big(1).stl`, `big.stl`, `small(1).stl`, `small.stl`, 684 bytes each | reply `files` `[W/each/small(1).stl, W/each/big(1).stl]`: a name the folder has gets a number |
| 5 | `project_export {format: "drc", path: W/both.drc}` | `both.drc` | reply `objects` as in 2, `bytes` the file's size |
| 6 | `project_export {format: "3mf", path: W/one.3mf, objects: [0]}` | nothing | refused, `objects_not_taken` |
| 7 | `project_export {format: "stl", path: W/five.stl, objects: [5]}` | nothing | refused, `no_such_object`, with the words "Object 5 does not exist (the project has objects 0..1)" |
| 8 | `meshes()` | | `[small, big]` at (100, 100) and (160, 100): the exports moved nothing |

Each file is then read back with `reimport()`; the files of a folder are imported one call each:

| File | Meshes |
|---|---|
| `small.stl` | `small.stl 12/8000 [20, 20, 20]` |
| `both.stl` | `both.stl 24/35000 [85, 30, 30]`: both cubes as placed, in one object |
| `each/small.stl`, then `each/big.stl` | `small.stl 12/8000 [20, 20, 20]`, `big.stl 12/27000 [30, 30, 30]` |
| `both.drc` | `both.drc 24/35000 [85, 30, 30]` |

**Scenario: exporting the model as a 3MF**

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | cube `box` 20 at (100, 100); `project_export {format: "3mf", path: W/model.3mf}` | `[box]` at (100, 100) | reply `bytes` the file's size; `project()` `Untitled · none · dirty`: the export does not make the file the project's; `recent()` lacks `model.3mf`; `undo_steps()` `[]` |
| 2 | `undo` | `[]` | undo takes back the cube: the export left nothing for it to take back first |
| 3 | `reimport(W/model.3mf)` | `box 12/8000 [20, 20, 20]` | reply `added` 1; `project()` `model · none · dirty` |

**Scenario: exporting the sliced file**

The scenario first turns on the printer's `use_3mf` ("Use 3MF instead of G-code") with `settings_set {type:
"printer"}`, as a person does on a printer outside Bambu Lab to be offered Export plate sliced file
(`MainFrame::available_print_actions`), and reads it back as `"1"`; it stays an unsaved edit of the printer, which the
next scenario's new project discards. After each export, `project()` is `Untitled · none · dirty`, `sliced_flags()` is
as the row's plates say (step 1: `[false]`, step 2: `[true]`, from step 3: `[true, false]`), and `undo_steps()` is `[]`
unless the row names a step. Steps 1 and 6 export while the plate written is not sliced, and each writes its
file: an export given a path works out no file name, so the Klipper process's `filename_format`, which an unsliced
plate's placeholder statistics cannot fill (`{filament_type[initial_tool]}`), is not read
(`Plater::export_gcode_3mf()` builds the file dialog's default name only when no path is given).

| # | Step | Plates after | Also checked |
|---|---|---|---|
| 1 | cube `a` 20 at (128, 128), `app_wait {for: "idle"}`; `project_export {format: "sliced_3mf", path: W/unsliced.gcode.3mf}` | `"" 1 *` | written; `sliced()` holds no G-code entry: a plate that is not sliced is written without G-code |
| 2 | `plate_slice`, then `project_export {format: "sliced_3mf", path: W/sliced.gcode.3mf}` | `"" 1 *` | outcome `ok`, `layers` 100; `sliced()` `[Metadata/plate_1.gcode]`, its `gcode()` `layers` 100, `labels` 1 |
| 3 | `plate_add`, then `object_add_primitive {shape: "cube", size: [20, 20, 20], name: "b", plate: 1}`, `plate_slice {plates: 0}`, `plate_select {plate: 0}` | `"" 1 *` · `"" 1` | outcome `ok`, `layers` 100 |
| 4 | `project_export {format: "sliced_3mf", path: W/half_current.gcode.3mf}` | as after 3 | `sliced()` `[Metadata/plate_1.gcode]` |
| 5 | the same with `plates: "all"`, to `W/half_all.gcode.3mf` | as after 3 | `sliced()` `[Metadata/plate_1.gcode]`: plate 1, not sliced, adds none |
| 6 | the same with `plates: 1`, to `W/half_1.gcode.3mf` | `"" 1` · `"" 1 *` | `sliced()` holds no G-code entry; the plate named is made current first, `undo_steps()` `["select partplate!", "select partplate!"]` |
| 7 | the same with `plates: 0`, to `W/half_0.gcode.3mf` | `"" 1 *` · `"" 1` | `sliced()` `[Metadata/plate_1.gcode]`; `undo_steps()` `["select partplate!", "select partplate!"]` |
| 8 | `project_export {format: "gcode", path: W/plate_1.gcode, plates: 1}` | `"" 1 *` · `"" 1` | refused, `plate_not_sliced`; no file; `undo_steps()` `[]`: a refused export leaves the current plate and the undo history as they were |

**Scenario: the G-code written**

The layer height is set so that the G-code shows a setting reaching the print: a 20 mm cube in layers of 0.25 mm,
the first included, is 80 layers, against 100 at the process's 0.2 mm.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | cube `box` 20 at (128, 128), `settings_set {values: {layer_height: 0.25, initial_layer_print_height: 0.25}}`, `plate_slice` | `[box]` | outcome `ok`, `layers` 80; `sliced_flags()` `[true]` |
| 2 | `project_export {format: "gcode", path: W/box.gcode}` | `[box]` | reply `{export: "ok", path: W/box.gcode}`; the file is there when the reply comes; `gcode()` `layers` 80, `labels` 1, `config.layer_height` `0.25`, `config.initial_layer_print_height` `0.25`; `undo_steps()` `[]`; `project()` `Untitled · none · dirty` |
| 3 | `project_export {format: "gcode", path: W/box_again.gcode}` at once | `[box]` | the same reply with its path, and the same `gcode()`; `undo_steps()` `[]`; `project()` `Untitled · none · dirty` |
| 4 | `sliced_flags()` | `[box]` | `[true]`: an export leaves the slice as it was; `project()` `Untitled · none · dirty` |
| 5 | `undo` | `[]` | undo takes back the cube: the exports left nothing for it to take back first |

**Scenario: a G-code export while a slice runs**

A second client, `OrcaMcp(port, token)` of the same session, runs in a thread of the test and starts the slice; the
test's own client exports meanwhile. The second client follows its call to its end with `await_job()`, so a reply that
outlasts the reply budget is followed as a job.

An attempt: `settings_set {values: {wall_loops: w}}` with a value neither the process (3) nor an earlier attempt has
(4, then 5, 6, ...), so that the slice starts over from the walls; `app_wait {for: "idle"}`; the second client starts
the action; the test reads `app_state.slicing` every 5 ms with `until()` until it is true or the second client's call
has ended (30 s without either fails), and on `slicing` exports at once to `W/during_<w>.gcode`. The app decides when
the slice starts and ends, and nothing lets the test hold it, so the overlap is a race the test retries: a Benchy's
slice lasts seconds and the export follows within a few tens of milliseconds, but the slice can end in between, or
before the test sees it. An attempt overlaps when the export is refused with "Busy: job": MCP's admission
(`McpCore::admit()`) gives that refusal only while an operation is held, and the second client's slice is the only one.
An attempt that does not overlap (the slice ended before the test saw it, or the export went through) is followed by
the next one once the second client's call has ended; the check fails when three attempts in a row do not overlap. A
refusal with other words fails at once.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | `project_import {paths: [3DBenchy.drc]}`, `app_wait {for: "idle"}` | `[3DBenchy.drc]` | reply `added` 1; `project()` `3DBenchy · none · dirty` |
| 2 | attempts with `action_run {id: "slice_and_preview"}` until one overlaps | as after 1 | the overlapping export refused, with the words "Busy: job"; its file `W/during_<w>.gcode` not there; `project()` `3DBenchy · none · dirty`. Its undo steps are not read: the interval holds the second client's slice, and Slice All selects plate 0 first, which logs a `select partplate!` step (`on_action_slice_all()` calls `Plater::select_plate()`) |
| 3 | the second client's call ends; `app_wait {for: "idle"}`; `project_export {format: "gcode", path: W/after_preview.gcode}` | as after 1 | that call's result `status` `done`, outcome `ok`, `layers` N; `sliced_flags()` `[true]`; the export's `export` `ok`; `gcode()` `layers` N and `config.wall_loops` the last attempt's `w`; `app_state.tab` `preview`; `project()` `3DBenchy · none · dirty`; `undo_steps()` `[]`, its mark set after the second client's call has ended |
| 4 | 2 and 3 again with `action_run {id: "slice_all"}`, the attempts going on with the next unused `w`, and the file `after_all.gcode` | as after 1 | the same checks, N being this result's `layers` |

**To settle in review:**
- A refused export of a plate by index selects the plate first (sliced file, step 8). `Ops::start_gcode_export()`
  (`src/slic3r/GUI/Operations/ExportOps.cpp`) calls `plater.select_plate(*args.plate)` and only then
  `Plater::export_gcode_to()`, which refuses an unsliced plate, so the refusal leaves plate 1 current and its "select
  partplate!" undo steps (`Plater::select_plate()` takes `take_plate_selection_snapshot()`). The fix, in
  `ExportOps.cpp`: an export that names a plate refuses, on that plate and before it selects it, all it can know
  there. `start_gcode_export()` makes `export_gcode_to()`'s checks in its order on the named plate:
  `plater.is_export_gcode_scheduled()` (`export_already_running`), `plater.is_slicing_any()` (`slicing_in_progress`),
  `plater.plate_validation_error(*args.plate)` not empty (`slice_has_errors`) and
  `plates.get_plate(*args.plate)->is_slice_result_valid()` (`plate_not_sliced`), and calls `select_plate()` only after
  them. `export_sliced_3mf()` checks the named plate's slice error the same way before `select_named_plate()`. What can
  fail only once the export runs on the selected plate (`export_not_started`; `sliced_file_not_written` for a
  configuration that fails validation, a post-processing script that fails on a printer outside the Bambu network, or a
  failed write; `file_not_written` of `export_model_3mf()` and `export_stl()`) selects the plate that was current back,
  so the current plate is as it was, with both selections' steps left in the undo history. The step expects the fixed
  behaviour: plate 0 current and no undo step.
- Making another plate current logs two "select partplate!" undo steps (sliced file, steps 6 and 7):
  `Plater::select_plate()` takes one, and `PartPlateList::select_plate()` takes another when the index changes
  (`PartPlate.cpp`, "erase unnecessary snapshot"). This is the app's own code, left as it is: `main` takes both
  snapshots (`take_snapshot("select partplate!")` in each function); this branch only routes them through
  `take_plate_selection_snapshot()`. The steps expect the two.
- An object index the project lacks (exporting meshes, step 7) is refused by the target check,
  `IndexCheck::object()` (`src/slic3r/GUI/Mcp/McpTargets.cpp`), with "object index 5 is out of range" and no code,
  before `Ops::export_objects_refusal()` runs, so the export's own refusal for it, `no_such_object`, never answers. The
  fix: `project_export`'s handler (`src/slic3r/GUI/Mcp/McpToolsProject.cpp`) runs `Ops::export_objects_refusal()` on
  the indices `objects` gives before `resolve_targets()`. The step expects the fixed behaviour: `no_such_object`, "Object
  5 does not exist (the project has objects 0..1)".
- `project_import` of model files leaves an untitled project named `Untitled`, while File > Import names it after the
  first file without its extension (`Plater::add_file()`, `p->set_project_name(from_u8(paths.front().stem()...))`).
  `Ops::import_models()` (`src/slic3r/GUI/Operations/ProjectOps.cpp`) calls `load_files()` alone, and `Plater` has no
  public way to set the name (`Plater::priv::set_project_name()` is private). The fix: a new public
  `Plater::name_untitled_project(const boost::filesystem::path& file)` that, when `get_project_name()` is `Untitled`,
  sets the name to the file's name without its extension through `p->set_project_name()` and updates the title
  (`wxGetApp().mainframe->update_title()`), leaving the project's file unset, and does nothing otherwise; and
  `import_models()`, once the load has added objects, calls it with its first path that is not a ZIP archive. An
  archive alone names nothing, as Import ZIP Archive names nothing (`Plater::preview_zip_archive()`). The steps expect
  the fixed behaviour (importing models, steps 1, 3 and 6; a STEP file's mesh options, step 1; model 3MF, step 3; the
  export while a slice runs). The recent projects stay as they are: `add_file()` adds a model there only with the
  `recent_models` preference, which is off.
- Opening a saved project with no objects (saving, step 2) leaves the project named `""` with no file, and the loader
  warns that the file holds no geometry. `Plater::load_project()` sets the project's file only when something loaded,
  and `reset()` has emptied the name; `load_files()` shows the no-geometry `Warning` whenever it loaded no object
  (`Plater.cpp`). This is the app's own code, left as it is: `main` has both (the condition there is `!res.empty()`;
  this branch only lets a Design-tab recipe count as loaded), a bug for a fix of its own. The step expects what the app
  does today.
- A failed import keeps its undo step (importing models, steps 9 and 10): `Ops::import_models()` takes the snapshot
  before loading and says the step stays, as after a failed File > Import.
- The undo steps a project replacement logs: `reset()` takes "Reset Project" and `new_project()` "New Project", or
  `load_project()` "Load Project", each a project separator that clears the history (`Plater::priv::take_snapshot()`),
  and `load_files()` takes none. The new-project steps 1 and 2 and the open step 2 expect exactly those two.
- A project's `.3mf` imported as models opens no dialog (importing models, step 6). The removed replay was ready to
  cancel one; `load_files()` with `LoadModel` alone skips the project-config messages, and the staged 3MF path offers
  no options for a 3MF project.
- An export while a slice runs (step 2 of that scenario) is refused by MCP's admission ("Busy: job ... running an
  operation"), since the slice is the second client's operation (`docs/HLSD/mcp-server.md`, Admission). The operations
  layer's own refusal, `slicing_in_progress`, answers only a slice no MCP call holds, such as a person's; no tool
  starts one without holding it.
- After New Project's question is answered Transfer (a new project, step 8), and after any new project or open, the
  project reads clean though the process differs from its stored preset: `new_project()` copies the edited presets into
  the saved copy the project's dirty state compares with (`update_saved_preset_from_current_preset()`, read by
  `has_unsaved_preset_changes()` in `ProjectDirtyStateManager::update_from_presets()`), while `unsaved()` compares with
  the preset as stored (`compare: ":saved"` finds the stored preset). After an open (opening, "opened") `unsaved("process")`
  is exactly `[wall_loops]`: the project's own `wall_loops` 5 is the one key that differs from the 0.20mm Standard
  @MyKlipper process it names.
- The layer count of the 0.25 mm layers (the G-code written): 0.25 is exact in binary, so the 80 layer tops fall on
  0.25 to 20.0; if the slicer adds a layer, the count is 81 and the expectation moves with it.
- A DRC file is read back exactly (exporting meshes): with `drc_bits` at its default 0, which Starts from checks,
  Draco stores the vertices without quantising them. If the reimport differs, the DRC rows take `mesh_volume` within 0.1 % and the size
  to 0.01 mm.

**Not checked:**
- The exports and the save of a project with no objects (`3mf` written, `stl` and `sliced_3mf` refused `no_objects`,
  and a `.gcode.3mf` name refused): `test_operation_routes`.
- A file per object and the toolpaths of a project or Preview with none, a ZIP entry the archive lacks, and the filament
  slot refusals (the only slot, a mix that uses the slot, a mixed slot, too many slots): `test_additions_refusals`.
- The menu items' routes with their file dialogs: the Export, Save, Import and Load Project actions, which run this
  test's commands through a file picker, are run by no test as actions; which actions open a picker is
  `test_person_forms`', and an action's command and arguments the catalog's unit test checks
  (`tests/slic3rutils/test_command_catalog.cpp`).
- Sending G-code: `test_send_gcode`, whose "No address" has the Send action of a printer with no print host refused
  `no_print_host`, nothing uploaded and the printer preset unchanged, and which uploads to a print host and reads its
  errors. A Send with no objects or with the plate sliced meets the same refusal: `Ops::print_route()` refuses a printer
  with no host before it reads the project or the slice (`PrinterOps.cpp`). A Send on the Bambu network, refused or
  not: no test, as `test_send_gcode`'s entry says.
- What `test_slice_preview` checks on the same code paths. The G-code export refused for a project with no objects, a
  plate not sliced and a slice with errors, and the sliced file of a slice with errors refused
  `sliced_file_not_written`: its nothing to slice, steps 6 and 10, and slicing by route, step 4. A G-code export of a
  plate the project lacks, which `Ops::start_gcode_export()` refuses `no_such_plate` itself where the other formats'
  `select_named_plate()` does: its plate index, step 2. The Preview's toolpaths as OBJ (`Ops::export_toolpaths_obj()`,
  the one route of `toolpaths_obj`), read with `obj_facts()`: its layers, steps 17 and 18.
- Exports that run the code of one kept here. A per-object DRC export runs exporting meshes' step 3 loop, which writes
  each object with the format's writer, and step 5's DRC writer (`Plater::export_stl()`). The sliced file of all plates
  with plate 1 current is the sliced file's step 5, since `Plater::export_gcode_3mf()` writes every plate whatever plate
  is current, and the current plate's sliced file while that plate is not sliced is its step 1.
- Opening a project under the Preferences' load behaviour: `test_project_open_settings`. A plate's names, locks and own
  settings through the project file: `test_objects` (what the project keeps).
- Two exports that overlap: a cube's G-code is written in well under a second, so no client can reliably send a second
  export while the first runs; MCP's refusal of a change during an operation is shown here for a slice and by
  `test_runtime_calls` for an arrange.
- What `undo` does once a new or opened project has cleared the history: the scenarios check the steps that clear it,
  and an undo with nothing to take back is the Undo command's, not the files'.
- The NewProject and ProjectOpened plugin events: they need a plugin that reports them.
- A G-code file opened with `project_open`: `test_additions_refusals` opens one; the exported G-code here is read as
  text.
- The filament slots in the project file: merging a slot into another and the mixed filaments a project keeps are
  `test_filaments`', which saves and reopens both. The STEP dialog answered Cancel, and its options' effect on the
  mesh.
- Inside the files, beyond what the readers take: a 3MF's thumbnails and metadata, the `.mtl`'s materials, and the
  G-code's moves beyond its layer count, label ids and configuration block.
- `overwrite` on an export: it is the same path check as `project_save`'s (`resolve_output_path()`), which the saving
  scenario checks.
- A Bambu Lab printer's own files: the `; model label id:` line and the header's time line of its G-code. The files are
  read as the Klipper printer writes them; what the two printers write alike (layers, configuration, sliced file
  entries) is checked once.
- The recent projects and user presets other tests left: their names, order and number. A row asks only about a file
  in `W` or the preset "MCP Files PLA", which are this test's.
- A remembered answer to the save questions (`save_project_choise`, `save_preset_choise` in the configuration), which
  would skip New Project's questions: no tool sets or clears one, and no test answers a question with Remember my
  choice.
- The preferences Starts from does not read back, such as the 3MF import's name options: a project's part names win
  over them (`Plater::priv::load_files()`), so the 3MF files here do not read them.

**Strength:** Strong — every export and save is read back from its file, and every refusal by its code or words with
the state written out after it.
