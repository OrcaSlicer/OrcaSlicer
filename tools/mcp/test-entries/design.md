# Design tab

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_cad_replay`
`test_cad_replay.py` · Python, 15 checks

**Tests:** That a Design-tab session made through `tool_call` and exported with `replay_export` replays to the same part. The session includes:
- failed calls (an empty commit, a missed hole, a fillet the kernel refuses), which the export must leave out;
- a shell that replies `ok` false, which stays in;
- face and edge ids passed with a `generation`, which the export must drop.

The replay runs in a new project in the same instance.

**Decides by:** Exits 1 when any named check prints FAIL. It also exits non-zero when any recording call other than the three expected refusals errors, or when the thread job fails (`await_job`'s `app_wait` raises).
- Live session:
  - "a sketch on XY and the plate are centred on modeling_origin": the frame origin equals `modeling_origin` exactly, and the centre of mass x/y is within 1e-6 relative.
  - "an empty sketch_finish fails" and "a hole that misses fails as an error": any error.
  - "a fillet the kernel refuses fails as an error with the kernel's reason": any error text without "Invalid arguments".
  - "a shell thicker than the plate reports the rejected rebuild in its reply (ok false)": `ok is False`.
  - "the thread is M5, inferred from the bore".
- Export: "the export leaves out the failed calls" (one `sketch_finish`, no `hole`), "... leaves out the refused fillet and keeps the one that ran" (one `fillet`), "... keeps the shell" (one `shell`), and "the export records no generation".
- Replay:
  - "the exported session replays": `passed` true.
  - "the same features" (type list) and "the same bodies and undo depth".
  - "the same volume and surface area" and "the same centre of mass", within 1e-6 relative of the live session.

**Not checked:**
- The replayed part is compared with the live session, not with known values; the volume is only printed. A modelling regression that changes both alike passes.
- The missed hole's and the refused fillet's error texts. The fillet's edge id comes from the `body_describe` read before the first fillet changed the body, and is passed without a `generation`, so the refusal may concern a different edge than the comment means.
- The exported replay carries no `expect`, so nothing checks that the replayed shell again replies `ok` false; only the end state is compared.
- Feature names and parameters: only feature types are compared.
- "Keeps the sketch open" after the empty commit has no check of its own. It is covered only because the next `sketch_draw` would raise.

**Strength:** Strong — the export's contents are checked exactly, and the replayed part's mass properties are compared exactly against the live session.

## `test_cad_slow_build`
`test_cad_slow_build.py` · Python, 7 checks (5 on Windows)

**Tests:** That the app stays inspectable while a long Design rebuild runs off the UI thread behind the progress dialog. The rebuild is an M8 thread tapped 60 mm deep. While it runs, `app_state` and a main-window screenshot answer, the progress dialog is listed and can be captured, and another Design command is refused as busy. The thread then lands as one feature.

**Decides by:** Exits 1 when a named check prints FAIL, or when an `app_state` or screenshot probe raises.
- Result: "the thread call succeeds" (the awaited result has `ok` true) and "one thread feature and one body".
- During the build, one probe runs. It fires once `widget_list` shows a "Design" dialog whose class contains "Progress"; on Windows it fires 2 s after the call starts.
  - "app_state answers during the build".
  - "a window screenshot succeeds during the build": width non-zero.
  - "another Design command is refused during the build": the `describe_design` error contains "Busy".
- Not on Windows:
  - "the progress dialog is listed while it builds".
  - "a screenshot of the progress dialog captures it": width non-zero and narrower than the main window.

**Not checked:**
- How long `app_state` or the screenshot takes. "Answers" only means the probe ran and returned. The measured `state_ms` is printed only on failure. The Busy check that follows bounds it loosely, since the build must still be running after both calls.
- On Windows the two dialog checks are dropped without a skip line. The probe fires on a fixed 2 s delay, not on any sign that the build is running; a build that finishes sooner fails the Busy check rather than passing.
- The two screenshots are saved but never compared; "captures it" only compares widths.
- The thread's geometry (designation, volume) is not read.

**Strength:** Partial — the Busy refusal and the single resulting feature are checked, but responsiveness has no time bound and Windows skips the dialog checks.

## `test_cad_failed_job`
`test_cad_failed_job.py` · Python, 9 checks

**Tests:** That a Design call whose job fails after its reply is left out of `replay_export`. A hole that misses a slow-rebuilding coil replies with a running job. An export made while it runs names it in `left_out_running`. The hole then fails, and the export after it keeps a later rename and leaves the hole out. That export replays to the same features and coil. The sweep runs it with a 4 s reply budget (`ORCA_MCP_REPLY_BUDGET_MS=4000`).

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
- Timing drives the set-up: the coil is sized from a timed 12-turn build and grown up to 40 turns. A machine too fast or too loaded fails the "running" checks rather than passing them.

**Strength:** Strong — the running-job bookkeeping, the failure text and the replayed result are each checked exactly.

## `test_cad_examples`
`test_cad_examples.py` · Python, one check per documented example (152 in `scripts/CAD/tool_atlas.json`, 6 of them skipped, plus any `undo`/`redo` examples)

**Tests:** That every example `tool_search {tool}` documents for the Design commands, and for `undo` and `redo`, still runs as written. Each example runs on a fresh empty design, with `from_call` arguments filled from earlier results.

**Decides by:** Exits 1 when any example prints FAIL. An example fails when:
- a call raises: a tool error, a failed job (`call_command` awaits running jobs), or a first reply blocked by a dialog;
- a `from_call` reference cannot be resolved;
- a result dict has `ok` false, a non-empty `error`, or `interactive` set.

The output is one line per example ("ok   <command>: <title>" or "FAIL ...") and a closing count.

**Not checked:**
- No result is compared with anything. The Design table's examples carry only `title`, `calls` and `needs`, so an example that builds the wrong geometry passes.
- Examples with `needs` are skipped and count as passes on every platform. There are 6 today: two of `sketch_insert_svg`, `body_compare`, `import_step`, `import_mesh` and `export_step`.
- There is no minimum count. If `tool_search {group: "Design"}` listed no commands, or the commands no examples, the test prints "0 examples" and exits 0.
- A result that is not a dict, or has neither `ok` nor `error`, passes without being examined.
- A failure that leaves a dialog open is charged to the next example, whose `fresh_design` then raises.

**Strength:** Smoke — passes whenever every call succeeds without `ok` false.

## `test_cad_click_edit`
`test_cad_click_edit.py` · Python, 75 checks (6 per typed field over 12 fields, plus 3)

**Tests:** That each value field a Design drawing tool opens on the canvas takes digits typed with no click into it, and that Return commits them. The fields come from:
- the line, rectangle, circle, slot, polygon, ellipse and arc, armed by their keys;
- the rounded rectangle, armed from the right-click offer;
- a rectangle's width label, clicked in Select to open its field again.

All input goes through the UI tools; the Design commands only read back.

**Decides by:** Exits 1 when any `gui.check` prints FAIL. A rung that raises, in the test or in a `cad_gui.py` helper, is recorded as a failed check named after the rung, and the run goes on after `recover()`.
- Per typed field:
  - "<rung>: a value field opened by itself".
  - "<title> took the keyboard with no click into it": the field is marked focused.
  - "took the typed <v>": the field's text equals the digits.
  - "the typed <v> is not <title>'s prefill".
  - "Return committed <title>: the field closed or the next one opened".
  - "<what> is the typed <v>": the matching `sketch_describe` quantity (length, width or height extent, radius, every side or flank) equals the value within 1e-6.
- Two rung checks:
  - "rounded_rect: the offer armed the rounded rectangle tool": `tool` is rect_rounded after choosing the menu path built from `scripts/CAD/tool_atlas.json`.
  - "label: the click at the label's centre took the label: one driving dimension added": the constraint count goes up by 1.
- From `CadGui.start()`: "the Design tab's first showing lays out its canvas as a later one does", which requires the canvas rects to be equal.
- `cad_gui.py` helpers whose own checks decide the result. Each raise below fails the rung it happens in.
  - `start()` exits 2 if the Design tab is off or the UI is not English, and raises if the sketch ribbon has no Cancel.
  - `reset_view()` raises if the camera's zoom or target is off by more than 1e-3.
  - Opening a sketch raises if no click along the XY diagonal opens one.
  - `calibrate()` raises if a probe Point lands more than 0.5 px from its click, if the probes add constraints, or if Ctrl+Z does not remove them exactly (1e-12).
  - `cancel()` raises if the sketch stays open.
  - `widgets()` raises if `widget_list` cuts its tree.
  - The test's own `inside()` raises if a click falls outside the calibrated region.

**Not checked:**
- Only the typed quantity of each shape is read back. The untyped ones, which the closing Escape keeps "as drawn", are not: the ellipse's minor axis, the slot's width, the arc's ends, the shape's position.
- Field titles are read but not checked.
- "the typed <v> is not <title>'s prefill" guards the test's own drawing, not the app. It fails only if a shape was drawn at the typed size.
- If the badge or label layout moves so that the label no longer covers a badge, the label rung raises on its precondition. That is a failure of the test's set-up, not of a field.

**Strength:** Strong — every typed value is read back from the sketch to 1e-6, and focus, text and commit are each checked separately.

## `test_cad_commands`
`test_cad_commands.py` · Python, about 345 checks

**Tests:** What the Design commands' arguments do when the Design panel handles them in the running app:
operation words, suppress and align conflicts; the open sketch's commands; re-editing a committed sketch with
`sketch_edit` (replace, cancel, refusals, a failed replace and its replay); projection, text and SVG art;
`feature_edit` refs and picks made again in an edit card; `view_set`'s Design options; `select_geometry` and the
menu items that act on a selection; `confirm_tool` and `cancel_tool` over cards, sketches, the Text dialog and a
plane pick; `body_place`; the Boolean card; Speed Dial picks and their replay. One rung also checks that each
drawing tool chosen from the right-click menu agrees with `sketch_draw`.

**Decides by:** Exits 1 when any check prints FAIL or a rung raises. `cad_gui.run()` records a raise as a FAIL
named after the rung, runs `recover()` and goes on to the next rung.
- Harness (`cad_gui.py`), part of the verdict: `start()` adds the check "the Design tab's first showing lays out
  its canvas as a later one does". Helpers raise, failing the rung, when `new_design` leaves features,
  `reset_view` leaves the camera more than 1e-3 off the zoom and target it asked for, `calibrate` probes land
  more than 0.5 px off their clicks or add constraints, `cancel`/`confirm` leave a sketch open, `value` finds no
  field taking the keyboard, or a menu choice's `ran` is not the path asked for. The test's own `settled` (60 s),
  `keep_as_drawn` (8 Esc presses) and `rename_selected` (no row editor) raise the same way.
- words: `operation` in the reply and in `feature_describe` equals the word given (`cut`, `join`); volumes from
  `body_mass_properties` are 3500, 5000 and 1000 mm³ to 1e-3; `feature_suppress` and `feature_describe` say
  suppressed and the bodies drop to 0; `align_conflicts` holds exactly the second align; `sketch_extend`,
  `sketch_pattern`, `sketch_transform` and `sketch_dimension` results are read by coordinate to 1e-6; two refusals
  by their text.
- shapes_match_tools (12 shapes, 5 checks each): the menu item arms the tool in `ARMS`, the tool adds exactly
  `adds` entities and one Ctrl+Z leaves `left`; a second sketch drawn with `sketch_draw` from the first one's
  geometry equals it in entities (1e-6, angles modulo a turn), `closed_loops`, constraint count, sorted
  `sketch_list_constraints` types and `dof`, before and after one undo.
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
  undo step, refuses stale ids with `-32010`, and undo restores it. The repick rungs click on the canvas through
  the calibrated map and require the exact edge or face id (`plane_face`, `plane_face_body`), the Hole's plane
  normal and origin to 1e-6 and `hole_x`/`hole_y` 0; clicks on a hidden body leave `selection` `none`.
  surface_revolve_flip: the Flip toggle reads `checked` and the one SurfaceRevolve has `flip` true.
- design_view: the `design` block of `view_set`'s reply (section `on`/`z`/`keep` exactly, `z` at the body's
  middle to 1e-6, each of four switches true then false); refusal texts; the section stays off after refused
  calls; `sketch_normal` looks along the XZ normal (cosine ≥ 0.999).
- select, select_loops: `selection.id` for each kind (`body_solid`, `face_planar`, `edge_str`, `vertex`, `sk_loop`,
  `none`); vertex point to 1e-6; renames through the row editor show in `describe_design`; `feature_delete` and
  its undo; `busy` texts for `feature_edit {}` and `sketch_constrain_selected`; refusals carrying `-32002` or
  `-32010` with the selection kept; `applies` true for `fillet`, `delete_face` and `zoom_to_selection`; after
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
`test_cad_sketching.py` · Python, about 210 checks (about 205 and one `skip` on Linux)

**Tests:** Drawing in an open sketch the way a person does, with clicks on the canvas and exact values typed into
the canvas's value field: each keyed tool's primitive, a profile with two voids, the edit operations (fillet,
chamfer, offset, mirror, mirror of arcs, trim, extend) driven by picks and on-canvas value labels, a dimension,
each constraint button in Constrain mode (and one in the open sketch), selecting and deleting a constraint by its
badge, undo inside a sketch and across its commit, save and open through the File menu, and drawing over a sketch
of 1204 entities loaded while a tool is armed.

**Decides by:** Exits 1 when any check prints FAIL or a rung raises (`cad_gui.run()` records the raise as a FAIL
named after the rung).
- Harness (`cad_gui.py`), part of the verdict: the `start()` check; `calibrate` raises when probes land more than
  0.5 px off; `value()` raises when no field takes the keyboard; `cancel`, `confirm` and `reopen` raise when the
  sketch does not close or open. The test's own `escape_field`, `keep_as_drawn` and `accept_shown_value` raise
  when a field does not open or close or a tool stays armed, and `pick()` adds one check per set of picks ("<rung>:
  both picks landed on an entity"), read from the Constrain status line's "N picked".
- Tools: rect sides [80, 80, 120, 120], area 9600 (1e-6), sides axis-aligned; circle radius 25 and area 625π; line
  50 long at 30° (1e-9 deg); arc radius 40, sweep 90° (1e-7 deg), chord 40√2; slot end radii 10, flanks 60, area;
  polygon sides 30 (1e-9) and a regular polygon's area; ellipse area 1000π (2e-2 mm²); point and circle centre
  within 0.6 mm of the click; one spline.
- voids: the eight sides, three closed loops, buildable with no open ends, the outer loop owns exactly the other
  two and they own none, areas and net material to 1e-6.
- Edit operations: fillet one arc of r 8, area, its centre r from both legs (1e-9); chamfer five sides, a side of
  10√2, legs 70 and 110 (1e-9), area; offset radii 25 and 20 with the inner a hole; mirror six lines, area, every
  vertex reflected about the drawn axis (1e-9); mirror_arcs one copy per entity, sources unchanged, copies
  reflected (1e-6), no reflex cap; trim and extend lengths and the new end on the crossing (1e-9); dimension a side
  of 90, DoF down, solved.
- Constraint rungs: each checks first that the geometry starts away from the relation, then reads the relation
  in Constrain mode to 1e-6 or 1e-9 (equal length, 90°, parallel, plumb, equal radius by both buttons, concentric,
  tangent, midpoint, symmetric about a line, x = 0 and y = 0, radius 22, diameter 30 giving r 15, fix with a gap of
  70 and the anchor unmoved, vertical gap 35, horizontal gap accepted with no move under 5e-3 then typed 40,
  coincident, collinear), with guards that nothing collapsed and the other coordinate is untouched. After Confirm
  and reopen, "the constraints survived the commit": `solve_ok` and more than 0 constraints (more than 1 for fix).
  constrain also needs DoF exactly 3, perpendicular 90° to 1e-9, equal_radius and coincident_points their geometry
  after the reopen.
- type_guards: Horizontal on a point moves nothing and stores no constraint; Angle on two circles opens no field
  within 1 s and stores none.
- badge: a click selects the side's Horizontal (`selected_constraint`) and deletes nothing; a double-click keeps
  only it selected; Delete removes exactly that one.
- undo, feature_undo: 3 → 2 → 1 entities with survivors moved under 1e-8; a feature deleted from its row comes back
  with Ctrl+Z as the same 120 x 80 rectangle and goes again with Ctrl+Y.
- roundtrip: the file is written, one feature, every entity identical to 12 decimals, three loops, the holes, outer
  area 9600 (1e-9).
- scale: all 1204 entities, 301 loops, the plate's area and 300 holes of side² (1e-9); the drawn line takes its
  typed length and angle 0 (1e-9) as the only entity added, and no existing entity moved (exact).
- calibration_cache: the harness's `full_calibrations` counter (map reused, refitted on a resized canvas) and a
  line of 40 drawn through the reused map.

**Not checked:**
- "the constraints survived the commit" counts constraints; it does not read the constrained geometry after the
  reopen for parallel, live_constrain, vertical, concentric, tangent, midpoint, the three symmetric rungs, radius,
  diameter, fix, distance_y, distance_xy and collinear. In parallel and live_constrain the two lines meet at a
  snapped endpoint, which the line tool stores as an automatic Coincident (`infer_auto_constraints` in
  `DesignSketchTool.cpp`), so the check passes even if Parallel is lost on commit. In distance_xy one of its two
  distances could be lost.
- rect, voids and feature_undo compare sorted side lengths, so typing into the Width and Height fields the other
  way round passes; ellipse is graded on its area only, not its axes or rotation.
- polygon: the side count is not pinned ("at least 3"); spline: only that one spline exists, not its four control
  points or its shape.
- scale: the bulk add and the gesture are timed but the times are only printed; no limit is checked.
- type_guards, "Angle on two circles opened no value field": a field that opens after 1 s passes.
- roundtrip prints `skip` and passes on Linux.
- calibration_cache tests the harness, not the app.

**Strength:** Strong — typed values and constraint results are graded exactly against numbers the test chose;
only the survival of most constraints through the commit is checked by count, and a few tools are loosely pinned.

## `test_cad_context_menu`
`test_cad_context_menu.py` · Python, about 105 checks (about 103 and one `skip` on Linux)

**Tests:** The sketch's right-click offer: its header names what was clicked and it lists exactly the label paths
the Design table gives for five selections, greying exactly the rows with nothing to offer; a right-click with a
line's first point down abandons the point and opens nothing. Every sketch verb with no key is run from the menu
and its result graded: the rectangles, polyline, circles, arcs, arc slot, elliptical arc, polygons, inscribed and
circumscribed, move, rotate, scale, array and polar array through their gizmo labels, Text through the Text
dialog, and an SVG import through the file picker.

**Decides by:** Exits 1 when any check prints FAIL or a rung raises (`cad_gui.run()` records the raise as a FAIL
named after the rung).
- Harness, part of the verdict: the `cad_gui.py` raises listed for `test_cad_sketching`, plus the test's own:
  `choose()` raises when the verb's path is not an enabled item of the menu or the reply's `ran` differs, `arm()`
  raises when no offer opens and adds a check for each verb in `ARMS` ("<rung>: <verb> armed the <tool> tool"),
  `clear_sketch()` raises when entities remain, `ok_text()` when the Text dialog stays open.
- vocabulary: for nothing selected, a line, a circle, a point and two entities, the header equals `HEADER`, the
  item set equals the paths computed from `scripts/CAD/tool_atlas.json` (by a re-implementation of
  `show_offer_menu`'s layout, for the selection `describe_design` names) with no duplicates, and the greyed set is
  exact; the five offered sets all differ and a line adds verbs; two picks give a selection of 2; with an anchor
  down the right-click returns no header and no items, and the next one offers again.
- author, no_shortcut: sides [80, 80, 120, 120], one loop of 9600 (1e-9); at least ten keyless verbs, the centre
  rectangle among them; the centre rectangle centred on its click within 1.5 px.
- curves: construction properties to 1e-9 (oblique rectangle's opposite sides equal, right corners, not
  axis-aligned; rounded rectangle's four equal radii and area W·H − r²(4 − π) to 1e-6; centre arc's ends on its
  radius; tangent arc square to the line; elliptical arc's ends on the ellipse and a > b > 0; polygons of 3, 4, 5, 8
  and 12 equal sides); click-relative ones within 1.5 px (2-point circle's centre and radius, 3-point circle
  through its clicks, polygon vertices on one circle); a typed radius of 30 exactly with DoF 2; circumscribed over
  inscribed circumradius 1/cos(π/5) to 1e-6.
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
- art: the letters' shapes are graded only as two loops of over 1 mm²; the SVG's absolute size is not checked,
  only its proportions; the SVG import prints `skip` and passes on Linux.
- "no_shortcut: at least ten verbs offered on an empty sketch have no shortcut" is a floor where the table gives
  the exact number; vocabulary's exact menu comparison covers it.

**Strength:** Strong — the menu is compared item for item with the Design table for five selections, and each
keyless verb's result is graded by exact construction properties; only coverage (static) and a few curves are
loose.

## Replays

These replays have no Python test in their place yet, and stay in the sweep until one is designed here.

### `cad_sketch_to_solid`
`tests/cad_sketch_to_solid.json` · replay, 37 steps, 59 expects, 1 optional

**Tests:** Sketch to solid through the Design commands. A sketch on a base plane passes through `modeling_origin`. An empty `sketch_finish` is refused and leaves the sketch open. A fillet is made inside the sketch. Then come extrudes (whole sketch as a new body, one loop alone, and a sketch on a body face joined onto that body) and a revolve about the sketch's only construction line.

**Decides by:** Fails when a non-optional call errors or an expect misses.
- Sketch frames: the XZ sketch reports `plane` XZ with normal +Y. The sketch on face 10 has frame origin z 8 ± 1e-3, x axis world −Y and y axis world +X (exact).
- Sketch contents: 6 entities after drawing and 7 after `sketch_fillet`. The fillet is entity 6 with `released_constraints` 1 and `solve_ok`. Committed, it is an `arc` of radius 3 ± 1e-4. Loop areas: 265.93–265.94 and 9600 ± 0.01. The plate is loop 5. Each commit's `sketch_index`/`features` is checked.
- Empty commit: the next `sketch_describe` (no `sketch` argument, so it errors when no sketch is open) must list no entities. The next commit must be `sketch_index` 0.
- Solids: the `extrude` and `revolve` replies (`extrude_index`, `operation` new_body/join, `body`, `bodies`, `loop`, `end` blind, `axis_entity` 4). `body_mass_properties` volumes are within 0.03 mm³ for the jig, lip and single loop, 0.16 for the ring and ±2 for the bracket. `body_describe` gives 11 faces with known kinds, and the bboxes run z 0–30, then 18 after the lip.

**Not checked:**
- The optional empty `sketch_finish`: its refusal text. The refusal and "stays open" are covered only because the next step errors without an open sketch and the next commit must be index 0.
- "Passes through `modeling_origin`": only `frame/origin/0 gt 1.0` and `bbox/min/0 gt 1.0`, not equality with `describe_design`'s `modeling_origin` (left loose because it depends on the bed).
- The holes on 32 mm centres and where the lip sits in its plane: only total volume and z extent.
- `closed_loops/0/entities/6 exists` and `holes/4 exists` only say the arrays are long enough, not what they hold, though the exact values are known.
- The bracket's volume band (±2 mm³) is about 20 times looser than its loop-area check implies.

**Strength:** Strong — every solid's volume is checked tightly, and every sketch reply by exact indices and counts.

### `cad_sketch_edit`
`tests/cad_sketch_edit.json` · replay, 39 steps, 54 expects, 3 optional

**Tests:** The sketch's own tools through the Design commands: polygon, slot, ellipse, spline, trim, extend, chamfer, fillet, linear and circular patterns, rotate and move. It also adds constraints and dimensions from entity picks and from named points (`origin`): a radius that over-constrains is rolled back and a constraint is deleted. Each result is read back from `sketch_describe` and `sketch_list_constraints` and from the extruded solid's volume.

**Decides by:** Fails when a non-optional call errors or an expect misses.
- Hex standoff: `sketch_draw`'s shape bookkeeping (`count` 6, `first` 6, 7 entities). The first edge lies at x 4 ± 1e-4. The hexagon loop has area 55.4256–55.4257 and holds the bore as hole 0. Volume 630.0–630.1.
- Trim/extend: `sketch_trim` cuts line 0 to end at x 0 ± 1e-4, with p0 still −10. `sketch_extend` takes line 2 to x 30 ± 1e-4. The sketch is then cancelled, and the next commit being `sketch_index` 0 shows that nothing was committed.
- Plate edits: the rectangle has `dof` 4. The chamfer and fillet create entities 4 and 5, then there are 6 in all. Shape indices for the slot, ellipse, circle and spline, and 13 entities. Linear pattern: `first_index` 13, `added` 4. Rotate pivot (−10, 10) ± 1e-4. Move reports entity 11. Circular pattern: `first_index` 17, `added` 3, `bound` 3. Copy 18 is centred at (22, 0) and the rotated slot's p0 x is −12, both ± 1e-4. Buildable.
- Constraints: 25 before. The distance dimension is added at index 25 with an `origin` ref and `solve_ok`. `equal` is stored as `equal_radius`. After the optional radius there are 27, with item 26 `equal_radius`. Deleting 25 removes a `distance` and leaves 26 in both the live (`scope` live) and the committed (`scope` committed) sketch.
- Final solid: 11106.1–11106.25 mm³.

**Not checked:**
- The optional `sketch_trim` at [100, 100]: nothing reads lines 1 and 3 after it, and the sketch is cancelled two steps later. Neither the refusal nor "refused calls change nothing" is verified for it.
- The optional fillet of radius 100 and the radius dimension are covered only indirectly. The next fillet must still be entity 5, and the constraint count must still be 27. Neither refusal text is checked.
- What the distance and equal constraints do to the geometry. Both match the geometry already there, so they could not move it anyway.
- The spline's `construction` flag.
- Where most edited entities end up (the ellipse, the other two pattern copies, the chamfer and fillet), beyond what the total volume implies.

**Strength:** Strong — every edit's reply is checked by exact index, count or coordinate, and the final volume to 0.15 mm³.

### `cad_topology_targets`
`tests/cad_topology_targets.json` · replay, 33 steps, 69 expects, 1 optional

**Tests:** Reading a body's topology and building on what it finds. It covers `body_describe` filters (convexity, face kinds, direction, extreme, adjacency, paging), a fillet on an inside edge found that way, and M4 countersinks seated on a face on a pilot hole's axis (`at` a cylindrical face). It also covers `measure` between faces and axes, datum planes (midplane, offset) with a sketch on one, and a face push/pull that joins its own body.

**Decides by:** Fails when a non-optional call errors or an expect misses.
- Topology reads, all ± 1e-4 or tighter:
  - Two concave edges: the first is id 7, a line next to face 2, with 3 faces in total.
  - After the fillet: one internal cylindrical face of radius 5.
  - Facing −X: 2 faces, with face 0 of area 985.86–985.87.
  - Two internal conical faces: half-angle 45°, radii 4.5 and 2.25, axes −X, 6 adjacent edges, the first convex and on face 0.
  - The −z extreme is face 10.
  - Paging: `faces/4` present, `faces/5` absent, `next_offset` 5.
- Countersinks: `off_plane` 5 ± 1e-4 for both. The first has `y` 0 and frame normal +X.
- Measures, all ± 1e-4:
  - Axis to bottom face: 25, at 90°.
  - Axis to axis: 25.
  - Cone to front face: 0 (≤ 1e-4).
  - Cone to back face: 2.75.
  - Plate gap and min distance: 5.
- Datums: `plane_index` 7 (normal −X) and 8 (origin z 70). The sketch on datum 8 is at z 70, is `sketch_index` 9, and makes body 1 spanning z 70–74.
- Push/pull: `operation` join on body 0, `sketch_index` −1, face 8, normal +X. Volumes 10352.82–.84 and 12689.20–.22.

**Not checked:**
- The optional tangent plane: its refusal text. "Changes nothing" is verified: the `describe_design` after it must have feature 8 a Plane, no feature 9, `undo_depth` 8 and `redo_depth` 0.
- The two pilot `hole` steps have no expect. Their positions show up only in the later measures; the upper hole's 50 mm height is checked only through the 25 mm spacing.
- The second countersink's `y` and frame.
- `edges/0/faces contains 2`, where the exact face pair is known.
- Face and edge ids are hard-coded ("this build's"). A renumbering fails the test with no change in behaviour (a false fail, not a false pass).

**Strength:** Strong — ids, radii, angles, distances and volumes are checked to 1e-4 or tighter.

### `cad_features`
`tests/cad_features.json` · replay, 45 steps, 59 expects, 6 optional

**Tests:** Threads, sweeps, lofts and plane splits through the Design commands:
- An internal thread on a bore, with its size inferred from the bore, and the same thread placed on the XY plane.
- A helix with round and square wire swept along it.
- A square-to-circle loft up to a datum plane.
- A split by that datum, then a split by XY flipped that keeps one side.

**Decides by:** Fails when a non-optional call errors or an expect misses.
- Thread on the bore: `thread_index` 3, `designation` M5, `internal`, `pitch` 0.8 ± 1e-4, `length` 15 ± 1e-4, one body. The body is solid at 600–614 mm³. Feature 3 is a Thread, with `undo_depth` 3.
- Thread on XY: after `undo` there are 3 features. The thread is at x = y = 0 on body 0, as M5, and the volume is again 600–614.
- Helix and sweeps: `helix_index` 0, with start tangent y 0.9968–0.9969 and z 0.0793–0.0794. The round wire is `sweep_index` 2, a new body, solid, 2638–2691 mm³. The square wire is `sweep_index` 4, body 1 of 2, 1492–1524 mm³.
- Loft: datum `plane_index` 0, sketches 1 and 2. `loft_index` 3 is body 0, solid, 11850–11861 mm³.
- Splits: `split_index` 4, upper body 0 and lower body 1, normal +Z, the bboxes meeting at z 20 ± 1e-3. The flipped split keeps body 1 at z 0–10 with 2 bodies, normal −Z, and `undo_depth` 6.

**Not checked:**
- The six optional refusals: a planar face, the unknown `M5x1`, a sweep given as the path, a one-profile loft, the datum plane given as a loft profile, and a split plane above the body.
  - Their texts are not checked, including the comment's "names the close ones".
  - A wrongful success is caught only because the next real call checks its index (`thread_index` 3, `sweep_index` 4, `loft_index` 3, `split_index` 4) and `undo_depth`.
  - A refusal that changed a body without adding a feature would pass. For example, body 0 is not measured again after the refused sweep.
- The thread and sweep solids are checked only within volume bands of about 2 % (600–614, 2638–2691, 1492–1524). A wrong thread profile or wire orientation that stays inside a band passes.
- The XY thread's `length` and `internal` are not read back.
- The side the flipped split discards: only `bodies` 2 and body 1's bbox are checked.

**Strength:** Partial — the loft and splits are checked closely, but the thread and sweep solids only within 2 % volume bands.

### `cad_doc`
`tests/cad_doc.json` · replay, 82 steps, 139 expects, 0 optional

**Tests:**
- Reading and editing a Design feature as its card does: a variable bound to an extrude's `distance` and then removed, and `feature_edit` under its own argument names and the aliases agents use (`index`, `params`, `values`).
- Handing the design to the plate: Commit to Plate; a recommit that replaces the first object and is undone in one Prepare step; and the bodies and assembly modes.
- A STEP export read back into a new design.
- The Design history and tree: undo/redo, suppress, move and delete, body name, visibility and colour, and a save and reopen.

**Decides by:** Fails when a non-optional call errors or an expect misses.
- Discovery: `tool_search` lists `feature_describe` as Design read-only and `feature_edit` as Design writable.
- Variables and edits:
  - Variable `h` = 12 shows as bound to `distance` in `describe_design` and in `feature_describe` (`params`, `expr`, `refs`, `used_by`, `editable`).
  - After unbinding and removing it, `variables` is empty and `bound` is absent.
  - `feature_edit` reports `changed` old/new pairs, and the `index`, `params` and `values` aliases each apply.
  - Volumes 6000 and 9000 ± 0.01.
- Plate hand-off:
  - The commit reply names object 0 "Design Body", 1 part, body 0 sent, on the `prepare` tab.
  - `object_list` then shows 1 object of height 15 ± 0.01, then 20 after the recommit (which reports `replaced` 0), and 15 again after one Prepare undo `action_run`.
  - The bodies and assembly commits create objects 1, 2 ("Design Body 2") and 3 ("Design Assembly", 2 parts), for a total of 4.
- STEP round trip: the export has `bytes` > 0 and one body. `new_design` clears 3 features and 1 body. The `import_step` body's volume is 12000 ± 1.2.
- History and tree:
  - Undo/redo depths, with `distance` going 15 → 10 → 15.
  - The rename trims to "Base", is its own undo step (`undo_depth` 4 → 3), and is undone and redone.
  - Suppress reports `changed` true, then false, with body counts.
  - `feature_move`'s reply and the new feature order, then its undo.
  - `body_set` name and visibility show in `describe_design`. A hidden body is left out of `bodies_sent` and listed in `bodies_hidden`.
  - `body_delete` and `feature_delete` report their counts, and their undos restore them.
  - A colour given as `#3080ff` reads back as `#3080FF`.
- Reopen: after a save, a new project and an open, there are 4 features and body 1 is "Lid" with colour `#3080FF`.

**Not checked:**
- `sketch_repair` only runs.
- What lands on the plate after the hidden-body commit and the second bodies-mode commit: only the commit replies are read, with no `object_list`.
- Most of the reopened design: body 0, the feature names ("Base"), visibility and volumes. `project_save` and `project_open` have no expect.
- The new body's default colour only has to match `^#[0-9A-F]{6}$`.

**Strength:** Strong — nearly every edit is read back by exact value, count or volume, and undo depths are pinned.

### `cad_canvas`
`tests/cad_canvas.json` · replay, 32 steps, 33 expects, 5 optional

**Tests:** The Design view as a canvas target:
- `view_set` on the Design tab, and its refusals of Prepare's and Preview's fields.
- Screenshots of the canvas and of its widget.
- A click that picks the block, and a right click that lists the pick's verbs.
- Keys reaching the Design panel's key hook: Shift+E opens the Extrude card, and Escape with no target closes it.
- The project's modified flag after a change made only in the Design tab, and reopening a design-only project with `discard_changes`.

**Decides by:** Fails when a non-optional call errors or an expect misses.
- View: `view_set` echoes tab `design` with camera `iso`, then `default`, then `fit_selection`. After an extrude, `app_state` reports tab `design`, `gizmo` null and the project dirty.
- Screenshots: the canvas and `wxGLCanvas` targets give source `canvas`, no `approximate` key and width > 0. The main window's width is > 0. `widget_list` finds one `OrcaSceneCanvas`.
- Pick: `widget_click` reports `clicked` canvas. The right-click menu's first item matches "of Extrude". `fit_selection`, refused earlier with nothing picked, now succeeds.
- Keys: `widget_list` "Extrude dist" finds 1 node after Shift+E and 0 after Escape. The next `extrude` returns `ok`.
- Project:
  - After the save: not dirty, named `design_only`.
  - After `set_variable`: dirty.
  - Opening with `discard_changes` reports a project with 0 objects. `app_state` then shows not dirty, the name and path match, and no dialogs are open.
  - The variable is gone and a second body exists.

**Not checked:**
- None of the five optional steps is verified:
  - Nothing reads the tab, camera or selection after the refused `fit_bed`, the refused `fit_selection` with nothing picked, the refused `layer` 2, or the refused `mouse_gesture` at a model point. "Refuses what belongs to Prepare and Preview" is not tested.
  - The refused `project_open` without `discard_changes` is not verified. A silent open would pass; only a dialog left open would be caught, by the final `/dialogs` empty.
- The three PNGs are saved but never compared. Nothing shows that they picture the Design view rather than Prepare's canvas.
- The right-click verbs: only the header item is matched (by regex), not the verbs listed.
- The camera is read only from `view_set`'s own reply.
- The reopened design's geometry: the test checks only that `bodies/1` exists, not that there is no third body.

**Strength:** Partial — the key hook, the modified flag and the reopen are checked, but the refusals and the screenshots are not.
