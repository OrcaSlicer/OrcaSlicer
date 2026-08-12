# The Design tab

Object-driven parametric CAD inside the slicer. Point at geometry; the geometry offers the
verbs that apply to it. Selection comes first and the tool consumes it. Draw a sketch,
constrain it, turn it into a solid, refine it, and send it straight to Prepare — without
leaving for another application and coming back through an STL.

The model is a **recipe**, not a mesh. Every action becomes a feature in a tree that is
replayed from the start whenever anything changes, so editing a dimension you set twenty
steps ago rebuilds everything downstream. The geometry kernel is OCCT, which the slicer
already ships for STEP import.

---

## Getting started

1. Open the **Design** tab.
2. Click a face or a reference plane in the viewport, then press `Shift+S` (Sketch). The offer
   opens with the sketch tools on it.
3. Draw a closed profile, then press **✓ Confirm** in the floating action bar.
4. With the sketch selected, press `Shift+E` (Extrude).
5. Press **Commit to Plate** to hand the solid to Prepare.

The status line under the toolbar is the thing to watch: it says what the current tool is
waiting for. When no plane is picked it reads *"Click a face or a reference plane in the
viewport, then a sketch tool"*; once one is picked it reads *"Sketching on <face> — pick a
tool"*. It is also where a refusal explains itself.

---

## Selecting

- One left-click selects what is under the cursor. There is no click-cycling through
  face → edge → body.
- A click near a corner takes the corner, not the face behind it.
- Left-drag sweeps a rubber band, and a rubber band takes the whole body.
- An open sketch line can be clicked, even where it bounds a region.
- Double-click a sketch stroke to edit it — the gesture belongs on the geometry.
- Editing a dimension's value **updates** that dimension instead of adding a second one next
  to it.
- The floating chrome that belongs to a sketch leaves with the sketch when it ends.
- Sketching happens on the face you clicked, first click.
- A sketch whose entities form no wire **fails** instead of extruding a default box. A
  subtraction that removes nothing is reported as an error instead of a silent success.

---

## The offer

Right-click on the geometry, released without moving the mouse (an 8 px budget — a
right-drag that orbits the camera does not open it). Left-click still only selects, so
pointing at things stays quiet.

The offer also opens by itself the moment you press Sketch on a face or plane, showing the
sketch tools — the app hands you the tools directly.

**Eight families, always in this fixed order:** Create, Add material, Remove, Dress-up,
Repeat, Transform, Reference, Modify.

- A family with at least one applicable verb shows it. Several applicable verbs collapse
  into a submenu under the family name.
- A family with nothing applicable is **shown greyed in place, with the reason** — e.g.
  *"Create — Click a face or a reference plane in the viewport, then a sketch tool"*. It is
  not hidden. A control that cannot be used still says what it is and what you would have to
  do first.
- Inside a sketch the offer shows the sketch verbs; outside it shows the feature verbs.

**Document-level actions never enter the offer**, because they act on the document and not
on a selection: Import STEP, Import mesh, Text, SVG, Export STEP, Commit to Plate, Undo,
Redo, Variables, Section view, Origin planes, World axes. They live in the toolbar.

---

## Keyboard

Single letters drive sketch tools **while a sketch is open**; Shift+letter drives feature
tools and single letters drive view toggles **when no sketch is open**. The two maps are
selected by the mode, not by whether a sketch session is running.

### Sketch (while a sketch is open)

| Key | Tool |
|---|---|
| `L` | Line — click start, then end |
| `R` | Rectangle — click two opposite corners |
| `C` | Circle — click centre, then radius |
| `A` | Arc — click start, end, then a point |
| `S` | Slot — two centreline ends, then width |
| `E` | Ellipse — centre, major end, minor point |
| `B` | Spline — click control points |
| `P` | Point — click to place |
| `G` | Polygon — click centre, then a vertex |
| `D` | Dimension — click 2 points or an entity |
| `T` | Trim — click a segment to trim it |
| `X` | Extend — click a line/arc to extend it |
| `O` | Offset — pick an entity, drag the distance |
| `M` | Mirror — pick axis, then entities |
| `F` | Fillet — pick two lines, set the radius |
| `H` | Chamfer — pick two lines, set the distance |
| `K` | Constrain — finish the live sketch and enter constrain |
| `Q` | Construction toggle — draw the next entity as construction geometry |
| `Del` | Delete the selected sketch entity |
| `Esc` | Cancel the live tool |

### Feature (when no sketch is open)

| Key | Tool |
|---|---|
| `Shift+S` | Sketch |
| `Shift+E` | Extrude — extrude a profile, or push/pull a picked face |
| `Shift+R` | Revolve |
| `Shift+W` | Sweep |
| `Shift+L` | Loft |
| `Shift+N` | Pattern |
| `Shift+G` | Surface Extrude |
| `Shift+J` | Surface Revolve |
| `Shift+O` | Surface Loft |
| `Shift+Q` | Surface Fill |
| `Shift+U` | Surface Offset |
| `Shift+V` | Thicken Surface |
| `Shift+P` | Plane |
| `Shift+A` | Axis |
| `Shift+C` | Coord Sys |
| `Shift+Y` | Transform |
| `Shift+Z` | Mirror |
| `Shift+B` | Boolean |
| `Shift+X` | Cut |
| `Shift+F` | Fillet / Chamfer |
| `Shift+D` | Draft |
| `Shift+K` | Shell |
| `Shift+H` | Hole |
| `Shift+T` | Thread |
| `Shift+I` | Import STEP |
| `Shift+M` | Import mesh |

### View toggles (single letters, when no sketch is open)

| Key | Action |
|---|---|
| `Home` | Axonometric view, fitted to the model |
| `P` | Origin planes on/off |
| `A` | World axes on/off |
| `X` | Section view on/off |

While the section is on: `PageUp` / `PageDown` move the cut plane, `F` flips which half is
kept. With no section on, `F` is Place on Face — lay the picked face flat on the bed.

---

## Sketching

A sketch is a closed (or open) 2D profile on a plane or on a flat face of an existing body.
Press `Shift+S`, click the face or plane you want to sketch on, and draw. The toolbar and
the offer both carry the sketch tools.

**Entities:** line, polyline, rectangle (corner / centre / oblique / rounded), circle
(centre-radius / 2-point / 3-point), arc (centre-point / 3-point / tangent), ellipse and
elliptical arc, polygon (inscribed / circumscribed), slot (straight / arc), spline, point,
and text.

**Editing:** move, rotate, scale, trim, extend, offset, mirror, and linear or polar arrays.

**Constraints:** coincident, horizontal, vertical, parallel, perpendicular, tangent, equal,
concentric, midpoint, symmetric, fix, plus dimensional radius, diameter, distance and angle.
The solver reports the remaining degrees of freedom and tells you when a sketch is fully
constrained — or when a constraint conflicts with one already there.

Sketches stay editable. Selecting one in the feature tree reopens it with its dimensions
live.

---

## Building solids

Grouped in the toolbar by what they do, one concept per drawer.

### Add material
| Tool | Shortcut | What it does |
|---|---|---|
| Extrude | `Shift+E` | Extrude a profile, or push/pull a face already on a body |
| Revolve | `Shift+R` | Revolve a profile about an axis |
| Sweep | `Shift+W` | Sweep a profile along a path — including a helix, for springs and augers |
| Loft | `Shift+L` | Skin between two or more profiles |
| Thicken | — | Offset a solid face into a thin plate as a new body |
| Rib | — | Grow a stiffening wall from an open sketch line, fused to a body |

Extrude offers blind, symmetric, two-sided, through-all and up-to-face end conditions, plus
a draft angle on the side wall, and can add, subtract, intersect or start a new body.

### Surface
Sheet bodies — surfaces with no thickness — for shapes that are easier to build as skins and
solidify afterwards.

| Tool | Shortcut |
|---|---|
| Surface Extrude | `Shift+G` |
| Surface Revolve | `Shift+J` |
| Surface Loft | `Shift+O` |
| Surface Fill | `Shift+Q` |
| Surface Offset | `Shift+U` |
| Thicken Surface | `Shift+V` |

Thicken Surface is how a sheet becomes a printable solid.

### Dress-up
| Tool | Shortcut |
|---|---|
| Fillet / Chamfer | `Shift+F` |
| Draft (taper a face) | `Shift+D` |
| Shell | `Shift+K` |
| Delete Face | — |

Delete Face removes faces and heals the solid — useful for stripping a feature off an
imported part.

### Holes
**Hole** (`Shift+H`) drills simple, counterbored or countersunk holes, with an ISO/ANSI
standards table so you can ask for an M6 clearance hole instead of computing a diameter.
**Thread** (`Shift+T`) cuts a real helical thread into a bore or onto a shaft.

### Placement
Operations that move a body without changing its shape: **Transform** (`Shift+Y`),
**Mirror** (`Shift+Z`), and **Mate** for assemblies.

### Combining
**Boolean** (`Shift+B`) unions, subtracts or intersects two bodies. **Cut** (`Shift+X`)
splits a body with a plane. **Pattern** (`Shift+N`) repeats a body linearly, in a circle, or
along a curve.

---

## Reference geometry

Datum features carry no material; they exist to give later features something to attach to.

- **Plane** (`Shift+P`) — offset, tilted, midplane, tangent, through two edges, or coincident
- **Axis** (`Shift+A`) — two points, a face normal, a cylinder centreline, the intersection of
  two planes, or along an edge
- **Coord Sys** (`Shift+C`) — a full frame, from a world point or from a face plus a
  direction edge
- **Helix** — a helical curve to sweep along
- **Project** — project a body's edges onto a plane as sketch geometry

On the Coord Sys tool, picking a direction **edge** is worth the extra click: without one the
frame takes its X from the face's first edge, which is deterministic but not necessarily the
direction you meant.

---

## Assemblies

**Mate** aligns two coordinate systems and moves one body onto the other. Five kinds:

| Kind | Leaves free |
|---|---|
| Fastened | nothing — 6 DOF locked |
| Planar | sliding in the plane |
| Revolute | rotation about the axis |
| Slider | sliding along the axis |
| Cylindrical | rotation *and* sliding |

**Check interference** reports every overlapping pair of solids with the overlapping volume,
so a clash is a number rather than an impression. Bodies that merely touch enclose no volume
and are not reported.

---

## Variables and expressions

Define named variables and drive dimensions from them. Any numeric field accepts an
expression — `width/2`, `wall*3` — and everything re-evaluates on recompute. Change one
variable and the whole model follows.

---

## Import and export

**Import STEP** brings in a real B-rep solid, not a mesh: its faces and edges can be filleted,
shelled and cut like anything modelled here.

**Import mesh** (STL/OBJ) converts triangles to a B-rep body and tells you honestly what it
got — whether the result is a closed solid or an open shell, with the boundary and
non-manifold edge counts. A large mesh becomes a large number of faces, which is slow to
edit; the importer warns before you commit to it.

**Export STEP** writes the model out for another CAD tool.

**Commit to Plate** sends the solid to Prepare for slicing. The whole feature recipe is saved
inside the 3MF, so reopening the project restores the editable model rather than a frozen
mesh.

---

## View controls

**Section view** (`X`) hides half the model so you can see inside — `PageUp`/`PageDown` move
the plane, `F` flips which half is kept. **Place on Face** (`F`, when section is off) lays a
picked face flat on the bed. Origin planes (`P`) and world axes (`A`) can be toggled on while
you orient yourself.

---

## Known limitations

Being straight about the edges, so nobody discovers them the hard way:

- **Rib** needs a sketch containing an explicit open line. A parametric rectangle sketch
  carries no individual entities, so Rib cannot use one.
- **Surface Loft** and **Surface Fill** have kernel tests but have not been exercised by hand.
- Card wiring for 9 of the 16 late-wired tools has never been click-tested.
- Mate resolves by composing transforms directly. There is no 3D assembly solver, so mates
  are applied in order rather than solved simultaneously, and mate limits are not implemented.
- Move-face and replace-face are not implemented — OCCT offers no clean primitive for them.
- There is no automated GUI test in CI. Every behaviour above is traced to code and to a
  hand pass, not to a synthetic click.

---

## Where the code lives

| Path | Role |
|---|---|
| `src/libslic3r/CadDocument.*` | the feature recipe and its replay |
| `src/libslic3r/GeometryEngine.*` | OCCT wrapper — faces, edges, booleans, healing |
| `src/libslic3r/SketchEngine.*` | profile → wire → solid |
| `src/libslic3r/SketchSolver.*` | constraint solving, over the vendored solver |
| `src/libslic3r/slvs/` | vendored 2D constraint solver (GPLv3) |
| `src/slic3r/GUI/DesignPanel.*` | the tab: toolbar, cards, feature tree |
| `src/slic3r/GUI/DesignCanvas.*` | viewport integration |
| `src/slic3r/GUI/DesignSketchTool.*` | in-canvas sketching |

Build with `-DSLIC3R_CAD=ON` (the default). With it OFF the tab is not compiled and the deps
prefix matches upstream exactly — see [cad_dependency_weight.md](cad_dependency_weight.md).
