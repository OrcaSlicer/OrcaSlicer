# Gizmos and canvas input

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

## `test_gizmos`
`test_gizmos.py` · Python, 1035 checks · in `sweep.json` as `{"path": "test_gizmos.py"}`

**Tests:** The Prepare view's gizmos, driven two ways. As a person drives them, through the generic GUI tools: gizmo
icons, fields, combos, buttons and links in their panels, and clicks and strokes at points of the model. And through
the commands that open a gizmo on a named target, and the ones among them that do the gizmo's job from arguments with
no gizmo: `object_paint`, `object_measure`, `object_simplify`, `part_boolean`, `object_cut`, `object_lay_on_face`,
`object_set_brim_ears`, and `object` and `type` of `object_add_text` and `object_add_svg`. A call that names only its
target opens the gizmo on it, and the same call again leaves it open; a call with an acting argument closes the open
gizmo first and leaves the model as the gizmo's own button would; each refusal is asserted by its code, with the
model read back as it was. Each gizmo's or call's work is read back from the model it leaves (placements, painted
facets, texture layers, text and SVG parts, brim ears, parts made or removed, positions in the assemble view), its
undo is read before any redo, and what a person keeps is read again from a saved project, once for each thing the
project file stores. It also tests the canvas input those gizmos rely on: a press held across gestures and released
by a gesture that fails, a toolbar item that a selection has just enabled, model points on a negative part and seen
from below the bed; and that auto-answer rules run the Transfer and Discard buttons of the preset prompt.

**Starts from:** `start(c)`, the Generic Klipper Printer, compared with `{printer: KLIPPER, process: KLIPPER_STD,
filaments: [GENERIC_PLA]}`: nothing the test drives is a Bambu Lab printer's own, and the brim ears' default radius
follows this process's first-layer line width (brim ears from positions). It reads back the baseline's values its
checks depend on: `app_state.window.size` is [1600, 900], since where the panels open, what covers a point of the
model and the pixels the canvas input scenario presses at follow the window's size; and `preferences_get` gives
`use_inches` "0", `camera_navigation_style` "0" and `middle_mouse_drag_action` "1": millimetres, in which the fields'
texts are written, and the default camera style with a middle-button drag that pans, which the canvas input
scenario's pans need. Beyond the baseline it sets Expert mode (the Expert action, then `app_state.mode` is
`expert`), since some panel items show only there.

**Leaves:** Expert mode. The last scenario's project, unsaved: A with its text and SVG parts and B, the SVG gizmo
open. The Klipper printer with `0.20mm Standard @MyKlipper` and one `Generic PLA @System` slot: openings refused
takes back to one the two slots the painting and opening scenarios make. The imported height map in the data
folder's texture library, `textures/displacement/heightmap.png` (or `heightmap (n).png`, see painting and texture
layers), first in the app's recent textures. The Text tool's last style, `ITALIC`, which the app stores as the gizmo
closes and a later Text opening starts with; the text commands pick no style and leave it so. The recent projects
and the last folder opened, naming the projects it saved in its own folder. `start()` puts back the mode and the
project, and nothing else it leaves changes the words or the units the app reads in, so the test restores nothing.

**Reads:**
- `scene()` gives each object of `object_list` as `{name, offset, rotation_deg, scale, bbox, brim_ears, overrides,
  layer_height_profile, parts}`, the first four of its instance 0, and each part as `{name, type, triangles,
  mesh_volume, bbox, painted, text, svg}`.
- `objects()` gives each object as `{name, offset, parts}`, its instance 0's offset and its number of parts;
  `shapes()` each object as `{name, offset, min, max, size, parts}`, its instance 0's offset and `bbox` and its parts
  as `(type, mesh_volume)`; `instances()` object 0's instances, each as `{offset, rotation_deg, min, size}`; and
  `a_parts()` object 0's parts as their `type` and, for a text, its `text.text`, for an SVG, its `svg.path` as the
  file's name.
- `gizmo()` gives `app_state.gizmo` as `{name, canvas}`, or none.
- `panel()` gives the open gizmo's ImGui panel as `widget_list` lists it: its texts in order, and its named items
  with the value each shows (a field's text, a combo's choice), an item without a name by its label. The panel is
  the largest ImGui window that is not a notification, since Measure draws the labels of its results on the canvas
  in small windows of their own.
- `assembly()` gives each object's position in the assemble view, `assemble_offset()` of its instance 0, from a
  `script_run` that reads `orca.host.model()`. No other tool reports it: `object_list` gives the Prepare placement.
- `setting(object, key)` gives `settings_get {target: object}`'s `{value, effective, source}` for one key.
- `selection()` gives `app_state.selection.objects`; `selected()` gives `selection()` from `checks.py` (`{objects,
  instances, parts}`), only its `parts` for a selection of parts; `rules()` gives `app_state.auto_answer_rules` with
  their `uses`.
- `toolbar(name)` gives a 3D view toolbar item's `{enabled, pressed}` from `widget_list`.
- `slots()` is the reader `test_objects` uses (the `mixed` of each `filament_list` row, one digit per slot), and
  `undo_steps()` is `checks.py`'s.
- `brim_file(path)` gives the saved project's `Metadata/brim_ear_points.txt` as `{object_id: [(x, y, z, r), …]}`. The
  file numbers the model's objects from 1 (`_add_brim_ear_points_file_to_archive` in `bbs_3mf.cpp`), so object 0 is
  `object_id` 1, and it holds each ear's position in the object's coordinates and its radius, as stored.

Every scenario starts with `project_new {discard_changes: true}` in the Prepare view and checks that `scene()` is
empty and `gizmo()` is none. Objects are then added with `object_add_primitive {shape, size,
position, name}` resting on the bed, so a 20 mm cube placed at `[100, 100, 0]` has its offset at (100, 100, 10).
Row 0 of each table reads the model (and whatever else the scenario's opening sets up) and checks the start written
there, before any gizmo or call acts.

Notation. An object is written as its name and its offset, `A (90, 100, 10)`, with `rot [x, y, z]` for
`rotation_deg`, `scale s` for a scale of s on all three axes and `size [x, y, z]` for `bbox.size` where a row gives
them; `·` separates objects. Offsets and sizes are checked to 0.001 mm and angles to 0.01°, unless a row gives
another tolerance. `undo` and `redo` are the tools, on the project's history.

In the scenarios through the panels, every gizmo is opened and closed with `widget_click {target: {gizmo: name}}`
(with `canvas: "assemble"` in the assemble view). Each opening replies `active: true` and is followed by `gizmo()`
giving that name and canvas; each closing, by that second click or by the panel's Done, Close or Return button, is
followed by `gizmo()` none. The rows below do not repeat this. A `widget_set_value` reply, the value as the field
shows it, is checked too, never instead of the read-back. A step that starts a job in the background says "then
idle": it is followed by `app_wait {for: "idle"}`, which also waits for a gizmo working in the background, before
its read-back, and that wait must be met within 45 s. Those are the Text icon that creates a text part, every edit in
the Text panel, `object_add_svg`, `svg_change`, a texture stroke, Simplify's ratio and Apply, Mesh Boolean's Union, the
cut and the variable layer height panel's Adaptive. Simplify's first ratio is the one such step without a wait, on
purpose.

**Scenario: prompts a rule answers**

The preset prompt that asks to transfer or discard unsaved changes is answered by an auto-answer rule naming the
button. `wall_loops` is read with `settings_get {type: "process"}` and the selected process preset from
`app_state`. Both presets, `0.20mm Standard @MyKlipper` and `0.16mm Optimal @MyKlipper`, inherit `wall_loops` "3"
from the Custom vendor's `fdm_process_common`. A rule presses its button once the prompt's modal loop runs
(`ModalTracker::Enter`), so a switch the prompt holds may first reply `blocked_by_dialog`; the test follows its job to
the end with `app_wait {for: "job"}` and checks the job's reply.

| # | Step | Process after | Also checked |
|---|---|---|---|
| 0 | start | 0.20mm Standard, `wall_loops` "3" | `rules()` lists no rule |
| 1 | `dialog_auto_answer {rules: [{match: "Transfer or discard", button: "Transfer"}]}`, then `preset_select {type: "process", name: "0.20mm Standard @MyKlipper"}` | 0.20mm Standard, "3" | `rules()`: the rule, `uses` 0 |
| 2 | `settings_set {type: "process", values: {wall_loops: 5}}` | 0.20mm Standard, "5" | |
| 3 | `preset_select` of "0.16mm Optimal @MyKlipper" | 0.16mm Optimal, "5" | the reply's `selected`; the rule's `uses` is 1: the prompt was asked once and Transfer moved the edit (the preset's own value is "3") |
| 4 | the rules replaced by `[{match: "Transfer or discard", button: "Discard"}]`, then `preset_select` of "0.20mm Standard @MyKlipper" | 0.20mm Standard, "3" (the edit was dropped, not moved back) | the reply's `selected`; the new rule's `uses` goes from 0 to 1; `app_state.project.dirty` is false |

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
| 6 | `position_x` set to 10 | A (90, 110, 10) rot [0, 0, 90] | the edit goes through, though the field moves A by what it is given and then reads `0.00` again; the move goes along A's own X axis, which the turn points along the bed's +Y |
| 7 | `coordinates` set to index 0 | as after 6 | reply `selected: "World"`; `panel()` shows `Position`, and `position_x` reads `90.00` |
| 8 | Move closed, Rotate opened, `absolute_rotation_z` set to 30 | A (90, 110, 10) rot [0, 0, 30] size [27.32, 27.32, 20] to 0.01 | reply `30.00` |
| 9 | `undo` | rot [0, 0, 90] size [20, 20, 20] | |
| 10 | `redo` | as after 8 | |
| 11 | `absolute_rotation_z` set to 0 | A (90, 110, 10) rot [0, 0, 0] size [20, 20, 20] | the edit goes through, though the panel then shows the turn as [180, 180, 180], the same turn as none (`get_rotation_by_quaternion` in `GizmoObjectManipulation.cpp`) |
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

**Scenario: painting and texture layers**

A 20 mm cube at (100, 100) with two filament slots (`filament_delete` until refused, then `add_slot()` with
`SLOT_COLOURS[1]`, `#00C1AE`) and support switched on for it (`settings_set {target: "object", object: 0, values:
{enable_support: true}}`), so painted enforcers raise no warning that could cover the panel. It is selected, the
camera at `iso` and fitted to it. Rows 1 to 16 paint it with the painting gizmos, and rows 17 to 27 give the same
part texture layers, so that one saved project, row 28, keeps both. Part 0's `painted` is written as `supports e/b`
(enforcers/blockers), `seam e/b`, `fuzzy n` and `mmu {slot: n}`; `>0` is a count a brush stroke gives, which the app
computes; `—` is an annotation `painted` does not list. A Fill click fills the face it lands on: the two triangles of
a cube's face.

From row 17 the table gives `painted.texture`, its layers in order, each as `slot name painted`, with `?` for a name
the scenario does not fix and `>0` for painted facets a stroke gives. A stroke is a drag from a spot through a
waypoint to a point of the cube's part frame. The panel names each closed layer's row by its place in the list,
"Layer 1" for the first; the open layer's card has no such row.

`H` is the name the imported height map takes, the stem of the file the import writes into the data folder's texture
library: `heightmap`, or `heightmap (n)` with n from 2 when the library already holds `heightmap.png`, which an
earlier run of this test or `test_runtime_calls`' import leaves there (`write_user_texture` in
`TextureLibrary.cpp` never overwrites a texture). A layer is `H` when its name is one of those and its `path` is
`<app_state.config_folder>/textures/displacement/<its name>.png`. The name is checked as that property rather than
as one name, since which copies the library holds differs with the tests that ran before and with a rerun on the same
folder; importing a copy of the fixture under a name of the test's own would not make it exact, as a rerun meets its
own first copy.

| # | Step | Painted after | Also checked |
|---|---|---|---|
| 0 | start | no `painted` | the cube (100, 100, 10) size [20, 20, 20], one part with 12 triangles; `slots()` "0,0"; `setting(0, "enable_support")` value and effective "1" from `object` (`settings_get` gives a value as the config writes it, a switch as "1" or "0") |
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
| 16 | `redo`, then Done | as after 14 | no texture layer |
| 17 | Texture Displacement opened | layers `0 ? 0` | the gizmo gives a part without layers one layer in slot 0, though the part has other painting |
| 18 | `gizmo_pick_file {action: "texture_import", path: heightmap.png}` | `0 H 0` | `depth_mm` 0.4 (the default) |
| 19 | `widget_click {target: {imgui_name: "add_layer"}}` | `0 H 0` · `1 ? 0` | |
| 20 | a stroke from `{spot: "top"}` through `[6, 0, 10]` to `[6, 6, 10]`, then idle | `0 H 0` · `1 ? >0` | the new layer, slot 1, is the active one, and a stroke paints only the active layer |
| 21 | `widget_click {target: {imgui_name: "Layer 1"}}`, the first layer's row | as after 20 | `panel()` lists the item `midlevel`: the card now open shows all its settings |
| 22 | a stroke from `{spot: "front"}` through `[6, -10, 0]` to `[6, -10, 6]`, then idle | `0 H >0` · `1 ? >0` | the paint went to slot 0: the click made the first layer the active one |
| 23 | `widget_click {target: {imgui_name: "more"}}` | as after 22 | `panel()` no longer lists `midlevel`: the link folded the open card's settings |
| 24 | `widget_click {target: {imgui_name: "Remove", index: 1}}`, the second layer's | `0 H >0` | slot 1 and its paint are gone |
| 25 | `undo` | `0 H >0` · `1 ? >0` | |
| 26 | `redo` | as after 24 | |
| 27 | `widget_click {target: {imgui_name: "close"}}` | as after 24 | the Close button closes the gizmo and leaves the layers; the other painting is as after 14 |
| 28 | `project_save` to a new file, `project_new`, `project_open` of the file | `0 H >0`, the other painting as after 14 | `depth_mm` 0.4; `slots()` "0,0"; the object's `enable_support` "1" and `fuzzy_skin` `none`: the painting, the texture layers and the settings the painting needs are kept in the project file |

**Scenario: text**

A 20 mm cube at (100, 100), selected, the camera at `top` and fitted to it, so the screen's centre is the middle of
the cube's top. The text is part 1, written as its `type`, `text.text`, `text.style` and its `bbox` z range `z
a–b`, to 0.01 mm. A text is embossed from 0.015 mm under the surface it stands on (`SAFE_SURFACE_OFFSET` in
`EmbossJob.cpp`), outward for a part and inward for a negative part, by its depth: 1 mm by default. The panel's style
combo shows the style's name, followed by the app's modified marker `*` (`Preset::suffix_modified()`, which
`GUI_App::load_language` sets to `"* "`) while the style's font or depth differ from the style as stored
(`draw_style_list` in `GLGizmoEmboss.cpp`). "Shows `ITALIC`" means exactly that text; "shows `ITALIC*`" means the text
starts with it. A new text takes the style the Text tool last closed with, which the app keeps in the data folder
(`StyleManager::store_styles_to_app_config`), so row 1 does not fix the style; the stored styles share the 1 mm depth,
so its z range does not depend on it, and row 3 picks the style the rows after it read.

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
| 9 | `undo` | negative, z 19.985–20.985 | the Cut takes two undo steps: the gizmo's for the type, then the job's that moves the text inward (`update_volume` in `EmbossJob.cpp`) |
| 10 | `undo` | part, z 19.985–20.985 | |
| 11 | `redo`, `redo` | as after 8 | |
| 12 | Done | as after 8 | |
| 13 | `project_save` to a new file, `project_new`, `project_open` of the file | negative "Orca\nMCP", style `ITALIC`, z 19.015–20.015 | |

**Scenario: SVG**

A 20 mm cube at (100, 100), nothing selected, the camera at `top` and fitted to the objects. `star.svg` is the
fixture `tools/mcp/tests/fixtures/star.svg`, whose star spans 18.6 × 17.6 mm; an SVG is embossed 10 mm deep by
default (`select_shape` in `GLGizmoSVG.cpp`). The panel names the file without its folder and `.svg`. `svg.path`
is written as the file's name, and checked as the whole path the step gave.

| # | Step | After | Also checked |
|---|---|---|---|
| 0 | start | the cube (100, 100, 10), one part | `selection()` is empty |
| 1 | `object_add_svg {path: star.svg}`, then idle | cube · star: one part, `svg.path` star.svg, size [18.6, 17.6, 10] to 0.05, `bbox.min` z 0 | `gizmo()` SVG; `panel()` shows `star` |
| 2 | `gizmo_pick_file {action: "svg_save_as", path: <test folder>/star-copy.svg}` | the star's `svg.path` is star-copy.svg | the file holds the star's path data (`M10 1 L12.6 7.2`); `panel()` shows `star-copy` |
| 3 | `gizmo_pick_file {action: "svg_change", path: star.svg}`, then idle | `svg.path` star.svg, size as after 1 | `panel()` shows `star`, and no longer `star-copy` |
| 4 | SVG closed, `object_select {objects: [0]}`, the camera at `top` and fitted to it, `object_add_svg {path: star.svg}`, then idle | the cube has 2 parts: part 1 a part with `svg.path` star.svg, z 19.985–29.985 | `gizmo()` SVG; the part's `bbox` centre lies over the top's square |
| 5 | SVG closed, `undo` | the cube has 1 part | adding the part is one undo step |
| 6 | `redo` | as after 4 | |
| 7 | `project_save` to a new file, `project_new`, `project_open` of the file | as after 4, but both SVG parts' `svg.path` is `star.svg` alone | both SVG parts keep `svg`, with `path_in_3mf` `3D/star.svg`: the file is stored in the project, and the project names the local file without its folder (`to_xml` in `bbs_3mf.cpp` writes `filename()` unless full paths are exported) |

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
| 2 | at once Simplify closed, while it works out the result, then idle | 24648 | that wait is met: closing leaves nothing running and applies nothing |
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
| 2 | `select_source_volume`, a click at `{object: 0, part: 0, spot: "front"}`, `select_tool_volume`, a click at `{object: 0, part: 1, spot: "top"}`, then `{imgui_name: "btn"}`, then idle | one part, type part, its name ending ` - union`, `mesh_volume` 8875 to 1 | the object's `bbox` is x 87.5–112.5, y 87.5–112.5, z -2.5–22.5: the union lands 2.5 mm off along each axis, its bounding-box centre where part 0's centre was, and nothing drops it back onto the bed; `gizmo()` none: with one part left the gizmo can no longer be open (`GLGizmoMeshBoolean::on_is_activable` asks for two), and the manager closes it (`GLGizmosManager::refresh_on_off_state`) |
| 3 | `undo` | as after 0 | the `bbox` as after 0 |
| 4 | `redo` | as after 2 | |

**Scenario: brim ears**

A 20 mm cube at (100, 100), selected, the camera at `iso` and fitted to it. The process's `brim_type` is `auto_brim`,
the setting's default, which no Custom process changes. What the project keeps of ears and of the brim type is
checked once, in brim ears from positions (rows 6 and 7), which reads the ears in the saved file: the gizmo as it
closes (`GLGizmoBrimEars::update_model_object`) and `object_set_brim_ears` (`set_brim_ears` in `GizmoOps.cpp`) both
leave the ears in the object's `brim_points`, which the project file writes through
`_add_brim_ear_points_file_to_archive` (`bbs_3mf.cpp`), and both leave the brim type in the object's settings.

| # | Step | After | Also checked |
|---|---|---|---|
| 0 | start | `brim_ears` 0; `setting(0, "brim_type")` value null, effective `auto_brim` from `global` | the cube (100, 100, 10) |
| 1 | Brim Ears opened | as after 0 | `panel()` shows the warning that the brim type is not set to "painted" |
| 2 | `widget_click {target: {imgui_name: "Set brim type to painted"}}` | `brim_ears` 0; `brim_type` `painted` from `object` | the warning is no longer shown |
| 3 | `{imgui: "Auto-generate"}` clicked | `brim_ears` 4 | one ear at each corner of the square first layer |
| 4 | `undo` | `brim_ears` 0 | `brim_type` still `painted`: the link took no undo step |
| 5 | `redo` | `brim_ears` 4 | |
| 6 | a click at `{object: 0, point: [0, -10, -9.5]}`, on the front face 0.5 mm above the bed | `brim_ears` 5 | |
| 7 | `undo` | `brim_ears` 4 | |
| 8 | `redo`, then Done | `brim_ears` 5 | |

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
middle-button drag pans the camera, as the baseline's camera style and `middle_mouse_drag_action`, which the test's
start reads back, make it.

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
| 13 | `object_select {none: true}`, `mouse_gesture {action: "move", at: {object: 0, part: 1, spot: "top"}}` | refused, its words "is in front of the point" | `selection()` still empty, the cube and its parts as after 0 and its profile as after 9, and the camera did not move during the step (`app_state.camera` reads as it did after `object_select`): the check runs for a negative part, and from below the cube hides its top |

The scenarios from here on call the commands. No step clicks the canvas, reads a panel's text or captures a
picture, so where on an object a text or an SVG part lands, which follows the camera and the window, is not checked.
A reply is written as the fields the row fixes; a target in a reply is written by its `index` (or `object`,
`instance`, `part`) and `name`, and its `object_id`, `part_id` or `instance_id` is not checked. Before each step that acts,
`undo_steps()` sets its mark, and the step lists the undo steps it takes; a refused step takes none.

**Scenario: opening a gizmo on a target**

Two filament slots (`filament_delete` until refused, then `add_slot()` with `SLOT_COLOURS[1]`); a 20 mm cube A at
(100, 100); a 20 mm cube B at (150, 100) with `part_add {object: 1, type: "part", shape: "cube", size: [10, 10, 10],
position: [160, 110, 20]}`; then `object_select {none: true}`. The model does not change in this scenario: after
every row it is as after 0.

| # | Step | Reply | Also checked |
|---|---|---|---|
| 0 | start | | A (100, 100, 10), one part · B (150, 100, 10), two parts; `slots()` "0,0"; `selected()` empty; `gizmo()` none |
| 1 | `object_paint {object: 0, type: "supports"}` | `gizmo` "FdmSupports", `active` true, `instances` [{object 0, instance 0, name "A"}] | `gizmo()` FdmSupports on `3d`; `selected()` objects [0], instances [[0, 0]] |
| 2 | the same call again | as after 1 | `gizmo()` FdmSupports: a second call does not close it |
| 3 | `object_paint {object: 0, type: "seam"}` | `gizmo` "Seam" | `gizmo()` Seam: the open gizmo was closed and Seam opened |
| 4 | `object_paint {object: 0, type: "color"}` | `gizmo` "Color Painting" | `gizmo()` Color Painting |
| 5 | `object_paint {object: 0, type: "fuzzy_skin"}` | `gizmo` "Fuzzy Skin Painting" | `gizmo()` Fuzzy Skin Painting |
| 6 | `object_select {none: true}` | | `gizmo()` none; `selected()` empty |
| 7 | `part_boolean {object: 1}` | `gizmo` "MeshBoolean", `instances` [{object 1, instance 0, name "B"}] | `gizmo()` MeshBoolean; `selected()` objects [1] |
| 8 | `object_simplify {object: 1, part: 1}` | `gizmo` "Simplify", `parts` [{object 1, part 1}] | `gizmo()` Simplify; `selected()` parts [[1, 1]] |
| 9 | `object_simplify {object: 0}` | `gizmo` "Simplify", `parts` [{object 0, part 0}]: A's only part | `gizmo()` Simplify; `selected()` objects [0], instances [[0, 0]]: an object of one part shows no part row in the object list, which selects it whole when its part is selected |
| 10 | `object_measure {objects: [0, 1]}` | `gizmo` "Measure", `objects` [{index 0, name "A"}, {index 1, name "B"}] | `gizmo()` Measure; `selected()` objects [0, 1] |
| 11 | `object_select {none: true}`, `object_select {parts: [[1, 1]]}`, then `object_measure {}` | `gizmo` "Measure", `parts` [{object 1, part 1}] | `gizmo()` Measure; `selected()` parts [[1, 1]]: with no target it opens on the selection as it is, a part |
| 12 | `object_select {none: true}` | | `gizmo()` none; `selected()` empty |

**Scenario: openings refused**

One filament slot (`filament_delete` until refused); A and B as in the scenario above. Each step starts from the
selection its row gives. After every row the model is as after 0, `gizmo()` is none, `undo_steps()` is empty but for
row 5's, and `selected()` is the selection the step started from. A selection change records an undo step of its own
("Selection-Add Instance"), which a refused opening must not leave behind; row 5's step, "Leaving Paint-on supports",
is the one the Support Painting gizmo records as it closes, as it does for any closing. Rows 1, 2 and 5 are refused
by the gizmo's condition, which the call asks after it has selected the named target, so they check that it puts back
the selection it found; row 3 is refused by the Simplify check, which comes before anything is selected; row 4 names
no target.

| # | Selection before | Step | Refused |
|---|---|---|---|
| 0 | | start | `selected()` empty; `slots()` "0" |
| 1 | `object_select {objects: [1]}`: objects [1] | `part_boolean {object: 0}` | `needs_two_parts` |
| 2 | `object_select {none: true}`: empty | `object_paint {object: 0, type: "color"}` | `needs_two_filaments` |
| 3 | `object_select {parts: [[1, 1]]}`: parts [[1, 1]] | `object_simplify {object: 1}` | `needs_one_part`: B has two parts and none is named |
| 4 | `object_select {none: true}`: empty | `object_measure {}` | `needs_selection` |
| 5 | `object_paint {object: 1, type: "supports"}`: objects [1], `gizmo()` FdmSupports | `part_boolean {object: 0}` | `needs_two_parts`; the FdmSupports gizmo it closed to ask stays closed |

**Scenario: cut from arguments**

A 20 mm cube A at (100, 100), nothing selected. A cut object leaves the list and its pieces are added at its end,
upper first (`Plater::apply_cut_object_to_model`), each named as the object was. A piece is written as its name, its
`bbox` size and its part's `mesh_volume` (to 1 mm³); every piece rests on the bed (`bbox.min` z 0). Rows 12 to 18 are
refused, on the whole cube row 11's undo leaves: after each, A is as after 0, `gizmo()` is none and the step took no
undo step; a refusal without a code is checked by its words. What the project keeps of cut pieces is checked once, in
cut with a connector (row 6): a call and the gizmo's Perform cut both cut with `Cut::perform_with_plane` and add the
pieces with `Plater::apply_cut_object_to_model`, and the project file writes the pieces as objects and their cut
information through `_add_cut_information_file_to_archive` (`bbs_3mf.cpp`), with the connectors only the gizmo adds.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 0 | start | A (100, 100, 10) size [20, 20, 20], one part 8000 | |
| 1 | `object_cut {object: 0}` | as after 0 | reply `gizmo` "Cut", `active` true; `gizmo()` Cut; undo steps ["Entering Cut gizmo"], the step the Cut gizmo records as it opens, as for a click on its icon |
| 2 | `object_cut {object: 0, z: 10}` | A [20, 20, 10] 4000 · A [20, 20, 10] 4000, both `bbox` x and y 90–110 | `gizmo()` none: the open Cut gizmo was closed first; reply `pieces` [{index 0, name "A", open_edges 0}, {index 1, name "A", open_edges 0}]; undo steps ["Leaving Cut gizmo", "Cut by Plane"]: the gizmo's own step as it closes, then the cut's |
| 3 | `undo` | as after 0 | |
| 4 | `redo` | as after 2 | |
| 5 | `undo` | as after 0 | |
| 6 | `object_cut {object: 0, plane: {point: [100, 100, 10], normal: [1, 0, 1]}}` | A, the upper piece: 4000, size z 14.14 to 0.01 · A, the lower piece: [20, 20, 20] 4000, `bbox` x 90–110 | the plane x + z = 110 halves the cube into two prisms; the upper one lies on its cut face, 10√2 mm below its far edge; the lower one stays as cut; reply `pieces` with 2 entries, `open_edges` 0 |
| 7 | `undo` | as after 0 | |
| 8 | `object_cut {object: 0, z: 10, keep: "upper"}` | A [20, 20, 10] 4000 | reply `pieces` [{index 0, name "A", open_edges 0}] |
| 9 | `undo` | as after 0 | |
| 10 | `object_cut {object: 0, z: 10, to_parts: true}` | A [20, 20, 20], two parts of type part, 4000 each, `bbox` x and y 90–110, z 0–20 | reply `object` {index 0, name "A"} and `parts` with 2 entries |
| 11 | `undo` | as after 0 | |
| 12 | `object_cut {object: 0, z: 50}` | as after 0 | refused `plane_misses_object` |
| 13 | `object_cut {object: 0, keep: "upper"}` | as after 0 | refused `needs_plane` |
| 14 | `object_cut {object: 0, z: 10, plane: {point: [100, 100, 10], normal: [0, 0, 1]}}` | as after 0 | refused, the words "z or plane, not both" |
| 15 | `object_cut {object: 0, plane: {point: [100, 100, 10], normal: [0, 0, 0]}}` | as after 0 | refused, the words "normal is zero" |
| 16 | `object_cut {object: 0, z: 10, keep: "upper", lower: "flip"}` | as after 0 | refused, the words "the lower piece is not kept" |
| 17 | `object_cut {object: 0, z: 10, keep: "upper", to_parts: true}` | as after 0 | refused, the words "to_parts keeps both pieces" |
| 18 | `object_cut {object: 0, z: 10, to_parts: true, upper: "flip"}` | as after 0 | refused, the words "to_parts places no piece" |

**Scenario: lay on face from a direction**

A 20 mm cube A at (100, 100), then `object_transform {object: 0, rotate: [30, 0, 0], mode: "absolute"}`, which drops
it back onto the bed, then `object_select {none: true}`. Turned 30° about X, A's own -Z face points along (0, 0.5,
-0.866) in the 3D view. Instances are written `i0`, `i1` with their offset and `rotation_deg`; angles to 0.01°. Rows
8 to 10 are refused calls on the turned cube row 7's undo leaves; after rows 8 and 9 A is as after 0 and the step
took no undo step. What the project keeps of the turns is `test_objects`' (objects.md, what the project keeps, step
3): a lay on face turns the instances as a rotation does (`Selection::flattening_rotate`, then
`GLCanvas3D::do_rotate`, in `lay_on_face` of `GizmoOps.cpp`), and the project file writes each instance's transform
whichever tool set it.

| # | Step | A after | Also checked |
|---|---|---|---|
| 0 | start | i0 (100, 100, 13.66) rot [30, 0, 0], size [20, 27.32, 27.32] to 0.01 | `bbox.min` z 0 |
| 1 | `object_lay_on_face {object: 0}` | as after 0 | reply `gizmo` "Flatten"; `gizmo()` Flatten |
| 2 | `object_lay_on_face {object: 0, normal: [0, 0, -1]}` | i0 (100, 100, 10) rot [0, 0, 0], size [20, 20, 20] | `gizmo()` none: Flatten was closed first; reply `object` 0, `instance` 0, `rotation` [0, 0, 0]; undo steps ["Gizmo-Place on Face"] |
| 3 | `undo` | as after 0 | |
| 4 | `redo` | as after 2 | |
| 5 | `undo` | as after 0 | |
| 6 | `object_lay_on_face {object: 0, normal: [0, 0.5, -0.866], frame: "world"}` | as after 2 | the world direction of A's -Z face gives the same turn |
| 7 | `undo` | as after 0 | |
| 8 | `object_lay_on_face {object: 0, frame: "world"}` | as after 0 | refused `needs_normal`: `frame` acts only with `normal`; `gizmo()` none |
| 9 | `object_lay_on_face {object: 0}`, then `object_lay_on_face {object: 0, normal: [0, 0, 0]}` | as after 0 | refused `zero_normal`; `gizmo()` Flatten: the call is refused before it closes the open gizmo |
| 10 | `object_select {none: true}` | | `gizmo()` none |
| 11 | `object_set_instances {object: 0, count: 2}` | i0 as after 0 · i1 rot [30, 0, 0] | the new instance copies i0's turn; i1's offset is the app's |
| 12 | `object_lay_on_face {object: 0, instance: 0, normal: [0, 0, -1]}` | i0 (100, 100, 10) rot [0, 0, 0] · i1 rot [0, 0, 0] | the other instance turns with it; both rest on the bed |

**Scenario: brim ears from positions**

A 20 mm cube A at (100, 100), nothing selected. The process's `brim_type` is `auto_brim`, the setting's default, and
its `initial_layer_line_width` is `120%` of the 0.4 mm nozzle (the Custom vendor's `fdm_process_common`), 0.48 mm, so
an ear's default radius is 3.84 mm, eight first-layer line widths (`GLGizmoBrimEars::get_brim_default_radius`). An
ear asked for at bed position (x, y) is stored at (x - 100, y - 100, -10.0001) in A's coordinates: the bed point at z
-0.0001 through the inverse of A's placement. Ears are written as `(x, y, z) r`, to 0.001.

| # | Step | After | Also checked |
|---|---|---|---|
| 0 | start | `brim_ears` 0; `setting(0, "brim_type")` value null, effective `auto_brim` from `global` | `settings_get` of the process's `initial_layer_line_width` "120%" |
| 1 | `object_set_brim_ears {object: 0}` | as after 0 | reply `gizmo` "Brim Ears"; `gizmo()` Brim Ears |
| 2 | `object_set_brim_ears {object: 0, points: [{position: [90, 90], radius: 3}, {position: [110, 90]}]}` | `brim_ears` 2 | `gizmo()` none; reply `ears` [(-10, -10, -10.0001) 3, (10, -10, -10.0001) 3.84], `brim_type` `auto_brim`, `takes_effect` false; undo steps ["Brim ears edit", "Leaving Brim Ears", "Set brim ears"]: the two steps the Brim Ears gizmo records as it closes, then the call's |
| 3 | `undo` | `brim_ears` 0 | |
| 4 | `redo` | `brim_ears` 2 | |
| 5 | `settings_set {target: "object", object: 0, values: {brim_type: "painted"}}`, then step 2's call again | `brim_ears` 2; `brim_type` `painted` from `object` | reply `ears` as in 2, `brim_type` `painted`, `takes_effect` true |
| 6 | `project_save` to a new file | as after 5 | `brim_file()` {1: [(-10, -10, -10.0001) 3, (10, -10, -10.0001) 3.84]} |
| 7 | `project_new {discard_changes: true}`, `project_open` of the file | `brim_ears` 2; `brim_type` `painted` | |
| 8 | `object_set_brim_ears {object: 0, clear: true}` | `brim_ears` 0 | reply `ears` [] |
| 9 | `object_set_brim_ears {object: 0}`, then `object_set_brim_ears {object: 0, points: [{position: [100, 90], radius: 5}]}` | `brim_ears` 1 | after the first, `gizmo()` Brim Ears; after the second, `gizmo()` none, and after `project_save` to a new file, `brim_file()` {1: [(0, -10, -10.0001) 5]}: the call's ear, not what the gizmo held |
| 10 | `object_set_brim_ears {object: 0, points: [{position: [90, 90], radius: 3}, {position: [110, 90], radius: 3}]}`, then `object_set_brim_ears {object: 0}` | `brim_ears` 2 | `gizmo()` Brim Ears; the opening keeps the two ears the call set with the gizmo closed, though the gizmo was last open on A holding none (D9: `reload_on_next_opening()`) |
| 11 | `object_set_brim_ears {object: 0, points: [{position: [100, 90], radius: 150}]}` | as after 10 | refused `radius_out_of_range`; `gizmo()` Brim Ears: a refused call leaves the gizmo open; no undo step |
| 12 | `object_set_brim_ears {object: 0, points: [{position: [100, 90]}], clear: true}` | as after 10 | refused, the words "points or clear, not both"; `gizmo()` Brim Ears |
| 13 | `object_select {none: true}` | `brim_ears` 2 | `gizmo()` none |
| 14 | `object_set_brim_ears {object: 0}`, then `object_set_brim_ears {object: 0, clear: true}` | `brim_ears` 0 | after the first, `gizmo()` Brim Ears; after the second, `gizmo()` none, and after `project_save` to a new file, `brim_file()` has no ears for `object_id` 1: the gizmo, closed first, did not write back the two ears it held |

**Scenario: text and SVG as a part of a named object**

A 20 mm cube A at (100, 100) and a 20 mm cube B at (150, 100), B selected (`object_select {objects: [1]}`).
`star.svg` is a copy of `tools/mcp/tests/fixtures/star.svg`, the SVG scenario's file, in the test's folder. Each step
that adds a part is followed by `app_wait {for: "idle"}`. A's parts are written as `a_parts()` gives them. An object
keeps its parts first, then its negative parts, then its modifiers (`ModelObject::sort_volumes`), so a part added
after a negative part comes before it. What the project keeps of text and SVG parts is checked in text (row 13) and
SVG (row 7): the project file writes a part's text and its SVG with the part whatever its type
(`TextConfigurationSerialization::to_xml` and the emboss shape's `to_xml`, per volume, in `bbs_3mf.cpp`), so the
negative text and the SVG parts there stand for the text part, the negative text and the SVG part and modifier here.

| # | Step | A's parts after | Also checked |
|---|---|---|---|
| 0 | start | part | B one part; `selected()` objects [1], instances [[1, 0]] |
| 1 | `object_add_text {object: 0}` | part · part "Embossed text" | `gizmo()` Text; B one part |
| 2 | `object_select {none: true}`, then `object_add_text {object: 0, type: "negative"}` | as after 1 · negative "Embossed text" | `gizmo()` Text |
| 3 | `object_select {objects: [1]}`, then `object_add_svg {object: 0, path: star.svg}` | part · part "Embossed text" · part star.svg · negative "Embossed text" | `gizmo()` SVG; B one part |
| 4 | `object_select {none: true}`, then `object_add_svg {object: 0, type: "modifier", path: star.svg}` | as after 3 · modifier star.svg | `gizmo()` SVG |
| 5 | `object_select {none: true}`, `undo` | as after 3 | adding the part is one undo step |
| 6 | `redo` | as after 4 | |
| 7 | `dialog_auto_answer {rules: [{match: ".", button: "ok", path: <test folder>/star.svg}]}`, then `object_add_svg {object: 0, type: "modifier"}` | as after 4 | the rule's `uses` is 1: the file picker opened; nothing was added, since the picker reads `GetPaths()`, which an automatic answer leaves empty |
| 8 | `object_select {objects: [0, 1]}`, then `object_add_text {type: "modifier"}` | as after 4 | refused `one_object_only`: `type` without `object` adds to the selected object, and two are selected; two objects, B one part; `selected()` objects [0, 1]; no undo step |
| 9 | `object_add_svg {type: "modifier", path: star.svg}`, the same selection | as after 4 | refused `one_object_only`; as in 8 |
| 10 | `object_select {parts: [[0, 3]]}`, A's negative part alone, then `object_add_svg {type: "modifier", path: star.svg}` | as after 4 · modifier star.svg | two objects: the SVG is A's. As a person does it: a part's menu offers no text or SVG, and a right click on A's row, whose menu offers them by type, selects A whole first; Add Modifier>Text there makes a modifier of A. So `type` alone selects the selected object whole first, as a call that names it does. (The action without `type`, the Speed Dial's, makes a new object here, since a text or SVG lands on a selected solid part, `find_closest()` in `EmbossJob.cpp`.) `gizmo()` SVG |

**To settle in review:**
- Mesh Boolean moves its result (step 2 of its scenario): a bug in the app's own code, which is on `main`, left as
  it is and a bug for a fix of its own. `GLGizmoMeshBoolean::generate_new_volume` builds the union in the object's
  frame (both parts' meshes transformed by their part matrices) and adds it with `ModelObject::add_volume`, which
  centres the mesh on its bounding box and sets the part's offset to that centre, so the union is still in place; it
  then sets the new part's offset to the source part's (`GLGizmoMeshBoolean.cpp:504`), dropping that shift, and the
  `ensure_on_bed` call after it is commented out. Step 2 expects where the union lands today: moved by (-2.5, -2.5,
  -2.5) mm, from x, y and z 90–115, 90–115 and 0–25 to 87.5–112.5, 87.5–112.5 and -2.5–22.5.
- Undo and redo lose a text part's font face name: a bug in the app's own code, on `main`, left as it is.
  `FontProp::save` and `load` (`TextConfiguration.hpp`), which undo and redo serialize the style through, leave out
  `face_name`, so after an undo `object_list`'s `text.font` gives the style's font descriptor (`style.path`, such as
  `1;9;-12;...;Times New Roman` on Windows) in place of the face name. No step reads `text.font` after an undo or a
  redo: the descriptor depends on the system's fonts.
- The links that set a setting (Fuzzy Skin's "Enable painted fuzzy skin", Brim Ears' "Set brim type to painted")
  change the object's setting without an undo step of their own (`GLGizmoFuzzySkin.cpp` and `GLGizmoBrimEars.cpp`
  assign the config directly): the app's own code, on `main`, left as it is. The entry expects that, so an undo of
  the next step keeps the setting.
- The text's Cut takes two undo steps (text, steps 9 to 11): the app's own code, on `main`, left as it is.
  `GLGizmoEmboss::draw_model_type` takes a snapshot and sets the part's type, then starts the job that moves the
  text inward, whose `update_volume` (`EmbossJob.cpp`) takes a snapshot of its own. The first undo takes back the
  move and leaves the part negative; the second takes back the type.
- The union closes Mesh Boolean (Mesh Boolean, step 2): the app's own code, on `main`. The gizmo can be open only on
  an object with more than one part (`GLGizmoMeshBoolean::on_is_activable`), and
  `GLGizmosManager::refresh_on_off_state` closes it once the union leaves one, so no step closes it.
- The SVG's file after reopening (SVG, step 7): the project keeps the SVG itself (`path_in_3mf`) and names the local
  file by its name alone, `star.svg`, since `to_xml` in `bbs_3mf.cpp` writes `filename()` unless full paths are
  exported: the app's own code, on `main`.
- `settings_get` gives a value as the config writes it, so a switch reads "1" or "0" (painting and texture layers,
  steps 0 and 28), as `wall_loops` reads "2".
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
- Other window sizes: the test runs at the baseline's 1600 × 900. An ImGui panel or a notification over a model
  point or an item is refused, which `tests/slic3rutils/test_mcp_results.cpp` checks without the app ("a click goes
  to the frontmost ImGui window", "what covers a click is named"), with the surface check's cases in
  `tests/slic3rutils/test_mcp_ui.cpp`.
- Grabbers dragged on the canvas: `mouse_gesture` sets them through the panel fields, as its description says.
- The Move panel's other align and distribute buttons and its align-to-plate mode; Rotate's and Scale's other
  fields, Reset buttons and non-uniform scale.
- Painting tools other than Fill and the circle brush (sphere, gap fill, triangle, height range), erasing with Shift,
  the smart fill angle, and where a brush's facets lie: a brush's counts are checked as above 0.
- The Painted Modifier gizmo: neither replay drives it.
- Texture Displacement's bake, its tile, rotation and midlevel values, the texture library popup, and the name of
  the texture a new layer starts with (the library's first, which depends on the installed textures). Which `n` the
  height map's copy takes. Texture layers on a part with no other painting: the scenario gives them to the part the
  painting gizmos painted, whose painting the project file writes apart from the layers (`add_texture_displacement`
  in `bbs_3mf.cpp`), and rows 27 and 28 check that painting stays as it was.
- Text: the font a style names, which depends on the system's fonts (the style combo's marker shows it is the
  style's own); the style a new text starts with, the one the Text tool last closed with; the size, boldness, line
  gap and alignment fields, the surface option, saving or renaming a style and the prompt when a modified style is
  changed, a text as a modifier, and the font after reopening.
- SVG: its size, depth and mirror fields, "Forget the file path" and reload from disk.
- Measure: edges, circles and points as features, the XYZ distance fields that move objects, and inches.
- Assembly: point-and-point mode, Flip by Face 2, rotation around the faces' centre and the Parallel button for faces
  at an angle.
- Cut: dowel and snap connectors, cutting by groove or contour, flipping or placing the halves, keeping one half,
  and the connector's size and depth, and so the lower half's height, which its connector part raises above the cut.
- Simplify by error, Mesh Boolean's difference and intersection and their Delete input option, and deleting or
  resizing brim ears in the gizmo.
- That Simplify is still working out its result when row 2 of its scenario closes it: the app decides when the
  calculation ends, and no tool reports it but `app_wait {for: "idle"}`, which waits for it to end. The row's checks
  hold whether it has ended or not.
- What a gizmo a command opens then does: the scenarios through the panels drive each one's panel. Where on A the
  text and the SVG parts a command adds land, which follows the camera; that they are A's is checked by A's parts.
- The upper piece's turn after a cut that places it on its cut face: its size and volume are checked. The call turns
  the cut plane from level by the shortest turn to its normal, which is the turn the Cut gizmo's plane takes when it
  is tilted about one axis; a plane tilted in the gizmo about two axes in turn can differ from it by a turn about the
  normal, which no argument of the call names. The unit cases of `cut_matrix()` check the turn, the straight-down
  plane (the gizmo's Flip) and a plane just off straight down.
- A cut object made of one dowel connector, which `object_cut` refuses as `dowel_alone`, and a cut object with
  connectors, refused as `has_connectors`: only the Cut gizmo's connectors make them, so the unit cases of
  `cut_refusal()` cover both.
- Variable layer height editing, under which the Cut gizmo does not open (`layers_editing_on`), and a printer that is
  not FFF (`needs_fff_printer`): unit cases of `gizmo_refusal()`.
- The default brim ear radius from a first-layer line width given in millimetres, as the Bambu Lab processes give it:
  the Klipper process gives it as a percentage of the nozzle, which the radius is checked from.
- The ears an undo brings back. With the Brim Ears gizmo closed, an undo restores an object's ears, but the gizmo
  keeps the ears it held and brings them back on its next opening. A person's own edit does the same (an ear clicked
  on, the gizmo closed, the ear undone, the gizmo opened: the ear is back), so it is the gizmo's behaviour, which the
  command leaves as it is (D9).
- That no ear is selected once the gizmo opens after a call: a check needs an ear selected first, by a click on its
  grabber at the bed under the object's edge, and the panel shows no state of it (`has_selected_points()` reads the
  ears, not the flag a click reads). `reload_on_next_opening()` clears it with the gizmo's own
  `select_point(NoPoints)`.
- `object_id`, `part_id` and `instance_id` in replies: the objects entry's naming by id checks them.
- What the painting, fuzzy skin, brim ears and connectors do to a slice: the test checks what the gizmos leave on the
  model and in the project file; turning it into toolpaths is the slicer's.
- What other tests assert: Select All with Move open, and refused with Lay on face open, in
  `test_menu_items_refusals`; Measure in the assemble view and Select All refused there in `test_person_forms`;
  arranging with Move open and from the assemble view in `test_operation_routes`; refs to the Text font button and a
  texture layer row surviving their renames in `test_runtime_calls`. The Text action is run by no test as an
  action; its command, `object_add_text`, is called in text and SVG as a part of a named object. Turned instances
  kept through the project file in `test_objects` (what the project keeps, step 5), which stands for the turns a lay
  on face leaves.

**Strength:** Strong — after every step it reads the model back through another tool and compares it with values
written out here, reads every undo before its redo, asserts each refusal by its code or words with the model, the
gizmo and the undo log read again, and checks what a person keeps through the project file, the brim ears in the
saved file itself; the counts a brush stroke gives, the Measure distance, the reverted font, where a text or SVG part
lands on the top, the cut pieces' volumes and the tilted piece's height are checked by a tolerance or a property, as
the app works them out.
