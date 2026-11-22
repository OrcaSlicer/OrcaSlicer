# Design tab

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## The Design tab on a shared data folder

Every test here runs on an app launched with `--cad`, which turns the Design tab on for that run and writes nothing
to the data folder. What the Design tab reads from the folder:

- the selected printer's bed, which it draws, which sizes the plane squares a click picks to open a sketch, and
  whose centre is `modeling_origin`, the point the base planes pass through: (125, 125, 0) on the Generic Klipper
  Printer;
- the Origin and Bed switches of its view (`design_show_origin`, `design_show_bed`), which `view_set` writes, and
  which `cad_gui.reset_switches()` sets as a new folder has them, Origin off and Bed on, for every test here;
- the Commit to Plate mode (`design_commit_mode`), which only a pick from the button's list writes, and no test
  makes one;
- the Text dialog's last font and height (`cad_text_font`, `cad_text_height`), which the dialog's OK writes and a
  text given no face or height takes;
- the weld rule of a new sketch (`auto_close_sketch_loops`), which no test writes, and the sidebar's layout, which
  the app saves only when it closes.

What the app keeps until it closes, beside the baseline `start()` puts back: the design itself, which each test and
part clears with `new_design`; the view's helper planes and axes (keys P and A) and its section view, which
`reset_switches()` turns off with the switches above; and the Design camera, which `CadGui.start()` sets
(`reset_view()`) and the replays point themselves.

So every Python test here starts with `start(client)` on the Generic Klipper Printer, and a replay whose checks
follow the bed selects that printer in its first steps. The only check that writes out a value the bed fixes is
`cad_sketch_to_solid`'s 125, the Klipper bed's centre, and that replay selects the Klipper printer itself; no other
check changes with the printer. The main window is the baseline's 1600 x 900 at every start. A project a test leaves
unsaved stays in the app's auto-backup, in that test's own temporary folder, and the data folder's settings point to
it (`last_backup_path`): while that folder exists, as it does after the test failed, the next launch on the data
folder opens on a prompt to restore it, which `orca_mcp.py launch` answers No, so the test after it starts on an
empty project.

## `test_cad_replays`
`test_cad_replays.py` · Python, 176 checks (178 off Windows)

**Tests:** The Design tab driven through its commands alone, in one app, one part after another:
- the six replays in `tests/`, `cad_sketch_to_solid`, `cad_sketch_edit`, `cad_topology_targets`, `cad_features`,
  `cad_doc` and `cad_canvas`, each run with `replay_run`, as `test_runtime_calls` runs `smoke_test.json`;
- round_trip: that a Design session made through `tool_call` and exported with `replay_export` replays to the same
  part;
- slow_build: that the app stays inspectable while a long Design rebuild runs off the UI thread behind the progress
  dialog;
- examples: that every example `tool_search {tool}` documents for the Design commands, and for `undo` and `redo`,
  still runs as written.

The replay files are the test's input; the sweep runs none of them on its own. `test_cad_failed_job` stays a test of
its own, since it runs with a reply budget of 4 s.

**Starts from:** Each part starts from the same state, whatever the part before it left: `start(client)`, the
baseline on the Generic Klipper Printer with `0.20mm Standard @MyKlipper` and `Generic PLA @System`, which closes
the open dialogs and what `cancel_tool` closes in the Design tab (a card, a sketch, the Text dialog, a plane pick);
then `reset_switches()` on the Design tab, and Prepare shown again. So a part that fails with a dialog or a sketch
open does not fail the next one. Each part then sets its own start, given with it below.

**Leaves:** What examples, the last part, leaves: the project unsaved, holding the objects its two `commit_to_plate`
examples sent to the plate. The recent projects name `design.3mf` (cad_doc) and `design_only.3mf` (cad_canvas), in
the folder the run makes in its own temporary folder and prints, which also takes the replays' `result.json` and
files, round_trip's exported session and its run, and slow_build's two screenshots. In the folder's settings only the
Origin and Bed switches, as a new folder has them: `sketch_text` reads the Text dialog's last font and height and
writes neither, and the `dialog_auto_answer` rule cad_doc registers ends with the replay. A rerun on the same data
folder starts each part over from `start()` and `reset_switches()`.

**Decides by:** Exits 1 when any check prints FAIL or a part breaks off: `Checks.scenario` records a raise, in the
part or in the reset before it, as "<part>: runs to its end", and the next part still runs. A replay is one check,
"<replay>: the replay passes", which holds when `replay_run`'s reply has `passed` true; on a failure its detail names
the failed step, its arguments, and its error or the expect that missed. A replay stops at its first failed step: a
call that errors, unless the step is `optional`, or an expect that misses. Each part's checks follow, in the order
the parts run; the checks of round_trip, slow_build and examples print after the part's name, as "round_trip: the
exported session replays".

**Part `cad_sketch_to_solid`** · `tests/cad_sketch_to_solid.json`, 38 steps, 59 expects, 1 optional

Sketch to solid through the Design commands. A sketch on a base plane passes through `modeling_origin`. An empty
`sketch_finish` is refused and leaves the sketch open. A fillet is made inside the sketch. Then come extrudes (whole
sketch as a new body, one loop alone, and a sketch on a body face joined onto that body) and a revolve about the
sketch's only construction line.

Its start: the Generic Klipper Printer, which its first step after `app_wait` selects (`preset_select {type:
"printer", name: "MyKlipper 0.4 nozzle", preset_changes: "discard"}`), since its checks of where the XZ sketch and
the bracket lie follow the bed; then its own `project_new`, `window_set` 1600 x 900 and `new_design`. On that bed the
XZ sketch's frame origin and the bracket's lowest corner are both at (125, 125, 0).
- Sketch frames: the XZ sketch reports `plane` XZ with normal +Y and frame origin x 125 (exact), the Klipper bed's
  centre. The sketch on face 10 has frame origin z 8 ± 1e-3, x axis world −Y and y axis world +X (exact).
- Sketch contents: 6 entities after drawing and 7 after `sketch_fillet`. The fillet is entity 6 with
  `released_constraints` 1 and `solve_ok`. Committed, it is an `arc` of radius 3 ± 1e-4. Loop areas: 265.93–265.94
  and 9600 ± 0.01. The plate is loop 5. Each commit's `sketch_index`/`features` is checked.
- Empty commit: the next `sketch_describe` (no `sketch` argument, so it errors when no sketch is open) must list no
  entities. The next commit must be `sketch_index` 0.
- Solids: the `extrude` and `revolve` replies (`extrude_index`, `operation` new_body/join, `body`, `bodies`, `loop`,
  `end` blind, `axis_entity` 4). `body_mass_properties` volumes are within 0.03 mm³ for the jig, lip and single loop,
  0.16 for the ring and ±2 for the bracket. `body_describe` gives 11 faces with known kinds, and the bboxes run z
  0–30, then 18 after the lip. The bracket's lowest x is 125 ± 1e-3.

**Part `cad_sketch_edit`** · `tests/cad_sketch_edit.json`, 39 steps, 54 expects, 3 optional

The sketch's own tools through the Design commands: polygon, slot, ellipse, spline, trim, extend, chamfer, fillet,
linear and circular patterns, rotate and move. It also adds constraints and dimensions from entity picks and from
named points (`origin`): a radius that over-constrains is rolled back and a constraint is deleted. Each result is
read back from `sketch_describe` and `sketch_list_constraints` and from the extruded solid's volume.

Its start: its own `project_new`, `window_set` 1600 x 900 and `new_design`. Every value it reads is in a sketch's own
coordinates or a volume, so no check depends on the printer.
- Hex standoff: `sketch_draw`'s shape bookkeeping (`count` 6, `first` 6, 7 entities). The first edge lies at x 4 ±
  1e-4. The hexagon loop has area 55.4256–55.4257 and holds the bore as hole 0. Volume 630.0–630.1.
- Trim/extend: `sketch_trim` cuts line 0 to end at x 0 ± 1e-4, with p0 still −10. `sketch_extend` takes line 2 to x
  30 ± 1e-4. The sketch is then cancelled, and the next commit being `sketch_index` 0 shows that nothing was
  committed.
- Plate edits: the rectangle has `dof` 4. The chamfer and fillet create entities 4 and 5, then there are 6 in all.
  Shape indices for the slot, ellipse, circle and spline, and 13 entities. Linear pattern: `first_index` 13, `added`
  4. Rotate pivot (−10, 10) ± 1e-4. Move reports entity 11. Circular pattern: `first_index` 17, `added` 3, `bound` 3.
  Copy 18 is centred at (22, 0) and the rotated slot's p0 x is −12, both ± 1e-4. Buildable.
- Constraints: 25 before. The distance dimension is added at index 25 with an `origin` ref and `solve_ok`. `equal` is
  stored as `equal_radius`. After the optional radius there are 27, with item 26 `equal_radius`. Deleting 25 removes
  a `distance` and leaves 26 in both the live (`scope` live) and the committed (`scope` committed) sketch.
- Final solid: 11106.1–11106.25 mm³.

**Part `cad_topology_targets`** · `tests/cad_topology_targets.json`, 33 steps, 69 expects, 1 optional

Reading a body's topology and building on what it finds. It covers `body_describe` filters (convexity, face kinds,
direction, extreme, adjacency, paging), a fillet on an inside edge found that way, and M4 countersinks seated on a
face on a pilot hole's axis (`at` a cylindrical face). It also covers `measure` between faces and axes, datum planes
(midplane, offset) with a sketch on one, and a face push/pull that joins its own body.

Its start: its own `project_new`, `window_set` 1600 x 900 and `new_design`. It reads positions in a plane's frame,
heights and distances, none of which follows the bed's size.
- Topology reads, all ± 1e-4 or tighter:
  - Two concave edges: the first is id 7, a line next to face 2, with 3 faces in total.
  - After the fillet: one internal cylindrical face of radius 5.
  - Facing −X: 2 faces, with face 0 of area 985.86–985.87.
  - Two internal conical faces: half-angle 45°, radii 4.5 and 2.25, axes −X, 6 adjacent edges, the first convex and
    on face 0.
  - The −z extreme is face 10.
  - Paging: `faces/4` present, `faces/5` absent, `next_offset` 5.
- Countersinks: `off_plane` 5 ± 1e-4 for both. The first has `y` 0 and frame normal +X.
- Measures, all ± 1e-4: axis to bottom face 25, at 90°; axis to axis 25; cone to front face 0 (≤ 1e-4); cone to back
  face 2.75; plate gap and min distance 5.
- Datums: `plane_index` 7 (normal −X) and 8 (origin z 70). The sketch on datum 8 is at z 70, is `sketch_index` 9, and
  makes body 1 spanning z 70–74.
- Push/pull: `operation` join on body 0, `sketch_index` −1, face 8, normal +X. Volumes 10352.82–.84 and 12689.20–.22.

**Part `cad_features`** · `tests/cad_features.json`, 45 steps, 59 expects, 6 optional

Threads, sweeps, lofts and plane splits through the Design commands:
- an internal thread on a bore, with its size inferred from the bore, and the same thread placed on the XY plane;
- a helix with round and square wire swept along it, both wires sketched before the first sweep, since a commit
  after a sweep rebuilds the sweep again (measured: the square wire's commit took 4.9 s after the round wire's sweep,
  under 0.3 s before it);
- a square-to-circle loft up to a datum plane;
- a split by that datum, then a split by XY flipped that keeps one side.

Its start: its own `project_new`, `window_set` 1600 x 900 and `new_design`. It reads positions in a plane's frame,
heights and volumes, none of which follows the bed's size. The thread on the bore is the M5 hex standoff at the
design's origin, which the Design tab puts at the bed's centre, (125, 125) on the Klipper bed.
- Thread on the bore: `thread_index` 3, `designation` M5, `internal`, `pitch` 0.8 ± 1e-4, `length` 15 ± 1e-4, one
  body. The body is solid at 600–614 mm³. Feature 3 is a Thread, with `undo_depth` 3.
- Thread on XY: after `undo` there are 3 features. The thread is at x = y = 0 on body 0, as M5, and the volume is
  again 600–614.
- Helix and sweeps: `helix_index` 0, with start tangent y 0.9968–0.9969 and z 0.0793–0.0794. The wires' sketches are
  `sketch_index` 1 and 2. The round wire is `sweep_index` 3, a new body, solid, 2638–2691 mm³. The square wire is
  `sweep_index` 4, body 1 of 2, 1492–1524 mm³.
- Loft: datum `plane_index` 0, sketches 1 and 2. `loft_index` 3 is body 0, solid, 11850–11861 mm³.
- Splits: `split_index` 4, upper body 0 and lower body 1, normal +Z, the bboxes meeting at z 20 ± 1e-3. The flipped
  split keeps body 1 at z 0–10 with 2 bodies, normal −Z, and `undo_depth` 6.

**Part `cad_doc`** · `tests/cad_doc.json`, 83 steps, 139 expects, 0 optional

Reading and editing a Design feature as its card does: a variable bound to an extrude's `distance` and then removed,
and `feature_edit` under its own argument names and the aliases agents use (`index`, `params`, `values`). Handing the
design to the plate: Commit to Plate; a recommit that replaces the first object and is undone in one Prepare step;
and the bodies and assembly modes. A STEP export read back into a new design. The Design history and tree: undo/redo,
suppress, move and delete, body name, visibility and colour, and a save and reopen.

Its start: the Generic Klipper Printer, which its first step after `app_wait` selects (`preset_select {type:
"printer", name: "MyKlipper 0.4 nozzle", preset_changes: "discard"}`), since it sends bodies to that printer's plate
and its project carries the printer; then its own `project_new`, `window_set` 1600 x 900 and `new_design`. The
objects `object_list` reads land on plate 0 of the 250 x 250 bed. The rule for the "Load project: unsaved changes"
prompt it sets before the reopen holds only for the replay.
- Discovery: `tool_search` lists `feature_describe` as Design read-only and `feature_edit` as Design writable.
- Variables and edits: variable `h` = 12 shows as bound to `distance` in `describe_design` and in `feature_describe`
  (`params`, `expr`, `refs`, `used_by`, `editable`). After unbinding and removing it, `variables` is empty and
  `bound` is absent. `feature_edit` reports `changed` old/new pairs, and the `index`, `params` and `values` aliases
  each apply. Volumes 6000 and 9000 ± 0.01.
- Plate hand-off: the commit reply names object 0 "Design Body", 1 part, body 0 sent, on the `prepare` tab.
  `object_list` then shows 1 object of height 15 ± 0.01, then 20 after the recommit (which reports `replaced` 0), and
  15 again after one Prepare undo `action_run`. The bodies and assembly commits create objects 1, 2 ("Design Body 2")
  and 3 ("Design Assembly", 2 parts), for a total of 4.
- STEP round trip: the export has `bytes` > 0 and one body. `new_design` clears 3 features and 1 body. The
  `import_step` body's volume is 12000 ± 1.2.
- History and tree:
  - Undo/redo depths, with `distance` going 15 → 10 → 15.
  - The rename trims to "Base", is its own undo step (`undo_depth` 4 → 3), and is undone and redone.
  - Suppress reports `changed` true, then false, with body counts.
  - `feature_move`'s reply and the new feature order, then its undo.
  - `body_set` name and visibility show in `describe_design`. A hidden body is left out of `bodies_sent` and listed
    in `bodies_hidden`.
  - `body_delete` and `feature_delete` report their counts, and their undos restore them.
  - A colour given as `#3080ff` reads back as `#3080FF`.
- Reopen: after a save, a new project and an open, there are 4 features and body 1 is "Lid" with colour `#3080FF`.

**Part `cad_canvas`** · `tests/cad_canvas.json`, 32 steps, 33 expects, 5 optional

The Design view as a canvas target: `view_set` on the Design tab, and its refusals of Prepare's and Preview's fields;
screenshots of the canvas and of its widget; a click that picks the block, and a right click that lists the pick's
verbs; keys reaching the Design panel's key hook (Shift+E opens the Extrude card, and Escape with no target closes
it); the project's modified flag after a change made only in the Design tab, and reopening a design-only project with
`discard_changes`.

Its start: its own `project_new`, `new_design` and `window_set` 1600 x 900, the size its clicks at (30, 30) and at
the canvas's centre are aimed in. The Design view's `default` camera is its iso view fitted to the block, whatever
the bed, and the project it saves carries the printer it reopens with.
- View: `view_set` echoes tab `design` with camera `iso`, then `default`, then `fit_selection`. After an extrude,
  `app_state` reports tab `design`, `gizmo` null and the project dirty.
- Screenshots: the canvas and `wxGLCanvas` targets give source `canvas`, no `approximate` key and width > 0. The main
  window's width is > 0. `widget_list` finds one `OrcaSceneCanvas`.
- Pick: `widget_click` reports `clicked` canvas. The right-click menu's first item matches "of Extrude".
  `fit_selection`, refused earlier with nothing picked, now succeeds.
- Keys: `widget_list` "Extrude dist" finds 1 node after Shift+E and 0 after Escape. The next `extrude` returns `ok`.
- Project: after the save, not dirty, named `design_only`; after `set_variable`, dirty. Opening with
  `discard_changes` reports a project with 0 objects. `app_state` then shows not dirty, the name and path match, and
  no dialogs are open. The variable is gone and a second body exists.

**Part `round_trip`**

That a Design-tab session made through `tool_call` and exported with `replay_export` replays to the same part. The
session includes failed calls (an empty commit, a missed hole, a fillet the kernel refuses), which the export must
leave out; a shell that replies `ok` false, which stays in; and face and edge ids passed with a `generation`, which
the export must drop. The replay runs in a new project in the same instance.

Its start: `replay_export {clear: true}`, so that the export holds this part's calls alone, then its own
`project_new` and `new_design`. No check depends on the printer: the checks compare with `modeling_origin` as
`describe_design` reads it, and the missed hole's x 500 is in the sketch plane's frame, off the plate on any bed. The
part breaks off when a recording call other than the three expected refusals errors, or when the thread's job fails.
- Live session:
  - "a sketch on XY and the plate are centred on modeling_origin": the frame origin equals `modeling_origin` exactly,
    and the centre of mass x/y is within 1e-6 relative.
  - "an empty sketch_finish fails" and "a hole that misses fails as an error": any error.
  - "a fillet the kernel refuses fails as an error with the kernel's reason": any error text without "Invalid
    arguments".
  - "a shell thicker than the plate reports the rejected rebuild in its reply (ok false)": `ok is False`.
  - "the thread is M5, inferred from the bore".
- Export: "the export leaves out the failed calls" (one `sketch_finish`, no `hole`), "... leaves out the refused
  fillet and keeps the one that ran" (one `fillet`), "... keeps the shell" (one `shell`), and "the export records no
  generation".
- Replay: "the exported session replays" (`passed` true); "the same features" (type list) and "the same bodies and
  undo depth"; "the same volume and surface area" and "the same centre of mass", within 1e-6 relative of the live
  session.

**Part `slow_build`**

That the app stays inspectable while a long Design rebuild runs off the UI thread behind the progress dialog. The
rebuild is an M8 thread tapped 250 mm deep, 200 turns, which takes several seconds (measured: 7.7 s with other tests
running on the machine). While it runs, `app_state` and a main-window screenshot answer, the progress dialog is
listed and can be captured, and another Design command is refused as busy. The thread then lands as one feature.

Its start: its own `project_new` and `new_design`, and a 30 x 30 x 250 block. No check depends on the printer.
"Captures it" needs the main window wider than the progress dialog, as the baseline's 1600 x 900 is. The part breaks
off when an `app_state` or screenshot probe raises, or when the build is seen neither running nor ended within 300 s.
- Result: "the thread call succeeds" (the awaited result has `ok` true) and "one thread feature and one body".
- During the build, one probe runs. It fires once the build is seen running: when `widget_list` shows a "Design"
  dialog whose class contains "Progress", or on Windows, where no window `widget_list` lists is the dialog, when
  `describe_design` is refused as busy. A build seen ending first fires none, which fails the three checks below.
  - "app_state answers during the build".
  - "a window screenshot succeeds during the build": width non-zero.
  - "another Design command is refused during the build": the `describe_design` error, asked after the two calls
    above, contains "Busy".
- Not on Windows:
  - "the progress dialog is listed while it builds".
  - "a screenshot of the progress dialog captures it": width non-zero and narrower than the main window.

**Part `examples`** · one check per documented example: 156 today, 6 of them skipped

That every example `tool_search {tool}` documents for the Design commands, and for `undo` and `redo`, still runs as
written. Each example runs on a fresh empty design, with `from_call` arguments filled from earlier results.

Its start: before each example, what `start()` closes is closed as between parts (`checks.close_open()`), and
`new_design` discards the rest. Two examples send bodies to the plate with `commit_to_plate`, which places them on
the Klipper printer's 250 x 250 bed; no check reads where. The `sketch_text` examples give no face, so they take the
Text dialog's last one, and are checked only for running.

An example fails ("examples: <command>: <title>") when:
- a call raises: a tool error, a failed job (`call_command` awaits running jobs), or a first reply blocked by a
  dialog;
- a `from_call` reference cannot be resolved;
- a result dict has `ok` false, a non-empty `error`, or `interactive` set.

An example with `needs` prints `skip` and the part's closing line counts the examples and the skipped ones.

**Not checked:**
- cad_sketch_to_solid:
  - The optional empty `sketch_finish`: its refusal text. The refusal and "stays open" are covered only because the
    next step errors without an open sketch and the next commit must be index 0.
  - "Passes through `modeling_origin`": only the x of the XZ frame's origin and of the bracket's lowest corner is
    read, as 125, not their y (125 on this bed) or the frame origin's z, nor `describe_design`'s `modeling_origin`
    itself.
  - The holes on 32 mm centres and where the lip sits in its plane: only total volume and z extent.
  - `closed_loops/0/entities/6 exists` and `holes/4 exists` only say the arrays are long enough, not what they hold,
    though the exact values are known.
  - The bracket's volume band (±2 mm³) is about 20 times looser than its loop-area check implies.
- cad_sketch_edit:
  - The optional `sketch_trim` at [100, 100]: nothing reads lines 1 and 3 after it, and the sketch is cancelled two
    steps later. Neither the refusal nor "refused calls change nothing" is verified for it.
  - The optional fillet of radius 100 and the radius dimension are covered only indirectly. The next fillet must
    still be entity 5, and the constraint count must still be 27. Neither refusal text is checked.
  - What the distance and equal constraints do to the geometry. Both match the geometry already there, so they could
    not move it anyway.
  - The spline's `construction` flag, and where most edited entities end up (the ellipse, the other two pattern
    copies, the chamfer and fillet), beyond what the total volume implies.
- cad_topology_targets:
  - The optional tangent plane: its refusal text. "Changes nothing" is verified: the `describe_design` after it must
    have feature 8 a Plane, no feature 9, `undo_depth` 8 and `redo_depth` 0.
  - The two pilot `hole` steps have no expect. Their positions show up only in the later measures; the upper hole's
    50 mm height is checked only through the 25 mm spacing.
  - The second countersink's `y` and frame, and `edges/0/faces contains 2`, where the exact face pair is known.
  - Face and edge ids are hard-coded ("this build's"). A renumbering fails the part with no change in behaviour (a
    false fail, not a false pass).
- cad_features:
  - The six optional refusals: a planar face, the unknown `M5x1`, a sweep given as the path, a one-profile loft, the
    datum plane given as a loft profile, and a split plane above the body. Their texts are not checked, including the
    comment's "names the close ones". A wrongful success is caught only because the next real call checks its index
    (`thread_index` 3, `sweep_index` 4, `loft_index` 3, `split_index` 4) and `undo_depth`. A refusal that changed a
    body without adding a feature would pass: body 0 is not measured again after the refused sweep, for one.
  - The thread and sweep solids are checked only within volume bands of about 2 % (600–614, 2638–2691, 1492–1524). A
    wrong thread profile or wire orientation that stays inside a band passes.
  - The XY thread's `length` and `internal` are not read back, nor the side the flipped split discards: only `bodies`
    2 and body 1's bbox are checked.
- cad_doc:
  - `sketch_repair` only runs.
  - What lands on the plate after the hidden-body commit and the second bodies-mode commit: only the commit replies
    are read, with no `object_list`.
  - Most of the reopened design: body 0, the feature names ("Base"), visibility and volumes. `project_save` and
    `project_open` have no expect.
  - The new body's default colour only has to match `^#[0-9A-F]{6}$`.
- cad_canvas:
  - None of the five optional steps is verified. Nothing reads the tab, camera or selection after the refused
    `fit_bed`, the refused `fit_selection` with nothing picked, the refused `layer` 2, or the refused `mouse_gesture`
    at a model point, so "refuses what belongs to Prepare and Preview" is not tested. The refused `project_open`
    without `discard_changes` is not verified either: a silent open would pass; only a dialog left open would be
    caught, by the final `/dialogs` empty.
  - The three PNGs are saved but never compared. Nothing shows that they picture the Design view rather than
    Prepare's canvas.
  - The right-click verbs: only the header item is matched (by regex), not the verbs listed. The camera is read only
    from `view_set`'s own reply.
  - The reopened design's geometry: the part checks only that `bodies/1` exists, not that there is no third body.
- round_trip:
  - The replayed part is compared with the live session, not with known values; the volume is only printed. A
    modelling regression that changes both alike passes.
  - `modeling_origin` itself: the frame and the centre of mass are compared with it as read, not with (125, 125, 0),
    where the Klipper printer's bed puts it.
  - The missed hole's and the refused fillet's error texts. The fillet's edge id comes from the `body_describe` read
    before the first fillet changed the body, and is passed without a `generation`, so the refusal may concern a
    different edge than the comment means.
  - The exported replay carries no `expect`, so nothing checks that the replayed shell again replies `ok` false; only
    the end state is compared. Feature names and parameters are not compared either: only feature types are.
  - "Keeps the sketch open" after the empty commit has no check of its own. It is covered only because the next
    `sketch_draw` would raise.
- slow_build:
  - How long `app_state` or the screenshot takes. "Answers" only means the probe ran and returned. The time
    `app_state` took is printed only on failure. The Busy check that follows bounds it loosely, since the build must
    still be running after both calls.
  - On Windows the two dialog checks are dropped without a skip line, and the probe fires on a busy refusal of
    `describe_design`, so the Busy check repeats a refusal the probe waited for; asked after the probes, it still
    says that the build ran while they were answered.
  - The two screenshots are saved but never compared; "captures it" only compares widths. The thread's geometry
    (designation, volume) is not read.
- examples:
  - No result is compared with anything. The Design table's examples carry only `title`, `calls` and `needs`, so an
    example that builds the wrong geometry passes.
  - Examples with `needs` are skipped and count as passes on every platform. There are 6 today: two of
    `sketch_insert_svg`, `body_compare`, `import_step`, `import_mesh` and `export_step`.
  - There is no minimum count. If `tool_search {group: "Design"}` listed no commands, or the commands no examples,
    the part checks nothing and passes.
  - A result that is not a dict, or has neither `ok` nor `error`, passes without being examined.

**Strength:** Partial — the replays and round_trip check their results by exact index, count, coordinate or a tight
volume band, but cad_features holds the thread and sweep solids to 2 % bands, cad_canvas leaves its refusals and
screenshots unchecked, slow_build has no time bound, and the examples only run.

## `test_cad_failed_job`
`test_cad_failed_job.py` · Python, 9 checks

**Tests:** That a Design call whose job fails after its reply is left out of `replay_export`. A hole that misses a slow-rebuilding coil replies with a running job. An export made while it runs names it in `left_out_running`. The hole then fails, and the export after it keeps a later rename and leaves the hole out. That export replays to the same features and coil. The sweep runs it with a 4 s reply budget (`ORCA_MCP_REPLY_BUDGET_MS=4000`).

**Starts from:** `start(client)`, then a `new_design` it waits for before it records anything: an app's first Design command builds the Design tab, which outlasts the 4 s budget on a loaded machine, and a build journaled as a job would leave the export waiting at replay for a job the built tab never starts. Each session it records starts with its own `project_new` and `new_design`, and it waits for each set-up call's job before the next. No check depends on the printer: the hole's x 500 is in the plane's frame.

**Leaves:** its project unsaved, and nothing in the folder's settings; the export and the replay's run go to its own temporary folder.

**Decides by:** Exits 1 when any check prints FAIL. It also exits non-zero when a set-up call errors, or when the hole fails inside the budget with an error other than "removed no material" (or does so at 40 turns).
- Running job:
  - "the missed hole replies with its running job": `status` running.
  - "an export while it runs leaves it out and names it": `left_out_running` equals exactly `[{"tool": "hole", "job": <its id>}]`.
  - "the hole then fails as an error": the awaited error contains "removed no material".
- Export after: "the export leaves the failed hole out", "and keeps the rename made after it", "and leaves nothing out" (no `left_out_running` key).
- Replay (each step given a 600 s timeout):
  - "the exported session replays": `passed` true.
  - "the same features, the coil renamed": the type lists are equal and feature 2 is named "Coil".
  - "the same coil": volume within 1e-6 relative of the live one.

**Not checked:**
- The coil's volume is compared only with the live session's, not with a known value.
- The rest of the export (helix, sketch and sweep steps) is not inspected. Leaving out the `app_wait` made during the hole has no check of its own; a wrongly exported one would show only as a replay failure.
- Timing drives the set-up: the coil is sized from a timed 12-turn build and grown up to 40 turns. A machine too fast or too loaded fails the "running" checks rather than passing them. The race is the app's, the hole's job outlasting its reply and the export after it, and the test holds no fixed wait for it: every set-up call's job is followed to its end, and the hole's is read once, at the export (`app_wait {for: "job"}` with `timeout_s` 0).

**Strength:** Strong — the running-job bookkeeping, the failure text and the replayed result are each checked exactly.

## `test_cad_commands`
`test_cad_commands.py` · Python, 311 checks

**Tests:** What the Design commands' arguments do when the Design panel handles them in the running app:
operation words, suppress and align conflicts; the open sketch's commands; re-editing a committed sketch with
`sketch_edit` (replace, cancel, refusals, a failed replace and its replay); projection, text and SVG art;
`feature_edit` refs and picks made again in an edit card; `view_set`'s Design options; `select_geometry` and the
menu items that act on a selection; `confirm_tool` and `cancel_tool` over cards, sketches, the Text dialog and a
plane pick; `body_place`; the Boolean card; Speed Dial picks and their replay. One rung also checks that six
drawing tools chosen from the right-click menu agree with `sketch_draw`.

**Starts from:** `CadGui.start()`: an English UI, then `start(client)`, the Design view's switches as a new data
folder has them (`reset_switches()`), the window widened when the sketch ribbon overflows it, and an empty design in
`reset_view()`'s camera. What follows the bed is read, not written out: `modeling_origin`, for the XY frame and the
cameras that look at it, and where Commit to Plate places an object, which place compares only between two parts of
one object. The rungs that read the Origin switch set it first: off for the plane refusals, on for the plane pick.
confirm_refused_add presses Commit to Plate by its label in assembly mode, the mode the folder keeps while no one
picks another from the button's list. A Speed Dial pick names its action with its source, OrcaSlicer, so an action
of the same title that a plugin left in the folder adds is not taken. The dial closes when another window takes the
focus, as it does for a person, and on a desktop the sweep's apps share, another test's app opening a dialog or
starting takes it: `checks.dial_pick()` then opens the dial again and picks again, until the pick is taken or 90 s
pass.

**Leaves:**
- its last project unsaved: the objects place commits to the plate go with the new project action_pick_replay
  replays into;
- the Speed Dial's run counts of Polar array and Trim, which put them in its Recent list;
- the Text dialog's last font and height, as text_dialog's OK stores the ones the dialog opened with;
- the Design view's switches as `CadGui.start()` set them: design_view, select and cancel_plane_pick put back each
  one they change at the rung's end, and after a rung that raised midway `recover()` sets them again.

The SVG, the exported replays and the screenshots go to its own temporary folder. A rerun on the same data folder
meets only these: the run counts order the Speed Dial's list, and a pick finds its action by label in any order.

**Decides by:** Exits 1 when any check prints FAIL or a rung raises. `cad_gui.run()` records a raise as a FAIL
named after the rung, runs `recover()` and goes on to the next rung.
- Harness (`cad_gui.py`), part of the verdict: helpers raise, failing the rung, when `reset_switches` reads a switch
  back otherwise, `new_design` leaves features, `reset_view` leaves the camera more than 1e-3 off the zoom and
  target it asked for, no click along the XY plane's diagonal opens a sketch, `calibrate` probes land more than 0.5
  px off their clicks or add constraints, `cancel`/`confirm` leave a sketch open, `value` finds no field taking the
  keyboard, or a menu choice's `ran` is not the path asked for. The test's own `settled` (60 s), `keep_as_drawn` (8
  Esc presses) and `rename_selected` (no row editor) raise the same way. Each wait polls its state every 0.05 s for
  up to 30 s. The Speed Dial picks go through `checks.dial_pick()`, which waits up to 30 s for the dial to show
  before it searches and clicks. Whether a click opened a sketch or a value field is read once, after `app_wait
  {for: "idle"}` answers (`CadGui.settle()`, `opened_field()`), since the app answers it after the work the click
  queued: `keep_as_drawn` presses Esc only on a field that is open then.
- words: `operation` in the reply and in `feature_describe` equals the word given (`cut`, `join`); volumes from
  `body_mass_properties` are 3500, 5000 and 1000 mm³ to 1e-3; `feature_suppress` and `feature_describe` say
  suppressed and the bodies drop to 0; `align_conflicts` holds exactly the second align; `sketch_extend`,
  `sketch_pattern`, `sketch_transform` and `sketch_dimension` results are read by coordinate to 1e-6; two refusals
  by their text.
- shapes_match_tools, six shapes, each standing for one way a tool builds its shape: the polyline (lines that are no
  shape of their own, one undo step per line), the corner rectangle (a grouped shape on the axes, one undo step), the
  oblique rectangle (right angles at a slant), the tangent arc (tangency to a line already drawn), the slot (equal
  ends and tangencies at a slant) and the square (a polygon's equal sides). For each, the tool adds exactly `adds`
  entities and one Ctrl+Z leaves `left`; a second sketch drawn with `sketch_draw` from the first one's geometry
  equals it in entities (1e-6, angles modulo a turn), `closed_loops`, constraint count, sorted
  `sketch_list_constraints` types and `dof`, before and after one undo. The corner rectangle's and the slot's menu
  items are checked to arm their tools (`ARMS`). The other four's are not: test_cad_context_menu's `arm()` checks
  what each of them arms, chosen from the same offer on empty space, where the menu runs the verb's action from the
  Design table (`DesignPanel::show_offer_menu`) as it does here.
- sketch_edit rungs: the reply (`replaced`, `sketch_index`, 4 entities, plane XY); volume 1000 → 4000 → 1000
  through the edit and one undo; undo depth +1; `state()` (feature types and names, undo depth, volume) unchanged
  after a cancel, a failed replace and an emptied sketch; refusal texts (`out of range`, `is not a sketch`,
  `not drawn from entities`, `Project`, `a sketch is already open`); the failed `sketch_finish` reply (`ok` and
  `replaced` false, the index, a reason); the next sketch is appended. The failed replace's export holds one
  `sketch_edit` and three `sketch_finish` steps, `replay_run` passes and ends with the live run's feature types.
  failed_leave, for `sketch_text {}` and K: status line text, no modeless dialog, no sketch, no card, `state()`
  unchanged, the sketch still 20 x 10.
- project_model, text, svg_art, text_edit, scale_artwork: Project feature index, type, `scope` and an XY `frame`;
  in a sketch, 4 construction lines, `first_index` 5, then 9 entities of which 8 construction; Text
  `text_string`, `text_height` and `regions` (4, then 2 after `feature_edit`); SVG art `regions` 1 of area 100
  (1e-3), `import_offset` [5, 5], and 4 lines of 20 at scale 2; refusals by text with no feature, undo step or
  `describe_design` error added; Scale artwork and Edit: undo depth +1 on Confirm, +0 on Cancel; `nothing_open`
  (no modal or modeless dialog, no sketch).
- Edit cards: `feature_edit refs` sets the Fillet's `refs.edges` to the id `body_describe {before}` gave, in one
  undo step, refuses stale ids with `stale_ids`, and undo restores it. The repick rungs click on the canvas through
  the calibrated map, an edge 1 mm in on the top face it bounds, since seen from above the edge is the block's
  outline and a click on the line, rounded to a whole pixel, can land just outside the block. They require the
  exact edge or face id (`plane_face`, `plane_face_body`), the Hole's plane normal and origin to 1e-6 and
  `hole_x`/`hole_y` 0; clicks on a hidden body leave `selection` `none`.
  surface_revolve_flip: the Flip toggle reads `checked` and the one SurfaceRevolve has `flip` true.
- design_view: the `design` block of `view_set`'s reply (section `on`/`z`/`keep` exactly, `z` at the body's
  middle to 1e-6, each of four switches true then false); refusal texts; the section stays off after refused
  calls; `sketch_normal` looks along the XZ normal (cosine ≥ 0.999).
- select, select_loops: `selection.id` for each kind (`body_solid`, `face_planar`, `edge_str`, `vertex`, `sk_loop`,
  `none`); vertex point to 1e-6; renames through the row editor show in `describe_design`; `feature_delete` and
  its undo; `busy` texts for `feature_edit {}` and `sketch_constrain_selected`; refusals carrying `design_busy` or
  `stale_ids` with the selection kept; `applies` true for `fillet`, `delete_face` and `zoom_to_selection`; after
  `zoom_to_selection` the camera's `look_at` is loop 1's centre (1e-3); three canvas screenshots (face, edge,
  vertex selected) differ from the nothing-selected one and from each other.
- confirm and cancel: `was` (`card`, `sketch`, `plane_pick`, `text`, `insert`), `ok`, `feature` index, `open`;
  refusal texts; feature and body counts; undo and redo depth unchanged after a refused or failed add; the card
  still open with its pick and the status line's reason; `object_list` count and `app_state.tab` unchanged by
  Commit to Plate.
- place: placement translation to 1e-6 and `moved` flags; Commit to Plate's part offset grows by 10 (1e-3);
  rotate keeps the centre and swaps 20 and 30; laying a face on the bed gives lowest z 0 and height 20 (1e-4);
  reset gives null; refused in a sketch; a vertex's `placed_point` is the placement applied to its point; a
  placement follows its body through renumbering and undo.
- boolean_opens, action_pick: `interactive` and `tool` boolean, the card open, its confirm adds a join or a cut and
  leaves one body, the direct cut leaves 3640 mm³ (1e-3); Speed Dial picks arm `array_polar` and `trim`; the
  exported `action_run` step carries `orca_command:sk_array_polar:orca` and `{"type": "circular"}`, and its replay
  passes and arms the tool again.

**Not checked:**
- shapes_match_tools: the geometry comparison is self-referential. The `sketch_draw` arguments are computed from
  the entities the tool drew, so equal geometry only shows that `sketch_draw` reproduces them. Whether the tool
  put its shape where it was clicked (a wrong radius, a corner off its click) is never compared. Only the armed
  tool, entity counts, undo grouping, inferred constraint kinds and DoF are independent of the tool's output.
- shapes_match_tools draws six of the drawing tools. What the other six draw is graded elsewhere: the centre and
  rounded rectangles, the 2-point and 3-point circles and the arc slot by test_cad_context_menu (no_shortcut,
  curves), the 3-point arc by test_cad_sketching (arc). That `sketch_draw` gives those six shapes the constraints and
  degrees of freedom their tools give them is not compared.
- select, "the canvas draws the {face, edge, vertex} selected" and "each drawn apart": the screenshots are
  compared only with each other, as "the upper three quarters differ". No baseline, so any repaint difference
  passes, and what the highlight looks like is not checked.
- design_view: the switches and the section are read back from the panel's own flags in `view_set`'s reply;
  nothing checks that the canvas draws the planes, axes, origin, bed or the cut.
- scale_artwork, and the Insert placement in confirm_refused_add: Scale artwork and Edit are confirmed or
  cancelled with no change made, and only the undo depth is read, so scaling the art is never exercised.
- Loose matchers where the value is known: "select: {what} is refused" and "select: a plane is refused while the
  planes are hidden …" accept any error; "place: describe_design reads the Commit button's mode" accepts either
  `assembly` or `bodies`; "place_select: fit_selection centres inside the placed body" accepts any point in its
  box; "text: the text extrudes into a body" needs only one body or more.
- The two replays (sketch_edit_failed_replace, action_pick_replay) are compared by feature types, or the armed
  tool, not by names, sketch content or geometry.
- project_model with no sketch open: the projected edges are not read, only the feature, `scope` and `frame`.
- action_pick: the Speed Dial click's own reply, and that the dial closes after the pick, are not checked.

**Strength:** Partial — most rungs check exact ids, volumes, indices and undo depths, but the shapes rung's geometry
oracle is the tools' own output, the selection drawing is checked only as "differs", the Design view switches
only as panel flags, and Scale artwork never scales.

## `test_cad_sketching`
`test_cad_sketching.py` · Python, 248 checks

**Tests:** Drawing in an open sketch the way a person does, with clicks on the canvas and exact values typed into
the canvas's value field with no click into it: each keyed tool's primitive, a profile with two voids, the edit
operations (fillet, chamfer, offset, mirror, mirror of arcs, trim, extend) driven by picks and on-canvas value
labels, a dimension, every constraint button pressed on shapes drawn for it in one sketch, which is committed and
opened again once and each relation read back from, selecting and deleting a constraint by its badge, undo inside a
sketch and across its commit, save and open through the File menu, and drawing over a sketch of 1204 entities
loaded while a tool is armed.

**Starts from:** a newly started app (`"launch": "fresh"`), then `CadGui.start(first_showing=True)`: an English UI;
the check of the Design tab's first showing, made before anything builds the tab, since the `cancel_tool` of
`start(client)` would build it unshown and the check is of the tab a person's first click on it builds; then
`start(client)`, the Design view's switches as a new data folder has them (`reset_switches()`), the window widened
when the sketch ribbon overflows it, and an empty design in `reset_view()`'s camera. Clicks go through a map
measured on the open sketch, so neither the window's size nor the bed fixes a pixel; the bed sizes only the XY
plane's square that opening a sketch walks out to. relations draws in a grid of 350 x 225 mm about the sketch's
origin, which must lie in the middle 60% of the canvas each way, clear of its panels, and raises when it does not:
at `reset_view()`'s zoom a canvas of about 940 x 600 click pixels, which the canvas of the baseline's 1600 x 900
window holds (1142 x 742 when measured). roundtrip saves and opens `roundtrip.3mf` in a folder it makes in its own
temporary folder, with a rule answering each file picker, so no folder the data folder remembers is used.

**Leaves:** its last project unsaved; roundtrip's `roundtrip.3mf` in the recent projects and
its folder as the last export folder (`last_export_path`), both pointing at a folder the rung deletes at its end. A
rerun on the same data folder saves under a new folder and reads neither.

**Decides by:** Exits 1 when any check prints FAIL or a rung raises (`cad_gui.run()` records the raise as a FAIL
named after the rung).
- Harness (`cad_gui.py`), part of the verdict: the `start()` check "the Design tab's first showing lays out its
  canvas as a later one does"; `reset_switches` raises when it reads a switch back otherwise; opening a sketch
  raises when no click along the XY plane's diagonal opens one, each click's outcome read once after `app_wait
  {for: "idle"}` answers; `calibrate` raises when probes land more than 0.5 px off; `value()` raises when no field
  opens or none takes the keyboard; `cancel`, `confirm` and `reopen` raise when the sketch does not close or open.
  The test's own `escape_field`, `keep_as_drawn` and `accept_shown_value` raise when a field does not open or close
  or a tool stays armed, and `pick()` adds one check per set of picks ("<rung>: both picks landed on an entity"),
  read from the Constrain status line's "N picked", which must count 1, 2 (3) after a click on empty space has
  dropped the picks the last button left. Each wait polls its state every 0.05 s for up to 30 s.
- Tools: each shape is drawn away from every value typed into it, so a field the digits never reach commits the
  size as drawn and fails the read-back. Its fields open on 104.5 and 64.7 for the rectangle, 56.8 and 15.3° for
  the line, radius 49.8 and 106° for the arc, 50.4, 23.9 and 9.2° for the slot, side 34.9 for the polygon, 39.8 and
  14.9 for the ellipse and radius 29.9 for the circle, set by the clicks to within a pixel. Read back: rect sides
  [80, 80, 120, 120] with 120 its extent along x and 80 along y (1e-9), area 9600 (1e-6), sides axis-aligned; circle
  radius 25 and area 625π; line 50 long at 30° (1e-9 deg); arc radius 40, sweep 90° (1e-7 deg), chord 40√2; slot
  flanks at the typed angle 0 (1e-9), end radii 10, flanks 60, area; polygon sides 30 (1e-9) and a regular
  polygon's area; ellipse semi-axes 50 and 20 (1e-6) and area 1000π (2e-2 mm²); point and circle centre within
  0.6 mm of the click; one spline.
- voids: the eight sides, three closed loops, buildable with no open ends, the outer loop owns exactly the other
  two and they own none, areas and net material to 1e-6.
- Edit operations: fillet one arc of r 8, area, its centre r from both legs (1e-9); chamfer five sides, a side of
  10√2, legs 70 and 110 (1e-9), area; offset radii 25 and 20 with the inner a hole; mirror six lines, area, every
  vertex reflected about the drawn axis (1e-9); mirror_arcs one copy per entity, sources unchanged, copies
  reflected (1e-6), no reflex cap; trim and extend lengths and the new end on the crossing (1e-9); dimension a side
  of 90, DoF down, solved.
- relations: each button's shapes are drawn in a cell of their own, in a grid of 70 x 45 mm cells with the middle
  one empty, all before any button is pressed. Parallel is pressed in the open sketch on two lines picked with a
  click and a Ctrl+click (the selection is exactly those two); the others in Constrain mode on the committed sketch.
  Each relation checks first that its shapes start away from it, then that it holds once pressed, to 1e-6 unless
  given: equal length (1e-9), 90° (1e-6 deg), parallel twice, plumb, concentric with the radii untouched,
  tangent, midpoint, symmetric about a drawn line and level, about x = 0 and y = 0 (1e-9) with the other coordinate
  untouched, radius 10 with the centre unmoved, diameter 24 giving r 12, fix with the anchor unmoved (1e-9) and a
  gap of 35 after it, vertical gap 30, horizontal gap accepted with no move under 5e-3 then typed 20, equal radius
  by the Equal length and the Equal radius buttons (1e-9), collinear, coincident, with guards that nothing
  collapsed. Horizontal on a lone point moves nothing (1e-9). Angle on two circles says on the status line "Angle
  applies between two lines", and once it has, no value field is open. After Confirm and opening the sketch again
  from its row (its entities those Constrain mode left, by type and order): every relation holds again as it did
  (perpendicular exactly 90° to 1e-9, the rectangle a square to 1e-9); `sketch_list_constraints` holds exactly one
  constraint of the button's type on the relation's entities, with the typed value for radius 10, diameter 24 and
  the gaps 35, 30 and 20; the point under Horizontal carries only its Coincident and the circles under Angle only
  their Equal radius; and the sketch solves with 73 degrees of freedom, the sum of the cells' (`DOF`: what each
  cell's shapes have, less what the constraints inferred while drawing and the buttons' take).
  Angles are read as atan2 of the two directions' cross and dot products, which is exact at 0, 90 and 180°; acos of
  the cosine reads nothing finer than about 1e-6° near 0 and 180°. The tolerances on the solved relations are what
  the solver promises: libslvs stops when every equation is within its `CONVERGE_TOLERANCE`, `LENGTH_EPS / 1e2` =
  1e-8 (libslvs `system.cpp:20`, `LENGTH_EPS` in `solvespace.h:127`, the SolveSpaceLib commit `deps/SLVS/SLVS.cmake`
  fetches), and the Design tab keeps a solve's geometry only when it converged (`SketchSolver.cpp:449`, `:486`). A
  Parallel's equation in the sketch's plane is the cross product of the two lines' directions in mm²
  (`constrainteq.cpp`, `Type::PARALLEL`), so both parallel relations are held to |a × b| ≤ 1e-8 mm², which for their
  lines of about 35 and 22 mm is an angle of about 7.5e-10°. A Perpendicular's is the direction cosine, so its bound
  is an angle of 1e-8 rad, 5.7e-7°, whatever the lengths: the 90° Constrain mode leaves is held to 1e-6°, and the
  reopened sketch's to 1e-9°, tighter than the bound, which it meets because it is solved again from geometry
  already that close and Newton's step from there lands far inside the tolerance.
- badge: a click selects the side's Horizontal (`selected_constraint`) and deletes nothing; a double-click keeps
  only it selected; Delete removes exactly that one.
- undo, feature_undo: 3 → 2 → 1 entities with survivors moved under 1e-8; a feature deleted from its row comes back
  with Ctrl+Z as the same 120 x 80 rectangle and goes again with Ctrl+Y.
- roundtrip: the file is written, one feature, every entity identical to 12 decimals, three loops, the holes, outer
  area 9600 (1e-9).
- scale: all 1204 entities, 301 loops, the plate's area and 300 holes of side² (1e-9); the drawn line takes its
  typed length and angle 0 (1e-9) as the only entity added, and no existing entity moved (exact).

**Not checked:**
- voids and feature_undo compare sorted side lengths, so typing their Width and Height the other way round passes;
  rect reads its sides along their axes. ellipse: its rotation is not read.
- polygon: the side count is not pinned ("at least 3"), and its typed angle 0, 2° from the angle drawn, is not read
  back; spline: only that one spline exists, not its four control points or its shape.
- The tools' prefills are not read: that each differs from the value typed rests on where the clicks draw the
  shape.
- relations: a constraint is matched by its type and the entities it names, not by which of their points it binds,
  and the horizontal gap the shown value set is not matched by its value. A button that moves another cell's shapes
  is caught only where that cell's own checks see it. The degrees of freedom are summed over the sketch, so a
  constraint lost in one cell and one too many in another cancel there; the lost one's own read-back still fails.
- scale: the bulk add and the gesture are timed but the times are only printed; no limit is checked.
- In an app that has built the Design tab before, as when the test runs after another in one app outside the sweep,
  the `start()` check compares two later showings.
- The perpendicular after the round trip is held tighter than the solver promises; a solve that stops just inside
  its tolerance there would fail it.

**Strength:** Strong — typed values and constraint results are graded exactly against numbers the test chose, and
every relation is read back from the reopened sketch as geometry and as its stored constraint; a few tools are
loosely pinned.

## `test_cad_context_menu`
`test_cad_context_menu.py` · Python, 131 checks

**Tests:** The sketch's right-click offer: its header names what was clicked and it lists exactly the label paths
the Design table gives for five selections, greying exactly the rows with nothing to offer; a right-click with a
line's first point down abandons the point and opens nothing. Every sketch verb with no key is run from the menu
and its result graded: the rectangles, polyline, circles, arcs, arc slot, elliptical arc, polygons, inscribed and
circumscribed, move, rotate, scale, array and polar array through their gizmo labels, Text through the Text
dialog, and an SVG import through the file picker. Also that the value fields a drawing opens take digits typed
with no click into them: the rounded rectangle's width, height and corner radius, and a rectangle's width field
opened again in Select by a click on its label, which lies over a constraint badge.

**Starts from:** `CadGui.start()`, as for `test_cad_commands`. The rounded rectangle's and the
label rung's clicks lie in the region the map is measured on at the baseline's 1600 x 900 window, and raise when one
does not. art types its text and no height or face, so its letters take the Text dialog's last font and height: in a
new folder 10 mm and the face the dialog lists for the system's UI font. "two regions over 1 mm²" holds at those. No
test in the sweep stores others: the dialog's OK stores the font and height it opened with unless they are changed
in it, and none changes them. The SVG comes from the repository's fixture through a rule answering the picker.

**Leaves:** its design unsaved, and the Text dialog's last font and height as it found them, which art's OK stores
again. The SVG picker remembers no folder.

**Decides by:** Exits 1 when any check prints FAIL or a rung raises (`cad_gui.run()` records the raise as a FAIL
named after the rung).
- Harness, part of the verdict: the `cad_gui.py` raises listed for `test_cad_commands`, plus the test's own:
  `choose()` raises when the verb's path is not an enabled item of the menu or the reply's `ran` differs, `arm()`
  raises when no offer opens and adds a check for each verb in `ARMS` ("<rung>: <verb> armed the <tool> tool"),
  `clear_sketch()` raises when entities remain, `ok_text()` when the Text dialog stays open, `inside()` when a
  click falls outside the calibrated region and `close_fields()` when Esc leaves a value field open. Each wait
  polls its state every 0.05 s for up to 30 s. `keep_as_drawn()` and `close_fields()` read whether a field is open
  once, after `app_wait {for: "idle"}` answers (`CadGui.opened_field()`), and press Esc only on one that is.
- Each typed field, five checks and a read-back: "<rung>: a value field opened by itself to take <v>" (or, for
  label, from the click on the label), "<title> took the keyboard with no click into it" (the field is marked
  focused), "<title> took the typed <v>" (its text is the digits), "the typed <v> is not <title>'s prefill", "Return
  committed <title>'s <v>" (the field closed or the next one opened), and "<what> is the typed <v>", the
  `sketch_describe` quantity the field sets, to 1e-6.
- vocabulary: for nothing selected, a line, a circle, a point and two entities, the header equals `HEADER`, the
  item set equals the paths computed from `scripts/CAD/tool_atlas.json` (by a re-implementation of
  `show_offer_menu`'s layout, for the selection `describe_design` names) with no duplicates, and the greyed set is
  exact; the five offered sets all differ and a line adds verbs; two picks give a selection of 2; with an anchor
  down the right-click returns no header and no items, and the next one offers again.
- author, no_shortcut: sides [80, 80, 120, 120], one loop of 9600 (1e-9); at least ten keyless verbs, the centre
  rectangle among them; the centre rectangle centred on its click within 1.5 px.
- curves: construction properties to 1e-9 (oblique rectangle's opposite sides equal, right corners, not
  axis-aligned; centre arc's ends on its radius; tangent arc square to the line; elliptical arc's ends on the
  ellipse and a > b > 0; polygons of 3, 4, 5, 8 and 12 equal sides); click-relative ones within 1.5 px (2-point
  circle's centre and radius, 3-point circle through its clicks, polygon vertices on one circle); a typed radius of
  30 exactly with DoF 2; circumscribed over inscribed circumradius 1/cos(π/5) to 1e-6. The rounded rectangle, drawn
  90 x 48 with corners of 15, takes the typed Width 63 (its extent along x), Height 41 (along y) and Radius 7 (every
  arc), then shows four equal radii (1e-9) and one loop of area 63·41 − 7²(4 − π) (1e-6).
- label: a rectangle as wide as puts its width label's centre on the top edge's third badge, kept as drawn and
  picked in Select; the click at the label's centre adds exactly one constraint (the width made a driving
  dimension, where the badge would have selected its Horizontal), and the field it opens takes 71, read back as the
  top edge's length.
- transforms: move +25 in X and 0 in Y (1e-9); rotate 30° keeping the length; scale ×3 keeping the direction;
  each array waits for its count; array of 4 at a pitch of exactly 20 with equal lengths; polar array of 6 about
  one centre, 60° apart.
- art: OK in the Text dialog adds a feature and the ribbon's Cancel takes it away; the kept text is one feature
  with `text_string` "LT", `region_loops` summing to 2 and two regions over 1 mm²; the SVG is offered and imports as
  4 lines in 1 loop with proportions 2:1 to 1e-6.
- coverage: the table's keyless sketch verbs minus the test's `DRIVEN` list is empty, `DRIVEN` holds nothing
  outside the table, and no sketch verb lacks an action.

**Not checked:**
- coverage reads only `tool_atlas.json` and the test's own `DRIVEN` list; it cannot fail on a change in the app,
  only on a table edit that adds a keyless verb with no rung.
- curves: the polyline is graded only as 4 lines and 1 loop, not where its points are; the arc slot only as "at
  least 2 arcs" with two sharing a centre, not its 4 arcs, caps or width; the elliptical arc's checks run only `if
  "radius" in` the entity, so a renamed field drops them silently; the inscribed and circumscribed verbs are not in
  `ARMS`, so what they arm is not checked (the radius ratio still fails a no-op).
- The value fields' titles are read and named in the checks, but not compared with the words they should show.
  "the typed <v> is not <title>'s prefill" guards the test's own drawing, not the app: it fails only when a shape is
  drawn at the typed size.
- label: if the badge or label layout moves so that the label no longer covers a badge, the rung raises on its
  precondition, a failure of its set-up rather than of the field.
- art: the letters' shapes are graded only as two loops of over 1 mm²; the SVG's absolute size is not checked,
  only its proportions.
- "no_shortcut: at least ten verbs offered on an empty sketch have no shortcut" is a floor where the table gives
  the exact number; vocabulary's exact menu comparison covers it.

**Strength:** Strong — the menu is compared item for item with the Design table for five selections, each keyless
verb's result is graded by exact construction properties, and every typed value is read back from the sketch to
1e-6 with its field's focus, text and commit checked apart; only coverage (static) and a few curves are loose.
