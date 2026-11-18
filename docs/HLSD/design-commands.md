# Design tab commands — High Level Design

## Purpose and scope

The Design tab is a parametric CAD modeller inside OrcaSlicer. Its MCP server gives a client 88 commands
for it, in the command catalog's group `Design`. A client that knows a mainstream CAD program can guess the
command, the argument that picks its variant and that argument's values, and can do from arguments what a
person does with the tab's tools. The same commands are what a person's Speed Dial tiles run, what Action
Replay and recordings show, and what the journal records and replays.

This document is the command set: how the commands are named and grouped, what each does and takes, how a
call opens a tool for a person instead of running, and how the commands relate to the right-click menu's
rows. How the server serves them (the catalog, `tool_search`, the gate, the journal) is in
[mcp-server.md](mcp-server.md), and the tab itself in [design-tab.md](design-tab.md).

One table defines them all: `scripts/CAD/tool_atlas.json`. `scripts/CAD/gen_offer_table.py` writes
`src/slic3r/GUI/CAD/DesignOffer.hpp` from it, and the commands' code is `src/slic3r/GUI/Mcp/McpToolsCad.cpp`.
What every command takes and replies is in its entry's tool block, which the agent reads through
`tool_search {tool}`; `tests/data/command_catalog.json` holds every command's schema, tiles and examples as
an agent sees them.

## Naming

The commands' names, their variant arguments and those arguments' values use the industry's terminology, as
popular CAD programs such as Fusion 360, Onshape and SolidWorks use it, so that people and agents who know
one of them learn the commands quickly. Three nouns are the app's own, as it shows them everywhere:
variable, body and feature.

1. **No prefix means the 3D model**: the commands that build and change the model's bodies (`extrude`,
   `fillet`, `mirror`, `pattern`, `boolean`, `split`) and the two Inspect commands that take any geometry
   (`measure`, `interference`). A prefix marks the other places:
   - `sketch_`: the sketch. The same verbs as the model's, in a sketch: `sketch_fillet`, `sketch_mirror`,
     `sketch_pattern`.
   - `surface_`: the commands that make sheet bodies.
   - `construct_`: reference planes, axes and coordinate systems.
   - `feature_`: one entry of the history: describe, edit, suppress, move, rename, delete.
   - `body_`: one body as an object: name and colour it, delete it, place it, describe and measure it.
     `body_transform` is here too, since `object_transform` is the Prepare tab's command.
2. **One command per operation, and one argument for its variants.** `type` says which kind of thing the
   command makes, or which way it makes it: a shape, a pattern's layout, a hole's kind, how a plane is
   defined. A shape drawn more than one way names the way (`rectangle_3_point`).
3. **`operation` says what the result does to the bodies already there**, in four words: `new_body`,
   `join`, `cut`, `intersect`. `extrude`, `revolve`, `sweep`, `loft`, `mirror` and `boolean` take it.
4. **Full words**: no abbreviations such as `sk_`, `surf_` or `props`.
5. **A menu row is a way to call a command, not a second command.** A call that gives only the arguments
   choosing a row opens that row's tool for a person; a call that gives more runs it.

**Body, solid and sheet.** A body is what the Bodies list shows: a solid body, which encloses a volume, or a
sheet body, which is open faces. "Solid" and "Surface" in the headings below are groups of commands, not
kinds of object: a command of the Solid group has no prefix whether it takes a sketch, faces and edges, or
whole bodies.

## The command set

The commands are listed under the headings `tool_search`' description names them under, in its order:
Document, Sketch, Solid, Surface, Construct, Inspect, Bodies, History and Menu rows. Main arguments only;
every argument, its default and its range are in the command's schema.

### Document

| Command | What it does | Variants and main arguments |
|---|---|---|
| `new_design` | Erase every feature, body and variable, and the Design undo history; committed objects stay on the plate | `discard_changes`, needed when the design is not empty |
| `describe_design` | The document: features, variables with their values, bodies with their placement, undo and redo depth, the Commit button's mode, the selection, the picked plane and the Feature list's selected row |  |
| `import_step` | Import a STEP file as bodies | `path` |
| `import_mesh` | Convert an STL or OBJ mesh into an editable body, reporting whether it is a solid or an open shell | `path`, `tolerance`, `merge_angle_deg` |
| `export_step` | Write every body to a STEP file, at its placement | `path`, `overwrite` |
| `commit_to_plate` | Send the visible bodies to Prepare at their placement, in one Prepare undo step | `mode`: assembly, bodies (left out, the Commit button's mode); `replace` |
| `set_variable` | Set a design variable to an expression, or remove it | `name`, `expr` |
| `select_geometry` | Select a body, a face, edges, a vertex, a reference plane, a sketch's loop or a feature by id, as a click selects it, for the menu-row forms to act on | one of: `body`; `body` with `face`, `edges` or `vertex`; `plane`; `sketch` with `loop`; `feature`; nothing, which clears |
| `confirm_tool` | Confirm what is open, as its Confirm button and Enter do |  |
| `cancel_tool` | Cancel what is open, as its Cancel button does |  |

### Sketch

**The sketch session**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `sketch_create` | Open a sketch on a plane or a planar face | `plane`: XY, XZ, YZ, `{datum}` or `{body, face}` |
| `sketch_edit` | Re-open a committed sketch to change its entities and constraints | `sketch` |
| `sketch_finish` | Commit the open sketch as a Sketch feature, or replace the re-opened one in place |  |
| `sketch_cancel` | Discard the open sketch; a re-opened one keeps what it had |  |
| `sketch_describe` | A sketch's entities, constraint count and loops; the open one's degrees of freedom, armed tool and selection | `sketch`, left out for the open one |
| `sketch_validate` | The open sketch's closed loops, open ends, defects and degrees of freedom | `tolerance` |
| `sketch_select` | Select sketch entities by index | `entities` |

**Create**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `sketch_draw` | Draw a shape, or a list of shapes | `type`: the 19 shapes below, with the shape's fields; or `entities`, a list of shapes; `construction` |
| `sketch_text` | Text as a Text feature on a plane or a planar face, outlined in a font | `text`, `font`, `bold`, `italic`, `height` (0.5 to 500 mm); `plane`, `position` |
| `sketch_insert_svg` | An SVG file's filled outlines: into the open sketch as lines, or onto the model as art on a plane | `path`, `scale`, `position`; `plane` with no sketch open |
| `sketch_mirror` | A mirrored copy of entities, bound Symmetric to their sources | `axis`: a line of the sketch, `x_axis` or `y_axis`; or `axis_a` and `axis_b`, two free points that bind nothing; `entities` |
| `sketch_pattern` | Copies of entities in a row or around a point, each bound to its source | `type`: linear, circular; `count`; `dx`, `dy` (linear); `angle`, `pivot` (circular); `entities` |
| `sketch_project` | A body's edges: with no sketch open a Project feature, a sketch on a plane that follows the body; with one open, copies in it as construction entities | `source_body`, `face` or `edges`; `plane` with no sketch open |
| `sketch_dimension` | A dimension: a distance, angle, radius or diameter | `type`: distance, distance_x, distance_y, angle, radius, diameter; `entities` or `refs`; `value` |
| `sketch_set_value` | The size of the selected entities, as the value field sets it: a line's length, a circle's diameter, an arc's radius, the angle or distance between two | `value` |

**Modify**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `sketch_fillet` | Round the corner of two lines | `entities`, `radius` |
| `sketch_chamfer` | Bevel the corner of two lines | `entities`, `distance` |
| `sketch_trim` | Trim the entity nearest a point, as a Trim click there does | `at`, `tolerance` |
| `sketch_extend` | Extend the line or arc nearest a point to the next entity | `at`, `tolerance` |
| `sketch_offset` | An offset copy of lines, arcs and circles; one entity's copy is bound Parallel or Concentric to it | `distance`, `entities` |
| `sketch_transform` | Move, rotate or scale entities | `type`: move, rotate, scale; `dx`, `dy`; `angle`; `scale`; `pivot`; `entities` |
| `sketch_blend_curve` | A smooth curve joining the ends of two entities of a committed sketch | `sketch`, `ent_a`, `end_a`, `ent_b`, `end_b` |
| `sketch_toggle_construction` | Switch entities between construction and normal | `entities` |
| `sketch_delete` | Delete entities | `entities` |
| `sketch_repair` | Weld endpoints that nearly meet and constrain them coincident, so loops close | `tolerance`, `ignore_construction` |

**Constraints**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `sketch_constrain` | Add a geometric constraint | `type`: fix, coincident, horizontal, vertical, lock_x, lock_y, equal_length, parallel, perpendicular, concentric, tangent, midpoint, symmetric, point_on_line, point_on_object, equal_radius, collinear, symmetric_about_y, symmetric_about_x, equal; `entities` or `refs`; `value` for lock_x, lock_y and point_on_line |
| `sketch_list_constraints` | A sketch's constraints and dimensions, by index, with their references and values | `sketch`, left out for the open one |
| `sketch_delete_constraint` | Delete one constraint or dimension; those after it move down one index | `index` |

### Solid

**Create**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `extrude` | Extrude a committed sketch, one of its loops, a face (push/pull) or a profile drawn in the call | `operation`: auto, new_body, join, cut, intersect; `end`: blind, symmetric, two_sided, through_all, up_to_face; `sketch`, `loop`, `face` or `profile`; `distance`, `taper` |
| `revolve` | Revolve a committed sketch or a profile drawn in the call about an axis | `operation`; `sketch`, `axis_entity` or `axis`; `angle`, `flip` |
| `sweep` | Sweep a committed sketch's closed profile along a sketch or a helix | `operation`; `sketch`, `path` |
| `loft` | Loft through two or more committed sketches, in order | `operation`; `profiles`, `ruled` |
| `rib` | A thin wall grown from a sketch line and fused to a body | `sketch`, `entity`, `thickness`, `depth` |
| `hole` | Drill a hole on a plane or a face | `type`: simple, counterbore, countersink; `diameter` or `designation` (a standard clearance size, M6); `depth`, `through`; `x`, `y` or `at` |
| `thread` | Cut a thread along a cylindrical face or a circular edge, or on a plane | `face` or `edge`; `designation`, `internal`, `diameter`, `pitch`, `length` |
| `thicken` | Thicken a face of a body, or a whole sheet body, into a solid | `type`: face, surface; `body`, `face`, `thickness`, `flip` |
| `pattern` | Copies of a body in a row, around the plane's normal, or along a sketch curve | `type`: linear, circular, curve; `count`; `spacing`, `dir` (linear); `angle` (circular); `sketch`, `entity` (curve) |
| `mirror` | Mirror a body about a plane | `plane`; `operation`: new_body, join; `keep_original` |

**Modify**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `fillet` | Round edges of a body | `edges`: edge ids, or the card's edge group top, bottom, lateral or all; `radius` |
| `chamfer` | Chamfer edges of a body | `edges`, as `fillet` takes them; `distance` |
| `shell` | Hollow a body to a wall thickness, inward | `thickness`; `face` to leave open |
| `draft` | Taper a face by an angle about its base | `face`, `angle` |
| `delete_face` | Remove faces and heal the gap | `body`, `faces` |
| `boolean` | Join two bodies, cut one from the other, or keep their overlap | `operation`: join, cut, intersect, required in a call that runs; `target`, `tool`, `keep_tool` |
| `split` | Split a body with a plane, or along the plane of a face | `plane`, `offset`, `flip`; or `face`; `keep_upper`, `keep_lower` |
| `align` | Move one body so its coordinate system lines up with another's; the two stay separate bodies | `type`: fastened, planar, revolute, slider, cylindrical; `cs_a`, `cs_b`, `offset`, `angle`, `flip` |
| `list_align_types` | Which `align` types fit a pair of coordinate systems, and why each other one does not | `cs_a`, `cs_b` |

### Surface

| Command | What it does | Variants and main arguments |
|---|---|---|
| `surface_extrude` | Extrude a sketch into an open sheet | `sketch`, `distance` |
| `surface_revolve` | Revolve a sketch into an open sheet | `sketch`, `angle`; `axis_entity` or `axis`; `flip` |
| `surface_loft` | Skin two or more sketches into an open sheet | `profiles`, `ruled` |
| `surface_patch` | Fill a sketch's closed boundary with one face | `sketch` |
| `surface_offset` | Offset a sheet body into another sheet | `body`, `offset` |

### Construct

| Command | What it does | Variants and main arguments |
|---|---|---|
| `construct_plane` | Add a reference plane | `type`: offset, angle, midplane, tangent, two_edges, coincident; `base`, `offset`, `angle`, `face_a`, `face_b`, `edge_a`, `edge_b` |
| `construct_axis` | Add a reference axis | `type`: two_points, face_normal, cylinder, plane_intersection, along_edge |
| `construct_coordinate_system` | Add a coordinate system, which `align` lines up | `type`: point_world, face_and_direction |
| `helix` | A helical curve, to sweep along | `radius`, `pitch`, `height`, `left_handed`, `taper_deg`, `plane` |

### Inspect

| Command | What it does | Variants and main arguments |
|---|---|---|
| `measure` | Distance, closest points, angle, and the gap between parallel faces, of two faces, edges, axes or points | `a`, `b` |
| `interference` | The solid bodies that overlap, with the overlapping volume | `min_volume` |
| `body_describe` | A body's faces and edges, with ids, kinds and geometry, filtered and paged; its vertices on request | `body`; filters by kind, direction, extreme and convexity; `adjacency`; `vertices`; `before`: a feature, for the body as it stood just before it |
| `body_mass_properties` | Volume, surface area, centre of mass, inertia | `body` |
| `body_section` | The cross-section of a body at a base plane through the world origin, as contours | `body`, `plane`, `offset` |
| `body_compare` | Compare a body with another body or a STEP file: volume, bounding box, centroid, surface deviation | `body`, `reference` |

### Bodies

| Command | What it does | Variants and main arguments |
|---|---|---|
| `body_transform` | Move or rotate a body, as a feature in the history | `dx`, `dy`, `dz`; `axis_x`, `axis_y`, `axis_z`, `angle`, pivot; `copy` |
| `body_set` | Rename, colour, show or hide a body | `name`, `color`, `visible` |
| `body_delete` | Delete a body with the feature that made it | `body` |
| `body_place` | Place a body where Commit to Plate and Export STEP send it, outside the history: move it, turn it, lay a face on the bed, or put it back | `body`; `move` and `rotate`, or `face`, or `reset` |

### History

| Command | What it does | Variants and main arguments |
|---|---|---|
| `feature_describe` | One feature as its card shows it: parameters, the editable ones with their ranges, expressions, references, what uses it, the bodies it made and the bodies just before it | `feature` |
| `feature_edit` | Change a feature's fields and what it refers to, as its card's Confirm does when editing | `feature`, `fields`, `refs` |
| `feature_set_expression` | Bind a feature's numeric field to an expression over the variables, or unbind it | `feature`, `field`, `expr` |
| `feature_suppress` | Leave a feature out of the rebuild, or bring it back | `feature`, `suppressed` |
| `feature_move` | Move a feature one place up or down the history | `feature`, `delta` |
| `feature_rename` | Rename a feature | `feature`, `name` |
| `feature_delete` | Delete a feature, with what is built on it | `feature` |

### Menu rows

These take no arguments and do what their row of the right-click menu does.

| Command | What it does |
|---|---|
| `rename_selected` | Rename the selected body, else the selected feature, in place |
| `zoom_to_selection` | Fit the view to what is picked in the view |
| `sketch_polygon_inscribed` | Set the Polygon tool to fit inside its circle |
| `sketch_polygon_circumscribed` | Set the Polygon tool to fit around its circle |
| `sketch_constrain_selected` | Open Constrain mode on the selected committed sketch |

### Outside the Design table

`undo` and `redo` with `{history: "design"}` step the Design history. `view_set` with `tab: "design"` takes
the Design view's options: `camera: "sketch_normal"`, which looks straight at the open sketch and is refused
with none open; `section` `{on, z, keep}`, a horizontal cut at world `z` (left out, the model's middle) that
keeps the lower or the upper half; and `origin_planes`, `world_axes`, `origin` and `bed`, which show or hide
the origin helper planes, the world axes, the Origin row's pickable planes and the bed.

## `sketch_draw`: one `type` for every shape

| `type` | The app's tool | Fields |
|---|---|---|
| `line` | Line | `p0`, `p1` |
| `polyline` | Polyline | `points`, two or more; a loop when the last is the first |
| `rectangle_2_point` | Corner rectangle | `p0`, `p1`: opposite corners |
| `rectangle_center` | Center rectangle | `center`, `corner` |
| `rectangle_3_point` | Oblique rectangle | `p0`, `p1`: one side; `width`, signed, to the left of `p0` to `p1` |
| `rectangle_rounded` | Rounded rectangle | `p0`, `p1`: opposite corners; `radius`, less than half the shorter side |
| `circle_center` | Center circle | `center`, `radius` |
| `circle_2_point` | 2-point circle | `p0`, `p1`: the ends of a diameter |
| `circle_3_point` | 3-point circle | `p0`, `p1`, `p2` |
| `arc_3_point` | 3-point arc | `p0`: start; `p1`: end; `on_arc` |
| `arc_center` | Center-point arc | `center`, `radius`, `start_angle`, `end_angle` |
| `arc_tangent` | Tangent arc | `end`; `entity`, the line or arc it leaves, at that one's end along its direction there: one the sketch has or one an earlier item of the call adds; left out, the entity added last |
| `slot` | Slot | `c0`, `c1`, `width` |
| `slot_arc` | Arc slot | `center`, `start`, `end` (its direction from the centre), `width` |
| `ellipse` | Ellipse | `center`, `radius` (semi-major), `rminor`, `rotation` |
| `ellipse_arc` | Elliptical arc | the ellipse's, `start_angle`, `end_angle` (parametric, counter-clockwise) |
| `spline` | Spline | `ctrl`, two or more poles |
| `polygon` | Triangle to Dodecagon | `center`, `sides` (3 to 1000), `radius` (to a vertex) or `across_flats`, `angle` |
| `point` | Point | `p` |

Points are `[x, y]` in the sketch plane, in mm; angles are radians, or degrees under the `_deg` name
(`start_angle_deg`, `rotation_deg`). A type refuses a field it does not take.

- **One shape or a list.** A call gives `type` with its fields beside it, or `entities`, a list of shapes
  drawn in order, each an object with its own `type` and fields. Beside `entities` a shape's field, `type`
  among them, is refused, and an empty list is refused. `construction` beside `type` is that shape's;
  beside `entities` it is each item's that gives none of its own. A call refused at any item leaves the
  sketch as it was.
- **Built as its tool builds it.** Each shape is turned into the clicks its tool takes and built by the
  tool's own builder (`DesignSketchTool::add_shape_scripted()`), with the tool's grouping: a rectangle, a
  slot or a polygon is one shape, which one undo of the sketch takes off whole, and a polyline is its lines,
  as its tool leaves them, so an undo takes off its last line.
- **Constraints.** Each shape carries the constraints its tool gives it, at any slant: a 3-point rectangle's
  and a square's right angles, a tangent arc's tangency to the line it leaves, a slot's tangencies and equal
  ends, an arc slot's equal ends, a rounded rectangle's level sides, tangencies and equal radii. A
  constraint between a shape and the sketch's axes, or what is already in the sketch, or between plain
  lines, is inferred only where it holds exactly, so that none moves a point the caller gave. A square drawn
  on the axes therefore has its tool's constraint count and degrees of freedom, with right angles where the
  tool pins its sides Horizontal and Vertical: it is level only on a side lying exactly on an axis.
- A polygon is lines; its side count is not kept once the sketch is committed.
- Reply: `first_index`, `added`, `shapes` (`{first, count}` per item), `entities`, `dof`.

## The arguments

### The arguments that say which variant

| Command | Argument | Values |
|---|---|---|
| `sketch_draw` | `type` | the 19 shapes above |
| `sketch_transform` | `type` | move, rotate, scale |
| `sketch_pattern` | `type` | linear, circular |
| `sketch_dimension` | `type` | distance, distance_x, distance_y, angle, radius, diameter |
| `sketch_constrain` | `type` | the 20 geometric constraints |
| `hole` | `type` | simple (the default), counterbore, countersink |
| `thicken` | `type` | face, surface; left out, face when `face` is given, else surface |
| `pattern` | `type` | linear (the default), circular, curve |
| `align` | `type` | fastened (the default), planar, revolute, slider, cylindrical |
| `construct_plane` | `type` | offset (the default), angle, midplane, tangent, two_edges, coincident |
| `construct_axis` | `type` | two_points (the default), face_normal, cylinder, plane_intersection, along_edge |
| `construct_coordinate_system` | `type` | point_world (the default), face_and_direction |
| `boolean` | `operation` | join, cut, intersect; no default |
| `extrude` | `operation` | auto, new_body, join, cut, intersect; left out, auto with a sketch or a face (join the body the profile touches, else a new body, as the card's default) and new_body without |
| `revolve`, `sweep`, `loft` | `operation` | new_body (the default), join, cut, intersect |
| `mirror` | `operation` | new_body (the default), join |

An argument of one variant is refused with another: a seat size on a plain hole, `spacing` with a circular
pattern, `dx` with a circular sketch pattern, `face` with `thicken {type: "surface"}`.

**A guessed word.** Where a command has no argument of the name a call gives and the value fits, the server
reads `type` as `operation` and `operation` as `type`, so `boolean {type: "cut"}` and `extrude {type:
"cut"}` cut; it reads `index` as `feature`, and `params` and `values` as `fields`. Each schema shows one name.

**A feature's fields.** `feature_describe`, `feature_edit` and `feature_set_expression` use the engine's
field names (`hole_diameter`, `mate_offset`, `cut_offset`), which the project file stores and expressions bind
to, except where a field holds a variant these commands name: the mode of a feature that makes or changes a
body is read and written as `operation` (new_body, join, cut, intersect), and an align's kind, a hole's style
and a pattern's layout as `type` in the commands' words. A feature's type name (`Mate`, `Cut`,
`SurfaceFill`, `CoordSys`) is the app's name for it in the history.

### Refused and ignored arguments

- A null argument is an argument not given, as the schemas' null defaults say.
- 31 commands have a closed schema, so an argument they do not have is refused as an unknown argument:
  `align`, `body_place`, `boolean`, `cancel_tool`, `chamfer`, `confirm_tool`, `extrude`,
  `feature_suppress`, `fillet`, `hole`, `loft`, `mirror`, `pattern`, `revolve`, `select_geometry`,
  `sketch_constrain`, `sketch_dimension`, `sketch_draw`, `sketch_extend`, `sketch_insert_svg`,
  `sketch_pattern`, `sketch_text`, `sketch_transform`, `sketch_trim`, `sweep`, `thicken`, and the five
  menu-row commands (`closed` in the tool block). The others have open schemas.
- **`generation`.** Every command but the menu-row ones takes `generation`, the generation (`describe_design`,
  `body_describe`) the face and edge ids it passes were read at. Ids expire when a feature rebuilds the model,
  and a stale id can still name a real face or edge, so a call whose ids were read before the model changed
  is refused.

### How a failure shows

A command that fails is an error (`code` `-32000`) and leaves the design as it was, so `replay_export` leaves
it out. That holds for argument errors and for a rebuild the engine rejects alike. Twenty-one commands
instead report a rejected rebuild in their reply, `ok` false or `error` not empty, after the same rollback,
so such a call is journaled and replays the same rejection: `body_transform`, `boolean`, `construct_axis`,
`construct_coordinate_system`, `delete_face`, `draft`, `feature_set_expression`, `import_mesh`,
`import_step`, `mirror`, `rib`, `set_variable`, `shell`, `sketch_blend_curve`, `sketch_finish`,
`sketch_project` and the five `surface_` commands (`ok_false` in the tool block). Each says so in its
description.

## When a call opens a tool

One rule holds for every Design command: **a call whose arguments are exactly those that choose a menu row
opens that row's tool for a person, as a pick of the row does; any other call runs from its arguments.**

- No arguments choose the command's own row. A command with no row of its own is chosen with no arguments
  by the rows folded into it with `{}`: Color for `body_set`, and the three value rows for
  `sketch_set_value`.
- A row may be chosen by arguments: `boolean {operation: "join"}`, `pattern {type: "circular"}`,
  `sketch_draw {type: "slot"}`, `sketch_draw {type: "polygon", sides: 6}`. All of a command's variants open
  the same way.
- A command that a call with no arguments opens has `silent` in its schema. `silent: true` beside the
  choosing arguments, or beside none, runs the command with its defaults. `sketch_draw`, `sketch_transform`
  and `sketch_pattern` have no `silent`: their rows are chosen by `type`, and `silent` beside it is refused.
- Arguments are compared after the server normalizes them, as JSON values. `generation`, or anything else
  beside the choosing arguments, makes the call run.
- The tool opens through `open_tool()` on the row's entry, only in the mode the row belongs to (a sketch row
  in a sketch, a model row outside one); in the other the call is refused, naming the mode. A row the menu
  greys for the selection is refused for it, naming the selection and the kinds the row accepts. A call made
  while a row of the Feature list is selected and nothing is picked in the view takes that row as what four
  rows act on: `feature_edit`, `rename_selected`, `feature_delete` and `sketch_constrain_selected`.
- The reply is `{"result": {"interactive": true, "tool", "applies", "modal", "selection_kind", "busy",
  "status"}}`: the command called, whether the row accepts the selection, whether a dialog the action
  opened held the call until it was answered, the selection's kind, what the tab has open after the action,
  and its status line. It claims nothing about what the action did, which may be to open a card, arm a
  sketch tool, act at once, or say on the status line why it cannot.
- A command's description ends with the calls that open a tool, each with its row's name, so that a search
  for "rounded rectangle" or "polar array" finds the command.

### The menu rows and the calls they make

| Command | Mode | Its rows, and the call each makes |
|---|---|---|
| `sketch_create` | model | Sketch `{}` |
| `extrude` | model | Extrude `{}` |
| `revolve` | model | Revolve `{}` |
| `sweep` | model | Sweep `{}` |
| `loft` | model | Loft `{}` |
| `thicken` | model | Thicken `{"type": "face"}`; Thicken Surface `{"type": "surface"}` |
| `rib` | model | Rib `{}` |
| `boolean` | model | Join `{"operation": "join"}`; Subtract `{"operation": "cut"}`; Intersect `{"operation": "intersect"}` |
| `surface_extrude` | model | Surface Extrude `{}` |
| `surface_revolve` | model | Surface Revolve `{}` |
| `surface_loft` | model | Surface Loft `{}` |
| `surface_patch` | model | Surface Fill `{}` |
| `hole` | model | Hole `{}` |
| `thread` | model | Thread `{}` |
| `shell` | model | Shell `{}` |
| `split` | model | Cut `{}` |
| `fillet` | model | Fillet `{}` |
| `chamfer` | model | Chamfer `{}` |
| `draft` | model | Draft `{}` |
| `surface_offset` | model | Surface Offset `{}` |
| `pattern` | model | Linear pattern `{"type": "linear"}`; Circular pattern `{"type": "circular"}` |
| `mirror` | model | Mirror `{}` |
| `body_transform` | model | Move `{}` |
| `align` | model | Mate `{}` |
| `construct_plane` | model | Plane `{}` |
| `construct_axis` | model | Axis `{}` |
| `construct_coordinate_system` | model | Coordinate system `{}` |
| `helix` | model | Helix `{}` |
| `sketch_project` | model | Project `{}` |
| `body_mass_properties` | model | Volume and area `{}` |
| `interference` | model | Interference `{}` |
| `feature_edit` | model | Edit `{}` |
| `rename_selected` | model | Rename… `{}` |
| `delete_face` | model | Delete Face `{}` |
| `zoom_to_selection` | model | Zoom to selection `{}` |
| `feature_delete` | model | Delete `{}` |
| `body_delete` | model | Delete Body `{}` |
| `body_set` | model | Color `{}` |
| `sketch_constrain_selected` | model | Constrain sketch `{}` |
| `sketch_polygon_inscribed` | sketch | Inscribed `{}` |
| `sketch_polygon_circumscribed` | sketch | Circumscribed `{}` |
| `sketch_text` | sketch | Text `{}` |
| `sketch_insert_svg` | sketch | SVG `{}` |
| `sketch_offset` | sketch | Offset `{}` |
| `sketch_trim` | sketch | Trim `{}` |
| `sketch_fillet` | sketch | Fillet `{}` |
| `sketch_chamfer` | sketch | Chamfer `{}` |
| `sketch_mirror` | sketch | Mirror `{}` |
| `sketch_dimension` | sketch | Dimension `{}` |
| `sketch_constrain` | sketch | Constrain `{}` |
| `sketch_toggle_construction` | sketch | Construction `{}` |
| `sketch_extend` | sketch | Extend `{}` |
| `sketch_delete` | sketch | Delete `{}` |
| `sketch_set_value` | sketch | Length… `{}`; Radius / diameter… `{}`; Angle / distance… `{}` |
| `sketch_transform` | sketch | Move `{"type": "move"}`; Rotate `{"type": "rotate"}`; Scale `{"type": "scale"}` |
| `sketch_pattern` | sketch | Linear array `{"type": "linear"}`; Polar array `{"type": "circular"}` |
| `sketch_draw` | sketch | Line `{"type": "line"}`; Polyline `{"type": "polyline"}`; Corner rectangle `{"type": "rectangle_2_point"}`; Center rectangle `{"type": "rectangle_center"}`; Oblique rectangle `{"type": "rectangle_3_point"}`; Rounded rectangle `{"type": "rectangle_rounded"}`; Center circle `{"type": "circle_center"}`; 2-point circle `{"type": "circle_2_point"}`; 3-point circle `{"type": "circle_3_point"}`; 3-point arc `{"type": "arc_3_point"}`; Tangent arc `{"type": "arc_tangent"}`; Center-point arc `{"type": "arc_center"}`; Slot `{"type": "slot"}`; Arc slot `{"type": "slot_arc"}`; Ellipse `{"type": "ellipse"}`; Elliptical arc `{"type": "ellipse_arc"}`; Spline `{"type": "spline"}`; Triangle `{"type": "polygon", "sides": 3}`; Square `{"type": "polygon", "sides": 4}`; Pentagon `{"type": "polygon", "sides": 5}`; Hexagon `{"type": "polygon", "sides": 6}`; Octagon `{"type": "polygon", "sides": 8}`; Dodecagon `{"type": "polygon", "sides": 12}`; Point `{"type": "point"}` |

`sketch_text {}` opens the Text dialog in a sketch, where its row is; its argument form runs on the model
and is refused while a sketch is open.

## The table's entries

`tool_atlas.json` has 127 entries. Each is one of these:

- **A command with a row**: the row's slot, key, action, accepts, mode, name, icon and hint, and the
  command's tool block, under the command's name. 47 commands.
- **An argument-only command**: a tool block and no action. 36 commands: 35 have no row, and `measure`'s
  row, which has no action, is shown disabled.
- **A menu-only command**: an action and no tool block. Five, under Menu rows.
- **A folded row**: a row that is no command. It keeps its id, which the menu, the keys, the toolbar, the
  tiles and the GUI tests use, and names its command and the arguments that choose it: `"fold": {"into":
  "sketch_draw", "args": {"type": "circle_center"}}`. 37 rows, folded into eight commands.
- **A greyed row**: a row with neither, shown disabled. Two: Pattern on Curve (`pat_curve`), whose function
  is `pattern {type: "curve"}`, and Align to (`align_to`), which is not built.

That is 88 commands and 89 rows with an action, each of which has a Speed Dial tile.

The right-click menu's rows and their order are ratified (`_ratified` in the atlas): a row's index is its
address, and changing one is a breaking change.

## Tiles, titles, the journal and replays

- Every entry with an action has a Speed Dial tile while the Design tab is shown, in its mode. A command's
  own row's tile is under the command's key (`orca_command:sketch_trim:orca`); a folded row keeps its tile
  under its own id (`orca_command:sk_array:orca`) with its row's title and icon.
- A pick of a folded row's tile runs its command with the row's arguments, with the MCP server on or off.
  Its action shows an agent no arguments, and `action_run` takes none for it, or exactly the row's; that is
  what the journal records, so a replay runs it again.
- A call of a command with a row's arguments is titled as that row's tile wherever a person is shown the
  call: a job notice, a recording's caption, an Action Replay step (`replay_what_text()`).
- A folded tile that is not shown is named, when a call asks for it, with the call that runs it from any
  tab: `tool_call {"tool": "sketch_pattern", "args": {"type": "linear"}}`.
- The MCP skill's reference (`tools/mcp/gen_skill_reference.py`) gives a command a row of its own only when
  it has an action of its own, and one row per folded tile with its arguments.

## When a command may run

A command's tool block has an `access`, which decides when the Design tab lets it run
(`DesignPanel::mcp_busy()`):

- **read** only looks, and runs under anything but a rebuild.
- **sketch** drives the sketch session: it opens, re-opens, finishes or cancels it, or acts on the open
  sketch, and `sketch_project` and `sketch_insert_svg` add to the open sketch when there is one.
- **change** edits the feature list or the variables, or sets what is selected in the model.
- Both sketch and change are refused under an open card, the Text dialog, Constrain mode or a body Move,
  and change under a sketch session too.
- **ui** acts on what the tab has open (`confirm_tool`, `cancel_tool`), so only a rebuild refuses it.
- The opening of a tool is refused only by a rebuild, as the Design tab's keys and menu are.

## Behaviour a caller relies on

- **A re-opened sketch.** `sketch_edit {sketch}` re-opens a committed sketch, as Edit on its Feature list
  row does; the `sketch_` commands then act on it. It is refused while a sketch is open, and for text,
  imported art, a Project sketch and a sketch not drawn from entities. `sketch_finish` replaces it in place,
  in one undo step, rebuilds what is built on it, and replies `replaced: true`. A replace the rebuild
  refuses, or a re-opened sketch left empty, ends the edit with the design as it was and replies `ok: false`,
  `replaced: false`, its `sketch_index` and an `error` that says why. `sketch_cancel` drops the changes. An
  extrude of one loop keeps its own copy of that loop and does not follow the edit.
- **What `refs` takes.** `feature_edit {refs}` changes what a feature refers to, under the names
  `feature_describe` reports, for what the feature's card can pick again when it edits: a revolve's axis, a
  sweep's path, a loft's profiles, a boolean's two bodies, a mirror's, split's, helix's, hole's or thread's
  plane, a rib's line, an align's coordinate systems, a fillet's or chamfer's edges or edge group, a shell's,
  draft's or thicken's face, the faces of a Delete Face, the picks of a plane, axis or coordinate system,
  and the target body. An extrude's profile and target are fixed. A feature index must name an earlier
  feature of the right type. Body indices and face and edge ids are those of the model just before the
  feature, which `body_describe {body, before: feature}` reads and `feature_describe`'s `bodies_before`
  lists; a Plane's, an Axis's and a Coordinate system's are the finished model's (`feature_describe`'s
  `ids_of` says which). `fields` takes a Text feature's words, font and height, which outline it again, and
  imported art's position. One undo step; a rebuild that fails is rolled back.
- **The selection.** `select_geometry` selects as a click does and replies the selection as
  `describe_design` reports it. A loop is numbered as `sketch_describe {sketch}` numbers `closed_loops` and
  `extrude {sketch, loop}` takes it, and is selected only while the view shows it. A plane is picked only
  where a click can pick one: while the Origin row shows the planes, or while Sketch waits for its plane,
  where picking it opens the sketch. It is refused while a card, a sketch, Constrain mode, a body Move or the
  Text dialog is open, where a click means something else.
- **What is open.** `confirm_tool` takes, in this order: the Text dialog, a value field, a body Move, a
  text or art placement, a feature card, a sketch, Constrain mode. A card adds or edits its feature in one
  undo step and closes; an add the card refuses, or whose rebuild fails, leaves no feature and no undo step
  and keeps the card open with the reason on the status line, and the reply says `ok: false`. It is refused
  when nothing is open, while Sketch waits for its plane, and while Confirm is off, with the status line as
  the reason. `cancel_tool` takes the same states, and Sketch waiting for its plane; an open sketch is
  discarded without the dialog a person gets. Both reply `was`, what was open.
- **A body's placement.** `body_place` sets where Commit to Plate and Export STEP send a body, as the Bodies
  row's Move and Place on Face do: outside the history, with no undo step, and not kept in the project file.
  `move` and `rotate` add to where the body stands, the turn about its centre; `face` lays that face on the
  bed; `reset` puts it back where the model has it. A placement follows its body when the bodies are
  renumbered. `describe_design` reports each body's `placement`.
- **Mirror and offset bindings.** `sketch_mirror {axis}` binds each copy Symmetric to its source about the
  axis, as the Mirror tool's Confirm does, unless that would move anything off its reflection. A fillet or a
  chamfer lets go of a Symmetric constraint on an end it trims. `sketch_offset` binds one entity's copy
  Parallel or Concentric to it. Each reply lists the constraints added.

## Where the app's label differs from the command's name

The app's labels follow the right-click menu, which is ratified, and in these places they differ from the
command a call names.

| Where | The app says | Command |
|---|---|---|
| Sketch, Repeat row | Linear array, Polar array | `sketch_pattern {type: linear}`, `{type: circular}` |
| Remove row | Cut, which divides a body with a plane | `split` |
| Boolean card and rows | Join, Subtract, Intersect | `boolean {operation: join}`, `{operation: cut}`, `{operation: intersect}` |
| Surface flyout | Surface Fill | `surface_patch` |
| Reference row | Volume and area | `body_mass_properties` |
| Rectangle flyout | Corner rectangle, Oblique rectangle | `sketch_draw {type: rectangle_2_point}`, `{type: rectangle_3_point}` |
| Transform row | Move on the menu row, Transform on the toolbar and the card | `body_transform` |
| Placement flyout | Mate | `align` |
| Feature row | the eye, show or hide | `feature_suppress` |
