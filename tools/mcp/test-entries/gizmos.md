# Gizmos and canvas input

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_gizmos`
`test_gizmos.py` · Python · takes the place of `gizmos.json` and `dialogs_canvas_input.json`

**Tests:** The Prepare view's gizmos driven as a person drives them, through the generic GUI tools: gizmo icons,
fields, combos, buttons and links in their panels, and clicks and strokes at points of the model. Each gizmo's work
is read back from the model it leaves (placements, painted facets, texture layers, text and SVG parts, brim ears,
parts made or removed, positions in the assemble view), its undo is read before any redo, and what a person keeps
is read again from a saved and reopened project. It also tests the canvas input those gizmos rely on: a press held
across gestures and released by a gesture that fails, a toolbar item that a selection has just enabled, model points
on a negative part and seen from below the bed; and that auto-answer rules run the Transfer and Discard buttons of
the preset prompt. It runs on the harness's default printer, the Bambu Lab X1 Carbon 0.4.

**Reads:**
- `scene()` gives each object of `object_list` as `{name, offset, rotation_deg, scale, bbox, brim_ears, overrides,
  layer_height_profile, parts}`, the first four of its instance 0, and each part as `{name, type, triangles,
  mesh_volume, bbox, painted, text, svg}`.
- `gizmo()` gives `app_state.gizmo` as `{name, canvas}`, or none.
- `panel()` gives the open gizmo's ImGui panel as `widget_list` lists it: its texts in order, and its named items
  with the value each shows (a field's text, a combo's choice).
- `assembly()` gives each object's position in the assemble view, `assemble_offset()` of its instance 0, from a
  `script_run` that reads `orca.host.model()`. No other tool reports it: `object_list` gives the Prepare placement.
- `setting(object, key)` gives `settings_get {target: object}`'s `{value, effective, source}` for one key.
- `selection()` gives `app_state.selection.objects`; `rules()` gives `app_state.auto_answer_rules` with their `uses`.
- `toolbar(name)` gives a 3D view toolbar item's `{enabled, pressed}` from `widget_list`.
- `slots()` is the reader `test_plates` defines (`filament_is_mixed`, one digit per slot).

The test switches to Expert mode first (the Expert action, then `app_state.mode` is `expert`), since some panel
items show only there. Every scenario starts with `project_new {discard_changes: true}` in the Prepare view and checks
that `scene()` is empty and `gizmo()` is none. Objects are then added with `object_add_primitive {shape, size,
position, name}` resting on the bed, so a 20 mm cube placed at `[100, 100, 0]` has its offset at (100, 100, 10).
Row 0 of each table reads `scene()` (and whatever else the scenario's opening sets up) and checks the start written
there, before any gizmo acts.

Notation. An object is written as its name and its offset, `A (90, 100, 10)`, with `rot [x, y, z]` for
`rotation_deg`, `scale s` for a scale of s on all three axes and `size [x, y, z]` for `bbox.size` where a row gives
them; `·` separates objects. Offsets and sizes are checked to 0.001 mm and angles to 0.01°, unless a row gives
another tolerance. `undo` and `redo` are the tools, on the project's history.

Every gizmo is opened and closed with `widget_click {target: {gizmo: name}}` (with `canvas: "assemble"` in the
assemble view). Each opening replies `active: true` and is followed by `gizmo()` giving that name and canvas; each
closing, by that second click or by the panel's Done, Close or Return button, is followed by `gizmo()` none. The rows
below do not repeat this. A `widget_set_value` reply, the value as the field shows it, is checked too, never instead
of the read-back. A step that starts a job in the background says "then idle": it is followed by `app_wait {for:
"idle"}` before its read-back, and that wait must be met. Those are every edit in the Text panel, `object_add_svg`,
`svg_change`, a texture stroke, Simplify's ratio and Apply, Mesh Boolean's Union, the cut and the variable layer
height panel's Adaptive. Simplify's first ratio is the one such step without a wait, on purpose.

**Scenario: prompts a rule answers**

The preset prompt that asks to transfer or discard unsaved changes is answered by an auto-answer rule naming the
button. `wall_loops` is read with `settings_get {type: "process"}` and the selected process preset from
`app_state`. Both presets inherit `wall_loops` "2" from `fdm_process_common`.

| # | Step | Process after | Also checked |
|---|---|---|---|
| 0 | start | the process preset `app_state` names | `rules()` lists no rule |
| 1 | `dialog_auto_answer {rules: [{match: "Transfer or discard", button: "Transfer"}]}`, then `preset_select {type: "process", name: "0.20mm Standard @BBL X1C"}` | 0.20mm Standard, `wall_loops` "2" | `rules()`: the rule, `uses` 0 |
| 2 | `settings_set {type: "process", values: {wall_loops: 5}}` | 0.20mm Standard, "5" | |
| 3 | `preset_select` of "0.16mm High Quality @BBL X1C" | 0.16mm High Quality, "5" | the reply's `selected`; the rule's `uses` is 1: the prompt was asked once and Transfer moved the edit (the preset's own value is "2") |
| 4 | the rules replaced by `[{match: "Transfer or discard", button: "Discard"}]`, then `preset_select` of "0.20mm Standard @BBL X1C" | 0.20mm Standard, "2" (the edit was dropped, not moved back) | the reply's `selected`; the new rule's `uses` goes from 0 to 1; `app_state.project.dirty` is false |

**Scenario: Move, Rotate and Scale**

A 20 mm cube A at (100, 100), selected, the camera at `iso` and fitted to the objects.

| # | Step | A after | Also checked |
|---|---|---|---|
| 0 | start | A (100, 100, 10) rot [0, 0, 0] scale 1 size [20, 20, 20] | `selection()` is [0] |
| 1 | Move opened | as after 0 | `gizmo()` Move on `3d` |
| 2 | `widget_set_value {target: {imgui_name: "position_x"}, value: 90}` | A (90, 100, 10) | reply `90.00` |
| 3 | `undo` | A (100, 100, 10) | |
| 4 | `redo` | A (90, 100, 10) | |
| 5 | Move closed, `object_transform {object: 0, rotate: [0, 0, 90], mode: "absolute"}`, Move opened, `widget_set_value {target: {imgui_name: "coordinates"}, value: "Object"}` | A (90, 100, 10) rot [0, 0, 90] size [20, 20, 20] | reply `selected: "Object"`; `panel()` shows the caption `Relative` where it showed `Position`, and `position_x` reads `0.00` |
| 6 | `position_x` set to 10 | A (90, 110, 10) rot [0, 0, 90] | the move goes along A's own X axis, which the turn points along the bed's +Y |
| 7 | `coordinates` set to index 0 | as after 6 | reply `selected: "World"`; `panel()` shows `Position`, and `position_x` reads `90.00` |
| 8 | Move closed, Rotate opened, `absolute_rotation_z` set to 30 | A (90, 110, 10) rot [0, 0, 30] size [27.32, 27.32, 20] to 0.01 | reply `30.00` |
| 9 | `undo` | rot [0, 0, 90] size [20, 20, 20] | |
| 10 | `redo` | as after 8 | |
| 11 | `absolute_rotation_z` set to 0 | A (90, 110, 10) rot [0, 0, 0] size [20, 20, 20] | |
| 12 | Rotate closed, Scale opened, `scale_x` set to 120 | A (90, 110, 12) scale 1.2 size [24, 24, 24] | reply `120.00`; uniform scale is on, so all three axes; A rests on the bed (`bbox.min` z 0) |
| 13 | `undo` | A (90, 110, 10) scale 1 size [20, 20, 20] | |
| 14 | `redo` | as after 12 | |
| 15 | `scale_x` set to 100 | A (90, 110, 10) scale 1 size [20, 20, 20] | |
| 16 | Scale closed | as after 15 | |

**Scenario: aligning in the Move panel**

A 20 mm cube A at (100, 100) and a 40 mm cube B at (150, 150): their front edges (`bbox.min` y) are at 90 and 130.
The Move panel aligns within the selection when two objects are selected.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 0 | start | A (100, 100, 10) size [20, 20, 20] · B (150, 150, 20) size [40, 40, 40] | |
| 1 | `object_select {all: true}`, Move opened | as after 0 | `selection()` is [0, 1] |
| 2 | `widget_click {target: {imgui_name: "Align front"}}` | A (100, 100, 10) · B (150, 110, 20) | B's front edge is at y 90, A's: the objects align on the front-most edge, not on their centres (B's centre would be at y 100) |
| 3 | `undo` | A (100, 100, 10) · B (150, 150, 20) | one undo step |
| 4 | `redo` | as after 2 | |
| 5 | Move closed | as after 2 | |

**Scenario: lay on face**

A 20 × 20 × 40 mm box P at (100, 100), standing, selected, the camera at `iso` and fitted to it.

| # | Step | P after | Also checked |
|---|---|---|---|
| 0 | start | P (100, 100, 20) rot [0, 0, 0] size [20, 20, 40] | |
| 1 | Flatten opened | as after 0 | |
| 2 | `mouse_gesture {at: {object: 0, spot: "front"}}` | P (100, 100, 10) rot [90, 0, 0] size [20, 40, 20] | the front face lies on the bed (`bbox.min` z 0) |
| 3 | `undo` | as after 0 | |
| 4 | `redo` | as after 2 | |
| 5 | Flatten closed | as after 2 | |

**Scenario: painting**

A 20 mm cube at (100, 100) with two filament slots (`filament_delete` until refused, then `filament_add`) and
support switched on for it (`settings_set {target: "object", object: 0, values: {enable_support: true}}`), so painted
enforcers raise no warning that could cover the panel. It is selected, the camera at `iso` and fitted to it. Part
0's `painted` is written as `supports e/b` (enforcers/blockers), `seam e/b`, `fuzzy n` and `mmu {slot: n}`; `>0` is
a count a brush stroke gives, which the app computes; `—` is an annotation `painted` does not list. A Fill click
fills the face it lands on: the two triangles of a cube's face.

| # | Step | Painted after | Also checked |
|---|---|---|---|
| 0 | start | no `painted` | the cube (100, 100, 10) size [20, 20, 20], one part with 12 triangles; `slots()` "0,0"; `setting(0, "enable_support")` value true from `object` |
| 1 | Support Painting (`FdmSupports`) opened, `{imgui_name: "Fill"}` clicked, then `mouse_gesture {at: {object: 0, spot: "front"}}` | supports 2/0 | |
| 2 | `undo` | no `painted` | |
| 3 | `redo` | supports 2/0 | |
| 4 | `Circle` clicked, `Brush size` set to 3, a right-button drag from `{spot: "top"}` to `{point: [5, 5, 10]}` | supports 2/>0 | the right button paints blockers; the front face's enforcers stay, since the stroke keeps 2 mm from the top's edges |
| 5 | Done | supports 2/>0 | |
| 6 | Seam opened, a click at `{spot: "front"}` | supports 2/>0, seam >0/0 | the default circle brush |
| 7 | `undo` | supports 2/>0, seam — | |
| 8 | `redo`, then Done | supports 2/>0, seam >0/0 | |
| 9 | `settings_set {target: "object", object: 0, values: {fuzzy_skin: "disabled_fuzzy"}}`, Fuzzy Skin Painting opened | as after 8, fuzzy — | `setting(0, "fuzzy_skin")` is `disabled_fuzzy` from `object`; `panel()` shows "Warning: Fuzzy skin is disabled, painted fuzzy skin will not take effect!" |
| 10 | `widget_click {target: {imgui_name: "Enable painted fuzzy skin"}}` | as after 9 | `fuzzy_skin` is `none` (Painted only) from `object`; the warning is no longer shown |
| 11 | Fill, then a click at `{spot: "left"}` | supports 2/>0, seam >0/0, fuzzy 2 | |
| 12 | `undo` | as after 9 | `fuzzy_skin` still `none`: the link took no undo step of its own |
| 13 | `redo`, then Done | as after 11 | |
| 14 | Color Painting opened, `{imgui_name: "extruder_color_2"}` clicked, Fill, a click at `{spot: "front"}` | as after 11, mmu {2: 2} | |
| 15 | `undo` | as after 11 | |
| 16 | `redo`, then Done | as after 14 | |
| 17 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 14 | `slots()` "0,0"; the object's `enable_support` true and `fuzzy_skin` `none`: painting and the settings it needs are kept in the project file |

**Scenario: texture layers**

A 20 mm cube at (100, 100), selected, the camera at `iso` and fitted to it. Part 0's `painted.texture` is written as
its layers in order, each as `slot name painted`, with `?` for a name the scenario does not fix and `>0` for painted
facets a stroke gives. A stroke is a drag from a spot through a waypoint to a point of the cube's part frame. The
panel names each closed layer's row by its place in the list, "Layer 1" for the first; the open layer's card has no
such row.

| # | Step | Layers after | Also checked |
|---|---|---|---|
| 0 | start | no `painted` | the cube (100, 100, 10) size [20, 20, 20] |
| 1 | Texture Displacement opened | `0 ? 0` | the gizmo gives a part without layers one layer in slot 0 |
| 2 | `gizmo_pick_file {action: "texture_import", path: heightmap.png}` | `0 heightmap 0` | `depth_mm` 0.4 (the default); `path` ends in `heightmap.png` and lies in `app_state.config_folder` |
| 3 | `widget_click {target: {imgui_name: "add_layer"}}` | `0 heightmap 0` · `1 ? 0` | |
| 4 | a stroke from `{spot: "top"}` through `[6, 0, 10]` to `[6, 6, 10]`, then idle | `0 heightmap 0` · `1 ? >0` | the new layer, slot 1, is the active one, and a stroke paints only the active layer |
| 5 | `widget_click {target: {imgui_name: "Layer 1"}}`, the first layer's row | as after 4 | `panel()` lists the item `midlevel`: the card now open shows all its settings |
| 6 | a stroke from `{spot: "front"}` through `[6, -10, 0]` to `[6, -10, 6]`, then idle | `0 heightmap >0` · `1 ? >0` | the paint went to slot 0: the click made the first layer the active one |
| 7 | `widget_click {target: {imgui_name: "more"}}` | as after 6 | `panel()` no longer lists `midlevel`: the link folded the open card's settings |
| 8 | `widget_click {target: {imgui_name: "Remove", index: 1}}`, the second layer's | `0 heightmap >0` | slot 1 and its paint are gone |
| 9 | `undo` | `0 heightmap >0` · `1 ? >0` | |
| 10 | `redo` | as after 8 | |
| 11 | `widget_click {target: {imgui_name: "close"}}` | as after 8 | the Close button closes the gizmo and leaves the layers |
| 12 | `project_save` to a new file, `project_new`, `project_open` of the file | `0 heightmap >0` | `depth_mm` 0.4 |

**Scenario: text**

A 20 mm cube at (100, 100), selected, the camera at `top` and fitted to it, so the screen's centre is the middle of
the cube's top. The text is part 1, written as its `type`, `text.text`, `text.style` and its `bbox` z range `z
a–b`, to 0.01 mm. A text is embossed from 0.015 mm under the surface it stands on (`SAFE_SURFACE_OFFSET` in
`EmbossJob.cpp`), outward for a part and inward for a negative part, by its depth: 1 mm by default. The panel's style
combo shows the style's name, followed by the app's modified marker `*` (`Preset::suffix_modified()`, which
`GUI_App::load_language` sets to `"* "`) while the style's font or depth differ from the style as stored
(`draw_style_list` in `GLGizmoEmboss.cpp`). "Shows `ITALIC`" means exactly that text; "shows `ITALIC*`" means the text
starts with it.

| # | Step | Text after | Also checked |
|---|---|---|---|
| 0 | start | no part 1 | the cube (100, 100, 10) size [20, 20, 20], one part |
| 1 | Text opened (`widget_click {target: {gizmo: "Text"}}`) | part "Embossed text", z 19.985–20.985 | the reply has `created: true` and `active: true`; 2 parts; the part's name is "Embossed text"; its `bbox` centre lies over the top's square (x and y 90 to 110): the text lands on the cube's top |
| 2 | `widget_set_value {target: {imgui_name: "Text"}, value: "Orca\nMCP"}`, then idle | part "Orca\nMCP" | reply value `Orca\nMCP`; the part's name is "Orca MCP" |
| 3 | `widget_set_value {target: {imgui: "Style"}, value: "ITALIC"}`, then idle | style `ITALIC` | reply `selected: "ITALIC"`; the style combo shows `ITALIC` |
| 4 | `Combo_Font` set to "HarmonyOS Sans SC", then idle | font "HarmonyOS Sans SC" | style still `ITALIC`; the style combo shows `ITALIC*` |
| 5 | `widget_click {target: {imgui_name: "Revert font"}}`, then idle | font not "HarmonyOS Sans SC" | the style combo shows `ITALIC` again: the font is the style's own |
| 6 | `Depth` set to 2.5, then idle | z 19.985–22.485 | reply `2.50 mm`; the style combo shows `ITALIC*` |
| 7 | `widget_click {target: {imgui_name: "Revert Depth"}}`, then idle | z 19.985–20.985 | the Depth field reads `1.00 mm`; the style combo shows `ITALIC` |
| 8 | `widget_click {target: {imgui: "Cut"}}`, then idle | negative, z 19.015–20.015 | the text cuts 1 mm into the top |
| 9 | `undo` | part, z 19.985–20.985 | |
| 10 | `redo` | as after 8 | |
| 11 | Done | as after 8 | |
| 12 | `project_save` to a new file, `project_new`, `project_open` of the file | negative "Orca\nMCP", style `ITALIC`, z 19.015–20.015 | |

**Scenario: SVG**

A 20 mm cube at (100, 100), nothing selected, the camera at `top` and fitted to the objects. `star.svg` is the
fixture `tools/mcp/tests/fixtures/star.svg`, whose star spans 18.6 × 17.6 mm; an SVG is embossed 10 mm deep by
default (`select_shape` in `GLGizmoSVG.cpp`). The panel names the file without its folder and `.svg`.

| # | Step | After | Also checked |
|---|---|---|---|
| 0 | start | the cube (100, 100, 10), one part | `selection()` is empty |
| 1 | `object_add_svg {path: star.svg}`, then idle | cube · star: one part, `svg.path` star.svg, size [18.6, 17.6, 10] to 0.05, `bbox.min` z 0 | `gizmo()` SVG; `panel()` shows `star` |
| 2 | `gizmo_pick_file {action: "svg_save_as", path: <test folder>/star-copy.svg}` | the star's `svg.path` is star-copy.svg | the file holds the star's path data (`M10 1 L12.6 7.2`); `panel()` shows `star-copy` |
| 3 | `gizmo_pick_file {action: "svg_change", path: star.svg}`, then idle | `svg.path` star.svg, size as after 1 | `panel()` shows `star`, and no longer `star-copy` |
| 4 | SVG closed, `object_select {objects: [0]}`, the camera at `top` and fitted to it, `object_add_svg {path: star.svg}`, then idle | the cube has 2 parts: part 1 a part with `svg.path` star.svg, z 19.985–29.985 | `gizmo()` SVG; the part's `bbox` centre lies over the top's square |
| 5 | SVG closed, `undo` | the cube has 1 part | adding the part is one undo step |
| 6 | `redo` | as after 4 | |
| 7 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 4 | both SVG parts keep `svg`, with `path_in_3mf` `3D/star.svg`: the file is stored in the project |

**Scenario: Measure**

A 20 mm cube at (100, 100), selected, the camera at `iso` and fitted to it. The panel is written as its two selection
lines, `Selection 1 / Selection 2`, and the rows of its result table, each a label and a value.

| # | Step | Panel after | Also checked |
|---|---|---|---|
| 0 | start | | the cube (100, 100, 10) size [20, 20, 20] |
| 1 | Measure opened | None / None, no result row | |
| 2 | a click at `{spot: "front"}` | Plane / None, no result row | |
| 3 | a click at `{spot: "left"}` | Plane / Plane, Angle `90.000°` | no Distance row: two faces at an angle give an angle only |
| 4 | a Shift-click at `{point: [-10, 0, -4]}` | Plane / Point on plane, Distance `d mm` | d within 0.25 of 10, the point's distance from the front face (the click lands within a pixel of the point); no Angle row |
| 5 | Measure closed | | the cube as after 0: measuring moved nothing |

**Scenario: Simplify**

A 20 mm sphere S at (100, 100), selected. Its mesh has 24648 triangles: `its_make_sphere` with the step
`primitive_angle_step` gives a radius of 10 mm, 158 sectors and 79 stacks, 2 × 158 × 78 triangles. A new shape may be
turned toward the part-cooling fan (`load_shape_object`), which leaves a sphere's triangle count and size as they are.

| # | Step | S's triangles after | Also checked |
|---|---|---|---|
| 0 | start | 24648 | S size [20, 20, 20] to 0.01 |
| 1 | Simplify opened, `use_count` set to true, `Decimate ratio` set to 60, with no wait | 24648 | reply `60.00%` |
| 2 | at once Simplify closed, while it works out the result, then `app_wait {for: "idle", timeout_s: 15}` | 24648 | that wait is met: closing leaves nothing running and applies nothing |
| 3 | Simplify opened, `use_count` true, `Decimate ratio` set to 90, then idle | 24648 | `panel()` shows `2465 triangles`, the count asked for (24648 × 10 %, rounded) |
| 4 | Apply, then idle | 2464 | the even count at or below 2465: each edge collapse removes two triangles from the even 24648; Apply closes the gizmo (`gizmo()` none) |
| 5 | `undo` | 24648 | |
| 6 | `redo` | 2464 | |

**Scenario: Mesh Boolean**

A 20 mm cube at (100, 100) and a 10 mm cube part added to it with `part_add {object: 0, type: "part", shape: "cube",
size: [10, 10, 10], position: [110, 110, 20]}`, which shares a 5 mm cube with the first at the corner. It is selected,
the camera at `iso` and fitted to it.

| # | Step | Parts after | Also checked |
|---|---|---|---|
| 0 | start | part 0 `mesh_volume` 8000 · part 1 1000, both type part | the object's `bbox` is x 90–115, y 90–115, z 0–25 |
| 1 | Mesh Boolean opened, `{imgui: "Union", index: 0}` clicked | as after 0 | |
| 2 | `select_source_volume`, a click at `{object: 0, part: 0, spot: "front"}`, `select_tool_volume`, a click at `{object: 0, part: 1, spot: "top"}`, then `{imgui_name: "btn"}`, then idle | one part, type part, its name ending ` - union`, `mesh_volume` 8875 to 1 | the object's `bbox` is x 87.5–112.5, y 87.5–112.5, z -2.5–22.5: the union lands 2.5 mm off along each axis, its bounding-box centre where part 0's centre was, and nothing drops it back onto the bed |
| 3 | `undo` | as after 0 | the `bbox` as after 0 |
| 4 | `redo` | as after 2 | |
| 5 | Mesh Boolean closed | as after 2 | |

**Scenario: brim ears**

A 20 mm cube at (100, 100), selected, the camera at `iso` and fitted to it. The process's `brim_type` is `auto_brim`.

| # | Step | After | Also checked |
|---|---|---|---|
| 0 | start | `brim_ears` 0; `setting(0, "brim_type")` value null, effective `auto_brim` | the cube (100, 100, 10) |
| 1 | Brim Ears opened | as after 0 | `panel()` shows the warning that the brim type is not set to "painted" |
| 2 | `widget_click {target: {imgui_name: "Set brim type to painted"}}` | `brim_ears` 0; `brim_type` `painted` from `object` | the warning is no longer shown |
| 3 | `{imgui: "Auto-generate"}` clicked | `brim_ears` 4 | one ear at each corner of the square first layer |
| 4 | `undo` | `brim_ears` 0 | `brim_type` still `painted`: the link took no undo step |
| 5 | `redo` | `brim_ears` 4 | |
| 6 | a click at `{object: 0, point: [0, -10, -9.5]}`, on the front face 0.5 mm above the bed | `brim_ears` 5 | |
| 7 | `undo` | `brim_ears` 4 | |
| 8 | `redo`, then Done | `brim_ears` 5 | |
| 9 | `project_save` to a new file, `project_new`, `project_open` of the file | `brim_ears` 5, `brim_type` `painted` | |

**Scenario: assembly in the assemble view**

Two 20 mm cubes, A at (100, 100) and B at (160, 100), both selected, then `view_set {canvas: "assemble"}` and the
camera at `iso`. Each object is written as its Prepare offset from `scene()` and its assemble-view position from
`assembly()`, `A (100, 100, 10) / (x, y, z)`. A new object gets its assemble-view position when it is added
(`Model::set_assembly_pos`, which shifts it along x and y only), so both stand at z 10 there.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 0 | start | A (100, 100, 10) · B (160, 100, 10); in the assemble view both at z 10 | `selection()` is [0, 1] |
| 1 | `object_select {objects: [0]}`, Move opened on `assemble`, `position_x` 100, `position_y` 100, Move closed; the same on B with 60 and 160 | A (100, 100, 10) / (100, 100, 10) · B (160, 100, 10) / (60, 160, 10) | a move in the assemble view changes only the assembly position |
| 2 | `object_select {all: true}`, the camera fitted to the objects, Assembly opened on `assemble`, `widget_set_value {target: {imgui: "Mode"}, value: "Face and face assembly"}` | as after 1 | reply `selected` |
| 3 | a click at `{object: 0, spot: "left"}`, then at `{object: 1, spot: "left"}` on `assemble` | as after 1 | `panel()` shows `Face 1 (Fixed)` and `Face 2 (Moving)` both `Plane`, and `parallel_distance_z` reads `40.00`, B's left face 40 mm from A's along A's normal |
| 4 | `parallel_distance_z` set to 0 | A as after 1 · B (160, 100, 10) / (100, 160, 10) | B moves along x until its left face is in A's |
| 5 | `undo` | as after 1 | |
| 6 | `redo` | as after 4 | |
| 7 | `widget_click {target: {imgui: "Return"}}` | as after 4 | `app_state.tab` is `prepare`, and `object_select {objects: [0]}` replies `canvas: "3d"`: it selects in Prepare's 3D view, so the assemble view is no longer shown |
| 8 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 4 | the assembly positions are kept in the project file |

**Scenario: cut with a connector**

A 20 mm cube named block at (100, 100), then a 10 mm cube named other at (150, 100); block selected, the camera at
`iso` and fitted to it. Cutting at z 8 leaves a 12 mm upper half and an 8 mm lower half; the cut object leaves the
list and its halves are added at its end, upper first (`Plater::apply_cut_object_to_model`). A plug connector becomes
a negative part of the upper half and a part of the lower half (`process_connector_cut` in `CutUtils.cpp`); a square
frustum connector's unit mesh (`its_make_frustum` with 4 sectors, a top half the base's width) has `mesh_volume`
(2 + 0.5 + 1) / 3 = 1.167, where a square prism's would be 2.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 0 | start | block (100, 100, 10) one part `mesh_volume` 8000 · other (150, 100, 5) | `selection()` is [0] |
| 1 | Cut opened, `move_Z` set to 8 | as after 0 | reply `8.00` |
| 2 | `{imgui: "Add connectors"}` clicked, `Style` set to `Frustum`, `Shape` to `Square`, then a click at `{object: 0, point: [100, 100, 8], frame: "world"}` with `check: "point"`, on the cut plane | as after 0 | the replies `selected: "Frustum"` and `"Square"` |
| 3 | `{imgui: "Confirm connectors"}`, then `{imgui: "Perform cut"}`, then idle | other (150, 100, 5) · block, the upper half: parts [part `mesh_volume` 4800, negative 1.167], size z 12 · block, the lower half: parts [part 3200, part 1.167] | both halves have `bbox.min` z 0 and stand where block stood (`bbox` x and y 90–110); the cut closes the gizmo |
| 4 | `undo` | as after 0 | the cut is one undo step |
| 5 | `redo` | as after 3 | |
| 6 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 3 | |

**Scenario: canvas input**

A 20 mm cube at (100, 100) with a negative part, `part_add {object: 0, type: "negative", shape: "cube", size: [6, 6,
6], position: [100, 100, 20]}`, whose top stands 3 mm above the cube's. The camera at `iso` and fitted to it. A
middle-button drag pans the camera on a new datadir (`middle_mouse_drag_action` defaults to Pan).

| # | Step | Reply and state after | Also checked |
|---|---|---|---|
| 0 | start | the cube (100, 100, 10) size [20, 20, 20]; parts [part, negative], the negative's `bbox` z 17–23 | `app_state.camera` read: the camera the pans below are measured from |
| 1 | `mouse_gesture {action: "down", button: "middle", x: 300, y: 300}` | `held: true` | `app_state.camera` as read at row 0: a press alone does not move it |
| 2 | `mouse_gesture {action: "move", x: 310, y: 300}` | `held: true`, `camera_moved: true` | `app_state.camera.look_at` differs from row 0's: the held button pans |
| 3 | `mouse_gesture {action: "move", at: {object: 0, point: [0, 0, 5000], frame: "world"}}` | refused, its words "off screen" | `scene()` as after 0; the camera did not move during the step: `app_state.camera` reads as it did at the end of step 2 |
| 4 | `mouse_gesture {action: "move", x: 320, y: 300}` | neither `held` nor `camera_moved` | the camera did not move: `app_state.camera` reads as it did just before this step. The failed gesture released the press |
| 5 | the control: `mouse_gesture {action: "down", button: "middle", x: 300, y: 300}`, `{action: "move", x: 330, y: 300}`, then `{action: "up", x: 330, y: 300}` | the move replies `held: true` and `camera_moved: true`; the up replies no `held` | `app_state.camera.look_at` after the move differs from its reading at the end of step 4: a new press still pans, so step 4's still camera is the released press, not a canvas that ignores the pointer |
| 6 | `object_select {none: true}`, then `widget_click {target: {gltoolbar: "layersediting"}}` | refused, its words "is disabled" | `toolbar("layersediting")` is `{enabled: false, pressed: false}` before and after the click; `widget_list` lists no `Adaptive` button; the cube has no `layer_height_profile` |
| 7 | `object_select {objects: [0]}` and at once the same click | reply `pressed: true` | `toolbar("layersediting")` is `{enabled: true, pressed: true}`; `widget_list` lists the variable layer height panel's `Adaptive` button |
| 8 | `widget_click {target: {imgui: "Adaptive"}}`, then idle | the cube has a `layer_height_profile` with points | the panel takes clicks as soon as it shows |
| 9 | the `layersediting` click again | reply `pressed: false` | `toolbar("layersediting")` is `{enabled: true, pressed: false}`; `Adaptive` is no longer listed; the profile stays |
| 10 | the camera at `top` and fitted, `object_select {none: true}`, `mouse_gesture {action: "move", at: {object: 0, part: 1, spot: "top"}}` with the default surface check | not refused | `selection()` is empty: a move selects nothing |
| 11 | `mouse_gesture {at: {object: 0, part: 1, spot: "top"}}` | | `selection()` is [0]: a click on the negative part lands on its object |
| 12 | the camera at `bottom` and fitted, `object_select {none: true}`, `mouse_gesture {at: {object: 0, spot: "bottom"}}` | not refused | `selection()` is [0]: from below the bed hides nothing |
| 13 | `object_select {none: true}`, `mouse_gesture {action: "move", at: {object: 0, part: 1, spot: "top"}}` | refused, its words "is in front of the point" | `selection()` still empty, `scene()` as after 0, and the camera did not move during the step (`app_state.camera` reads as it did after `object_select`): the check runs for a negative part, and from below the cube hides its top |

**To settle in review:**
- Mesh Boolean moves its result (step 2 of its scenario): a bug in the app's own code, which is on `main`, left as
  it is and a bug for a fix of its own. `GLGizmoMeshBoolean::generate_new_volume` builds the union in the object's
  frame (both parts' meshes transformed by their part matrices) and adds it with `ModelObject::add_volume`, which
  centres the mesh on its bounding box and sets the part's offset to that centre, so the union is still in place; it
  then sets the new part's offset to the source part's (`GLGizmoMeshBoolean.cpp:504`), dropping that shift, and the
  `ensure_on_bed` call after it is commented out. Step 2 expects where the union lands today: moved by (-2.5, -2.5,
  -2.5) mm, from x, y and z 90–115, 90–115 and 0–25 to 87.5–112.5, 87.5–112.5 and -2.5–22.5. The earlier `gizmos.json`
  run shows the same shift, its union moved 9.87 mm along y.
- Undo and redo lose a text part's font face name: a bug in the app's own code, on `main`, left as it is.
  `FontProp::save` and `load` (`TextConfiguration.hpp`), which undo and redo serialize the style through, leave out
  `face_name`, so after an undo `object_list`'s `text.font` gives the style's font descriptor (`style.path`, such as
  `1;9;-12;...;Times New Roman` on Windows) in place of the face name. No step reads `text.font` after an undo or a
  redo: the descriptor depends on the system's fonts.
- The links that set a setting (Fuzzy Skin's "Enable painted fuzzy skin", Brim Ears' "Set brim type to painted")
  change the object's setting without an undo step of their own (`GLGizmoFuzzySkin.cpp` and `GLGizmoBrimEars.cpp`
  assign the config directly): the app's own code, on `main`, left as it is. The entry expects that, so an undo of
  the next step keeps the setting.
- The style combo's marker (text, steps 3 to 7): right after the pick the style reads unmodified, since a text
  created on the top under the `top` camera has no angle or surface distance of its own (`calc_up` and
  `calc_distance` give none for 0), and those are part of the comparison.
- The simplified count (Simplify, step 4): 2464 follows from the stopping rule in `its_quadric_edge_collapse`
  (`actual_triangle_count > triangle_count`) if every collapse removes exactly two triangles; one that removed more
  would leave fewer. A tolerance of 2464 down to 2460 is the fallback.
- The ears Auto-generate makes on a 20 mm cube (brim ears, step 3): 4, one per corner. `generate_points` works it out
  from the first layer; the earlier run gave 4 on a 25.6 mm cube.
- Undo while a gizmo is open (brim ears, Texture Displacement's Remove, Assembly's parallel distance): each of those
  takes a gizmo-action snapshot, so undo is expected to restore the model as before that step, with the gizmo still
  open. The earlier replay only undid painting and Move this way.
- The Measure distance tolerance (Measure, step 4): 0.25 mm covers a click landing a pixel off the point at this zoom.
- `path_in_3mf` after saving SVG parts (SVG, step 7): `3D/star.svg` for both, from `create_unique_3mf_filepath`,
  the second taking the first's name because it has the same local file.

**Not checked:**
- How gizmos draw: no screenshot is taken or compared.
- Other window sizes. The sweep runs the test at 1600×900. An ImGui panel or a notification over a model point or
  an item is refused, which `tests/slic3rutils/test_mcp_results.cpp` checks without the app ("a click goes to the
  frontmost ImGui window", "what covers a click is named"), with the surface check's cases in
  `tests/slic3rutils/test_mcp_ui.cpp`.
- Grabbers dragged on the canvas: `mouse_gesture` sets them through the panel fields, as its description says.
- The Move panel's other align and distribute buttons and its align-to-plate mode; Rotate's and Scale's other
  fields, Reset buttons and non-uniform scale.
- Painting tools other than Fill and the circle brush (sphere, gap fill, triangle, height range), erasing with Shift,
  the smart fill angle, and where a brush's facets lie: a brush's counts are checked as above 0.
- The Painted Modifier gizmo: neither replay drives it.
- Texture Displacement's bake, its tile, rotation and midlevel values, the texture library popup, and the name of
  the texture a new layer starts with (the library's first, which depends on the installed textures).
- Text: the font a style names, which depends on the system's fonts (the style combo's marker shows it is the
  style's own); the size, boldness, line gap and alignment fields, the surface option, saving or renaming a style
  and the prompt when a modified style is changed, a text as a modifier, and the font after reopening.
- SVG: its size, depth and mirror fields, "Forget the file path" and reload from disk.
- Measure: edges, circles and points as features, the XYZ distance fields that move objects, and inches.
- Assembly: point-and-point mode, Flip by Face 2, rotation around the faces' centre and the Parallel button for faces
  at an angle.
- Cut: dowel and snap connectors, cutting by groove or contour, flipping or placing the halves, keeping one half,
  and the connector's size and depth.
- Simplify by error, Mesh Boolean's difference and intersection and their Delete input option, and deleting or
  resizing brim ears.
- What the painting, fuzzy skin, brim ears and connectors do to a slice: the test checks what the gizmos leave on the
  model and in the project file; turning it into toolpaths is the slicer's.
- What other tests assert: Select All with Move open, and refused with Lay on face open, in
  `test_menu_items_refusals`; Measure in the assemble view and Select All refused there in `test_person_forms`;
  arranging with Move open and from the assemble view in `test_operation_routes`; refs to the Text font button and a
  texture layer row surviving their renames in `test_ref_locator_export`; the Text action opening its gizmo on a new
  object in `test_every_action`.

## Replays this area replaces

Each leaves the sweep, with its entry, in the commit that adds the tests above.

### `gizmos`
`tests/gizmos.json` · replay, 251 steps, 92 expects, 1 optional

**Tests:** Every Prepare gizmo driven through the generic GUI tools (gizmo icons, ImGui panel fields, combos and links, clicks and strokes at model points): Move, Rotate, Scale, Flatten, the four painting gizmos, Texture Displacement, Text, SVG, Measure, Simplify, Brim Ears, Mesh Boolean, Assembly (assemble view) and Cut, and what each leaves on the model.

**Decides by:** Fails when a non-optional call errors or an expect misses.
- Open and close: `widget_click {gizmo}` replies `/active` true then false for each gizmo; `app_state /gizmo/name` is "Move" and "SVG" when expected, `/gizmo` null after Move and after Assembly's Return.
- Panel fields: each `widget_set_value` reply `/value` or `/selected` equals the text set ("90.00", "30.00", "120.00", "3.00", "2.50 mm", "60.00", "0.00", "10.00", "ITALIC", "Frustum", "Square", "HarmonyOS Sans SC", "Object"/"World").
- Transforms in `object_list`: Move X `offset/0` 90, undo 100, redo 90; Rotate `rotation_deg/2` > 29.9; Scale `scale/2` > 1.19, undo 1; Flatten `rotation_deg/0` > 89, undo < 1.
- Painting: supports `painted/supports/enforcers` > 0, `painted` gone after undo, `blockers` > 0 after a right-drag; seam `enforcers` > 0, gone after undo; fuzzy skin's link sets the object's `fuzzy_skin` to "none" and `painted/fuzzy_skin` > 0; colour `painted/mmu/2` > 0; texture layer `name` contains "heightmap" and `painted` > 0, a second layer present after `add_layer` and absent after Remove.
- Text and SVG: the text part's `text/text` is "Orca\nMCP" and `text/style` "ITALIC"; after Revert Depth no node reads "2.50 mm"; the SVG object makes `total` 3 with `svg/path` containing star.svg, then star-copy.svg after Save as, star.svg after Change file; a second SVG becomes part 2 of object 0.
- Others: Measure's panel has nodes matching "90.000", "Point on plane" and "Distance"; Simplify closed mid-calculation leaves the app idle (`/satisfied` true) and Apply at 90 % leaves `triangles` < 1000; Brim Ears' link puts `brim_type` in the object's overrides and Auto-generate gives `brim_ears` > 0; Mesh Boolean removes part 2 and undo restores it; Cut makes `total` 4.
- Held press: `mouse_gesture down` replies `/held` true; after a gesture that fails (the optional step), the next move has no `/held`.

**Not checked:**
- Move's alignment cannot fail: both cubes already stand at y 100 with the same size, so `/plates/0/objects/1/instances/0/offset/1` eq 100 is the value before Align front is clicked.
- Undo for Rotate, Fuzzy skin and Color painting: undo and redo run back to back and only the redone value is read, so an undo that does nothing passes.
- Bounds looser than the known value: rotation > 29.9 (30 set), scale > 1.19 (1.2 set), flatten > 89 (90), none with an upper bound; the brim override is checked for the key `brim_type`, not its value.
- Only run, never read back: the font Revert, Rotate's reset to 0 and Scale's to 100, texture "Layer 1", "more" and "close", the brim ear added by a click (the ear count is not read again), Assembly's move apart (only the field reads "60.00") and the face-to-face mate (no object position is read), Cut's connectors (only the combos; the cut is checked by object count, not by connector parts).
- Absence-only text checks: Revert Depth and "The panel shows the new file's name" check only that "2.50 mm" and "star-copy" are not shown; both pass if the panel shows nothing, and the new file name is never looked for.
- Measure's results are matched by text search; I could not confirm whether the "Distance" label appears before any measurement.
- Simplify: the triangle count before is not read, so the < 1000 bound does not show a reduction.
- 17 canvas screenshots are saved and never compared.
- The sweep runs it at 1600×900 only; the 1100×760 run in the header is manual.

**Strength:** Partial — most gizmos' main model edit is read back, but several outcomes (Assembly, Cut connectors, Brim click, Measure) are only run or matched by text, three undos are unverified, and the alignment check cannot fail.

### `dialogs_canvas_input`
`tests/dialogs_canvas_input.json` · replay, 32 steps, 9 expects, 0 optional

**Tests:** That auto-answered "Transfer or discard" prompts on a process preset switch run their buttons' handlers; that a toolbar item a selection just enabled (variable layer height) takes a click at once; and that a negative part, and model points seen from below through the bed, resolve as `mouse_gesture` targets.

**Decides by:** Fails when a call errors (a point that does not resolve or is hidden is refused) or one of its 9 matchers misses.
- Transfer: after `wall_loops` 5 and a switch to the 0.16 mm preset (`/selected` exact), `wall_loops` reads `"5"`.
- Discard: after switching back (`/selected` exact), `wall_loops` `ne "5"` and `app_state /project/dirty` = false.
- Toolbar: `layersediting` click `/pressed` = true, then false.
- Negative part: Text gizmo `/created` = true; after Cut and Done, `object_list` part 1 type = `negative`.
- Canvas targets: the three `mouse_gesture move` steps only have to not be refused.

**Not checked:**
- Discard is read back as `ne "5"`, not as the preset's own value.
- The `mouse_gesture` steps have no expect; the negative-part step turns the visibility check off (`check: point`), so only resolving the part is tested.
- "The text lands on the cube's top" is not checked; only the part's type.

**Strength:** Partial — the auto-answers and toolbar click are read back, but the canvas targets are only "not refused".
