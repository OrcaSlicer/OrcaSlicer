# Design tab — High Level Design

## Purpose and scope

The Design tab is a parametric CAD environment inside the slicer: sketch, constrain, build
solid features, commit the result to the plate. It exists because the alternative is a round
trip through an external CAD application, and that round trip discards design intent at both
ends — a part edited after slicing comes back as a mesh rather than as the feature history
that produced it. Keeping the model in the project means a dimension can be changed after the
part has been sliced, with the nozzle diameter, the build volume and the material already
known.

Its coupling to the rest of the application is deliberately narrow. It adds no stage to the
slicing pipeline and touches neither the preset system nor `Tab`. It reaches the rest of Orca
in two places: **Commit to Plate**, which hands finished solids to Prepare as ordinary model
objects (by default one assembly object with a part per body, which keeps the bodies' relative
placement, or one object per body), and one optional 3MF archive entry that carries the recipe.
Everything else is contained in `src/libslic3r/CAD/` and `src/slic3r/GUI/CAD/`.

The user-facing manual lives in the wiki
([Design Tab](https://www.orcaslicer.com/wiki/design_tab)), not here. This document covers the
parts of the design that the code does not make evident.

## The model is a recipe

`CadDocument` holds an ordered list of `CadFeature` and nothing else that matters. Bodies,
meshes and display geometry are **derived**: `recompute()` replays the feature list from the
start and rebuilds them. Editing a dimension set twenty features ago is therefore an ordinary
edit — everything downstream is rebuilt by the same replay that built it the first time.

Two consequences follow from deriving rather than storing:

- **Undo is a snapshot of `features` alone.** The caller calls `checkpoint()` before the
  mutations that make up one user action; undo restores that snapshot and recomputes. Because
  everything else is derived, one checkpoint is exactly one `Ctrl+Z` step and the restored
  state is exact rather than approximately reconstructed. The tab keeps this stack itself; it
  is not Orca's project snapshot system, which operates on `Model` objects the Design tab does
  not own until Commit.
- **Face and edge ids are session-scoped.** They are indices into `TopExp::MapShapes`, so a
  rebuild invalidates every one of them. `CadDocument::topo_generation` is bumped on every
  rebuild so a holder of an id can discover that it is stale instead of silently addressing a
  different edge. The counter is deliberately not serialized: an id means nothing outside the
  run that produced it.

## Geometry kernel and dependency surface

The kernel is OCCT, which OrcaSlicer **already** links — `Format/STEP.cpp`, `Format/svg.cpp`
and `Shape/TextShape.cpp` use it upstream. The Design tab adds no third-party dependency; it
widens the existing OCCT build by one module flag in `deps/OCCT/OCCT.cmake`:

```cmake
-DBUILD_MODULE_ModelingAlgorithms=${SLIC3R_CAD}
```

Most of that module's toolkits are built either way, because `DataExchange`, which the STEP
importer uses, depends on them. With the flag on, OCCT also builds `TKFillet` (used through
`BRepFilletAPI`), `TKOffset` (`BRepOffsetAPI`), and `TKFeat`, `TKHelix`, `TKXMesh` and
`TKExpress`, which nothing here references but which the module flag builds anyway, because
OCCT's module flags are all-or-nothing. On macOS and Linux OCCT links statically, so an
unreferenced toolkit costs build time and no shipped bytes. On Windows OCCT builds shared and
only the linked toolkits ship, so the tab adds the `TKFillet`, `TKOffset` and `TKBool` DLLs.

On Windows the packaging step asserts that every linked OCCT toolkit has a shipped DLL and
fails the configure with the name of any that is missing, because the alternative failure — a
deps prefix built with a different `SLIC3R_CAD` setting than the app — otherwise surfaces as a
missing DLL at first launch.

## The sketch constraint solver

`src/libslic3r/slvs/` is a vendored subset of SolveSpace's `libslvs`: self-contained, no
external dependencies, **GPL-3.0**, with its `LICENSE` preserved verbatim in the directory.
`SketchSolver.cpp` is its only consumer and drives every sketch constraint in the tab.

OrcaSlicer is AGPL-3.0. GPLv3 §13 permits combining a GPLv3 work with an AGPLv3 work and
AGPLv3 §13 grants the converse, so the combined work is distributable under AGPL-3.0 with the
solver's GPLv3 terms preserved. The solver is vendored rather than fetched as a dependency
because it is a pinned subset with no build system of its own; the cost of that choice is
upstream-sync burden, paid deliberately to keep `deps/` unchanged.

## The SLIC3R_CAD gate

`SLIC3R_CAD` (default ON) compiles the tab and selects the OCCT module flag above. With it OFF
the tab is not built and the deps prefix matches upstream exactly. The gate is cheap because
the hooks the Design tab adds to shared GUI code — chiefly the `m_design_sketch_tool` member
and the render, mouse and key hooks in `GLCanvas3D` — are null-guarded on the path they extend,
so removing the tab removes behaviour rather than requiring the host code to be rewritten.

The flag has to agree between the dependencies and the application; that is what the DLL
assertion above is checking.

## Project persistence

A project stores the recipe as one optional archive entry, `Metadata/orca_cad.bin`, backed by
a single `std::string cad_recipe` on `Model`. The entry is written only when the string is
non-empty, and readers that do not know it ignore it, so projects that contain no CAD model are
byte-identical to what upstream would have written and older readers are unaffected.

The blob is a cereal binary archive whose layout is the field order of `CadFeature`'s
save/load. That makes the format the one irreversible decision in the subsystem, and the rules
that keep it survivable are:

- **Append only, never reorder.** Enums serialize positionally as their underlying integer, so
  inserting a value in the middle of `SketchConstraintType` or `CadFeatureType` reinterprets
  every constraint in every saved project. New fields go at the end.
- **Features are length-framed.** Since v5 each feature is a length-prefixed, self-contained
  cereal stream, so a reader can skip a feature written by a newer build and stop cleanly on an
  older one. This is what makes appending a field a non-breaking change from here on. v4 and
  earlier still open through the pre-framing flat path; v1 is deliberately not loadable and has
  no migration path.
- **A newer stamp is refused, not guessed at.** `deserialize_recipe` rejects a blob whose
  version exceeds `ORCA_CAD_RECIPE_VERSION` with a message naming both versions.
- **The rules are held by fixtures, not by discipline.** `tests/data/cad_recipe_v{3,4,5}.bin`
  are checked-in blobs from the builds that wrote them, and the tests that load them fail if a
  field is reordered — which the in-memory round-trip test cannot detect. A regeneration test
  (`[.regen]`, not run by default) produces a fresh fixture when a new version is stamped.

`Import` features embed the imported solid as an OCCT BRep string inside the recipe rather than
referencing the source file, so a project opens without the STEP or mesh it was built from.
The cost is that saved projects are coupled to an OCCT BRep revision.
`tests/data/cad_brep_occt76.brep` holds a solid written by OCCT 7.6, and its test fails if the
bundled OCCT can no longer read it.
A Text feature follows the same rule: it stores the outlines it was vectorised into alongside
its string, font and height, so the project opens identically on a machine that lacks the font;
the three parameters are only what an edit reopens the dialog with.

## The interaction contract

Three inputs carry the whole modelling loop — left click, right click and `Esc` — and the
contract between them is stated in code rather than spread across handlers.

`DesignInteraction.hpp` defines a four-level LIFO stack whose enum value *is* the depth, so
"which level does this press belong to" is a comparison:

| Level | Holds | One `Esc` press |
| --- | --- | --- |
| `Transient` | a value field or a popup menu | closes it; the tool stays armed |
| `Gesture` | an uncommitted delta — an entity being drawn, a body being dragged | reverts it; committed work is untouched |
| `Tool` | a feature card, an armed sketch tool, a constrain session | exits it; drawn entities survive |
| `Idle` | nothing transient | clears the selection, a Feature tree or Bodies row included; leaves a sketch session only if it is empty |

`cad_escape_level()` is a `constexpr` free function over a POD of four booleans rather than a
method on the panel, so the ordering that is the entire contract is checkable without a window,
a GL context or an event loop — five `static_assert`s in the header do exactly that at compile
time.

**The strict invariant: no level of `Esc` deletes a feature, discards a sketch that holds
geometry, or rolls history back.** Destroying work needs a gesture that says so — `Del` on an
explicit selection, the sketch ribbon's Cancel, which asks first, or `Ctrl+Z`. A sketch
*session* is deliberately not a `Tool` level; it is the environment the `Idle` level lives in,
which makes the destructive path unrepresentable rather than merely unlikely.

Right-click is read at button-up against one budget, 3 px of drift, applied to the whole press
rather than to its end points: a press that wandered past the budget at any moment is
navigation, even if it comes back to where it started, which is what stops a slow, careful
orbit from ending in a menu. There is no time budget — a gesture that means something different
when it is slow is exactly what the interaction charter rules out. The raycast uses the press
position, not the release. An armed sketch tool that already consumed the right
button (to terminate a chain, say) declines to also open a menu, through a read-and-clear flag.
Past either budget the event is navigation, and navigation does not transition the state
machine.

Navigation itself is Prepare's: the camera reads the drag actions set in Preferences > Control
for each button. The left button is shared with picking, so a whole body is swept with a
rectangle on plain left-drag only while no camera action is assigned to it, and with
Shift+left-drag otherwise — Prepare's own rectangle selection.

Entering a sketch changes three things at once so the mode is legible: a banner above the
canvas (a sibling of the canvas, not a child over it — on GTK a child window over a
`wxGLCanvas` is a native window and does not reliably stack over GL), the printer bed muted so
a plate grid is never read as a sketch grid, and `N` to look normal to the plane. Code that
changes any of the three belongs with a change to this section.

## Rendering the bodies

The tab draws its bodies through the same `GLCanvas3D` object path as Prepare, so how they look
is decided in the shared object shader, not in the tab. The slicer's two lights both sit near
the camera, which leaves the sides of a part in nearly one tone; the Design canvas asks for a
studio model instead — a world-space sky/ground hemisphere, a key and a fill light, a
plastic-like highlight and a darker silhouette — through `GLCanvas3D::set_studio_lighting()`
and the phong shader's `lighting_model` uniform. The program is shared by every canvas, so each
use sets the uniform (0 for the slicer's canvases) rather than relying on a default: a canvas
that left it alone would inherit whatever the last canvas chose.

The B-rep edges of every body are drawn over it by the sketch overlay as thin view-facing
ribbons, depth tested and pulled a few pixels toward the eye so they win against the faces that
meet at them and still hide behind faces in front; lines are not used because they do not
rasterise under the software GL context the tab also supports. Seams of closed surfaces and
degenerate edges are left out (`GeometryEngine::display_edges`), and the polylines are sampled
once per shape, keyed by its `TShape`, because a recompute that leaves a body unchanged is the
common case.

## Showing what is selected

A selection is drawn on the faces it names, never as a tint over the body: a translucent
selection colour blended into the body's own colour turns a different hue on every body and
vanishes on one close to it. Selected faces are split out of their body into a volume of their
own, which the canvas draws opaque in the selection colour through the same shader and lighting
as the body (`DesignCanvas::rebuild_bodies`); the sketch overlay outlines them with a cased line
— a dark band under a selection-coloured one — so the outline still reads on a body that wears
the selection colour itself. A body picked whole, a face picked in the viewport and the faces of
the Feature tree's selected feature all draw this way. The hover pre-highlight is the outline
alone, uncased: it promises a click, it is not one. The automatic body colours keep clear of the
selection colour's blues and teals, so no body looks selected before anything is picked; a colour
the user sets on a body is theirs, and the cased outline keeps its selection readable.

Selecting a feature row lights the faces that feature made, not the whole body it sits on, so a
fillet row shows its round and the extrude under it keeps the faces the fillet trimmed.
`CadDocument::faces_made_by` answers it without per-feature history: it replays the recipe to
just before the feature and then the feature alone, and a face of the finished model belongs to
the feature when an interior point of it lies on the boundary afterwards and not before, facing
the same way — the facing keeps a block stacked on a base the owner of its bottom face. A feature
that makes no face of its own, such as a Boolean union, answers with the bodies it changed. The
replay costs up to a recompute, so the panel finds the faces once per row and topology
generation, off the UI thread, and only while no feature card is open. One selection is live at
a time: a viewport pick clears the feature row and a feature row clears the viewport pick, as the
Feature tree and Bodies list do between themselves. `Esc`, a click on empty space and an
empty rubber band all let go of it, whichever list or pick made it.

## Following the app

The tab is a page of Orca's main window and answers to the same settings as Prepare.

- **Theme.** Its chrome is coloured from a table of light/dark token pairs. A theme switch
  reaches `DesignPanel::on_sys_color_changed` from `MainFrame`, which moves every colour that is
  one theme's token onto the other theme's and then runs the app's own dark pass; the icons are
  Orca's sidebar grey, which the icon cache maps per theme, so they are re-rasterised rather
  than re-tinted.
- **Scale.** Sizes are in DIP, and a DPI change reaches `DesignPanel::msw_rescale`, which
  re-rasterises every icon (button faces, flyout rows, card headers, the feature and body lists'
  row icons).
- **Sidebar icons.** Every clickable icon in the sidebar shows a hover chip. The card-header and
  constraint-row buttons are Orca's self-painted `Button`, because a native button cannot take a
  hover background on macOS. The Feature tree and Bodies lists are a custom-drawn
  `DesignRowList` rather than a `wxTreeCtrl`, so each row carries its own actions — Edit,
  Show/hide and Delete on a feature, Move, Show/hide and Delete on a body — and the eye shows
  whether that row is hidden.
- **Viewport text.** The status line and the active tool's values are drawn by the canvas in
  its ImGui pass, so they go with the canvas: a top-level window over GL does not follow its
  frame and was left floating over other applications.
- **Undo.** The tab keeps its own history (the recipe is not part of Prepare's snapshots), but
  it has no Undo/Redo of its own: the top bar, `Ctrl+Z` and Edit drive it while the tab is
  shown, greyed to what an undo would actually do.
- **Docking.** The sidebar docks like Prepare's — either side, floating, resized, or collapsed
  with the canvas's collapse button or `Shift+Tab` — through its own AUI manager under the
  toolbar, because Prepare's manages the Plater and the Plater is not on this page. The button is
  the canvas's own toolbar rather than Prepare's, which collapses Prepare's sidebar. The layout,
  collapse included, is kept apart from Prepare's (`design_window_layout`) and starts where
  Prepare's sidebar is, at its width, so the canvas edge holds still across the tab switch until
  the user moves one of them. A floating sidebar is a top-level window, so it is hidden with the
  tab rather than left over the other pages, and View > Reset Window Layout resets both tabs.

## The offer is generated, not hand-written

Right-clicking geometry opens the *offer*: eight families in a fixed order, each verb at a
permanent row index, verbs that do not apply shown disabled **in place with their reason**
rather than removed. The invariant is that a verb's row index is identical in every selection
where it appears and that adding a verb never moves an existing one — the hand learns the
position, so the menu is never re-sorted, compacted or adaptively ordered. Above the families
sits one *flat* row, holding Rename and Color — what a selection is opened for most: its verbs are
items of their own at the top of the menu rather than a family's submenu. It is appended after
the eight, so it moved no existing index, and it reads the same from the viewport and from a row
of the Bodies list.

An invariant across 92 verbs and 20 selection kinds does not survive by review, so the map
exists once, as data: `scripts/CAD/tool_atlas.json` carries every verb with its row, key, icon,
accepted selections, preconditions and refusal string, and `scripts/CAD/gen_offer_table.py`
emits `src/slic3r/GUI/CAD/DesignOffer.hpp` from it. The header is checked in and never
hand-edited; `scripts/CAD/run-all-checks.sh` runs the generator with `--check` as its first
rung, which is what makes "GENERATED — DO NOT EDIT" a fact rather than a request. The generator
also refuses an atlas with a duplicate verb id, since `mcp_run_verb` resolves a verb by id and
would make the second one unreachable.

The atlas and its generator sit in `scripts/CAD/` rather than in `docs/`: they are build inputs
for a checked-in header, not documentation.

## Automation surface

`McpControl` exposes the document over JSON-RPC when `ORCA_CAD_MCP` is set in the environment,
with `tools/orca_cad_mcp_bridge.py` as the client side. It describes the scene, queries
topology, measures, and runs the same verbs the offer does — it re-implements nothing, so a
scripted action and a clicked one cannot diverge. It is off unless the variable is set.

## Where the code lives

| Path | Role |
| --- | --- |
| `src/libslic3r/CAD/CadDocument.*` | the feature recipe, its replay, undo and serialization |
| `src/libslic3r/CAD/GeometryEngine.*` | OCCT wrapper — faces, edges, booleans, healing |
| `src/libslic3r/CAD/SketchEngine.*` | profile → wire → solid |
| `src/libslic3r/CAD/SketchSolver.*` | constraint solving, over the vendored solver |
| `src/libslic3r/slvs/` | vendored 2D constraint solver (GPLv3) |
| `src/slic3r/GUI/CAD/DesignPanel.*` | the tab: toolbar, feature cards, tree, key maps |
| `src/slic3r/GUI/CAD/DesignRowList.*` | the Feature tree and Bodies lists, with per-row actions |
| `src/slic3r/GUI/CAD/DesignCanvas.*` | viewport integration |
| `src/slic3r/GUI/CAD/DesignSketchTool.*` | in-canvas sketching |
| `src/slic3r/GUI/CAD/DesignInteraction.hpp` | the Esc level contract |
| `src/slic3r/GUI/CAD/DesignOffer.hpp` | generated offer table |
| `scripts/CAD/tool_atlas.json` | source of truth for the offer |

## Verification

The kernel is covered by Catch2 suites in `tests/libslic3r/` (`test_caddocument`,
`test_sketchconstraints`, `test_sketchedit`, `test_sketchimport`, `test_sketchinference`,
`test_sketchprofile`, `test_slvs_constraints`), which need no display;
`scripts/CAD/run-kernel-tests.sh` builds only `libslic3r_tests` and runs them headless.

The GUI half is not covered by CI, which has no OpenGL canvas or synthetic input: the ladders
in `scripts/CAD/` drive a running application in a local rig instead, and
`scripts/CAD/run-all-checks.sh` is the gate that runs all of them. A green kernel run says
nothing about the viewport, so the two are reported separately rather than as one number.
