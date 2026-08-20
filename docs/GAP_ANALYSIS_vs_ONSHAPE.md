# SnapOrca-CAD vs Onshape — capability gap analysis

Generated 2026-07-22 by enumerating the source, not from recollection:
`CadFeatureType` and `add_*` in `src/libslic3r/CAD/CadDocument.hpp`, `Tool` in
`src/slic3r/GUI/CAD/DesignPanel.hpp`, `Mode` in `src/slic3r/GUI/CAD/DesignSketchTool.hpp`,
`SketchConstraintType` + `SketchEntity::Type` in `src/libslic3r/CAD/SketchEngine.hpp`,
and the JSON-RPC dispatch in `src/slic3r/GUI/CAD/McpControl.cpp`.

**Scope note.** Onshape is a cloud PLM platform; SnapOrca is a Design tab inside a
slicer. A large share of Onshape's surface (release management, branching, real-time
collaboration, FEA, rendering, PDM) is out of scope by construction and is listed
separately at the bottom rather than counted as a "missing tool".

---

## 1. What SnapOrca already has

### 2D sketcher — near parity with Onshape
This is the strongest area. Very little is missing.

| Category | SnapOrca |
|---|---|
| Entities | Line, Polyline, Arc (3-point / tangent / center), Circle (center / 2-point / 3-point), Point, Ellipse, Elliptical arc, B-spline |
| Shapes | Rectangle (corner / center / oblique / rounded), Slot, Arc-slot, Polygon |
| Edit ops | Fillet, Chamfer, Offset, Mirror, Trim, Extend |
| Transforms | Move, Rotate, Scale, Linear array, Polar array |
| Constraints (19) | Fix, Coincident, Horizontal, Vertical, Distance, LockX, LockY, EqualLength, Parallel, Perpendicular, Concentric, Tangent, Midpoint, Symmetric, Angle, Radius, Diameter, PointOnLine, PointOnObject |
| Dimensions | Length, Diameter, Radius, Angle, Distance, Distance-to-line |

Solver: vendored SolveSpace (`libslvs`, GPL-3.0) — the same solver lineage as a
commercial-grade sketcher.

### Part features

| Present | Notes |
|---|---|
| Extrude | + up-to-face / up-to-point, taper, flip |
| Revolve | angle-arc gizmo |
| Sweep | along a path |
| Loft | multi-profile |
| Fillet / Chamfer | edge-level |
| Draft | face taper |
| Shell | wall thickness + open face |
| Hole / Thread | face-aware placement |
| Pattern | linear + circular |
| Boolean | New / Add / Cut / Intersect, with face-mating |
| Cut | plane-based, signed offset |
| Datum plane | offset / 2-face / 2-edge derived |
| Import | STEP (B-rep) + mesh→B-rep (native mesh2step port) |
| Export | STEP (native B-rep, not tessellated) |
| Multi-body | + per-body colour |
| Section view | with flip |
| Undo/redo | full feature-tree recompute |
| 3MF persistence | parametric recipe survives save/load |

### Automation
9 MCP JSON-RPC methods: `describe_tools`, `describe_scene`, `query_topology`,
`measure`, `slice_body`, `import_step`, `import_mesh`, `validate_against`, plus
build actions `extrude`, `revolve`, `fillet`, `chamfer`, `hole`, `boolean`, `pattern`.
Onshape's equivalent is its REST API + FeatureScript.

---

## 2. Missing tools — ranked by impact

### Tier 1 — structural absences (whole subsystems)

**1. Assemblies and mates.** Entirely absent. No assembly document, no mate
connectors, no fastened / revolute / slider / cylindrical / planar / ball / pin-slot
mates, no assembly patterns, no interference detection, no exploded views.
`bool_target_face` / `bool_tool_face` do face-to-face *mating* for a boolean, which
is geometric alignment, not a kinematic joint.
*Impact:* multi-part products cannot be positioned or validated as a mechanism.
*Note:* an MCP-side `align_instance_to_face` / `create_*_mate` vocabulary already
exists on the Onshape bridge in this workspace, so the target semantics are known.

**2. Drawings / 2D documentation.** Absent. No drawing sheets, dimensioned views,
section/detail views, GD&T, title blocks, or BOM.
*Impact:* nothing manufacturable-by-a-third-party leaves the tool. For 3D printing
this matters less than for machining, which is the honest reason it is Tier 1 by
CAD convention but arguably Tier 3 for this product.

**3. Variables, equations, configurations.** Absent — no `add_variable`, no
expression evaluation, no configuration table. Every dimension is a literal double.
*Impact:* this is the biggest *parametric* gap. "Make this bracket for an M4 vs M5
bolt" requires re-editing every dependent feature by hand. Onshape's Variable
Studio + configurations are a core differentiator, and this is the cheapest Tier 1
item to close for the size of the payoff.

**4. Surface modelling.** Absent. No surface extrude/revolve/loft/sweep, no fill,
knit, trim/extend surface, offset surface, or thicken. SnapOrca is solid-only.
*Impact:* organic/complex shapes and repair of imported junk geometry are impossible.
OCCT already provides all of it (`TKOffset`, `TKBRep`), so the kernel is not the
blocker — only UI and feature plumbing.

**5. Sheet metal.** Absent. No flange, bend, tab, relief, or flat-pattern unfold.
*Impact:* arguably out of scope for an FDM slicer; listed for completeness.

### Tier 2 — individual features with clear demand

| Missing | Why it matters | Cheap? |
|---|---|---|
| **Mirror body** (part-level) | Sketch mirror exists; mirroring a *solid* about a plane does not. Extremely common. | Yes — OCCT `gp_Trsf` mirror + fuse |
| **Helix / spiral curve** | No helix ⇒ no springs, no custom threads, no spiral vase geometry. Sweep exists but has no helical path to sweep along. | Yes |
| **Move / rotate body as a real feature** | `m_body_xform` exists but is **display-only** (memory #1655) — it never enters the B-rep. Export/boolean see the original position. | Medium |
| **Split body** | Cut removes material; splitting one body into two independently-usable bodies is absent. Very relevant for print-in-parts. | Medium |
| **Thicken** | Solid from a surface/face offset. | Needs surfaces |
| **Rib** | Standard structural feature. | Medium |
| **Delete face / move face / replace face** | Direct/dumb-solid editing — the main tool for fixing imported STEP. Given SnapOrca imports STEP *and* meshes, its absence is felt. | Medium |
| **Datum axis, coordinate system** | Only datum *planes* exist. Axes are needed for revolve/pattern references. | Yes |
| **Mass properties** | `GeometryEngine` computes a volume internally, but there is no volume/mass/COM/inertia readout. For print cost/time estimation this is nearly free to expose. | Yes — trivial |
| **Measure tool in the GUI** | `measure` exists over MCP but there is no interactive measure in the UI. | Yes |
| **Hole standards library** | Hole exists, but no counterbore/countersink/tapped standards (ISO/ANSI) with callouts. | Medium |
| **Project / convert edges into a sketch** | Cannot reference existing solid edges as sketch geometry ("Use" in SolidWorks). A significant sketcher gap given everything else is present. | Medium |
| **Construction geometry** | Could not confirm a construction/reference-line flag on sketch entities. | Yes if absent |
| **Curve tools** | Projected curve, bridging curve, composite curve, 3D fit spline. | Medium |
| **Pattern on curve / pattern faces** | Pattern is linear + circular of whole bodies only; no curve-driven pattern, no feature/face pattern. | Medium |
| **Wrap / emboss** | Text or sketch wrapped onto a curved face. | Hard |
| **Enclose** | Solid from bounded void regions. | Medium |

### Tier 3 — platform capabilities (out of scope by construction)

Version control with branching/merging, release management, real-time multi-user
collaboration, cloud PDM, FeatureScript custom-feature authoring, simulation/FEA,
photorealistic rendering, app store/integrations. These are Onshape-the-platform,
not Onshape-the-modeller. Not defects in SnapOrca.

---

## 3. Recommended priority

If the goal is "credible parametric CAD inside a slicer", the ordering that buys
the most capability per unit of work:

1. **Variables + expressions** — unlocks genuine parametric reuse; no new kernel work.
2. **Mass properties + GUI measure** — nearly free, immediately useful for printing.
3. **Mirror body, datum axis, helix** — small, self-contained, high-frequency features.
4. **Promote move/rotate body from display-only to a real B-rep feature** — closes a
   correctness gap, not just a missing tool (exports currently disagree with the view).
5. **Split body** — high value for print-in-parts workflows.
6. **Project edges into sketch** — the sketcher's most conspicuous hole.
7. **Surface modelling** — large, but OCCT already ships the algorithms.
8. **Assemblies** — largest effort; only worth it if SnapOrca targets multi-part products.

Deliberately last: drawings and sheet metal — high cost, low relevance to an
FDM-oriented tool.

---

## 4. Honest summary

SnapOrca's **sketcher is at or near Onshape parity**, and its **solid feature set
covers the mainstream modelling path** (sketch → extrude/revolve/sweep/loft →
dress-up → boolean/pattern). What is absent is *breadth*: assemblies, surfaces,
sheet metal, drawings, and — most importantly for a tool calling itself parametric —
**variables and configurations**.

The single most defensible criticism is #3: without variables, the feature tree is
parametric in *structure* but not in *value*, so the promise of "change one number
and the model updates" is only half delivered.
