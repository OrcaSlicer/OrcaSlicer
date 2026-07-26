# The Design tab

A parametric CAD modeller inside the slicer. Draw a sketch, turn it into a solid, refine it,
and send it straight to Prepare — without leaving for another application and coming back
through an STL.

The model is a **recipe**, not a mesh. Every action becomes a feature in a tree that is
replayed from the start whenever anything changes, so editing a dimension you set twenty
steps ago rebuilds everything downstream. The geometry kernel is OCCT, which the slicer
already ships for STEP import.

---

## Getting started

1. Open the **Design** tab.
2. Pick a plane — XY, XZ or YZ — and press **Sketch**.
3. Draw a closed profile, then press **✓** to finish the sketch.
4. Select the sketch and press **Extrude** (`Shift+E`).
5. Press **Commit to Plate** to hand the solid to Prepare.

The status line under the toolbar is the thing to watch: it says what the current tool is
waiting for, and it is where a refusal explains itself.

---

## Sketching

A sketch is a closed (or open) 2D profile on a plane or on a flat face of an existing body.

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

**Section view** hides half the model so you can see inside — `PageUp`/`PageDown` move the
plane, Flip shows the other half, `Delete` removes it. **Place on Face** (`F`) lays a picked
face flat on the bed. The bed and its grid can be toggled off when they get in the way, and
origin planes and world axes can be shown while you orient yourself.

---

## Known limitations

Being straight about the edges, so nobody discovers them the hard way:

- **Rib** needs a sketch containing an explicit open line. A parametric rectangle sketch
  carries no individual entities, so Rib cannot use one.
- **Surface Loft** and **Surface Fill** have kernel tests but have not been exercised by hand.
- Two kernel tests are known-broken and excluded from the suite: a tangent-constraint case
  that aborts inside the vendored solver, and an internal-thread groove volume below its
  asserted threshold. Both are tracked, neither is fixed.
- Mate resolves by composing transforms directly. There is no 3D assembly solver, so mates
  are applied in order rather than solved simultaneously, and mate limits are not implemented.
- Move-face and replace-face are not implemented — OCCT offers no clean primitive for them.

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
