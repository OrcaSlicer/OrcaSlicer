# 2D sketcher tool architecture — the batch series

The refactor target for `DesignSketchTool`'s tool layer: a flat `Mode` enum of 36 ids plus one
event `switch` per tool becomes **ToolManager / InteractiveContext / ModalOperator**.

Why the file exists: batches 1–8 were delivered in-session and only their cross-cutting results
were recorded (the six archetypes and the two defect classes, below). From batch 9 on the design
lives here, so an implementer who was never in the room can build it.

Every claim about the current code carries a `file:line` from `src/slic3r/GUI/CAD/`, verified by
reading the tree. Claims I did not verify are marked **UNVERIFIED**.

---

## 0. Cross-cutting results (batches 1–8)

### 0.1 The three components

| Component | Owns | Replaces |
|---|---|---|
| **ToolManager** | activation + a *suspension stack* (`HUDEdit` > `CameraSuspend` > tool), the ESC ladder, key routing **by content** not by focus holder, the sticky `ArmState` the Construction toggle needs | `m_mode` + `set_tool()` + scattered `is_*_mode()` predicates |
| **InteractiveContext** | one `SolveSnap(ray, plane, targets, SnapPolicy)` (const, allocation-free), `PickTopology(ray, TopAbs_ShapeEnum)`, an RAII overlay registry | three inconsistent snap paths — circles/arcs use raw `screen_to_plane`, arcs snap only their endpoints |
| **ModalOperator** | one FSM + one `Params()` block per tool; Tab order **is** the array order | per-tool `switch` arms and hand-written `m_awaiting_*` chains |

### 0.2 The two defect classes (both belong in the **base** operator)

1. **Silent no-op** — a failed solve returns `{}` and the anchors are cleared anyway. Three
   collinear points create nothing (`DesignSketchTool.cpp:2748` + `:10942`); a minor point on the
   major axis creates nothing (`:2995`); `set_slot` bails on a degenerate direction or a rebuild
   that changed the entity count (`:2081`, `:2089`). ⇒ a `Refused` state that **keeps** the
   anchors, names the reason, and resumes the moment a mouse move makes a solution exist.
2. **Silent clamp** — the kernel replaces the user's number with a bound and the label then shows
   a value nobody typed. Ellipse `a ≥ b` (`:1887`), arc slot `w < Rc` (`:2042`), polygon
   circumradius from a side (`:1548`). ⇒ `WriteOutcome{Ok, ClampedTo, Refused}` returned by every
   parameter write, so the operator can name the bound.

### 0.3 Archetypes (one per batch)

`NSolveOperator` (multi-click, possibly-unsolvable last click) · `DerivedDimOperator` (label value
≠ kernel field) · `CoupledAxisOperator` (parameters not independent) · `UnboundedChainOperator`
(no closed form, explicit terminate) · `ProvisionalPlacementOperator` (modal input → provisional
feature → confirm/cancel) · `DeriveOperator` (a pick sequence over foreign topology).

Batches: **1** Line/Polyline/Point/Construction · **2** Rect×4 · **3** Circle×3 + Arc×3 ·
**4** Slot/ArcSlot/Polygon · **5** Ellipse/EllipseArc · **6** Spline(+edit) · **7** Text/SVG ·
**8** Project/Construction/Intersection · **9** Select/Measure · **10** Dimensioning ·
**11** Constraints · **12** Modification · **13** Dress-up · **14** Transform · then Phase 2 (3D).

Designed so far, below: 9, 10, 11, 12, 13, 14 — the 2D series is complete.

---

## Batch 9 — Select and Measure

Two tools that look unrelated and share one root: **a query with no document mutation is still a
tool, and its state is still an FSM** — but this codebase has no place to put a non-mutating
tool, so both were built as side effects of something else.

### 9.1 The architectural claim: Select is not a tool

`Mode::Select` is a `Mode` value, but nothing about it behaves like one: it has no anchors, no
parameter block, no commit step, and every other tool *returns to it*
(`DesignSketchTool.cpp:354` `set_tool(Mode::Select)`, `:113` `begin(plane, Mode::Select)`).
It is the InteractiveContext's **rest state plus a selection model**, and the selection model is
what is missing. Today three unrelated pick paths write three unrelated stores:

| Path | Writes | Scope |
|---|---|---|
| `handle_solid_click` (`:3470`) | `m_solid_sel, m_sel_body, m_sel_face, m_sel_edge, m_sel_vertex_pt` — **one triple** | solid topology |
| Select-mode sketch branch (`:10355`+) | `m_selection` (`std::vector<int>`), `m_point_sel` (`vector<pair<int,role>>`) | live sketch |
| `pick_bodies_in_rectangle` (`:3298`) | `select_body()` → the same single body slot | whole bodies |

`DesignPanel::offer_selection_kind()` (`:6084`) then *reconstructs* a type from those stores, and
it can never return `OfferSel::Bodies2`: the enum member exists (`DesignOffer.hpp:27`) and
**nothing in the tree produces it** (`grep -rn Bodies2 src/` → the enum line only). So Union,
Subtract, Intersect and Interference — the four verbs whose `accepts` mask is exactly
`offer_bit(Bodies2)` — are permanently unreachable from a viewport selection, and
`pick_bodies_in_rectangle`'s own comment says why: *"the body with the most samples inside wins
because the selection callback downstream carries exactly one body… Real multi-body selection (and
the homogeneous-set rule that goes with it) is 9xw"* (`:3295-3297`).

**Verdict.** Select is the InteractiveContext's selection model, and batch 9 builds it. The
`Mode::Select` enum member survives only as the name of the rest state.

### 9.2 Verified current behaviour (what an implementer must not break)

Solid picks, in precedence order, measured in **screen pixels** (`:3387-3392`):
vertex 11 px beats edge 8 px beats face; on a narrow face both tolerances shrink to a third of the
face's shorter on-screen side, so a 3 mm plate's middle stays reachable (`:3443-3450`).
`resolve_solid_pick` is **const** and writes only into its out-param — that is what makes hover
safe. Clicking the same sub-element twice (**at the level that was picked**, `:3507-3518`)
escalates to the whole body; the comment at `:3491-3497` argues the escalation is announced by the
status line before the click (I have not traced that announcement to its status text).
Whole bodies also come from the Parts list and from the rubber band.

The escalation is gated by a boolean the **panel** sets — `set_escalate_on_repick`
(`DesignSketchTool.hpp:108`, `:1192`), flipped in nine places (`DesignPanel.cpp:3878`, `:3889`,
`:3912`, `:6718`, `:6725-6730`, `:6764`, `:6771`, `:6826`, `:6839`), with the clearest comment
being *"While a pick is armed, a click must CAPTURE the face under the cursor, never escalate"*
(`:6725-6730`). That is a fourth store of *what a click means*, readable only by whoever set it
last — the disease this batch exists to cure, in one flag.

Rubber band (`:3286-3335`, `:10011-10046`): a left-drag past an 8 px budget starts
`GLSelectionRectangle` in state `Select`; on release every sample point **inside** the rectangle
counts (crossing semantics, comment at `:3296`), and the body with the most samples wins.

Sketch picks (`:10355`+): a point hit grabs the point and arms a drag; an entity hit selects with
Shift/Ctrl to toggle; a double-click adds `connected_loop(hit)`; a plain click on a constraint
badge **deletes that constraint** (`:10370-10378`); a click on a dimension label opens its editor
or promotes a live quote (`:10460-10475`); a click inside a closed region calls
`on_face_selected(reg)` (`:10608-10614`); empty space clears the selection.

Hover: `update_solid_hover` (`:3530`) pre-highlights what a click *would* take — verified on the
rig 2026-08-14 (snaporca-9xw notes). For **sketch** geometry there is no equivalent:
`update_hover` (`:870`) tracks handles only — `hit_test_handle` — and the sole hover member is
`Handle m_hover_handle` (`DesignSketchTool.hpp:1017`). No `m_hover_entity` exists.

Keyboard: `DesignSketchTool` has **no key handler at all**. Esc, Del/Backspace, undo and the offer
key arrive at `DesignPanel`'s char hook (`:4231`, `:4260`, `:4243`) and route through
`DesignPanel::escape()` (`:11401`) — the four-level ladder in
`docs/CAD/ux/interaction-model.md`.

Camera: both the pick (`:3386`) and the rubber band (`GLSelectionRectangle.cpp:41`) resolve the
camera as `wxGetApp().plater()->get_camera()`. In the Design tab this is the *right* camera only
because `DesignCanvas` swaps its own camera with the plater's on enter/leave
(`DesignCanvas.hpp:73-77`, `:361`, `:376`) — a dependency, not a coincidence to build on.

Cost: `resolve_solid_pick` calls `GeometryEngine::edges_of_face` and `sample_edge_world` (each
returning a fresh `std::vector<Vec3d>`) and runs per motion event through
`update_solid_hover`. It is bounded to one face, not the whole body, but it allocates on the
pointer path.

### 9.3 The selection model — blueprint

New header, `src/slic3r/GUI/CAD/DesignSelection.hpp`. Everything below is a value type; nothing
owns a GL resource, so the whole model is testable without a canvas.

```cpp
enum class RefKind : uint8_t {
    None, Face, Edge, Vertex, Body, DatumPlane, DatumAxis, CoordSys, Art, SkEntity, SkPoint, SkRegion
};

// The FINE class — one level below RefKind, because the offer vocabulary splits what RefKind
// joins: a Face is planar, cylindrical or other; an Edge is straight or circular. This is
// OfferSel (DesignOffer.hpp:19), and classify() is the logic that today lives split between
// DesignPanel::offer_selection_kind() (:6084) and GeometryEngine::circle_of_edge/_cylinder_of_face.
RefClass classify(const Ref&, const CadDocument&);
using RefClass = OfferSel;   // no second vocabulary: the offer table's own, from DesignOffer.hpp:19

// A reference to one thing, plus the generation it was picked at. Ids expire on every
// recompute (CadDocument::topo_generation); a stale ref must REFUSE, never guess.
struct Ref {
    RefKind  kind{RefKind::None};
    int32_t  body{-1};        // body index, or the sketch feature index for sketch refs
    int32_t  item{-1};        // face id / edge id / entity index / region id
    int32_t  role{-1};        // SketchPointRole for SkPoint picks, else -1
    uint32_t generation{0};
};

struct Pick {
    Ref    ref;
    Vec3d  point;    // the geometry the pick carried (vertex, edge midpoint, face centroid)
    Vec3d  dir;      // edge direction / face normal; zero when the ref has none
    bool   has_dir{false};
};

enum class SetOp : uint8_t { Replace, Add, Toggle, Remove };

// What a tool will accept as its picks. A set is NOT globally homogeneous: 73 of the 86 offer
// verbs accept more than one class (measure accepts twelve, transform nine, delete nine) because
// the mask answers "what can start this verb", not "what one set may contain". So homogeneity is
// a per-tool POLICY, and Measure's is a pair of possibly-different classes, not a set.
struct PickPolicy {
    uint32_t accepts{0};         // the same OfferSel bitmask the offer row already carries
    uint8_t  max_picks{1};       // 1 = single, 2 = a pair (Measure, Mate), 0 = unbounded (Fillet)
    bool     homogeneous{true};  // every pick shares the class (Fillet) or not (Measure, Delete)
};

class SelectionSet {
public:
    RefKind  kind() const;       // RefKind::None while empty
    RefClass class_of() const;   // the class of front(); a mixed set reports its primary's
    bool     empty() const { return m_refs.empty(); }
    size_t   size()  const { return m_refs.size(); }
    bool     contains(const Ref& r) const;
    // Ordered: front() is the PRIMARY pick (the anchor), back() the most recent. Two-ref
    // queries (Measure, and later Mate) read front()/back() rather than sorting by distance.
    const Pick& front() const;
    const Pick& back()  const;
    // Returns what happened, so the caller can say it: refused a stale ref, refused by the
    // policy, replaced, added, removed. Silence here is the same disease as a silent clamp.
    enum class Outcome : uint8_t { Added, Removed, Replaced, RefusedStale, RefusedPolicy };
    Outcome apply(const Pick& p, SetOp op, const PickPolicy& policy, uint32_t live_generation);
    void clear();
private:
    RefKind            m_kind{RefKind::None};
    std::vector<Pick>  m_refs;   // bounded by policy.max_picks (0 = the sketch-select case)
};
```

Why this shape and not a `std::variant` per kind: the whole tab reads selection through one
question — *which offer rows are live* — and that question is answered by one bitmask over one
vocabulary. A variant would move the vocabulary into the type system and then re-derive the
bitmask at every call site.

`OfferSel` is also the *only* place that carries cardinality (`Sk2Ent`, `Bodies2`) — a count
folded into a kind vocabulary, which is why `offer_selection_kind()` (`DesignPanel.cpp:6084`)
must collapse `n >= 2` into `Sk2Ent` and loses what the two things are. With `size()` on the set
and the class on the refs, those two enum members retire.

The two existing stores become projections of this one: `m_selection` (entity indices) is
`items(kind() == SkEntity)`, `m_point_sel` is `items(kind() == SkPoint)`, and the solid triple is
`front()` + `size() == 1`. `offer_selection_kind()` turns into one non-reconstructing switch:

```cpp
// One place, exhaustive, no inference from three stores.
int offer_kind_from(const SelectionSet& s);   // Bodies2 when kind()==Body && size() >= 2
```

### 9.4 Select — FSM

Select is the rest state, so its FSM has no `Armed` and no `Commit`; it is a **hover + press
machine**. Budget: `kClickPx = 8` (`:10029`).

| State | Meaning | Event | Guard | Action | Next |
|---|---|---|---|---|---|
| `Idle` | nothing selected | `Move` | over topology | solve pick, store promise, repaint | `Hover` |
| `Hover` | a promise is on screen | `Move` | candidate changed | update promise | `Hover` |
| | | `Move` | left the geometry | clear the promise, repaint | `Idle` |
| | | `LeftDown` | always | latch (`x,y`, remember the promise), **fall through so the canvas may orbit** | `PressPending` |
| `PressPending` | press latched, click vs band undecided | `Drag` | `max(|dx|,|dy|) > 8` | start the band at the ORIGINAL press point | `Band` |
| | | `LeftUp` | — | commit the pick (below) | `Idle` |
| `Band` | rubber band sweeping | `Drag` | — | move the rectangle | `Band` |
| | | any button-up event | — | resolve bodies, stop the band | `Idle` |
| `Grab` | a point/handle was grabbed | `Drag` | left down | move + live re-solve | `Grab` |
| | | `LeftUp` | — | release the grab | `Idle` |
| `Region` | clicked inside a closed loop | — | — | hand to `on_face_selected`, which owns the commit | `Idle` |

Commit rule (the whole of "what did that click mean"):

```
pick = hit_test_point(tol)      -> SkPoint, arm Grab
    || handle (derived grips)   -> SkEntity + Grab
    || hit_test(topology)       -> Vertex | Edge | Face   (solid)
    || hit_test(entity)         -> SkEntity
    || constraint badge         -> delete the constraint (plain click only)
    || dimension label          -> open/promote the value editor
    || region                   -> Region
    || nothing                  -> clear
```

Escalation stays exactly as it is: pressing the *same ref* twice at the level that was picked
escalates to its body — one comparison on `Ref.kind/body/item`, no field-tuple equality.

ESC ladder: `Idle` = `clear()` (the floor — never exits the session, `DesignInteraction.hpp`
invariant). `Hover`/`Band` are not ladder levels: a band is cancelled by releasing the button,
and a band is not a "gesture" in the `CadLevel::Gesture` sense because nothing has changed yet.

### 9.5 Select — event routing and focus handshake

| Event | Canvas | Tool | Panel / char hook | Camera | Notes |
|---|---|---|---|---|---|
| `Move` | — | `update_hover` / `update_solid_hover` (**non-consuming**, returns false) | — | passthrough | hover only asks for a repaint, `:10022-10025` |
| `LeftDown` | orbit may begin | latch press, **return false** | — | consume-or-orbit is the canvas's call | `:10058-10063`; consuming here killed orbit once already |
| `LeftDrag` > 8 px | — | start + drive band, **consume**, when Shift is held or the left button has no camera action | — | otherwise the left button's drag action (Preferences > Control) | the band is Prepare's Shift+left-drag rectangle selection |
| `LeftUp` | — | commit pick **or** resolve band | — | — | `:10035-10041` |
| `MiddleDrag` / `RightDrag` | — | must not see it | — | the button's drag action (Preferences > Control) | camera gestures never reach the FSM; a right press reaches the tool only once its release shows it was a click |
| `RightClick` | — | — | offer menu | — | `snaporca-xmh6` open: a right-click that only clears the sketch selection eats the offer |
| `Esc` | — | — | `escape()` ladder | — | one route whatever holds focus (`:4228-4231`) |
| `Del` / `Backspace` | — | `delete_selected_or_last_sketch_entity()` | char hook | — | `:4260` |
| `Menu` / `Shift+F10` | — | — | `show_offer_menu(offer_anchor())` | — | `:4243` |
| `Wheel` / SpaceMouse | — | must not see it | — | zoom / fly | never through `wxMouseEvent` |

The invariant the matrix encodes: **hover rules, camera rules, and the pick rules — and each of
the three consumes only what it owns.** A band that starts on a camera gesture is the classic
failure; so is a tool that eats the press and silently kills orbit.

### 9.6 Select — the five defects to fix, and how the architecture prevents them

1. **No sketch hover pre-highlight.** Add `std::optional<Pick> m_hover` to the context, filled by
   the same const `hit_test` the click uses, drawn by the overlay registry. Solids already prove
   the pattern; the asymmetry is what makes precedence unlearnable in a sketch.
2. **Rubber band has no direction.** `GLSelectionRectangle::EState` is `{Off, Select, Deselect}`
   (`GLSelectionRectangle.hpp:15-19`) — no topology. Add `bool is_crossing() const` returning
   `m_start_corner.x() > m_end_corner.x()` (one read of existing members) and use the **same
   sample set with two predicates**: LTR = every sample inside (enclosed), RTL = any sample inside
   (crossing). No new sampling, no change to the plater's own use of the class.
3. **One body, because the callback carries one.** Make the callback carry a `SelectionSet` and
   delete the "most samples wins" rule. `Bodies2` then becomes producible, which unblocks Union /
   Subtract / Intersect / Interference in the offer — four verbs that today cannot be reached.
4. **The camera is looked up, not passed.** `solve_pick(const Camera&, …)` and a band that takes
   the camera as an argument. Today both work only because `DesignCanvas` swaps the plater's
   camera on enter; the day a second canvas is on screen the promise and the click disagree.
5. **The pick allocates on the pointer path.** `sample_edge_world` returns a fresh vector per
   edge per motion event. Give the context a reusable scratch buffer (`std::span` out-param) —
   the same allocation rule the tool blueprints already carry.

### 9.7 Measure — the archetype: `TwoRefQueryOperator`

A tool that mutates nothing, has no commit, and cannot fail — and therefore has no home in this
codebase. Current state, verified:

- Offer row `{"measure", "Measure", 6, …}` (`DesignOffer.hpp:120`) has `action == nullptr` and
  `refusal == nullptr`. The table's own contract (`DesignOffer.hpp:43-48`) is *"nullptr → kernel
  support exists, no GUI path yet (row shows disabled)"*, so the row renders as
  **"Measure (no GUI route)"** and is permanently disabled (`DesignPanel.cpp:6519`). With no
  refusal in the row, it contributes nothing to the family's "why" line either.
- The maths exists, for MCP callers only: `resolve_ref` + `measure` (`McpControl.cpp:522`, `:546`)
  — refs `{face|edge|point}`, distance = `|pa - pb|` of **representative points** (face centroid,
  edge mid-sample), angle = `acos(da·db)` when both refs carry a direction.
- The correct OCCT primitive is already linked and already used: `BRepExtrema_DistShapeShape`
  (`GeometryEngine.cpp:31`, `:457`).
- The Prepare tab ships a full measure gizmo — `GLGizmoMeasure` (`src/slic3r/GUI/Gizmos/`) over
  `Measure::SurfaceFeature{Point, Edge, Circle, Plane}` (`libslic3r/Measure.hpp:16`). It is
  **mesh/triangle-based** (a Plane feature is a triangle index + normal + point) and bound to the
  plater's model objects. `DesignSketchTool.cpp:6959` already borrows its *label* style and
  nothing else.

**Verdict.** Measure must not be `measure()` wired to a row. `measure()` is a centroid proxy: for
point↔face the honest answer is the perpendicular distance to the plane, for two skew edges the
minimum distance between the segments, for a cylindrical face the radius, for two parallel faces
the material thickness — none of which is "distance between two chosen points". The proxy is right
for exactly the case where both refs are points, which is the case a user reaches last.

### 9.8 Measure — engine, blueprint

Kernel addition (`GeometryEngine`, next to `surface_deviation`):

```cpp
struct MeasureResult {
    enum class Kind : uint8_t { PointPoint, PointLine, PointPlane, LineLine, LinePlane,
                                PlanePlane, SameCircle, Mixed };
    Kind   kind{Kind::Mixed};
    bool   ok{false};
    double distance{0.0};          // BRepExtrema on the two sub-shapes; exact, not a proxy
    Vec3d  witness_a, witness_b;   // PointOnShape1/2 — the line the UI draws
    bool   has_angle{false};
    double angle_deg{0.0};         // between directions (edge dir / face normal), 0..90 folded
    bool   has_intrinsic{false};
    double intrinsic{0.0};         // a lone pick: length / radius / diameter / area
    const char* refused{nullptr};  // never silent: why no measurement exists
};
MeasureResult measure_refs(const TopoDS_Shape& a_shape, const TopoDS_Shape& b_shape,
                           const Pick& a, const Pick& b);
```

Rules the engine must honour: the sub-shape is materialised from the ref (`TopoDS_Shape(face)` /
`TopoDS_Shape(edge)`; a vertex from `Pick.point`), so a face-to-face measurement is a *shape*
measurement. `distance` is always the **minimum** distance; `angle` is reported separately and
folded to 0–90° (a CAD user does not care which side of 180° the normals are on); a stale ref
(`Pick.ref.generation != live`) refuses with a reason instead of measuring a moved face.

Tool state (`ModalOperator`, no mutation, no commit):

| State | Event | Guard | Action | Next |
|---|---|---|---|---|
| `Idle` | `Move` | over a ref | hover promise, same const pick path as Select | `Hover` |
| | `LeftDown` | on a ref | `apply(Replace)` | `OneRef` |
| `OneRef` | `Move` | over a second ref | preview the two-ref result live | `OneRef` |
| | `LeftDown` | on a second ref | `apply(Add)` → compute | `TwoRefs` |
| | `LeftDown` | empty space | clear | `Idle` |
| | `Esc` | — | disarm the tool (`CadLevel::Tool`) | `Idle` |
| `TwoRefs` | `Move` | over another ref | keep the result, hover the third | `TwoRefs` |
| | `LeftDown` | on any ref | restart with it as the primary | `OneRef` |
| | `Esc` | — | drop the second ref, keep the first | `OneRef` |
| | `RightClick` | — | offer menu | `TwoRefs` |

`OneRef` is the state the current app cannot express at all: it is what makes the tool feel like
Onshape's — pick a face and you already read its area, its normal and its radius if it is a
cylinder, before the second pick exists.

Readout, one line per available fact, never an invented one: `distance` · `ΔX, ΔY, ΔZ` (world-axis
components, computed from the two witnesses) · `angle` · a lone pick's `intrinsic`. The readout is
the same label primitive the dimension chain already draws (`render_dimensioning`,
`DesignSketchTool.cpp:6959`), so it costs one more caller, not one more renderer.

### 9.9 Measure — event routing

| Event | Tool | Panel | Camera | Notes |
|---|---|---|---|---|
| `Move` | hover promise + live preview | — | passthrough | identical to Select's hover rule |
| `LeftDown/Up` | pick, extend, restart | — | orbit may begin on press | same press-fallthrough as Select |
| `Esc` | `TwoRefs→OneRef→Exit` | ladder routes it | — | the ladder already handles "tool armed" |
| any key | — | swallowed while the field is open | — | a query tool must not steal letters: no tool keys while Measure is armed |
| `MiddleDrag`/`RightDrag` | — | — | orbit/pan | a 3D measurement is unreadable from one angle; the camera must stay live *inside* the tool, exactly like Select |
| MCP `measure` | same engine | — | — | the RPC becomes one caller of `measure_refs`, so the agent surface and the GUI cannot disagree |

### 9.10 Measure — three friction points, and how the architecture prevents them

1. **"Pick a face, get the distance to a face I cannot see."** The witness line is the only thing
   that proves *which* faces were measured, and a centroid proxy cannot draw one. ⇒
   `witness_a`/`witness_b` from `PointOnShape1/2` are part of `MeasureResult`, not a UI
   afterthought; the tool refuses to show a number without a line.
2. **Re-measuring after an edit silently reads the old face.** Ids expire on recompute and
   nothing says so (`snaporca-o1l2`). ⇒ `Ref.generation` travels with the pick and a stale ref
   refuses **with a reason in the readout**, instead of returning a number that used to be true.
3. **The unit and the coordinate frame are invisible.** A distance in mm on a rotated part is
   ambiguous the moment you also show `ΔX/ΔY/ΔZ`. ⇒ components are world-axis and labelled as
   such, and every number in the readout passes through the document's unit formatter (the one
   the dimension chain uses), so there is exactly one formatting path in the tab.

---

## Batch 10 — Dimensioning

Archetype: **`ValueDriveOperator`** — one interaction (a label on the geometry, a number typed into
a field over it) over **three different backends**, where the backend is currently decided by which
lambda happened to be built rather than by anything the user can see.

### 10.1 The architectural claim: there are two Dimension tools, and they disagree

| | click-to-place (`Mode::Dimension`) | selection-based (the `sk_dimension` row) |
|---|---|---|
| entry | arm the tool, pick in the viewport (`:10651`) | select first, then apply (`DesignCanvas.cpp:1417-1430`) |
| kinds it can make | Length, Diameter, Radius, Distance, DistanceToLine | **also Angle between two lines** |
| sets the value by | `place_dimension` → `constraint_for` → `upsert_constraint` + `upsert_dimension` → live-solve, field optional (`:1160`) | `apply_dimension` → **moves the geometry first**, then `record_dimension_constraint` (`:604`, `:661`) |
| operand roles | the picked `DimAnnot.ra/rb` (`:1133`) | derived: `Point`→`P0`, anything else→`Center` (`:667`) |

Same six `DimType` values, same labels, two code paths that resolve the same word "Distance" into
different roles and different order of operations. The click path lets the solver drive the
geometry to the number; the selection path moves the geometry to the number and then records a
constraint that asks for it. They agree whenever the sketch is free and disagree as soon as it is
constrained — which is the case the user cares about.

### 10.2 Verified current behaviour

**The click path's FSM is two booleans.** `m_dim_has0` + `m_dim_e0/m_dim_r0` (`:10651-10702`).
First `LeftDown`: a point hit anchors and waits; a whole-entity hit maps Line→Length,
Circle→Diameter, Arc→Radius. Second `LeftDown`: a different point → Distance; a line → DistanceToLine.
`RightDown` clears the anchor only. A double-click on a label reopens its editor (`:10653`).

**Three silent no-ops on that path**, all verified by reading the branch:
click a Point, an Ellipse, an EllipseArc or a BSpline as the FIRST pick — the `if/else if` chain has
no `else`, the event is consumed, nothing happens and nothing is said (`:10679-10687`);
click something that is neither a point nor a line as the SECOND pick — same, and the anchor is
cleared anyway (`:10689-10699`); abort a value field and the placed dimension **keeps driving** at
its measured value (`cancel_dimension_value`, `:2174` — deliberate, but it means a cancelled
gesture has already added a constraint, and nothing says so).

**`apply_dimension` records a poison value.** Its per-case thresholds move nothing for
`v <= 0` (Length/Diameter/Radius, `:611-613`) or `v < 0` (Distance/DistanceToLine, `:622`, `:639`) —
and then `record_dimension_constraint(v)` runs **unconditionally** (`:659`). The file's own sibling
says it best, at the socket boundary: *"a negative/zero/NaN value that moved nothing would still be
pushed to the solver as a constraint it must satisfy and cannot, silently corrupting the sketch
rather than failing. (`apply_dimension` has the same flaw for any other caller; this guard protects
the socket, not the tool.)"* (`McpControl.cpp:1572-1577`). So the agent surface validates and the GUI
does not — the GUI is the less safe caller of its own function.

**`record_dimension_constraint` never dedupes at all.** It `push_back`s unconditionally
(`:674`, `:684`, `:691`, `:700`), so applying the selection-path dimension twice leaves **two**
constraints on the same operands — the identical defect `upsert_constraint` (`:1074`) was written to
kill on the click path (`:1056-1068`). The two paths differ in their dedupe policy as well as their
order of operations.

**`Distance` changes type when the number is zero, and the change is not reversible in place.**
`constraint_for` and `record_dimension_constraint` both emit `Coincident` for `v < 1e-9`
(`:1146`, `:688`); `upsert_constraint` matches on `type` (`:1078`), so type `0`, then type `5`, and a
fresh `Distance` is appended beside the `Coincident` — over-constrained by construction, which is
exactly the failure the `upsert_constraint` comment (`:1056-1068`) says was fixed for duplicates.

**`DistanceToLine` cannot express a side.** `measure_dim` returns `std::abs(...)` (`:1085`) and the
solver emits `SLVS_C_PT_LINE_DISTANCE` with `std::abs(c.value)` (`SketchSolver.cpp:319-323`), so the
sign is discarded at both ends: the user's number cannot say which side of the line the point is on,
and the solver takes the nearer one. (`apply_dimension` does keep the side, but only when it is the
one moving the geometry — `:645-648` — so the two paths disagree here too.)

**The label is never re-measured after a solve.** `m_dimensions[i].value` is written by
`place_dimension` (`:1162`), by `set_dimension_value` (`:2167`) and by the chain's lambda (`:1426`)
and by nothing else; `dim_text` prints `a.value` (`:2179`). The only `a.value = measure_dim(a)` in
the file is the auto-edit chain re-measuring a *live quote* before it becomes a step (`:1423`).
So when the solver refuses, the label shows the number you typed while the geometry is unchanged —
the same "the number changed and nothing moved" reading the duplicate-constraint comment describes,
now caused by conflict instead of duplication. The feedback that does exist is aggregate:
`apply_dof_status` prints "✗ Conflicting constraints" or "N degrees of freedom" for the whole sketch
(`DesignPanel.cpp:4426-4445`), and `m_entity_conflict` tints per entity (`:8765`) — **no dimension is
ever named**.

**One field, two backends.** `AutoEditStep{label, value, apply(v), hi, title}` (`DesignSketchTool.hpp:982`)
carries a `std::function<void(double)> apply` and nothing else: no outcome, no kind, no target.
The chain fills it either with a solver write (`upsert_constraint(constraint_for(a))`, `:1424-1430`)
or with a direct geometry write (`set_line_angle`, `set_polygon_side`, `set_ellipse_axis`,
`open_rounded_rect_editor`, …), and the two are indistinguishable on screen. Memory 1635 records
why the split exists: libslvs has no constraint for a polygon's side count or an arc's sweep, so
those dims are free by construction.

**Focus is the known-broken part.** The field is a borderless always-on-top frame; the window
manager may refuse it key status (`projects-1p5` is the open bug: the activation request still goes
out with timestamp 0), so `DesignPanel` forwards Enter/Tab and routes Esc (`:4220-4232`, `:4231`).

**Dead residue:** `DesignCanvas::{pending_dimension_type, set_sketch_dimension_value,
cancel_sketch_dimension}` (`DesignCanvas.hpp:281-283`, defined `:1467-1481`) have no callers —
the retired value-card era. The live path is `open_value_editor` → `on_inline_edit` (installed at
`DesignCanvas.cpp:116`) → the field's own closures.

### 10.3 The dimension as a first-class value (blueprint)

```cpp
// One dimension = one target + one value + one explicitly named way of driving it.
enum class DriveKind : uint8_t {
    Solver,      // a libslvs constraint: Distance / Radius / Diameter / Angle / PointOnLine
    Geometric,   // no solver constraint exists: the number writes the geometry directly
    Reference    // measured only; never drives. Today's m_live_quotes.
};

struct Dimension {
    DimType   type{DimType::None};
    DriveKind drive{DriveKind::Solver};
    Pick      target;          // batch 9's Ref + the geometry it carried — one vocabulary
    Pick      second;          // Distance / DistanceToLine / Angle: the other operand
    double    value{0.0};      // what the user asked for (Solver/Geometric)
    double    measured{0.0};   // what the geometry says NOW, refreshed on every solve
    int       con{-1};         // slot in m_constraints when drive == Solver
    // Labels and refusals share one channel so the field can re-open holding the typed text.
    WriteOutcome last{WriteOutcome::Ok};
};
```

Three rules the type enforces, each closing a defect above:

1. **`measured` is refreshed on every solve; `value` is what was asked for.** The label renders
   `value` only while `|value - measured| <= tol`; otherwise it renders `measured` in the refusal
   colour with the requested value as the secondary text (`30.0 → 24.6`). A lie becomes a
   contradiction you can see. `drive == Reference` renders `measured` only, and the palette is the
   one already in the tab (green quote = measured, driving colour = driven).
2. **`apply(value)` returns a `WriteOutcome`** (`Ok | ClampedTo | Refused`, the base-operator type
   from §0.2) — the same type for the solver path, the geometric path and the socket. The socket
   guard already written in `McpControl.cpp:1572-1577` moves *into* `apply`, so the GUI cannot be the
   unsafe caller of its own function any more. `Refused` keeps the field open with the typed text
   and the reason, which is also what makes the `Coincident`-flip reversible: the dimension keeps
   its `type` and only its value changes.
3. **One dimension per (type, operand pair) lives in one `std::vector<Dimension>`**, and the
   constraint slot is a *projection* of it (`upsert` semantics from `:1074-1130`, kept). The click
   path and the selection path both become thin callers of one `Dimension` builder, so the operand
   roles (`ra/rb` vs derived `Center`) are chosen once.

### 10.4 Dimension — FSM (the place sequence, one machine for both entry points)

Budget: pick tolerance as Select (`hit_test_point` / `hit_test`). `Tab` cycles the chain's steps,
`Enter` commits and advances, `Esc` steps the ladder.

| State | Event | Guard | Action | Next |
|---|---|---|---|---|
| `Idle` | `LeftDown` | on a point | anchor it | `Anchor` |
| | `LeftDown` | on a Line / Circle / Arc | create Length / Diameter / Radius and open the field | `Editing` |
| | `LeftDown` | on anything else | **refuse out loud**: "no dimension for an ellipse yet — pick a line, arc, circle or two points" | `Idle` |
| `Anchor` | `LeftDown` | on a different point | create `Distance` | `Editing` |
| | `LeftDown` | on a Line | create `DistanceToLine` (signed; the side is the one the point is already on) | `Editing` |
| | `LeftDown` | on anything else | refuse out loud, keep the anchor (the user can still pick a valid second target) | `Anchor` |
| | `RightDown` / `Esc` | — | drop the anchor | `Idle` |
| `Editing` | `Enter` / `Tab` | value parses | `apply` → `Ok` ⇒ commit, advance the chain (`Tab` skips forward only) | `Idle` / `Editing` |
| | `Enter` | `apply` → `Refused`/`ClampedTo` | keep the field open with the typed text + the reason | `Editing` |
| | `Esc` | — | keep the dimension at `measured` (today's rule) but **report it**: one status line, because a cancelled gesture that added a constraint must say so | `Idle` |
| | `LeftDown` outside | — | commit as `Enter` | `Idle` |
| `Re-edit` | double-click a label | — | reopen the field on that dimension (solver-backed ones re-open on `value`, geometric ones on `measured`) | `Editing` |

Hover: Dimension inherits Select's hover promise (§9.6 defect 1) — over a line it must say
*Length*, over a circle *Diameter*, in `Anchor` over a second point *Distance*. Without that, the
first click of a two-click gesture is a guess.

### 10.5 Dimension — event routing and focus handshake

| Event | Tool | Value field (canvas-hosted) | Panel char hook | Camera |
|---|---|---|---|---|
| `Move` | hover promise + highlight the candidate (`hi` span) | — | — | passthrough |
| `LeftDown` | pick / advance | **must not reach the tool** — a click outside commits | — | orbit may begin |
| `Enter` / `Tab` | — | commit + advance when the field has focus | forwarded when it does not (`:4220-4232`) | — |
| `Esc` | — | close the field, keep the value (`CadLevel::Transient`) | routes it when focus is refused | — |
| a letter | — | — | **not** a tool switch while a field is open, except the sketch tool keys, which commit the field on the way through (`:4288-4293`) | — |
| `MiddleDrag`/`RightDrag` | — | — | — | orbit / pan: a dimension being placed must not trap the camera |

The one-route rule is already written and must not be re-derived per widget: *"ONE Esc, ONE route,
whatever holds focus"* (`DesignPanel.cpp:4228-4231`).

### 10.6 Dimension — three friction points, and how the architecture prevents them

1. **"I typed 30 and nothing moved, but the label says 30."** The most expensive lie in the tab:
   it destroys trust in every number on screen. ⇒ `value` vs `measured` (§10.3 rule 1) plus the
   refusing dimension named in the status line, using the per-entity conflict flags that already
   exist (`m_entity_conflict`, `:749`, `:8765`) — the information is there, only unattributed.
2. **"Is this number driving the sketch or just describing it?"** Three backends, one appearance.
   ⇒ `DriveKind` is visible: the field's title says which (`Length` vs `Length · geometric` vs
   `Length · reference`) and the label colour follows the existing quote palette. A user who cannot
   tell a driving dimension from a measured one cannot reason about over-constraint at all.
3. **"The number I type is not the number that sticks."** Silent thresholds (`v <= 0` no-ops,
   `abs()` on a signed distance, `Coincident` for zero) all discard the user's intent without a
   word. ⇒ every discard returns `ClampedTo`/`Refused` with the bound, and the field re-opens with
   the typed text — the same rule the base operator already applies to `set_slot` and `set_ellipse_axis`
   (§0.2 class 2). The zero-distance case then needs no type flip at all: `Coincident` becomes an
   *interpretation* of `Distance = 0` at solve time, not a mutation of the dimension.

---

## Batch 11 — Constraints

Archetype: **`PlannedConstraintOperator`** — the pick is generic, the meaning is planned, and the
plan is already the best-designed value type in the tab. This batch extends its reach; it does not
invent it.

### 11.1 The architectural claim: a constraint belongs to the sketch, not to a mode

`Mode::Constrain` is a **mode you enter** (`DesignSketchTool.cpp:2266`, `:2281`), and the pick lives
in the tool (`m_pick0/1/2`, `:10306-10327`) while the constraint list lives in **two** places: the
live session's `m_constraints` and the committed feature's `entity_constraints`, chosen by
`live_constraint_scope()` inside the card (`DesignPanel.cpp:3053-3058`). The same list, two owners,
one mode-shaped door.

The mode itself is already half-fiction, which is why that scope switch had to be invented:
`DesignCanvas::is_sketching()` is `m_sketch_tool.is_active()` (`DesignCanvas.cpp:632`) — so a
Constrain session on a *committed* sketch reports "sketching", and the panel has to guard it with
`(m_ui_mode == UiMode::Sketch) &&` (`DesignPanel.cpp:4169`) to get the truth back. A constraint is a
property of a sketch; the mode is a way of editing one.

Constraints are also the last large block of verbs living outside the offer. The offer has 92 rows
(`DesignOffer.hpp`) and the whole constraint vocabulary is **one** of them (`sk_constrain`, `:168`);
the other 20 are toolbar buttons (`DesignPanel.cpp:1516-1539`). Five enum members have no button at
all: `Distance` comes from the Dimension tool, `LockX`/`LockY` have solver bindings
(`SketchSolver.cpp:191`, `:240`, `:245`) and only a label (`DesignPanel.cpp:8051`), `PointOnLine` is produced only as
the *implementation* of Batch 10's distance-to-line, and `PointOnObject` has no producer — it is
built in exactly one place, the re-anchor path (`:8533`). So the offer's own promise — *"every verb
has ONE row index, that index is the same in every selection"* (`DesignOffer.hpp:4-7`) — is unmet for
20 verbs, and Batch 9's `PickPolicy` cannot describe them.

### 11.2 Verified current behaviour (what an implementer must NOT rebuild)

**`ConstraintPlan` is already the archetype.** `SketchEngine.hpp:181`:
`{Kind{Reject, Apply, AskValue}, reason, defs, prefill}` — pure, no wx, no translation (the caller
maps `reason`), testable without a window. `plan_entity_constraint` (`SketchEngine.cpp:2107`) is *"the
ONE place that knows how many picks a type takes"* (`DesignPanel.cpp:7891`) — including the ergonomic
retarget of `EqualLength` on two rounds to `EqualRadius` (`:2118-2121`) and the `Horizontal`/`Vertical`
refusal that exists because *"with a Point or Circle picked, P1 is a role the solver silently drops
while STORING the constraint, so the sketch claims to be constrained when it is not"* (`:2135-2142`).

**`ConstraintReject` has thirteen reasons** (`SketchEngine.hpp:170-179`) and every one has a specific,
product-voiced message (`constraint_reject_text`, `DesignPanel.cpp:7817-7845`), several of them
type-dependent ("Angle applies between two lines" vs "Parallel, perpendicular and equal length apply
to two lines").

**Commit is already a rollback transaction.** `try_add_constraints` (`:2399`): append → solve → keep,
or `resize(mark)` and keep the geometry (`pl5`: the solver only writes back on success).

**A constraint already has a visible undo.** Badges render in a live sketch (`:8798-8808`, with the
comment recording that they used to be invisible outside Constrain mode), a click deletes
(`remove_constraint_near :2418` → `remove_constraint :2426`, which re-runs `resolve_live()` so the DoF
readout and the conflict tint cannot go stale), and the success message teaches it: *"Applied
constraint · its badge is on the sketch — click the badge to remove it"* (`:7915-7917`).

**Draw-time inference exists and is measured.** `infer_auto_constraints` (`:2452`): coincidence
(batched, no fallback — "consistent by construction"), H/V (batched, then one at a time), relations
only for batches ≤ 16 entities (`kRelInferMaxBatch`), with the reasoning recorded: `EqualRadius`
couples far-apart entities and merges the connected components the solver partitions on, which cost
main-thread timeouts on seven large sheets in the corpus rung (2026-08-31, `yww4`); and *"a SCRIPTED
ADD IS NOT A DRAWN GESTURE"* (`8xg1`).

**The solver names the guilty constraint.** `SketchSolveResult.bad` = *indices into `constraints` of
the conflicting ones* (`SketchSolver.hpp:18`, filled at `SketchSolver.cpp:394`, `:521-522`).

### 11.3 The three defects

1. **The solver names the constraint and the UI throws the name away.** `r.bad` is converted to
   `m_entity_conflict[e] = 1` per *entity* (`:745-751`) for a red tint, and the user is told
   "Constraint rejected (over-constrained)" (`DesignPanel.cpp:7908`). On the live path the rolled-back
   batch is always the *new* one, so the conflict is almost always with something already in the
   sketch — precisely what `r.bad` identifies. This is Batch 10's "no dimension is ever named"
   wearing different clothes: the same information, discarded at the same place.
2. **Ctrl+Z does not undo a constraint.** Documented in the code as a known limitation
   (`DesignPanel.cpp:7887-7895`): the undo stack holds committed *features*, so during a sketch
   Ctrl+Z calls `undo_last_sketch_entity()` (`:4252`) and deletes the last **entity** instead. The
   universal undo is therefore both wrong and more destructive than the badge's undo, and the two are
   not the same operation.
3. **`roles_of` exists twice and has already diverged once.** Two file-local lambdas
   (`:2451` in `infer_auto_constraints`, `:9532` in `heal_coincidences`); the first was missing
   `EllipseArc` while the second had it, so *"an ellipse arc's endpoints could be WELDED by the healer
   but never auto-inferred coincident at draw time — the same gesture behaved differently depending on
   which path ran"* (`:2455-2461`). Fixed in place; the duplication that caused it is still there.
4. *(Not a defect, but a silent policy a user meets without warning)*: relations are simply **not
   inferred** when a batch is larger than 16 entities. The reason is sound and measured; the silence
   is the part that needs a voice.

### 11.4 The operator — FSM

The pick deliberately does not know the type; the *plan* does. So the FSM asks the plan first and the
pick becomes typed: `archetype → PickN → Plan → (Reject | AskValue | Apply)`.

| State | Event | Guard | Action | Next |
|---|---|---|---|---|
| `Idle` | button / offer row | `plan_entity_constraint(…, type)` with zero picks | `Reject` ⇒ say the reason **before** the first click ("Parallel applies to two lines"), and arm | `PickN` |
| `PickN` | `Move` | over an entity | hover promise naming the slot: *"slot 1 of 2 — Parallel"* | `PickN` |
| | `LeftDown` | slot k of n unfilled | fill it, highlight it red | `PickN` |
| | `LeftDown` | all slots filled | restart from slot 1 (today's rolling rule, `:10322`), announced in the promise | `PickN` |
| | `Esc` | — | drop the picks, keep the tool armed (base `Gesture` level) | `Idle` |
| | `RightClick` | — | offer menu | `PickN` |
| `Plan` | — | always | `plan_entity_constraint` again, now with picks | branch below |
| | | `Reject` | keep the picks, name the slot and the reason | `PickN` |
| | | `AskValue` | open the field with `prefill` (Angle in degrees, DistanceX/Y, Radius, Diameter) | `Editing` |
| | | `Apply` | `try_add_constraints(defs)` | `Idle` on ok, `Refused` on conflict |
| `Editing` | `Enter` | — | `write_plan_value` → commit → same two exits | `Idle` / `Refused` |
| | `Esc` | — | close the field, drop the picks, no constraint | `Idle` |
| `Refused` | — | always | name the constraint from `r.bad` and its operands, keep the picks, offer "remove the conflicting one" | `PickN` |

`Refused` is the state the base operator already requires (§0.2 class 1) and the one this batch is
really about: today a refusal is a status line that forgets everything.

### 11.5 Constraints — event routing and focus handshake

| Event | Tool | Card (the list) | Panel / char hook | Camera |
|---|---|---|---|---|
| `Move` | hover promise + slot legend | — | — | passthrough |
| `LeftDown` | fill the next slot | row click = highlight its entities | — | orbit may begin on press (same rule as §9.5) |
| click a badge | delete that constraint (`remove_constraint_near`) | rebuild the list | — | — |
| `Esc` | drop picks (`Gesture`), then leave Constrain (`Tool`) | — | one route (`:4231`) | — |
| `Ctrl+Z` | **must undo the last constraint** (see 11.6.2) | rebuild | routes it today (`:4252`) | — |
| `Tab` | next slot / next field | — | — | — |

### 11.6 Constraints — three friction points, and how the architecture prevents them

1. **"Which constraint is complaining?"** The refusal is anonymous while `r.bad` knows the answer.
   ⇒ `Refused` names it: the plan already carries the type, the def carries `ea/eb/ec`, and
   `constraint_label` (`DesignPanel.cpp:8040+`) already renders a human phrase per def. The card row
   and the badge are one lookup away, so the message can read *"Parallel on Line 3 conflicts with
   Horizontal on Line 3"* — and offer to remove the older one, which is a real decision the user is
   making at that moment and today is left to guesswork.
2. **"Ctrl+Z deleted my line."** The universal undo must be the universal undo. ⇒ the live sketch gets
   its own undo stack (entities *and* constraints in one sequence, which is also what Batch 12's
   Trim/Extend needs); the badge click stays as the direct, discoverable route.
3. **"Nothing says how many picks this needs, or which one I am on."** The rolling 3-slot pick is
   colour-identical per slot (`:8666-8673` red for any picked slot) and the arity is checked *after*
   the pick. ⇒ the operator arms from the plan, so `n` and the slot names are known before the first
   click — `Symmetric` announces three slots ("two entities, then the axis", matching
   `ConstraintReject::NeedAxisLine`), and the promise says which slot the click will fill.

---

## Batch 12 — Modification (Trim, Extend, Split, Offset, Mirror)

Archetype: **`ConstraintAwareTransform`** — a modification does not only move geometry, it invalidates
the constraints and dimensions that referred to it. This batch is the first where that second half
exists at all, and today it happens behind the user's back.

### 12.1 The architectural claim: a modification is a transaction over a constraint-bearing model

A creation tool adds geometry and infers constraints onto it. A modification **edits geometry that
already carries constraints**, and must therefore decide, for every ref it touches, one of three
things: keep it, re-bind it, or drop it. The tree has three different answers and none of them is
reported:

| Path | What happens to the constraints | Said to the user |
|---|---|---|
| Trim / Extend (`:1622`) | `drop_constraints_referencing(ei)` — every ref to the cut entity is erased (`:1582`) | nothing |
| Rotation, typed angle (`:1295`, `:1715`, `:1764`, `:1928`) | `drop_orientation_constraints` — H/V/Parallel/Perp/Angle/Lock on the rotated span erased (`:1559`) | nothing |
| Fillet / Chamfer (`confirm_op`, `:7932`) | **surgery**: erase the stale corner Coincident + each leg's own length `Distance`, then re-bind the new entity through a decreasing-ambition ladder of constraint sets until one is accepted | nothing |
| Offset (`:7976-7988`) | adds a relationship between source and copy — `Parallel` for a line, `Concentric` for an arc/circle | nothing |
| Mirror | the copies carry no constraints at all | nothing |
| **Committed** edit ops (`DesignPanel::apply_edit_op`, `:8505-8540`) | drops the stale refs **and then re-anchors** the moved endpoint with a `PointOnObject` onto whatever entity it now lies on — with a rollback if the solver rejects the new binding | nothing |

That last row is the disagreement worth naming: the **same** operation behaves differently depending
on whether the sketch is live or committed. The live scissors drop (`DesignSketchTool.cpp:1622`,
`drop_constraints_referencing` and nothing else); the committed path re-anchors
(`DesignPanel.cpp:8510-8539`, Line/Circle cutters only, rollback at `:8537-8539`). Memory 1603 records
the re-anchor as the intended design. So a trim on a live sketch loses the coincidence, and the same
trim on a committed one keeps the loop closed — which is the Batch 10 pattern again, one batch later.

And the drop paths leave the **dimension behind**: both drop functions only remap `DimAnnot.con`
(`:1577`, `:1598`), so a dimension whose constraint was erased stays in `m_dimensions` with `con = -1`
and its label keeps rendering `a.value` (Batch 10: `dim_text` prints the stored value). A trim can
therefore turn a driving dimension into pure decoration, silently. That is the same lie as Batch 10's
"the label says 30 and nothing moved", arrived at from the other direction — and it is exactly what a
`DriveKind` transition (`Solver → Reference`) should *report*.

### 12.2 Verified current behaviour (what an implementer must NOT break)

**Trim/Extend are the reference implementation of the hover promise** that §9.6 defect 1 says is
missing everywhere else. `compute_trim_preview` is pure and const, replays the pick and the kernel cut
on a **copy**, and returns the exact sub-polyline a click would remove; the renderer repaints it in red
from the cursor every frame (`:9134-9148`, never persisted). Refusal is spoken — the tool's own comment
cites `nde #15: don't fail silently` and the readout names what to click instead
(`:10156-10162`). One cut per click, right-click exits, drag falls through to the camera. Also:
`trim_entity`/`extend_entity` **slide one endpoint without adding or removing an entity**, which is
why the indices — and therefore every stored constraint index — stay valid (`:1600-1603`).

**The edit-op family's commit rule is deliberate and documented.** A ready op commits when: the value
is typed and Enter pressed (`open_op_editor`'s commit calls `confirm_op`, `:7872`, with the comment
recording the old failure — *"the most obvious route of all — click the radius, type it, press
Return — ended with the value set, the ghost drawn, and no geometry written"*); the user clicks empty
space (`:10202`); **another tool is armed** (`:237`, and the comment explains both the rule and the
ordering trap that made it read `op_ready() == 0` with `a=0 b=3 val=28.205` sitting right there); and
when the sketch is committed (`:2241`). Commit-on-leave is a rule, not an accident.

**The kernel already refuses by design and says so in its own documentation**, which is where the
UI message should come from: `offset_entities` *"Ellipses and splines are not offset (a parallel of
either is not the same kind of curve) and are dropped from the result"* (`SketchEngine.hpp:312-318`),
and it preserves chains (shared endpoints offset together, miter-repaired) so a closed profile stays
closed. `mirror_entities` reflects about a line, `make_bridge` exists with no GUI at all.

### 12.3 The four defects

1. **Silent constraint loss**, and with it the silent demotion of a dimension to decoration
   (§12.1). Nothing on screen changes when a constraint disappears except the badge vanishing — which
   is also how the *user* deletes one deliberately (Batch 11).
2. **Offset on an ellipse or a spline is a silent no-op.** The kernel drops them by design, so
   `out.empty()` hits `reset_op(); return;` (`:7981`) with no message: the ghost disappears and
   the click did nothing. The refusal text exists in the kernel's comment and not in the product.
3. **`mirror_entities` hands out a reversed half** — `snaporca-stn3`, verified open: the producer
   "reflects each entity in place and leaves the result traversing the opposite way round", which has
   already caused three defects in one day (exact loop area, `572f794c84`; offset to the wrong side,
   `3974f8a170`; and any future direction-sensitive op). Both consumers are now defensive; the
   producer is still wrong. The issue carries the proposal: emit the reflected entities in **reverse
   order, each individually flipped**, so appending the result yields one consistently-oriented chain.
4. **The hover preview shows the geometry change and not the constraint change.** Trim says nothing
   about the two constraints it is about to erase, although `drop_constraints_referencing` already
   knows exactly which ones.
5. **The live and committed paths of the same operation disagree** (table above): live drops, committed
   re-anchors. One of the two is the design; the batch's job is to make it the same one.

### 12.4 The family's FSM

Two shapes share one machine: `pick → ready → (ghost | value) → commit`, with Trim/Extend as the
instant form (no ghost, no confirm) and Offset/Fillet/Chamfer/Mirror as the confirmed form.

| State | Event | Guard | Action | Next |
|---|---|---|---|---|
| `Idle` | tool armed | — | — | `Pick` |
| `Pick` | `Move` | over a candidate | **preview** (pure, const): the geometry delta *and* the constraint delta | `Pick` |
| | `LeftDown` | candidate accepted | Trim/Extend: apply, re-solve, stay armed for the next cut | `Idle` |
| | | candidate accepted | Offset: pick the source, seed `value`, ghost | `Ready` |
| | | candidate accepted | Mirror: first pick = axis line, then targets toggle | `Ready` |
| | | candidate refused | refuse with the kernel's own reason (ellipses/splines are not offset) | `Pick` |
| | `RightDown` | — | exit the tool (today's `request_exit()`) | `Idle` |
| | `Esc` | — | disarm (base `Tool` level) | `Idle` |
| `Ready` | arrow `Drag` | Offset | signed value → ghost | `Ready` |
| | `Enter` in the field | — | **commit** (the value *is* the commit) | `Idle` |
| | `LeftDown` on empty space | — | commit | `Idle` |
| | another tool armed | — | commit, then arm the new tool (documented rule) | `Idle` |
| | `Esc` / `RightDown` | — | `reset_op()` — discard the ghost, change nothing | `Idle` |
| **Commit** | — | kernel returns empty / refuses | **refuse and keep the ghost**, naming the reason | `Ready` |

The last row is the whole point: today `out.empty()` silently discards the gesture, and a commit that
cannot happen must be a refusal (base operator, §0.2).

### 12.5 Modification — event routing

| Event | Tool | Preview overlay | Panel | Camera |
|---|---|---|---|---|
| `Move` | pick + `preview()` | geometry delta (red, as Trim does today) + a one-line constraint delta | — | passthrough |
| `LeftDown` (Trim) | apply at once | — | status line after the cut: *"trimmed · dropped 2 constraints, 1 dimension now reference"* | orbit may begin on press |
| `Enter` / field | commit | — | — | — |
| tool key while ready | commit then switch (`:237`) | — | — | — |
| `Esc` | `Gesture` → drop the ghost; `Tool` → disarm | — | one route (`:4231`) | — |
| `Ctrl+Z` | must undo the *modification* — the same journal Batch 11 needs | — | routes it today | — |

### 12.6 Modification — three friction points, and how the architecture prevents them

1. **"My sketch lost its constraints and I did not see it."** ⇒ the operator computes a
   `ConstraintDelta{kept, rebound, dropped}` and the refusal/preview channel speaks it; a
   modification that drops anything leaves one line on the status bar and one undo entry. The ladder
   `confirm_op` already uses for fillet (tangent+tangent → tangent → coincident) generalises into
   *rebind before dropping*, so the common case stops being a loss at all.
2. **"Offset did nothing."** ⇒ the kernel's own documented limitation becomes the UI's refusal text
   (ellipses and splines are not offset because a parallel of either is not the same kind of curve),
   the ghost is not drawn for an ineligible source, and the commit keeps the state instead of
   resetting it.
3. **"I mirrored a half and the result walks the wrong way round."** ⇒ `mirror_entities` emits the
   reflection in reverse order with each entity flipped (`snaporca-stn3`'s proposal), so the output is
   a continuation of the chain rather than a mirror image of it. The consumers' defensive handling
   stays — it is what makes offset correct for imported geometry the tool did not generate.

---

## Batch 13 — Dress-up (sketch Fillet and Chamfer)

Archetype: **`CornerOperator`** — the only family whose subject is *not an entity*. A fillet's input
is two lines and the vertex between them, and that vertex exists only as a computation, performed
twice, in two files, by hand.

### 13.1 The architectural claim: the corner is computed twice and must agree

`DesignSketchTool::op_corner` (`:7739-7762`) intersects the two lines to get the corner `C`, the
inward angle bisector and the angle — used to anchor the arrow. `SketchEngine::fillet_lines` /
`chamfer_lines` (`SketchEngine.hpp:343-352`) compute the same corner again to place the arc or the
setback segment. The tool's own comment: *"Mirrors SketchEngine::fillet_lines's geometry so the arrow
tracks the op exactly"* — a hand-maintained duplicate, exactly the `roles_of` pattern from Batch 11
(`:2455-2461`, where the two copies had already diverged). Any divergence here is visible as an arrow
that points one way while the fillet cuts another.

### 13.2 Verified current behaviour (what an implementer must NOT break)

**Kernel scope, stated by the kernel:** `chamfer_lines` is *"symmetric chamfer between two lines
meeting at a corner… False if the lines are parallel or `d` overruns either line"*; `fillet_lines` is
the line-pair equivalent. **Lines only** — a line/arc or arc/arc corner has no implementation, while
Onshape's sketch fillet works on any two entities meeting at a corner.

**The pick is two lines, in order.** `op_pick` (`:7798-7822`): a non-line pick returns immediately
and silently (`:7804`); the first line becomes `m_op_a`, the second `m_op_b` and seeds the value at
**20 % of the shorter leg** (`max(0.001, 0.2 * min(la, lb))`, `:7811`).

**Dress-up has a real geometry preview.** `recompute_op_ghost` (`:7762-7780`) runs the *kernel op*
into `a_out/b_out/extra` and stores it in `m_op_ghost`, which `render_op_gizmo` draws ghost-green via
`draw_entities_preview`. The arrow runs along the inward bisector from the corner, its length is the
value, and the label sits at the tip (`R…` for fillet, bare for chamfer, `:7894-7900`). So this family
already does the thing §9.6 says is missing for Select and §12.6 says is missing for the constraint
delta: it previews the *result*, not just the pick.

**The commit is a decreasing-ambition ladder.** `confirm_op` (`:7936-7971`) first erases the stale
corner `Coincident` and each leg's own length `Distance`, then tries, for a fillet,
`{tangent(a), tangent(b)}` → `{tangent(a)}` → `{}` (coincident bindings only) and keeps the first set
`try_add_constraints` accepts; a chamfer tries the coincident bindings alone. The new entity is
appended and bound; the source lines are replaced in place, so indices stay stable.

**And `op_ready()` does not check the corner.** It is `m_op_a >= 0 && m_op_b >= 0` (`:7725-7729`), so
a **parallel pair is "ready"**: the arrow is drawn, confirming calls the kernel, the kernel returns
false, and `confirm_op` does `reset_op(); return;` (`:7941`) — the ghost disappears, nothing happens,
nothing is said. Same shape as Batch 12's offset-on-an-ellipse.

### 13.3 The defects — the first one is above the level of this batch

1. **`DimAnnot.con` goes stale in two of the five mutators, and it is a corruption path, not a
   cosmetic one.** `m_dimensions[i].con` is an *index* into `m_constraints`. Two mutators renumber it
   and repair the links (`drop_orientation_constraints` `:1579`, `drop_constraints_referencing`
   `:1596`). Two do not: **`remove_constraint`** (`:2434` — the badge click, which is the *taught*
   way to delete, Batch 11) and **`confirm_op`** (`:7960` — every fillet and chamfer). After either,
   a dimension whose constraint sat after the erased slot points at a *different* constraint; and
   `set_dimension_value` validates only the range (`a.con < m_constraints.size()`, `:2168`), not the
   identity — so **editing that dimension's label silently overwrites an unrelated constraint**, and
   the solver then obeys a def nobody asked for. Reachable in three gestures: draw two dimensioned
   lines, click one constraint's badge, then click the other dimension's label and type a value.
   This is a data-integrity defect and should be fixed outside the batch's normal ordering.
2. **A doomed op is announced as ready.** `op_ready()` must require `op_corner()` to succeed; then
   the parallel pair is refused *before* the ghost, and the overrunning value is refused at the
   kernel's own stated condition ("`d` overruns either line") instead of at a silent `reset_op()`.
3. **The corner is computed twice** (§13.1). One `SketchEngine::corner_of(a, b)` — the exact function
   `fillet_lines` already needs internally — should be exposed and used by the tool.
4. **The arrow keeps a stale anchor** when `op_corner` fails: `m_op_anchor`/`m_op_dir` are only
   assigned inside the `if (op_corner(...))` (`:7768-7769`), so the arrow is drawn at the *previous*
   corner while the ghost is absent — the two halves of the preview disagree about which corner is
   being edited.
5. **Scope: lines only.** No line/arc or arc/arc corner, so the most common real corner in a drawing
   (a fillet between a line and an arc) cannot be made at all.

### 13.4 The family's FSM

| State | Event | Guard | Action | Next |
|---|---|---|---|---|
| `Idle` | tool armed (F / H, `:421-422`) | — | — | `Pick1` |
| `Pick1` | `Move` | over a line | highlight; hover promise naming what is needed ("pick the second line") | `Pick1` |
| | `LeftDown` | a line | `m_op_a = ei` | `Pick2` |
| | `LeftDown` | anything else | **refuse**: "a fillet needs two lines — arcs are not supported yet" | `Pick1` |
| `Pick2` | `LeftDown` | a second line, `corner_of` succeeds | `m_op_b`, seed 20 %, ghost, arrow on the bisector | `Ready` |
| | `LeftDown` | a second line, parallel / 180° | **refuse** with the reason; keep `m_op_a` | `Pick2` |
| `Ready` | `Drag` arrow | — | value = projection on the bisector, clamped to what the kernel can take | `Ready` |
| | `Enter` in the field | — | commit (the value *is* the commit, `:7872`) | `Idle` |
| | `LeftDown` empty / another tool | — | commit, then (for a tool) arm it (`:237`) | `Idle` |
| | `Esc` / `RightDown` | — | `reset_op()` — discard, change nothing | `Idle` |
| **Commit** | — | kernel false | **refuse and keep the op**: name the bound the kernel named (`d` overruns the leg) | `Ready` |
| | — | all ladder sets rejected | **refuse**: "the corner is over-constrained — remove a constraint on either leg" | `Ready` |

The last two rows are what the ladder buys once it reports: today the ladder silently degrades
(tangent×2 → tangent → coincident), and the user cannot tell a tangent fillet from one that merely
touches at two points.

### 13.5 Dress-up — event routing

| Event | Tool | Preview | Panel | Camera |
|---|---|---|---|---|
| `Move` | pick + `recompute_op_ghost` | ghost entity set + arrow + label | — | passthrough |
| `LeftDown` | pick / confirm-on-empty | ghost | status after commit: which ladder rung was accepted | orbit may begin on press |
| `Enter` / field | commit | — | — | — |
| tool key while `Ready` | commit then switch (`:237`) | — | — | — |
| `Esc` | drop the op, keep the tool | — | one route (`:4231`) | — |
| `Ctrl+Z` | undo the fillet **and restore what it erased** — the journal Batch 11/12 need, here with the erased `Distance` and `Coincident` | — | routes it today | — |

### 13.6 Dress-up — three friction points, and how the architecture prevents them

1. **"I picked two lines and nothing happened."** Parallel legs, a 180° corner and an overrunning
   value all end in a silent `reset_op()`. ⇒ `op_ready()` consults `corner_of`, the kernel's own
   refusal condition becomes the product's message, and a doomed op is refused at the pick instead of
   at the commit.
2. **"The fillet is not tangent and I cannot tell."** The ladder degrades in silence, and a
   non-tangent arc at a corner reads as a modelling error. ⇒ the accepted rung is named once, at the
   commit, and the badge for the arc carries its `Tangent` constraints, so the loss is visible on the
   geometry (Batch 11's badge language) as well as in the line.
3. **"Esc left a mess / Ctrl+Z does not bring the corner back."** A fillet erases two `Distance`
   constraints and a `Coincident`; the only way back is to undo the whole sketch gesture. ⇒ the
   `SketchEdit` journal (Batch 11/12) records the erased defs with the geometry change, so one Ctrl+Z
   restores the legs, their dimensions and the corner`Coincident` together — and with defect 1 fixed,
   it restores the dimension *links* too, not just the defs.

---

## Batch 14 — Transform (Move, Rotate, Scale, Array, PolarArray, TransformArt)

Archetype: **`PivotSetOperator`** — the subject is a *set*, the pivot is *derived*, and the parameter
is a *relationship* (a vector, an angle, a factor, a count) rather than a scalar on one entity. It is
also the only family in the tab with **two** numeric parameters on one gesture, and the only one whose
maps can preserve or break constraints depending on the map — so it is where the constraint table had
to be written explicitly.

### 14.1 The family, and the sixth archetype's shape

Five modes share the `m_tf_*` block (`DesignSketchTool.hpp:1081-1097`): `m_tf_targets` (toggle-select
like Mirror, `tf_pick :8137`), `m_tf_pivot` (the targets' centroid, `compute_tf_pivot :8103`),
`m_tf_delta` / `m_tf_angle` / `m_tf_scale` / `m_tf_count{3}`, a ghost and two label anchors.
`tf_ready()` is just "any target" (`:8098`). The handle is *derived per mode* (`tf_handle_pos
:8208-8218`): the translated tip, the ring position at the current angle, or the scaled +X point.
Two labels are drawn and both are pickable — the primary at the handle, the count at the pivot
(`render_tf_gizmo :8283-8340`) — so Array and PolarArray are the tab's only two-parameter gestures,
with `open_tf_editor_a` (`:8250`) and `open_tf_editor_count` (`:8273`).

Seeding is deliberately lazy (`tf_pick :8150-8168`): Move/Array seed a step of
`max(1.5 * handle_r, 1 mm)` (Array perpendicular to a lone line, else +X), Rotate 45°, PolarArray a
full 360°, Scale ×2 — and only while still neutral, *"so re-picking more targets keeps a value the
user already dialled in"*.

`TransformArt` is the odd member and is genuinely a different tool: the subject is an imported
*feature* (Text/SVG) rather than entities, the parameters are a 2D offset plus **two independent
scale factors** (`m_xform_sx/sy`, non-uniform by design, floored at 1e-4), and the gizmo is a bounding
box with five square handles — four corners and the centre (`render_xform_gizmo :7706-7724`) — where a
corner drag solves the offset so that the *opposite* corner stays put (`:7690-7700`, `emit_xform :7702`).

### 14.2 Verified current behaviour: the constraint table, read in the right direction

`confirm_transform` (`:8342-8445`) `remove_if`s — **returning true means REMOVED** — over every
constraint that touches a target (`is_target` checks `ea`, `eb` and `ec`):

| Mode | Removed | Kept |
|---|---|---|
| Move | Coincident, PointOnLine, PointOnObject, Concentric, Symmetric, Midpoint, **Fix, LockX, LockY**, cross-entity `Distance` | Horizontal, Vertical, Parallel, Perpendicular, Angle, EqualLength, Radius, Diameter, Collinear, **self**-`Distance` |
| Rotate | everything else — Coincident, Concentric, Tangent, Parallel, Perpendicular, Midpoint, Symmetric, PointOnLine/Object, Horizontal, Vertical, Angle, Fix, Lock | EqualLength, Radius, Diameter, self-`Distance` |
| Scale | everything except the five orientation types | Horizontal, Vertical, Parallel, Perpendicular, Angle |
| Array / PolarArray | additive: copies are appended and bound to their source through a ladder — lines get Parallel+EqualLength (linear) or EqualLength alone (polar), arcs/circles a per-copy Radius plus Concentric when the polar pivot *is* the source's centre | the originals are untouched |

Then `resolve_live()`. The rationale in the comment is real — *"Surviving classes are satisfied by
construction; the re-solve folds in the new placement"* — and the array ladder is the same
degrade-and-keep-the-first-set pattern as Batch 13's fillet, which is the right instinct.

### 14.3 The defects

1. **Two families inside one function disagree about what leaving means.** `set_tool` commits a ready
   edit-op (`:237`) and its comment states the rationale in the strongest terms — *"Discarding it here
   is most of why Fillet looked like it simply did not work"* — and then, thirty lines later,
   `reset_tf(); // drop any in-progress transform gizmo` (`:295`). So arming Line with a pending
   fillet keeps the fillet, and arming Line with a pending rotation **throws the rotation away** —
   targets picked, value dialled or typed, ghost visible. Same gesture, opposite outcome, no message.
2. **The keep/drop table over-removes, silently.** For every mode there are constraint types the map
   provably preserves that are removed anyway: Move removes Coincident/PointOnLine/PointOnObject even
   when *all* their operands are targets (a rigid translation of a whole loop preserves them); Rotate
   removes Coincident, Concentric, Tangent, Parallel, Perpendicular, Midpoint, Symmetric and
   PointOnObject although a rotation preserves all of them; Scale removes Coincident, Tangent,
   Midpoint, Symmetric, PointOnLine, PointOnObject and EqualLength although a uniform scale preserves
   them (and it keeps Horizontal/Vertical, which only a *non-uniform* scale would break — Scale here
   is uniform, so that half is right). The user sees badges vanish and the DoF readout climb, while the
   geometry — which *is* correctly mapped — looks fine, so the loss stays invisible until a later edit.
3. **Move releases an explicit anchor.** `Fix`, `LockX` and `LockY` are removed by a translation. A
   point the user pinned to protect it can be moved, and the protection silently disappears. Either the
   map should obey the anchor (refuse, or carry it) or the release must be reported.
4. **A third erasure site without a `con` remap** (`:8368`), alongside `remove_constraint :2434` and
   `confirm_op :7960` — the corruption path recorded in its own memory: a dimension's index now points
   at a different constraint, and editing its label overwrites that one.
5. **The action bar's ✓ does not confirm a transform.** For `m_active == Tool::None` inside a sketch it
   calls `finish_sketch()` (`DesignPanel.cpp:11304-11307`), and finishing applies the pending transform
   on the way (`:2242`). So the only gesture-level confirm is a **click on empty space** (`:10256`),
   which nothing announces, and the button a user would reach for ends the sketch instead. Right-click
   with targets discards (`:10260`); Esc reaches the same through the ladder.
6. **The array ladder fails silently.** *"else the geometry stays unconstrained"* — the copies appear
   with no relationship to their source and nothing says the ladder was rejected.

### 14.4 The family's FSM

| State | Event | Guard | Action | Next |
|---|---|---|---|---|
| `Idle` | tool armed (no key for these — see the open calls) | — | — | `Pick` |
| `Pick` | `LeftDown` | on an entity | toggle into `m_tf_targets`, recompute the pivot, seed if still neutral, ghost | `Pick` |
| | `Move` | — | hover promise: the pivot marker and the ghost already *are* the promise | `Pick` |
| | `LeftDown` | empty space, no targets | nothing (today it is a no-op click) | `Pick` |
| `Ready` | `Drag` handle | — | primary parameter; `Ctrl`/`Shift` snaps (not implemented today) | `Ready` |
| | `Enter` / field | — | commit | `Idle` |
| | `LeftDown` empty | — | commit (today's only route) | `Idle` |
| | **another tool armed** | — | **commit**, like a ready edit-op — not `reset_tf()` | `Idle` |
| | ✓ in the action bar | — | commit the gesture, *not* the sketch | `Idle` |
| | `RightDown` / `Esc` | — | discard the gesture, keep the tool | `Pick` |
| **Commit** | — | any constraint removed | one line naming the count and classes, and one undo entry | `Idle` |

### 14.5 Transform — event routing

| Event | Tool | Ghost/labels | Panel | Camera |
|---|---|---|---|---|
| `Move` | pick + `recompute_tf_ghost` | ghost, spoke, handle square, pivot marker, two labels | — | passthrough |
| `LeftDown` on an empty point | commit | — | status: what was mapped, what was dropped | orbit may begin on press |
| `LeftDown` on the count label | open the count field | — | — | — |
| `Enter` in either field | commit | — | — | — |
| ✓ / Finish | commit then `finish_sketch()` (`:2242`) | — | `tool_confirm :11304` | — |
| tool key while `Ready` | commit, then arm (must match `:237`) | — | — | — |
| `RightDown` | discard | — | — | — |
| `Esc` | discard, then disarm (ladder) | — | one route (`:4231`) | — |
| `Ctrl+Z` | undo the map **and restore the constraints it removed** — the journal Batch 11-13 need; here it is the difference between "I rotated it" and "I rotated it and lost my sketch's structure" | — | routes it today | — |

### 14.6 Transform — three friction points, and how the architecture prevents them

1. **"I moved to the next tool and my rotation disappeared."** The gesture is visible, dialled and
   committed-by-intent, yet leaving drops it. ⇒ one `leave()` rule for the whole tool layer: a ready
   gesture commits, whatever family it belongs to, and the edit-op rationale (`:228-244`) applies
   verbatim. The undo entry makes the commit safe; the drop makes it infuriating.
2. **"Where did my constraints go?"** Three modes erase four to fifteen types each, by a hand-written
   table, with no message and no way back. ⇒ the table becomes derived rather than written — for each
   `(type, map)` pair the answer is a property of the map, not a choice — and whatever it removes is
   named once, at the commit, with one Ctrl+Z to restore it. `Move`'s dropping of `Fix`/`Lock`
   deserves to be a visible decision (`"the anchor on point 4 was released by this move"`), because
   silently releasing the one constraint whose purpose is to prevent movement is the worst version of
   this class.
3. **"The ✓ finished my sketch."** A gesture-level confirm must exist and be the button the user
   already presses. ⇒ `tool_confirm` gains the gesture level above the mode level (which is what
   `CadLevel::Gesture` already means in `DesignInteraction.hpp`), and `finish_sketch` stops being the
   only thing ✓ can mean while a ghost is on screen.

---

## Open scope calls

- **Batch 8 (carried):** wire edge picking (25a) and the intersection point (27) — both are
- **Batch 9 — resolved by reading the table, not by arguing:** the "homogeneous selection set"
- **Batch 9:** "select other" (a pick list for a face occluded by another) is *not* in this design.
- **Batch 10 — which of the two Dimension paths wins?** Recommended: the **solver drives** (the
- **Batch 10 — signed `DistanceToLine` needs a spike.** The port maps it to
- **Batch 10 — `AutoEditStep::apply` must return `WriteOutcome`**, which touches every chain builder
- **Batch 11 — naming the conflict needs one signature change.** `try_add_constraints` returns `bool`
- **Batch 11 — the live-sketch undo stack is shared work.** It must sequence entities, constraints and
- **Batch 11 — should the 20 constraint verbs get offer rows?** Adding 20 rows to a 92-row table whose
- **Batch 11 — inference for batches > 16 entities should speak.** The policy is correct and measured;
- **Batch 12 — Split has no implementation at all.** Onshape's Split cuts an entity at a crossing and
- **Batch 12 — fix `snaporca-stn3` while the batch is open.** `mirror_entities` emitting a
- **Batch 12 — the constraint delta and Batch 11's conflict name are the same type.** Batch 11 wants
- **Batch 13 — fix `DimAnnot.con` staleness now, and make it impossible.** Two mutators renumber
- **Batch 13 — one `corner_of`, then arc corners.** `SketchEngine::corner_of(a, b)` is the function
- **Batch 13 — `op_ready()` must consult `corner_of`.** Three of the four silent failures in this
- **Batch 14 — no transform tool has a keyboard shortcut.** Nothing in `m_keys_sketch` maps to Move,
  Rotate, Scale, Array, PolarArray or TransformArt (`DesignPanel.cpp:408-422` binds sixteen letters,
  none of them these), so the family is reachable only from the offer. Either assign keys from the free
  letters (G I J K N Q U V W Y Z — K is already Constrain) or state that the family is deliberately
  offer-only: `snaporca-fqwu` counts 47 of 86 verbs with no shortcut, and this is a sixth of them.
- **Batch 14 — derive the keep/drop table, do not maintain it.** `confirm_transform :8357-8387` is a
  hand-written per-type switch that over-removes in all three mutating modes; for each `(type, map)`
  pair the answer is a property of the map (does it preserve the relation?), with the genuine
  exceptions (`Fix`/`Lock`, an absolute `Angle`) named as exceptions.
- **Phase 2 (3D) needs its batch list written before it starts** — otherwise it will be designed by
  whoever opens the file first.

---

## Closing note — Phase 1 (2D) is designed

Fourteen batches, and six cross-cutting changes were asked for by more than one of them. An
implementer should land them in this order, because each one is what makes the next one's refusals,
names and undos possible:

| # | Change | Asked for by | Why first |
|---|---|---|---|
| 1 | `DimAnnot.con` → a stable `SketchConstraintId` (and one `erase_constraint(i)` until then) | 13 (bug) | a data-integrity bug that a later step would build on top of |
| 2 | `WriteOutcome{Ok, ClampedTo, Refused}` on every parameter write, and `AutoEditStep::apply` returning it (19 sites) | 10, 12, 13 | without it every refusal stays silent by construction |
| 3 | `try_add_constraints` returns its `SketchSolveResult`; one `ConstraintDelta{kept, rebound, dropped}` | 11, 12, 13, 14 | the solver already knows the answer; three siblings want to say it |
| 4 | `SelectionSet` + `PickPolicy` (typed, ordered, per-tool) | 9, 11, 14 | the substrate the operators read their subject from |
| 5 | One `SketchEdit` journal + Ctrl+Z in a live sketch | 11, 12, 13, 14 | every modification erases something; today none of it is reversible |
| 6 | `ToolManager.leave()` = commit a ready gesture, one ESC ladder, one hover promise | 9, 12, 13, 14 | the three families currently answer "what does leaving mean" three ways |

Phase 2 (3D) has no batch list yet — it needs one before it starts, or it will be designed by
whoever opens the file first.
