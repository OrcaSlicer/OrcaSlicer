#ifndef slic3r_DesignSketchTool_hpp_
#define slic3r_DesignSketchTool_hpp_

#include "libslic3r/Point.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/CadDocument.hpp"   // CadBody for per-body solid picking
#include "libslic3r/CAD/SketchInference.hpp"
#include "libslic3r/CAD/SketchSolver.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLSelectionRectangle.hpp"   // left-drag rubber band over the committed bodies
#include <functional>
#include <vector>
#include <string>
#include <utility>

class wxMouseEvent;
class wxPoint;

namespace Slic3r {

class TriangleMesh;   // fwd (libslic3r) — solid-pick mesh, non-owning pointer

namespace GUI {

class GLCanvas3D;
class Camera;     // fwd — move_gizmo_arm() sizes the gizmo from the current zoom

// Onshape-style sketch session. `begin` enters a session on a plane; the active
// drawing tool (Mode) can be switched mid-session via `set_tool` while entities
// accumulate. `finish` commits the whole entity list as one sketch feature;
// `cancel` aborts. Constrain is a separate legacy mode that operates on a
// committed profile's points (entity constraints land in a later chunk).


// ONE colour means SELECTED — a face, an edge, a vertex, a whole body, a 2D sketch region.
// Nothing else on screen may wear it. Before this there were four near-identical cyans plus a
// constant still named sel_gold that had long since become cyan, and the UNSELECTED region fill
// was blue (0.30,0.60,1.0) — one shade from the selected one — so an ordinary region read as
// picked. Selection is a state, not a decoration: it gets its own colour and keeps it.
inline ColorRGBA design_selection_color(float alpha = 1.0f)
{
    return ColorRGBA(0.20f, 0.85f, 1.00f, alpha);
}
// Unselected geometry — 2D regions and faces — is neutral translucent grey, so the only
// coloured thing in the viewport is the thing you picked.
inline ColorRGBA design_idle_face_color()
{
    return ColorRGBA(0.72f, 0.76f, 0.80f, 0.14f);
}
class DesignSketchTool {
public:
    enum class Mode { Select, Dimension, Polyline, Line, CornerRect, CenterRect, ObliqueRect,
                      RoundedRect, CenterCircle, TwoPointCircle, Point,
                      ThreePointCircle, ThreePointArc, TangentArc, CenterArc, Slot, ArcSlot, Polygon,
                      Ellipse, EllipseArc, BSpline,
                      // In-canvas edit-op TOOLBAR tools (drag-arrow + label, no numeric card):
                      Fillet, Chamfer, Offset, Mirror,
                      // Standalone scissors: click a segment to trim/extend it (immediate, no card):
                      Trim, Extend,
                      // In-canvas transform TOOLBAR tools (pick targets + drag handle/label, no card):
                      Move, Rotate, Scale, Array, PolarArray,
                      // In-canvas bounding-box transform for imported Text/SVG art:
                      TransformArt,
                      Constrain };
    // Which tool is armed, and how many anchors it has down. Read-only, for the offer ladder:
    // "the menu armed the verb I chose" is otherwise unassertable, and a menu walk that lands one
    // row off arms a NEIGHBOURING tool and then grades whatever that drew. snaporca-ekt9.
    Mode mode() const { return m_mode; }
    int  pending_points() const { return int(m_points.size()); }
    void emit_step_hint();   // fires on_step_changed when the step actually moved
    // Is an in-canvas value field open? While one is, the canvas is frozen and every letter is
    // swallowed — the single most common reason a driven gesture "does nothing".
    bool value_field_open() const { return m_awaiting_length; }
    bool is_edit_op_mode() const { return m_mode == Mode::Fillet || m_mode == Mode::Chamfer ||
                                          m_mode == Mode::Offset || m_mode == Mode::Mirror; }
    bool is_transform_mode() const { return m_mode == Mode::Move || m_mode == Mode::Rotate ||
                                            m_mode == Mode::Scale || m_mode == Mode::Array ||
                                            m_mode == Mode::PolarArray; }
    // Creation tools that get draw-then-edit: on commit the new entity/feature is selected
    // and its primary value editor opens. Line is handled inline (its own length field);
    // Polyline/BSpline/Point have no single primary value, so they opt out.
    bool is_creation_autoedit_mode() const {
        switch (m_mode) {
        case Mode::Line:
        case Mode::CornerRect: case Mode::CenterRect: case Mode::ObliqueRect:
        case Mode::RoundedRect: case Mode::CenterCircle: case Mode::TwoPointCircle:
        case Mode::ThreePointCircle: case Mode::ThreePointArc: case Mode::TangentArc:
        case Mode::CenterArc: case Mode::Slot: case Mode::ArcSlot: case Mode::Polygon:
        case Mode::Ellipse: case Mode::EllipseArc:
            return true;
        default: return false;
        }
    }
    // The host (DesignCanvas) flags the canvas frozen while an inline value editor is open,
    // so a stray click/move can't draw under the floating field. Reuses m_awaiting_length
    // (Line's existing freeze flag) as the single "inline editor open" gate.
    void set_inline_busy(bool b) { m_awaiting_length = b; }
    bool inline_busy() const { return m_awaiting_length; }   // true while a value field is open
    // Does the live session hold anything a cancel would throw away? Escape must not silently
    // destroy drawn geometry; the panel asks this before treating Escape as "discard sketch".
    bool live_sketch_has_work() const { return !m_entities.empty(); }

    // Clicking the same sub-element again escalates to the whole body. That is right for free
    // picking and WRONG while a card has armed a face/edge pick: the card says "click a FACE",
    // the user clicks the face it is already showing, and the escalation turns it into a
    // whole-body pick that the armed capture then rejects. The host turns this off for as long
    // as a pick is armed.
    void set_escalate_on_repick(bool on) { m_escalate_repick = on; }
    bool constrain_value_anchor(wxPoint& out) const; // screen anchor over the picked constrain geometry

    void begin(const SketchPlane& plane, Mode mode = Mode::Polyline);
    // Re-open a committed entity sketch for full in-canvas editing: load its entities +
    // driving constraints, re-detect the polygon/rect/slot grouping, and live-solve. The
    // caller re-commits via finish() (the panel replaces the feature, see m_edit_index).
    void begin_edit(const std::vector<SketchEntity>& entities,
                    const std::vector<SketchEntityConstraintDef>& constraints,
                    const SketchPlane& plane);
    // Drop rigid 2D art (Text / SVG outlines) INTO the live sketch as ordinary line entities,
    // so it joins the sketch being drawn instead of committing a separate Sketch feature.
    // `regions` are loops in PLANE coordinates; every closed loop becomes a closed polyline, so
    // the result is editable, constrainable and extrudable like anything else drawn by hand —
    // unlike imported_regions, which are rigid and carry no solver entities.
    // Returns false when no session is live, so the caller can fall back to a new feature.
    bool add_imported_regions(const std::vector<std::vector<std::vector<Vec2d>>>& regions);
    void set_tool(Mode mode);                 // switch tool, keep accumulated entities
    void set_plane(const SketchPlane& plane) { m_plane = plane; }  // re-plane a live sketch (a reference plane was clicked mid-session); entities are 2D, re-lifted through the new plane
    void set_construction(bool c) { m_construction = c; }
    void set_polygon_sides(int n) { m_polygon_sides = (n < 3 ? 3 : n); }
    void set_polygon_circumscribed(bool c) { m_polygon_circumscribed = c; }
    void finish();                            // emit accumulated entities, end session
    void cancel();
    bool is_active() const { return m_active; }
    bool has_entities() const { return !m_entities.empty(); }
    bool on_mouse(wxMouseEvent& evt, GLCanvas3D& canvas);
    // Right-click on a draw tool: true when an in-progress anchor was abandoned, false when
    // there was nothing to abandon — and false is what lets the offer menu open. snaporca-ghcz.
    bool right_abandon();
    // True if the LAST right-press was consumed as a gesture terminator (end a polyline chain,
    // abandon an anchor, exit a tool). Read-and-clear: the canvas asks on the matching release to
    // decide whether that right-click was the user's, in which case it opens the offer.
    bool take_right_consumed() { const bool b = m_right_consumed; m_right_consumed = false; return b; }
    void render(GLCanvas3D& canvas);

    // Persistent committed sketches to draw even when no session is active (e.g. an
    // un-consumed sketch left visible after its extrude is removed). Each carries its
    // own plane. render() draws these as translucent faces + outlines.
    struct DisplaySketch { std::vector<SketchEntity> entities; SketchPlane plane; int feature{-1}; };
    void set_display_sketches(std::vector<DisplaySketch> ds) { m_display_sketches = std::move(ds); }
    void set_highlight_sketches(std::vector<std::pair<int, ColorRGBA>> hl) { m_hl_sketches = std::move(hl); }
    // Solid pick is resolved on LeftUp (see on_mouse): consuming the press broke orbit/pan.
    int  m_pick_press_x = 0;
    int  m_pick_press_y = 0;
    bool m_pick_pending = false;

    bool has_display() const { return m_active || !m_display_sketches.empty()
                                      || (m_solid_bodies != nullptr && !m_solid_bodies->empty())
                                      || !m_datum_planes.empty()
                                      || m_show_planes || m_show_axes
                                      || m_ex_active || m_mv_active || m_fl_active
                                      || m_hl_active || m_th_active || m_sh_active
                                      || m_dr_active || m_ct_active || m_dz_active || m_dbp_active || m_hx_active || m_rb_active; }

    // View helpers: the 3 world origin planes (XY/XZ/YZ) and the world axis triad, each
    // shown/hidden by a toggle (keys P / A). Off by default so the idle scene stays clean.
    void set_show_planes(bool s) { m_show_planes = s; }
    void set_show_axes(bool s)   { m_show_axes = s; }
    bool toggle_show_planes() { m_show_planes = !m_show_planes; return m_show_planes; }
    bool toggle_show_axes()   { m_show_axes   = !m_show_axes;   return m_show_axes; }

    // Solid topology selection on the committed bodies: clicking a solid cycles
    // whole-solid -> face -> edge (Onshape-style) to target fillet/chamfer/extrude. With
    // multiple bodies the pick resolves WHICH body was hit (per-triangle body id).
    // Appended, never reordered: DesignPanel maps this to a level int (Whole=1, Face=2,
    // Edge=3, Vertex=4) and the offer table keys off it.
    enum class SolidSel { None, Whole, Face, Edge, Vertex };
    // Point the tool at the current bodies + their concatenated tessellation (non-owning;
    // pass nullptr to clear). Call after each recompute — selection resets (ids invalidate).
    // tri_face = per-triangle face id within its body; tri_body = per-triangle body index.
    void set_solid_pick(const std::vector<CadBody>* bodies, const TriangleMesh* mesh,
                        const std::vector<int>* tri_face, const std::vector<int>* tri_body,
                        const std::vector<bool>* visible = nullptr,
                        const std::vector<Transform3d>* xform = nullptr);
    // Body-focus picking: >=0 restricts every pick to that one body, so a face behind
    // another solid is reachable without hiding anything. -1 = no restriction.
    // Survives set_solid_pick() — it is owned by the panel, not by the mesh feed.
    void set_pick_only_body(int b) { m_pick_only_body = b; }
    void clear_solid_selection();
    bool has_solid_selection() const { return m_solid_sel != SolidSel::None; }
    // Select a whole body by index (from the Parts list) — Whole-level highlight, no face/edge.
    // body < 0 or out of range clears the selection.
    void select_body(int body);

    // Move-body gizmo (M5): translate a whole body with three world-axis drag arrows
    // (X red / Y green / Z blue) anchored at the body centroid. Display-only — the host
    // keeps a per-body Transform3d and re-feeds the moved display/pick meshes; the OCCT
    // shape (and thus face/edge global ids) is never touched. Drag fires on_body_move_changed
    // live; a stationary click on an arrow opens the inline offset editor for that axis.
    void set_move_gizmo(int body, const Vec3d& pivot, const Transform3d& base_xform,
                        double body_radius = 0.0);
    void clear_move_gizmo();
    bool moving_body() const { return m_mv_active; }
    int  move_body_index() const { return m_mv_body; }
    // F key forwarded from the canvas (Prepare's Place on Face): returns true if it acted.
    bool request_place_on_face() { return on_place_on_face ? on_place_on_face() : false; }
    std::function<bool()> on_place_on_face;
    std::function<void(int body, const Transform3d& xform)> on_body_move_changed;
    // Fired on each cycle change: (level 0=None/1=Whole/2=Face/3=Edge, body index, face id, edge id).
    std::function<void(int level, int body, int face, int edge)> on_solid_selection_changed;
    // Click a committed sketch overlay (no live session) -> select that loop: the Sketch
    // feature index + the clicked closed-region index within it (-1 = no specific loop).
    // entity = the sketch entity index under the cursor when the click landed on a loop
    // STROKE, else -1 for an interior/region hit. Carried because a tool can legitimately
    // want the LINE you pointed at, not just the loop it belongs to (Rib, snaporca-3648).
    std::function<void(int feature, int region, int entity)> on_display_sketch_selected;
    // Double-click on a committed sketch stroke: open THAT feature for editing. Selecting a line
    // and then hunting for an Edit button in a panel is the dependency this tab exists to remove.
    std::function<void(int feature)> on_display_sketch_activated;
    // Entities forming the currently click-selected loop (for a per-loop extrude); empty
    // if no loop is selected.
    std::vector<SketchEntity> selected_loop_entities() const;
    // Per closed loop, the indices into `ents` that form it (for hiding already-extruded
    // loops from the committed-sketch overlay).
    std::vector<std::vector<int>> region_entity_indices(const std::vector<SketchEntity>& ents) const;
    // Same, but each region's entry is its OWN entities followed by the entities of each of its
    // holes, in that order — the exact list a per-loop extrude of a region WITH holes stores
    // (see selected_loop_entities()). Needed to match a consumed loop against its source sketch.
    std::vector<std::vector<int>> region_entity_indices_with_holes(const std::vector<SketchEntity>& ents) const;
    void clear_display_pick() { m_display_pick = -1; m_display_pick_region = -1; }
    // Adopt a loop pick the tool did not make itself. The live-sketch path resolves the region
    // BEFORE the sketch is committed, so once finish_sketch() has turned it into a display
    // sketch there is nothing left that would set this — and selected_loop_entities(), which is
    // what Extrude consumes, reads exactly these two fields.
    void set_display_pick(int feature, int region) { m_display_pick = feature; m_display_pick_region = region; }

    // Visual Extrude gizmo (C5b). The Extrude tool is a DesignPanel docked card, so the
    // sketch tool is NOT active during it; the panel feeds the profile plane + a 2D centroid
    // (arrow anchor) + the live depths/flags, and the tool renders an in-canvas world-space
    // depth arrow along plane.normal with a draggable handle + editable label. TwoSided draws
    // a second arrow along -normal driven by depth2. Drag/edit fire on_extrude_depth_changed
    // back to the panel, which writes the spin value + refreshes the ghost preview.
    void set_extrude_gizmo(const SketchPlane& plane, const Vec2d& centroid,
                           double depth, double depth2, bool two_sided, bool flip);
    void clear_extrude_gizmo();
    // (new_depth, second_side): second_side=false drives the primary depth, true the 2nd side.
    std::function<void(double depth, bool second)> on_extrude_depth_changed;

    // Datum-plane resize gizmo (C3). The Plane tool is a DesignPanel docked card (sketch tool
    // NOT active), so the panel resolves the candidate plane's frame + current u/v extent and
    // feeds them here; the tool draws the rectangle outline + 4 edge-midpoint handles. Dragging a
    // handle changes the u (left/right) or v (top/bottom) extent live and fires on_datum_size_changed
    // back to the panel, which writes the Size spins + re-pushes the rendered datum.
    void set_datum_gizmo(const SketchPlane& plane, double usize, double vsize,
                         const Vec3d& base_origin, const Vec3d& base_normal,
                         double offset, bool offset_on);
    void clear_datum_gizmo();
    std::function<void(double usize, double vsize)> on_datum_size_changed;
    std::function<void(double offset)>              on_datum_offset_changed;

    // Visual Helix gizmo. The Helix tool is a DesignPanel docked card (sketch tool NOT active),
    // so the panel resolves the axis plane and feeds the live parameters here; the tool draws
    // the helix curve itself plus three handles — radius on the base circle, height at the top
    // of the axis, pitch at the end of the first turn. Taper and handedness stay on the card:
    // one is a shape modifier and the other is a flag, and L2 governs numbers you can point at.
    void set_helix_gizmo(const SketchPlane& plane, double radius, double pitch, double height,
                         double taper, bool left_handed);
    void clear_helix_gizmo();
    std::function<void(double radius, double pitch, double height)> on_helix_changed;

    // Visual Rib thickness gizmo. The rib is a thin slab grown either side of an open sketch
    // line, so its thickness is an IN-PLANE offset perpendicular to that line — the depth arrow
    // (which points along the plane normal) cannot express it. Two handles, one per side,
    // dragged symmetrically: the slab is centred on the line, so a drag on either side sets the
    // full thickness rather than one half.
    void set_rib_gizmo(const SketchPlane& plane, const Vec2d& p0, const Vec2d& p1, double thickness);
    void clear_rib_gizmo();
    std::function<void(double thickness)> on_rib_thickness_changed;

    // Graphical base/origin pick: while the Plane card is open, the candidate base planes
    // (XY/XZ/YZ origin planes + existing datums) draw as translucent clickable ghosts. A click
    // on one fires on_datum_base_picked(base) with that plane's base index (0/1/2 or 3+N).
    void set_base_pick(std::vector<SketchPlane> planes, std::vector<int> bases,
                       std::vector<std::string> labels = {});
    void clear_base_pick();
    std::function<void(int base)> on_datum_base_picked;

    // Visual Fillet/Chamfer gizmo. The Dressup tool is a DesignPanel docked card, so the sketch
    // tool is NOT active during it; when a solid EDGE is picked the panel passes the body centroid
    // + current radius and the tool anchors a world-space radius arrow at the picked edge midpoint
    // (from m_sel_edge_pts), perpendicular to the edge, pointing outward (away from the centroid).
    // Dragging the arrow changes the radius live; a stationary click opens the inline editor; both
    // fire on_fillet_radius_changed back to the panel, which writes the spin + refreshes the ghost.
    // Returns true if it could anchor (needs a picked edge with >=2 sample points).
    bool set_fillet_gizmo(const Vec3d& body_centroid, double radius);
    void clear_fillet_gizmo();
    bool filleting() const { return m_fl_active; }
    std::function<void(double radius)> on_fillet_radius_changed;

    // Visual Hole gizmo. Like Dressup, the Hole tool is a DesignPanel docked card, so the sketch
    // tool is NOT active during it; the panel passes the hole plane + position + diameter + depth +
    // through flag, and the tool draws an on-plane footprint circle plus a radial diameter arrow,
    // a normal-axis depth arrow (only when !through), and a draggable centre marker. Dragging the
    // centre repositions (plane u/v), the diameter arrow resizes, the depth arrow deepens — all
    // live; a stationary click on an arrow opens its inline editor. Every change fires
    // on_hole_changed back to the panel, which writes the spins + refreshes the ghost.
    void set_hole_gizmo(const SketchPlane& plane, double x, double y,
                        double diameter, double depth, bool through);
    // Provide the face (u,v) bounds so the hole's construction dims read from the face sides.
    void set_hole_face_bounds(bool has, double umin, double umax, double vmin, double vmax);
    void clear_hole_gizmo();
    bool holing() const { return m_hl_active; }
    std::function<void(double x, double y, double diameter, double depth)> on_hole_changed;

    // Visual Thread gizmo. Same docked-card story as Hole: the panel feeds the thread plane +
    // axis position + nominal radius + length; the tool draws an on-plane footprint circle plus a
    // radial radius arrow and a normal-axis length arrow (always shown — a thread has no "through")
    // and a draggable centre. Pitch/depth/internal stay in the card. Drag is live; a stationary
    // click on an arrow opens its inline editor; every change fires on_thread_changed.
    void set_thread_gizmo(const SketchPlane& plane, double x, double y,
                          double radius, double height);
    void clear_thread_gizmo();
    bool threading() const { return m_th_active; }
    std::function<void(double x, double y, double radius, double height)> on_thread_changed;

    // Visual Shell gizmo. The panel passes the picked open-face centroid + an inward direction
    // (-outward normal) + the current wall thickness; the tool anchors a single thickness arrow
    // there (mirrors the fillet radius arrow). Dragging sets the thickness live; a stationary
    // click opens the inline editor; both fire on_shell_thickness_changed.
    void set_shell_gizmo(const Vec3d& face_centroid, const Vec3d& inward_dir, double thickness);
    void clear_shell_gizmo();
    bool shelling() const { return m_sh_active; }
    std::function<void(double thickness)> on_shell_thickness_changed;

    // Datum/reference planes (Plane feature) carry no solid; the panel feeds their resolved
    // SketchPlanes so they render as translucent rectangles in feature mode (otherwise a
    // Plane feature is invisible in the canvas).
    void set_datum_planes(std::vector<SketchPlane> planes, std::vector<Vec2d> sizes = {}) {
        m_datum_planes = std::move(planes); m_datum_sizes = std::move(sizes);
    }

    // Mate connectors. Until now a connector was visible only to a program — resolve_datum_coordsys
    // had exactly one consumer, the MCP socket — so the frame a mate is built on could not be seen
    // at all. The glyph has to answer two questions on sight (snaporca-wgsc): which way does Z point
    // (the VERSE), and which of the pair is anchored versus driven (the POLARITY). Nothing in any
    // surveyed CAD system encodes the second one.
    struct MateConnectorGlyph {
        Vec3d origin{0, 0, 0};
        Vec3d x{1, 0, 0};          // roll reference; the filled quadrant spans x -> y
        Vec3d y{0, 1, 0};
        int   role{0};             // 0 = neutral, 1 = fixed (receives), 2 = driven (moves)
        bool  roll_undefined{false};
    };
    void set_mate_connectors(std::vector<MateConnectorGlyph> g) { m_mate_connectors = std::move(g); }
    // The pair line: the two origins of every mate — committed ones, plus the live pick of an open
    // Mate card. Two connectors a mate binds are ONE object with a gap still in it; drawn as two
    // separate frames they read as unrelated.
    void set_mate_links(std::vector<std::pair<Vec3d, Vec3d>> l) { m_mate_links = std::move(l); }
    void clear_mate_connectors() { m_mate_connectors.clear(); m_mate_links.clear(); }

    // Visual Revolve gizmo. The panel feeds the sketch plane + profile centroid + axis (0=plane X,
    // 1=plane Y) + angle + flip while its Revolve card is open; an angle-arc is drawn in the
    // revolve plane at the profile radius. Dragging the tip sweeps the angle, a stationary click
    // edits it; both fire on_revolve_angle_changed.
    void set_revolve_gizmo(const SketchPlane& plane, const Vec2d& centroid,
                           int axis_sel, double angle, bool flip);
    void clear_revolve_gizmo();
    bool revolving() const { return m_rv_active; }
    std::function<void(double angle)> on_revolve_angle_changed;

    // Visual Draft angle-arc gizmo (taper a picked face; axis = world +Z, arc in XY).
    void set_draft_gizmo(const Vec3d& face_centroid, const Vec3d& face_normal, double angle);
    void clear_draft_gizmo();
    bool drafting() const { return m_dr_active; }
    void set_on_draft_angle_changed(std::function<void(double)> cb) { m_on_draft_angle_changed = std::move(cb); }

    // Visual Cut gizmo (plane normal arrow + plane rectangle preview).
    void set_cut_gizmo(const SketchPlane& plane, double offset, const Vec3d& body_center, double half_extent);
    void clear_cut_gizmo();
    bool cutting() const { return m_ct_active; }
    void set_on_cut_offset_changed(std::function<void(double)> cb) { m_on_cut_offset_changed = std::move(cb); }

    // Visual Pattern gizmo. Linear: a 3D arrow along the world axis (plane X/Y per `dir`) of length
    // spacing*(count-1) with a tick at each copy; dragging the end sets the spacing. Circular: a
    // revolve-style angle-arc about the plane normal through the plane origin sweeping `angle`.
    // Both fire on_pattern_changed (spacing for linear, angle for circular).
    void set_pattern_gizmo(const SketchPlane& plane, const Vec3d& body_centroid, bool circular,
                           int count, int dir, double spacing, double angle);
    void clear_pattern_gizmo();
    bool patterning() const { return m_pt_active; }
    std::function<void(double value)> on_pattern_changed;

    // Constrain mode: load an already-committed profile for entity picking +
    // constraint application (the geometry is solved in the kernel, not here).
    void begin_constrain(const SketchProfile& prof, const SketchPlane& plane);
    bool is_constraining() const { return m_active && m_mode == Mode::Constrain; }
    // Replace the displayed profile (e.g. after the kernel re-solved it).
    void set_profile_points(const std::vector<Vec2d>& pts) { m_points = pts; }
    // The currently picked segment's endpoint indices into the profile.
    bool selected_segment(int& a, int& b) const;

    // Entity-aware Constrain (Fase 4.2): load a committed entity sketch and pick
    // Line entities (constraints are solved against entity endpoints in the kernel).
    void begin_constrain_entities(const std::vector<SketchEntity>& ents, const SketchPlane& plane);
    bool is_constraining_entities() const { return m_active && m_mode == Mode::Constrain && m_constrain_entities; }

    // In-canvas bounding-box transform of imported Text/SVG art (replaces the Move/Scale
    // dialog). `base_regions` are the untransformed region contours; the gizmo shows the
    // current bbox with 4 corner scale-handles + a centre move-handle. Dragging fires
    // on_imported_transform live with the new offset/scale, which the host writes back to
    // the feature. Exiting (Esc/right-click) ends the session.
    void begin_imported_transform(int feat,
                                  const std::vector<std::vector<std::vector<Vec2d>>>& base_regions,
                                  const SketchPlane& plane, const Vec2d& offset,
                                  double scale_x, double scale_y);
    std::function<void(int feat, Vec2d offset, double scale_x, double scale_y)> on_imported_transform;
    // Up to two picked line-entity indices; returns true if at least one is picked.
    bool selected_constrain_entities(int& e0, int& e1) const { e0 = m_pick0; e1 = m_pick1; return m_pick0 >= 0; }
    // Third pick slot (Symmetric axis): only filled after slots 0 and 1 are set.
    int  pick2() const { return m_pick2; }
    // Plane-coords of the click that filled slot 0 (for pick-point edit ops: trim/extend).
    bool pick0_point(Vec2d& out) const { out = m_pick0_pt; return m_pick0 >= 0; }
    // Refresh the displayed entities after the kernel re-solved them.
    void set_constrain_entities(const std::vector<SketchEntity>& ents) { m_entities = ents; }
    // Constraint manager (C3.4): entity indices the panel asks to highlight (the
    // entities a selected constraint references); rendered yellow in Constrain mode.
    void set_constraint_highlight(std::vector<int> v) { m_constraint_hl = std::move(v); }
    // The committed feature's constraints, supplied so Constrain-mode render can draw
    // an iconic glyph badge per constraint near its primary entity (C3.4b).
    void set_constraint_glyphs(std::vector<SketchEntityConstraintDef> v) { m_constrain_cons = std::move(v); }

    // Line tool: after a single segment is placed, the panel pops a length dialog
    // (length, angle_deg are the as-drawn values); it then resolves via
    // apply_segment_length() (exact length) or keep_segment_as_drawn() (cancel).
    std::function<void(double length, double angle_deg)> on_segment_drawn;
    void apply_segment_length(double len);  // rescale the pending segment, then commit it
    void keep_segment_as_drawn();           // commit the pending segment unchanged

    // Live readout while drawing a Line/Polyline segment (anchor->cursor metrics).
    std::function<void(double length, double angle_deg, bool locked)> on_cursor_metrics;

    // Live step guidance (snaporca-1c0c). The armed tool reports WHICH STEP of its gesture the
    // user is on, every time that changes, so the status line can name the next click instead of
    // repeating the one-shot sentence written when the tool was armed. step = anchors/picks
    // already down (Mirror: 0 = no axis, 1 = axis down, 2 = ready to apply); picks = size of the
    // set the gesture accumulates (mirror targets, transform targets, Select's selection).
    std::function<void(Mode mode, int step, int picks)> on_step_changed;
    // Fired when the live constraint SET changes (one added or removed), never on a re-solve.
    // The panel rebuilds its constraint rows from this; binding it to on_solve_state instead
    // would rebuild the whole list on every frame of a drag.
    std::function<void()> on_constraints_changed;

    // DoF feedback (P3): solver state after each live solve. dof>0 = under-constrained,
    // dof==0 = fully constrained, ok==false = conflicting/inconsistent constraints.
    // has_constraints is false while the sketch carries no driving constraints yet.
    std::function<void(int dof, bool ok, bool has_constraints)> on_solve_state;

    // Selection (Mode::Select): pick points/lines/arcs/circles of the in-session
    // sketch; Shift/Ctrl extends, double-click grabs the whole connected loop.
    const std::vector<int>& selection() const { return m_selection; }
    // Entities OR bare points: clear_selection() drops both, so "is anything picked" must ask
    // about both, or Esc at idle would report nothing to do while a point sat highlighted.
    bool sketch_has_selection() const { return !m_selection.empty() || !m_point_sel.empty(); }
    // Type of the first selected entity. False when nothing is selected, so the offer menu can
    // tell a line from an arc from a point and stop collapsing every sketch selection to "none".
    bool first_selected_type(SketchEntity::Type& out) const {
        if (m_selection.empty()) return false;
        const int i = m_selection.front();
        if (i < 0 || i >= int(m_entities.size())) return false;
        out = m_entities[i].type;
        return true;
    }
    // Right-click pick: select the entity (or point handle) under the given canvas pixel, so
    // the context menu describes what was pointed at. No-op when it is already selected, or
    // when nothing is there. Returns true if the selection changed.
    bool select_at_screen(GLCanvas3D& canvas, int sx, int sy);
    // Open the in-canvas value field on the SELECTION's defining number (a line's length, an
    // arc's radius, a circle's diameter, the angle between two lines, a point-to-point or
    // point-to-line distance). False when the selection has no such number.
    bool open_selection_dimension_editor();
    // ---- Scripted surface (MCP) -------------------------------------------------------
    // The same operations the right-click offers, reachable without a mouse gesture, so the 2D
    // layer can be driven and asserted headlessly. Everything here goes through the SAME code a
    // gesture goes through — append + infer_auto_constraints + live solve — because a test that
    // exercises a private shortcut proves nothing about the tool the user drives.
    const std::vector<SketchEntity>&              entities()    const { return m_entities; }
    const SketchPlane&                            plane()       const { return m_plane; }
    int  dof()      const { return m_dof; }
    bool solve_ok() const { return m_solve_ok; }
    // Append entities exactly as a finished gesture does. Returns the index of the first one.
    int  add_entities_scripted(const std::vector<SketchEntity>& ents);
    // Replace the selection with these entity indices (out-of-range ones are ignored).
    bool select_indices(const std::vector<int>& idx);
    // Append candidates, live-solve, and roll back the batch if it turns the system
    // inconsistent. Returns true when the batch was kept. Public so the panel's live-constraint
    // path can commit a plan through the SAME append→solve→keep-or-rollback the gestures use.
    bool try_add_constraints(const std::vector<SketchEntityConstraintDef>& cands);
    // Click-to-delete on a constraint badge: drop the constraint whose glyph sits under `p`
    // and re-solve. Returns true if one was removed (the caller then repaints).
    bool remove_constraint_near(const Vec2d& p);
    // Drop constraint `idx` from the live session and re-solve. Same operation the badge click
    // performs, addressed by index instead of by position — the panel list needs the index form.
    bool remove_constraint(int idx);

    // The loop report: what is CLOSED, what its internal voids are, and where a chain is still
    // open. This is the answer to "is my profile buildable", and it is the one question the
    // sketch layer could never be asked from outside.
    struct LoopInfo {
        std::vector<int>   ents;         // entities of this loop, in chain order
        std::vector<int>   holes;        // indices into LoopReport::loops that this loop encloses
        bool               closed{false};
        double             area{0.0};    // signed shoelace area of the loop polyline
    };
    struct LoopReport {
        std::vector<LoopInfo> loops;
        std::vector<Vec2d>    open_ends;   // free endpoints: where a chain fails to close
    };
    LoopReport loop_report() const;

    // FreeCAD's ValidateSketch, as one call: weld endpoints that are within `tol` of each other
    // and RECORD the Coincident constraints, so a loop that was closed by floating-point luck
    // becomes closed by construction and survives every later solve. Returns how many pairs were
    // welded. Construction geometry is skipped when `ignore_construction`.
    int heal_coincidences(double tol, bool ignore_construction);

    void clear_selection();
    // Flip the selection between construction and real geometry (whole Feature groups).
    // Returns how many entities changed; 0 when nothing is selected.
    int  toggle_selection_construction();
    void delete_selected();                         // erase selected entities
    // Abort any pending/queued draw-then-edit value-field sequence. Removing an entity that
    // still has a deferred auto-edit would otherwise open a field on a now-deleted entity and
    // freeze the flow (its live quote label also lingers). Mirrors set_tool's resync.
    void reset_autoedit() {
        m_awaiting_length  = false;
        m_autoedit_pending = false;
        m_autoedit_dims.clear();
        m_autoedit_dim_idx = -1;
        m_autoedit_seen    = int(m_entities.size());
        m_live_quotes.clear();   // rebuilt from current geometry on the next render
    }
    // Take down the session's floating chrome: the open value field (dismiss = keep-as-drawn),
    // the queue of fields behind it, and the corner readout. All three are top-level windows fed
    // only while the tool is live, so nothing else would ever clear them — reset_autoedit() alone
    // clears the flag and leaves the frame on screen. Called by finish()/cancel(); safe when
    // nothing is open.
    void close_session_chrome() {
        if (on_inline_dismiss) on_inline_dismiss();   // no-op when no field is open
        reset_autoedit();
        if (on_readout) on_readout(std::string());    // the HUD is not redrawn once the tool stops
    }
    // Ctrl+Z while sketching: drop the last drawn entity (reuses delete_selected's remap).
    bool undo_last_entity() {
        if (!m_active || m_entities.empty()) return false;
        m_selection.assign(1, int(m_entities.size()) - 1);
        delete_selected();
        reset_autoedit();
        return true;
    }
    // Delete while sketching: the selected entities, or the last drawn one if none is selected.
    bool delete_selected_or_last() {
        if (!m_active) return false;
        if (m_selection.empty()) {
            if (m_entities.empty()) return false;
            m_selection.assign(1, int(m_entities.size()) - 1);
        }
        delete_selected();
        reset_autoedit();
        return true;
    }
    std::function<void(int count)> on_selection_changed;

    // Dimension tool: infer a driving dimension from the current selection and set
    // it exactly. Sizing: 1 line=Length, 1 circle=Diameter, 1 arc=Radius,
    // 2 lines=Angle. Positioning (a value of 0 makes them coincident):
    // 2 point-likes (point/circle-centre/arc-centre)=Distance, moving the 2nd onto
    // the 1st; a point-like + a line=DistanceToLine, moving the point-like's
    // reference point onto/away-from the line (e.g. a circle centre onto an axis).
    enum class DimType { None, Length, Diameter, Radius, Angle, Distance, DistanceToLine };
    DimType dimension_kind() const;     // what the selection supports (None if invalid)
    double  dimension_current() const;  // current value, to pre-fill the dialog
    void    apply_dimension(double v);  // set it exactly, then clear the selection

    // Onshape-style Dimension tool (Mode::Dimension): with the tool active you click
    // directly in the viewport — 2 points -> Distance, a line -> Length, a circle ->
    // Diameter, an arc -> Radius, a point then a line -> DistanceToLine. A quote line
    // with extension lines, arrowheads and a numeric label is PLACED in the sketch and
    // drives the geometry (auto-offset; label editable). on_dimension_pick_complete
    // fires when a pick resolves so the panel can pop the value card pre-filled.
    std::function<void(double current)> on_dimension_pick_complete;
    DimType pending_dimension_type() const;  // type of the dim awaiting a value, or None
    void    set_dimension_value(double v);   // apply the typed value to the placed dim
    void    cancel_dimension_value();        // keep the placed dim at its measured value

    // Onshape-style in-canvas value editing: open a floating text editor at the given
    // screen pixel, pre-filled with `current`; commit applies the value, cancel keeps
    // it. The owner (DesignCanvas) hosts the wxTextCtrl over the GL canvas. This is the
    // single numeric-entry path for all sketch dimensions (replaces the modal cards).
    std::function<void(wxPoint screen_px, double current, const std::string& title,
                        std::function<void(double)> commit,
                        std::function<void()> cancel)> on_inline_edit;
    // Force-close any open inline field (runs its cancel = keep-as-drawn). Used by the polyline
    // terminators (right-click / double-click) to end the chain even mid per-segment edit.
    std::function<void()> on_inline_dismiss;
    // Accept and close an open inline value field. dismiss() CANCELS; this one keeps the value,
    // which is what leaving a tool should do — see set_tool().
    std::function<void()> on_inline_commit;

    // Bottom-right viewport readout: emitted each frame with the active tool's current
    // values (live segment length/angle while drawing a line, or the selected entity's
    // characteristic dimensions). Empty string -> hide the HUD. The owner (DesignCanvas)
    // shows it as a floating corner label over the GL canvas.
    std::function<void(const std::string&)> on_readout;

    // Driving dimension constraints accumulated during the session (the Dimension
    // tool records a SketchEntityConstraintDef per applied dimension); committed
    // alongside the entities on finish() so the kernel keeps enforcing them.
    const std::vector<SketchEntityConstraintDef>& constraints() const { return m_constraints; }

    // Emitted by finish() with the accumulated entities + driving constraints.
    std::function<void(const std::vector<SketchEntity>&,
                       const std::vector<SketchEntityConstraintDef>&,
                       const SketchPlane&)> on_commit_entities;
    // Legacy single-profile commit (kept for compatibility; unused by entity tools).
    std::function<void(const SketchProfile&, const SketchPlane&)> on_commit;
    // Emitted when a closed-loop face is clicked in Select mode (Onshape: a region
    // becomes a selectable face → extrude). The panel commits the sketch + extrudes.
    std::function<void(int)> on_face_selected;   // region index into region_loops(m_entities)
    // Esc pressed while the tool is active with nothing left to unwind and no geometry to
    // lose: leave the session (the panel restores Feature mode).
    std::function<void()> on_exit;
    // request_exit() declined to leave because the session holds geometry. The panel owns the
    // status line, so the tool reports through this instead of writing text itself.
    std::function<void()> on_exit_refused;
    std::function<void()> on_move_exit;   // right-click finished the move-body gizmo
    // The two inner Esc levels, callable on their own so the panel can route one press to one
    // level (see DesignInteraction.hpp). Each returns whether it had anything to unwind.
    bool abort_gesture();   // CadLevel::Gesture — drop the entity being drawn
    bool disarm_tool();     // CadLevel::Tool    — armed draw/edit tool falls back to Select
    void request_exit();
    // Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y) while the Design canvas is focused: undo/redo the
    // committed feature history. The tool just forwards to the host, which owns the
    // CadDocument (the tool has no document of its own). redo == true requests redo.
    std::function<void(bool /*redo*/)> on_undo_redo;
    void request_undo_redo(bool redo);

private:
    bool screen_to_plane(GLCanvas3D& canvas, const wxMouseEvent& evt, Vec2d& out) const;
    // True when the cursor is close enough to the open chain's FIRST point to close the loop,
    // on the same screen tolerance as every other snap; snaps `p` exactly onto that point so
    // the rubber band previews the closing segment and the snap marker lights.
    bool snap_chain_start(GLCanvas3D& canvas, const wxMouseEvent& evt, Vec2d& p) const;

    // Onshape-style angle inference: snap the direction anchor->raw to the nearest
    // of {0,30,45,60,90} deg (replicated every 90 deg) when within tolerance, keeping
    // the same length. Sets `locked` when a snap was applied. Suppressed by m_snap_off.
    Vec2d snap_dir(const Vec2d& anchor, const Vec2d& raw, bool& locked) const;
    // Snap a placed point onto the nearest existing entity endpoint within ~8 px so
    // chains join across entities (a line + an arc can close into one loop). Shift
    // disables it. `snapped` reports whether a vertex was hit.
    Vec2d snap_vertex(GLCanvas3D& canvas, const wxMouseEvent& evt, const Vec2d& raw, bool& snapped) const;

    // --- P1 inference / auto-constraint engine ---------------------------------
    // Plane-units tolerance equivalent to ~`px` screen pixels at the cursor.
    double screen_tol(GLCanvas3D& canvas, const wxMouseEvent& evt, const Vec2d& at, double px = 8.0) const;
    // Run kernel inference at the cursor, cache the target for the hint renderer.
    InferenceSnap infer_at(GLCanvas3D& canvas, const wxMouseEvent& evt, const Vec2d& raw) const;
    // True if m_constraints already holds an equivalent Coincident between the two refs.
    bool has_coincident(int ea, SketchPointRole ra, int eb, SketchPointRole rb) const;
    // After entities [base, end) were committed, auto-emit the constraints that make
    // the new geometry stick: Coincident between co-located endpoints (so loops close
    // on their own) and Horizontal/Vertical on axis-aligned new segments.
    // ang_tol_rad is how far from an axis a segment may be and still be CALLED axis-aligned.
    // A gesture needs the default 3 degrees — nobody clicks a horizontal line exactly — but
    // that same slack MOVES geometry that was given exactly, so the scripted path passes a
    // tolerance tight enough to recognise only what is already true. See add_entities_scripted.
    // ang_tol_rad: how far off axis a segment may be and still be called Horizontal/Vertical.
    // weld_tol:    how far apart two endpoints may be and still be called Coincident.
    // Both default to GESTURE slack. A scripted add passes zero for both: the caller has
    // already said exactly what it means, and every non-zero window is a window in which the
    // inference rewrites it. snaporca-8xg1.
    void infer_auto_constraints(int base, double ang_tol_rad = 3.0 * M_PI / 180.0,
                                double weld_tol = 1e-3);

    // Selection helpers (Mode::Select).
    int hit_test(const Vec2d& p, double tol) const;       // nearest entity within tol, or -1
    std::vector<int> connected_loop(int seed) const;      // entities joined by shared endpoints
    void apply_angle_between(int ia, int ib, double deg); // rotate line B to set the A^B angle
    bool selection_valid() const;                         // all selection indices in range
    void record_dimension_constraint(double v);           // append the driving def for the selection
    void resolve_live();                                  // solve accumulated constraints on m_entities now
    // Drag-aware re-solve: pins the dragged point at its current coord and lets the
    // solver move the rest (Slvs dragged[]). Used live while a point grab is active.
    void resolve_live_drag(int dragged_ei, SketchPointRole dragged_role);

    // Placed dimension annotation. References entity points/entities (not cached
    // coords) so the quote follows the geometry as the kernel solves it. `value`
    // drives the constraint stored at index `con` in m_constraints.
    struct DimAnnot {
        DimType         kind{DimType::None};
        int             ea{-1}; SketchPointRole ra{SketchPointRole::P0};
        int             eb{-1}; SketchPointRole rb{SketchPointRole::P0};
        double          value{0.0};
        double          side{1.0};   // perpendicular offset sign of the quote line
        int             con{-1};     // slot in m_constraints driving this dimension
        Vec2d           label_pos{0, 0};  // cached label centre (plane coords), for picking
    };

    // --- Onshape-style visual editing: handles + parametric feature grouping -----
    // A draggable handle on a defining point of an entity (or a derived point of a
    // feature group). GUI-only; recomputed from solved geometry every frame (never
    // persisted), so handles always track the current solve. Derived roles (radius,
    // slot width/centres, rect corners, polygon vertex, ellipse axes) let tools that
    // decompose into raw Line/Arc entities still expose their parametric controls.
    enum class HandleRole { P0, P1, Center, RadiusHandle,
                            SlotCenter0, SlotCenter1, SlotWidth,
                            RectCorner, PolygonVertex, MajorAxis, MinorAxis, BSplineCtrl };
    struct Handle {
        HandleRole role{HandleRole::P0};
        int   ei{-1};          // primary entity index
        int   group{-1};       // index into m_features, or -1 for a raw-entity handle
        int   ctrl_index{-1};  // BSplineCtrl pole index
        Vec2d pos{0, 0};       // current plane coords (recomputed each frame)
        bool  hovered{false};
    };
    // A parametric grouping over a contiguous run of entities produced by one gesture.
    // Slot/Rect/Polygon/etc. have no SketchEntity type of their own — they decompose
    // into raw Line/Arc entities — so the Feature carries the gesture's anchors so
    // derived handles + characteristic dimensions can be reconstructed.
    enum class FeatureKind { Free, Line, Circle, Arc, CornerRect, CenterRect,
                             Slot, ArcSlot, Polygon, Ellipse, RoundedRect, BSpline };
    struct Feature {
        FeatureKind kind{FeatureKind::Free};
        int    begin{0}, end{0};   // [begin,end) into m_entities
        Vec2d  c0{0, 0}, c1{0, 0}; // slot centres / rect corners / ellipse centre+major
        double param{0.0};         // slot half-width / polygon circumradius / fillet radius
        int    sides{0};           // polygon side count
    };
    // Build the live handle set for the current selection / just-drawn feature.
    std::vector<Handle> build_handles() const;
    // Nearest handle to plane-point p within tol; fills `out`. (Phase A: stub.)
    bool hit_test_handle(const Vec2d& p, double tol, Handle& out) const;
    // Move a handle to `target`, applying the role-specific geometry edit + re-solve.
    void set_handle(const Handle& h, const Vec2d& target);
    // On a no-button move, recompute the hovered handle; returns true iff it changed
    // (so the caller forces exactly one repaint). No-op for non-Moving events.
    bool update_hover(GLCanvas3D& canvas, wxMouseEvent& evt);
    // Index of the Feature whose [begin,end) entity span contains ei, or -1.
    int  feature_of(int ei) const;
    // Re-detect parametric Feature groups (polygon / rect / slot) from the raw entity
    // list — used when a committed sketch is re-opened, where m_features is empty.
    void rebuild_features_from_entities();
    // Open/close a Feature record around the entities a single gesture appends.
    void begin_feature(FeatureKind kind);
    void end_feature(const Vec2d& c0 = Vec2d(0, 0), const Vec2d& c1 = Vec2d(0, 0),
                     double param = 0.0, int sides = 0);

    bool point_at(int ei, SketchPointRole role, Vec2d& out) const;          // current coords
    void set_point(int ei, SketchPointRole role, const Vec2d& v);           // move an entity point
    bool hit_test_point(const Vec2d& p, double tol, int& ei, SketchPointRole& role) const;
    int  hit_test_dimension(const Vec2d& p, double tol) const;              // nearest dim label
    void edit_dimension(int di);                                            // reopen value card for di
    // Representative plane-coords anchor of a dimension (label centre if known, else a
    // geometric midpoint/centre) — where the in-canvas value editor is positioned.
    Vec2d dim_anchor(const DimAnnot& a) const;
    // Open the in-canvas value editor on dimension `di` (falls back to the modal
    // pick-complete callback when no inline-edit host is wired).
    void open_value_editor(int di);
    // In-canvas editor for a line's angle-to-horizontal; commit rotates the segment
    // geometrically about P0 (no single-line angle constraint in libslvs).
    void open_angle_editor(int ei);
    void set_line_angle(int ei, double deg);
    // Draw-then-edit (all creation tools): open the inline editor on the freshly-drawn
    // selection's PRIMARY characteristic value. Called after render_live_quotes has computed
    // the selection's quotes, so it dispatches on the same live-quote state a Select-mode
    // click would use.
    void open_primary_autoedit();
    // Compact "current values" string for the bottom-right HUD (see on_readout).
    std::string build_readout() const;
    // Open a characteristic live quote as a TENTATIVE driving dimension: the constraint is
    // appended only if the user commits a value (Enter); cancel (Esc) adds nothing — so
    // drawing never silently over-constrains. (place_dimension is the eager Select-mode twin.)
    void open_next_autoedit_dim();   // opens m_autoedit_dims[idx]; commit -> next, Esc -> stop
    void arm_polyline_segment_edit();// per-segment Length+Angle edit of the pending chain vertex
    // In-canvas editors for a regular polygon's side length and orientation. Both edit
    // the whole loop GEOMETRICALLY (polygon has no centre entity): side scales it
    // uniformly about its centre, angle rotates it. set_polygon_radius is the shared
    // uniform-scale primitive (circumradius).
    void open_polygon_side_editor(int fi);
    void open_polygon_angle_editor(int fi);
    void set_polygon_side(int fi, double side);
    void set_polygon_angle(int fi, double deg);
    void set_polygon_radius(int fi, double R);
    // Arc sweep-angle quote: geometric edit (SLVS angle is line-to-line only). Keeps the
    // arc start point + radius fixed and moves the end point to span `deg` degrees.
    void open_arc_angle_editor(int ei);
    void set_arc_sweep(int ei, double deg);
    // Arc handle drag (3 grips): Center rigidly translates; the START point changes the
    // radius (keeps both sweep angles); the END point changes the sweep angle (keeps the
    // radius). Geometric — no solver (SLVS has no arc radius/angle handle concept here).
    void drag_arc_handle(int ei, SketchPointRole role, const Vec2d& target);
    // Ellipse axis labels (geometric edit of the semi-axes a/b; phi via the major grip).
    void open_ellipse_axis_editor(int ei, bool major);
    void set_ellipse_axis(int ei, bool major, double v);
    void set_ellipsearc_sweep(int ei, double deg);   // draw-then-edit: included sweep of an elliptical arc
    void set_rect_angle(int fi, double deg);         // draw-then-edit: orientation of an oblique rect
    // EllipseArc endpoint drag: Center translates; P0/P1 move the sweep start/end to the
    // parametric angle of the cursor on the ellipse frame (radius/shape preserved).
    void drag_ellipsearc_handle(int ei, SketchPointRole role, const Vec2d& target);
    // Drop orientation constraints (H/V/Parallel/Perp/Angle/LockX/LockY) on entities in
    // [begin,end). A ROTATION makes inferred per-edge H/V inconsistent, so re-solving
    // against them collapses the shape — drop them first (fixes up DimAnnot.con indices).
    void drop_orientation_constraints(int begin, int end);
    // Drop every live constraint that references entity `ei` (Trim/Extend slide an endpoint,
    // invalidating its constraints) and fix the dimensions' cached constraint indices.
    void drop_constraints_referencing(int ei);
    // Standalone Trim/Extend scissors on the LIVE sketch: pick the entity nearest `p` (within
    // `tol` plane units) and cut it back to / out to its nearest intersection with the others.
    // Returns true if an entity was modified.
    bool apply_live_trim(const Vec2d& p, double tol, bool extend);
    // Pure-computation hover preview for Trim/Extend: mirror apply_live_trim's pick + the
    // engine's cut on a COPY (mutating nothing) and return, via `removed_poly`, the polyline
    // of the sub-portion a click would REMOVE (Trim) or ADD (Extend). `subject_ei` is the
    // picked entity. Returns false if nothing is in range or nothing would change.
    bool compute_trim_preview(const Vec2d& p, double tol, bool extend,
                              int& subject_ei, std::vector<Vec2d>& removed_poly) const;
    // Drag a polygon vertex while keeping the loop REGULAR: scale + rotate the whole
    // polygon about its centroid so the grabbed vertex follows `target` (adjusts
    // circumradius + orientation together).
    void drag_polygon_vertex(int fi, int ei, SketchPointRole role, const Vec2d& target);
    double measure_dim(const DimAnnot& a) const;                            // value from geometry
    std::string dimtype_title(DimType k) const;
    SketchEntityConstraintDef constraint_for(const DimAnnot& a) const;      // driving def
    // One driving constraint (and one visible quote) per kind+operands: re-typing a value must
    // UPDATE it, not append a rival asking for something else. Both return the index.
    int upsert_constraint(const SketchEntityConstraintDef& c);
    int upsert_dimension(const DimAnnot& a);
    int  place_dimension(DimAnnot a);                                       // create+drive+notify
    std::string dim_text(const DimAnnot& a) const;                          // rendered label string
    void render_dimensions(double unit_per_px);                            // quote lines + labels
    // Draw ONE dimension's quote (extension/dimension lines, arrowheads, label) and
    // return its label centre in out_label; false if the annot can't be drawn. Shared
    // by render_dimensions (placed driving quotes) and render_live_quotes (live ones).
    bool draw_dim_quote(const DimAnnot& a, double th, const ColorRGBA& col, Vec2d& out_label);
    // Live, non-driving characteristic quotes for the entity being edited (point/handle
    // drag, or a lone selection): the tool's defining dimensions shown Onshape-style so
    // editing shows live values; click one (m_live_quotes) to promote it to a driving
    // dim. Self-gates; skips a dim already driven on that entity.
    void render_live_quotes(double unit_per_px);
    // Iconic constraint badges (C3.4b): for each m_constrain_cons entry, append a
    // small screen-constant glyph (H, V, ∥, ⊥, =, ○, …) near its primary entity into
    // `out`; glyphs touching the same entity stack so they don't overlap.
    void build_constraint_glyphs(double unit_per_px,
                                 const std::vector<SketchEntityConstraintDef>& cons,
                                 std::vector<std::pair<Vec2d, Vec2d>>& out);
    void draw_strokes(GLModel& model, const std::vector<std::pair<Vec2d, Vec2d>>& segs,
                      double hw, const ColorRGBA& color);
    void draw_text(GLModel& model, const std::string& s, const Vec2d& center,
                   double height, const ColorRGBA& color);                  // GL stroke font
    void draw_dim_label(const std::string& txt, const Vec2d& plane_center);

    // Entity builders: append to m_entities (honoring the construction flag).
    void push_line(const Vec2d& a, const Vec2d& b);
    void push_closed_lines(const std::vector<Vec2d>& corners);
    void push_open_chain(const std::vector<Vec2d>& pts);
    void push_circle(const Vec2d& center, double radius);
    void push_point(const Vec2d& p);

    // Multi-click tool builders: return the entities for a finished gesture so
    // both on_mouse (append) and render (preview) share one geometry path.
    std::vector<SketchEntity> make_three_point_circle(const Vec2d& a, const Vec2d& b, const Vec2d& c) const;
    std::vector<SketchEntity> make_three_point_arc(const Vec2d& start, const Vec2d& end, const Vec2d& on_arc) const;
    std::vector<SketchEntity> make_tangent_arc(const Vec2d& start, const Vec2d& end) const;
    // Center-start-end arc: click center, then start (sets radius), then a third
    // point whose direction from the center sets the CCW end angle.
    std::vector<SketchEntity> make_center_arc(const Vec2d& center, const Vec2d& start, const Vec2d& end_dir) const;
    std::vector<SketchEntity> make_slot(const Vec2d& c0, const Vec2d& c1, double half_width) const;
    std::vector<SketchEntity> make_arc_slot(const Vec2d& center, const Vec2d& start,
                                            const Vec2d& end_dir, double half_width) const;
    std::vector<SketchEntity> make_rounded_rect(const Vec2d& a, const Vec2d& b, const Vec2d& radius_pt) const;
    std::vector<SketchEntity> rounded_rect_entities(double xmin, double ymin,
                                                    double xmax, double ymax, double r) const;
    // Rounded-rect grouped edit: W/H/fillet-R labels rebuild the 8-entity span in place.
    void open_rounded_rect_editor(int fi, int which);   // 0=Width 1=Height 2=fillet R
    void set_rounded_rect(int fi, double w, double h, double r);
    // Arc-slot grouped edit: centreline-radius + width labels rebuild the 4-arc span.
    void open_arc_slot_editor(int fi, bool radius);     // true=centreline R, false=width
    void set_arc_slot(int fi, double Rc, double w);
    // Straight-slot grouped edit: centreline-length + width labels rebuild the 4-entity span.
    void open_slot_editor(int fi, int which);           // 0=inter-centre distance, 1=radius, 2=angle
    void set_slot(int fi, double length, double w);
    void set_slot_angle(int fi, double deg);            // rotate the centreline about c0, keep len+radius
    // Grouped derived-handle drag: resize an axis-aligned rect by a corner (opposite corner
    // fixed); move a slot end by its cap centre. Both rebuild the feature span geometrically.
    void drag_rect_corner(int fi, const Vec2d& cursor);
    void drag_slot_handle(int fi, const Vec2d& cursor);
    std::vector<SketchEntity> make_polygon(const Vec2d& center, const Vec2d& vertex, int sides) const;
    // Ellipse: click center, then major-axis endpoint (sets a + rotation phi),
    // then a point whose perpendicular distance to the major axis sets b.
    std::vector<SketchEntity> make_ellipse(const Vec2d& center, const Vec2d& major_end,
                                           const Vec2d& minor_pt) const;
    // Elliptical arc: same 3 axis clicks, then start and end points whose parametric
    // angles on the ellipse bound the CCW sweep.
    std::vector<SketchEntity> make_bspline(const std::vector<Vec2d>& ctrl) const;
    std::vector<SketchEntity> make_ellipse_arc(const Vec2d& center, const Vec2d& major_end,
                                               const Vec2d& minor_pt, const Vec2d& start_pt,
                                               const Vec2d& end_pt) const;
    void append_entities(const std::vector<SketchEntity>& ents);
    void draw_entities_preview(const std::vector<SketchEntity>& ents, const ColorRGBA& color);

    // --- In-canvas edit-op gizmo (Fillet/Chamfer/Offset/Mirror toolbar tools) --------
    // These replace the docked numeric card: pick the entities in-canvas, then a draggable
    // arrow with a value label is projected toward the corner/centre (Fillet/Chamfer/Offset),
    // or a two-phase pick (axis line, then targets) drives a live mirrored ghost. The
    // SketchEngine op is recomputed live so a translucent ghost previews the result; confirm
    // applies the geometry and binds constraints into m_constraints (try_add_constraints).
    bool op_corner(int a, int b, Vec2d& C, Vec2d& bis, double& theta) const; // line-line vertex + inward bisector
    void op_pick(int ei);                       // route an entity pick to the active op
    void recompute_op_ghost();                  // rebuild m_op_ghost from m_op_value
    void render_op_gizmo(double unit_per_px);   // ghost + arrow + value label (caches m_op_label)
    bool hit_test_op_arrow(const Vec2d& p, double tol) const;
    void drag_op_arrow(const Vec2d& target);    // project cursor onto m_op_dir -> value
    void open_op_editor();                      // inline-edit the value label
    void confirm_op();                          // apply + bind, then reset for the next gesture
    void reset_op();                            // clear gizmo state (keeps the tool active)
    bool op_ready() const;                      // required entities picked -> arrow/ghost live

    // Sample an entity into a 2D polyline for the overlay renderer.
    std::vector<Vec2d> entity_polyline(const SketchEntity& e, bool& closed) const;

    // Closed regions formed by the current (non-construction) entities: each a CCW-
    // ordered boundary polygon on the plane. A circle is its own region; line/arc
    // chains are walked endpoint-to-endpoint into loops. Used to fill faces.
    std::vector<std::vector<Vec2d>> closed_regions() const;
    std::vector<std::vector<Vec2d>> closed_regions(const std::vector<SketchEntity>& ents) const;
    // Same loops, but each carries the indices of the entities that form it — so a single
    // loop can be highlighted / extruded on its own (per-region selection on the plate).
    // A selectable sketch region: its own boundary, plus the loops nested INSIDE it, which
    // are its holes. Modelling holes is what makes "the plate with the hole in it" a thing the
    // user can point at — without it a sketch is N disjoint filled polygons and the only
    // selectable things are the rectangle alone or the circle alone (snaporca-txp8).
    struct RegionLoop {
        std::vector<Vec2d> poly;
        std::vector<int>   ents;
        std::vector<int>   holes;   // indices into the same vector; one nesting level
    };
    std::vector<RegionLoop> region_loops(const std::vector<SketchEntity>& ents) const;
    // Index of the closed region containing plane-point p (point-in-polygon), or -1.
    int region_at(const Vec2d& p) const;

    void draw_quad_strip(GLModel& model, const std::vector<Vec2d>& pts, bool closed, const ColorRGBA& color);
    // half_size is the square marker half-extent in PLANE units. Callers pass a
    // zoom-scaled value (k / zoom) for screen-constant handles; the default keeps
    // legacy point markers exactly as before.
    void draw_vertices(GLModel& model, const std::vector<Vec2d>& pts, const ColorRGBA& color,
                       double half_size = 1.3);
    void draw_fill(GLModel& model, const std::vector<Vec2d>& poly, const ColorRGBA& color);
    // Same, with the region's holes cut out, so a selected plate-with-a-hole is drawn as an
    // ANNULUS instead of a filled rectangle painted straight across its own bore.
    void draw_fill_holed(GLModel& model, const std::vector<Vec2d>& outer,
                         const std::vector<std::vector<Vec2d>>& holes, const ColorRGBA& color);
    const ColorRGBA* sketch_hl_color(int feature) const;

    bool                m_active{false};
    SketchPlane         m_plane;
    std::vector<Vec2d>  m_points;       // clicks of the in-progress entity / chain
    std::vector<SketchEntity> m_entities; // committed entities of this session
    bool                m_construction{false};
    int                 m_polygon_sides{6};
    bool                m_polygon_circumscribed{false};
    Vec2d               m_cursor{0,0};
    bool                m_has_cursor{false};
    bool                m_snap_off{false};      // Shift held -> suppress angle snapping
    InferenceSnap       m_cursor_snap;          // last cursor inference target (for hint render)
    bool                m_cursor_locked{false}; // rubber-band segment is angle-locked
    bool                m_awaiting_length{false}; // inline value editor open -> freeze canvas
    int                 m_autoedit_seen{-1};      // entity count baseline for draw-then-edit
    bool                m_autoedit_pending{false};// a new entity just committed -> open editor
    // Draw-then-edit step queue: every characteristic dimension of the freshly-drawn shape
    // (scalar quote OR geometric editor) becomes one step, opened in sequence over its label.
    struct AutoEditStep {
        Vec2d                       label;   // anchor (plane coords) — field opens over this
        double                      value;   // initial value shown
        std::function<void(double)> apply;   // commit: set the dimension
        std::vector<int>            hi;      // entities to highlight while THIS field is open
        std::string                 title;   // label shown above the value field
    };
    std::vector<AutoEditStep> m_autoedit_dims;    // queued steps to edit in sequence
    int                 m_autoedit_dim_idx{-1};   // index into m_autoedit_dims (-1 = idle)
    std::vector<int>    m_selection;              // selected entity indices (Mode::Select)
    std::vector<std::pair<int, SketchPointRole>> m_point_sel;  // selected individual points
    int                 m_last_mouse_x{0};        // last cursor pos (canvas client px), for
    int                 m_last_mouse_y{0};        // anchoring the in-canvas value editor
    bool                m_dragging_point{false};  // a point grab is in progress (Mode::Select)
    int                 m_drag_ei{-1};            // entity whose point is being dragged
    int                 m_drag_poly_fi{-1};       // >=0 if the grabbed point is a polygon
                                                  // vertex: drag scales+rotates the loop
    int                 m_drag_rect_fi{-1};       // >=0 if dragging an axis-aligned rect corner
    Vec2d               m_drag_rect_anchor{0,0};  //   the fixed (opposite) corner
    int                 m_drag_slot_fi{-1};       // >=0 if dragging a slot cap centre
    bool                m_drag_slot_c1{false};    //   true=cap@c1, false=cap@c0
    SketchPointRole     m_drag_role{SketchPointRole::P0};
    std::vector<SketchEntityConstraintDef> m_constraints; // driving dims, committed on finish

    // Onshape-style visual editing state.
    bool                  m_show_handles{false};   // draw + interact with handles
    bool                  m_dragging_handle{false};// a handle grab is in progress
    Handle                m_drag_handle;           // the handle being dragged
    bool                  m_has_hover_handle{false};// cursor is near a handle (highlight it)
    Handle                m_hover_handle;          // the hovered handle (recomputed on move)
    std::vector<DimAnnot> m_live_quotes;           // live non-driving characteristic quotes,
                                                   // clickable to promote to driving dims
    Vec2d                 m_live_poly_side_label{0,0};  // polygon side-length quote label
    Vec2d                 m_live_poly_angle_label{0,0}; // polygon orientation quote label
    int                   m_live_poly_fi{-1};           // their Feature (geometric edits)
    Vec2d                 m_live_arc_angle_label{0,0};  // arc sweep-angle quote label
    int                   m_live_arc_ei{-1};            // the arc it belongs to (geometric edit)
    Vec2d                 m_live_ellipse_major_label{0,0}; // ellipse semi-major quote label
    Vec2d                 m_live_ellipse_minor_label{0,0}; // ellipse semi-minor quote label
    Vec2d                 m_live_ellipsearc_sweep_label{0,0}; // elliptical-arc sweep quote label
    int                   m_live_ellipse_ei{-1};        // the ellipse the labels belong to
    Vec2d                 m_live_obrect_angle_label{0,0}; // oblique-rect orientation quote label
    int                   m_live_obrect_fi{-1};         // an OBLIQUE rect Feature (angle editable)
    Vec2d                 m_live_rrect_w_label{0,0};    // rounded-rect width quote label
    Vec2d                 m_live_rrect_h_label{0,0};    // rounded-rect height quote label
    Vec2d                 m_live_rrect_r_label{0,0};    // rounded-rect fillet-radius label
    int                   m_live_rrect_fi{-1};          // the rounded-rect Feature (rebuild edits)
    Vec2d                 m_live_aslot_r_label{0,0};    // arc-slot centreline-radius label
    Vec2d                 m_live_aslot_w_label{0,0};    // arc-slot width label
    int                   m_live_aslot_fi{-1};          // the arc-slot Feature (rebuild edits)
    Vec2d                 m_live_slot_len_label{0,0};   // straight-slot inter-centre distance label
    Vec2d                 m_live_slot_w_label{0,0};     // straight-slot radius (half-width) label
    Vec2d                 m_live_slot_angle_label{0,0}; // straight-slot centreline angle label
    int                   m_live_slot_fi{-1};           // the straight-slot Feature (rebuild edits)
    std::vector<Feature>  m_features;              // parametric groups over m_entities
    int                   m_open_feature{-1};      // index of the Feature being built, or -1

    // In-canvas edit-op gizmo state (Fillet/Chamfer/Offset/Mirror). GUI-only, reset by
    // set_tool/cancel. Fillet/Chamfer: m_op_a,m_op_b = the two lines; Offset: m_op_a = src;
    // Mirror: m_op_a = axis line, m_mirror_targets = entities to mirror.
    // Last (mode, step, picks) reported through on_step_changed; -1 mode = nothing reported yet.
    int    m_step_mode_last{-1};
    int    m_step_last{-1};
    int    m_step_picks_last{-1};
    int    m_op_a{-1};
    int    m_op_b{-1};
    double m_op_value{0.0};                 // radius / setback / signed offset distance
    Vec2d  m_op_anchor{0,0};                // arrow base (corner vertex / entity midpoint)
    Vec2d  m_op_dir{0,0};                   // unit arrow direction (inward bisector / outward normal)
    Vec2d  m_op_label{1e18,1e18};           // cached arrow-label centre, for picking
    std::vector<SketchEntity> m_op_ghost;   // live result preview (recomputed on value change)
    bool   m_op_dragging_arrow{false};      // arrowhead drag in progress
    std::vector<int> m_mirror_targets;      // Mirror: entities to be mirrored (axis = m_op_a)

    // In-canvas imported-art transform gizmo (Mode::TransformArt). GUI-only. The art's
    // untransformed contours + its bbox in base coords; the live offset/scale; the grabbed
    // handle (0..3 = corners, 4 = centre move, -1 = none) and the fixed world anchor (the
    // opposite corner during a corner-scale drag).
    std::vector<std::vector<std::vector<Vec2d>>> m_xform_base;
    int    m_xform_feat{-1};
    Vec2d  m_xform_min{0,0}, m_xform_max{0,0};   // bbox of m_xform_base (untransformed)
    Vec2d  m_xform_offset{0,0};
    double m_xform_sx{1.0}, m_xform_sy{1.0};
    int    m_xform_handle{-1};
    Vec2d  m_xform_anchor{0,0};
    void   xform_world_corners(Vec2d out[4]) const;   // 4 bbox corners in plane coords
    int    hit_test_xform_handle(const Vec2d& p, double tol) const;
    void   drag_xform_handle(const Vec2d& target);
    void   render_xform_gizmo();
    void   emit_xform();
    void   reset_xform();

    // In-canvas transform gizmo state (Mode::Move/Rotate/Scale/Array/PolarArray). GUI-only,
    // reset by set_tool/cancel. Pick one or more subject entities (m_tf_targets), then a
    // single draggable handle drives the continuous parameter and a live translucent ghost
    // previews the result; Array/PolarArray add a second editable label for the copy count.
    // Mutating ops (Move/Rotate/Scale) drop the constraint classes the map invalidates;
    // additive ops (Array/PolarArray) bind each copy to its source. See confirm_transform().
    std::vector<int>          m_tf_targets;       // picked subject entity indices
    Vec2d                     m_tf_pivot{0,0};    // rotate/scale/polar pivot = set centroid
    Vec2d                     m_tf_delta{0,0};    // Move translation / Array per-step vector
    double                    m_tf_angle{0.0};    // Rotate angle / PolarArray total sweep (rad)
    double                    m_tf_scale{1.0};    // Scale factor
    int                       m_tf_count{3};      // Array/PolarArray copy count (incl. original)
    double                    m_tf_handle_r{1.0}; // ring/handle reference radius (set on pick)
    std::vector<SketchEntity> m_tf_ghost;         // live result preview
    int                       m_tf_handle{-1};    // 0 = primary drag handle grabbed, -1 = none
    bool                      m_tf_dragging{false};
    Vec2d                     m_tf_label_a{1e18,1e18};  // primary-param label centre (picking)
    Vec2d                     m_tf_label_b{1e18,1e18};  // count label centre (Array/PolarArray)
    bool                      tf_ready() const;   // >=1 target picked -> gizmo + ghost live
    void                      tf_pick(int ei);    // accumulate a subject, seed defaults once
    void                      compute_tf_pivot(); // centroid + extent of the target set
    void                      recompute_tf_ghost();
    Vec2d                     tf_handle_pos() const;   // world position of the drag handle
    bool                      hit_test_tf_handle(const Vec2d& p, double tol) const;
    void                      drag_tf_handle(const Vec2d& target);
    void                      render_tf_gizmo(double unit_per_px);
    void                      open_tf_editor_a();      // inline-edit the continuous parameter
    void                      open_tf_editor_count();  // inline-edit the copy count
    void                      confirm_transform();     // apply geometry + constraint web
    void                      reset_tf();

    // DoF feedback state, refreshed by resolve_live() from the libslvs solve result.
    int               m_dof{-1};          // remaining DoF; 0 = fully constrained, <0 = unknown
    bool              m_solve_ok{true};   // solver consistent (no conflicting constraints)
    std::vector<char> m_entity_conflict;  // per-entity flag: touched by a conflicting constraint
    std::vector<DimAnnot> m_dimensions;           // placed dimension quotes (Mode::Dimension)
    int                 m_dim_e0{-1};             // first picked point's entity (Dimension)
    SketchPointRole     m_dim_r0{SketchPointRole::P0};
    bool                m_dim_has0{false};        // a first point is pending
    int                 m_pending_dim{-1};        // dim awaiting a value-card entry
    Mode                m_mode{Mode::Polyline};
    int                 m_sel_a{-1};   // picked segment endpoints (legacy Constrain mode)
    int                 m_sel_b{-1};
    bool                m_constrain_entities{false}; // Constrain mode acts on entities
    int                 m_pick0{-1};   // picked line-entity indices (entity Constrain)
    int                 m_pick1{-1};
    int                 m_pick2{-1};   // third slot (Symmetric axis)
    Vec2d               m_pick0_pt{0,0}; // plane-coords of the slot-0 pick (trim/extend)
    std::vector<int>    m_constraint_hl; // entities highlighted by the constraint manager
    std::vector<SketchEntityConstraintDef> m_constrain_cons; // for glyph badges (C3.4b)
    // Where each badge landed, so a click can find the constraint it stands for. Rebuilt by
    // build_constraint_glyphs on every render, which is always the frame the user clicked on.
    struct GlyphHit { Vec2d c{0,0}; int con{-1}; };
    std::vector<GlyphHit> m_glyph_hits;
    double                m_glyph_r{0.0};   // badge half-size in plane units (hit radius)
    GLModel             m_line_model;
    GLModel             m_vertex_model;
    GLModel             m_highlight_model;
    int                 m_dim_label_seq{0};
    float               m_render_scale{1.0f};   // canvas scale for Measure-style dim labels
    GLModel             m_fill_model;       // translucent face fill for closed regions
    std::vector<DisplaySketch> m_display_sketches;  // committed sketches drawn persistently
    std::vector<std::pair<int, ColorRGBA>> m_hl_sketches;  // feature index -> outline colour (Sweep/Loft operands)
    int m_display_pick{-1};        // FEATURE index of the click-selected display sketch (-1 none)

    // Solid (whole/face/edge) selection on the committed bodies. Pointers are non-owning,
    // into CadDocument (bodies + display_mesh + per-triangle face/body ids), refreshed each
    // recompute via set_solid_pick. m_sel_edge_pts caches the picked edge's world polyline.
    const std::vector<CadBody>* m_solid_bodies{nullptr};
    const TriangleMesh*     m_solid_mesh{nullptr};
    const std::vector<int>* m_solid_tri_face{nullptr};
    const std::vector<int>* m_solid_tri_body{nullptr};
    const std::vector<bool>* m_solid_visible{nullptr};  // per-body visibility; hidden bodies aren't pickable
    int m_pick_only_body{-1};        // >=0: only this body catches clicks (body-focus x-ray for CoordSys picking)
    const std::vector<Transform3d>* m_solid_xform{nullptr};  // per-body display transform (for edge sampling)
    Vec3d body_xform_pt(int body, const Vec3d& p) const;     // map an OCCT-shape point through the body xform
    bool body_pickable(int b) const;                    // false when the body is explicitly hidden
    SolidSel                m_solid_sel{SolidSel::None};
    int                     m_sel_body{-1};   // which body the face/edge selection is on
    int                     m_sel_face{-1};
    int                     m_sel_edge{-1};
    std::vector<Vec3d>      m_sel_edge_pts;
    Vec3d                   m_sel_vertex_pt{Vec3d::Zero()};   // world point of a picked vertex
    bool handle_solid_click(GLCanvas3D& canvas, const wxMouseEvent& evt);  // pick + notify
    // What a click at (mx,my) WOULD take, resolved without touching the selection. One
    // implementation, two callers: the click, and the hover pre-highlight that promises what the
    // click is about to do. Split so the promise cannot drift from the act.
    struct SolidPick {
        SolidSel           kind{SolidSel::None};
        int                body{-1}, face{-1}, edge{-1};
        std::vector<Vec3d> edge_pts;
        Vec3d              vertex_pt{Vec3d::Zero()};
    };
    bool resolve_solid_pick(GLCanvas3D& canvas, int mx, int my, SolidPick& out) const;
    // HOVER PRE-HIGHLIGHT (snaporca-9xw part 3). Vertex-beats-edge-beats-face is a rule the user
    // cannot see until after they commit to a click; showing the outcome under the pointer is
    // what makes the precedence learnable at all, and is the charter's L5 (one click, one visible
    // change) read honestly — the change has to be predictable before the click, not only after.
    SolidPick m_pre;                       // what the pointer is currently over (kind None = nothing)
    bool update_solid_hover(GLCanvas3D& canvas, const wxMouseEvent& evt);  // true when it changed
    // Left-drag rubber band: sweep a rectangle over the plate to take a whole body. Orbit
    // moves to middle-drag in this canvas (DesignCanvas::set_cad_navigation) so the left
    // button is free for it, which is the CAD convention (Onshape/SolidWorks).
    GLSelectionRectangle m_rubber;
    void pick_bodies_in_rectangle();       // resolve the swept rectangle -> whole-body selection
    bool on_mouse_impl(wxMouseEvent& evt, GLCanvas3D& canvas);   // the body; on_mouse wraps it
    // Nearest stroke + enclosing region of ONE committed sketch. Shared by the click and
    // double-click paths so they cannot disagree about what is under the pointer.
    void hit_display_sketch(const DisplaySketch& d, const Vec2d& p, double tol,
                            int& edge_feat, int& edge_reg, int& edge_ent,
                            double& edge_d, int& face_feat, int& face_reg) const;
    bool m_right_consumed{false};          // last RightDown was a gesture terminator, not a menu
    bool m_escalate_repick{true};          // re-picking the same sub-element takes the whole body
    void render_solid_highlight();
    // The shared body of the above: one highlight from explicit arguments, so the committed
    // selection and the hover pre-highlight cannot drift apart in how they look.
    void render_solid_sel(SolidSel kind, int body, int face, const std::vector<Vec3d>& edge_pts,
                          const Vec3d& vertex_pt, const ColorRGBA& rgb, float alpha_mul);
    void render_datum_planes();           // translucent rectangles for datum/reference planes
    void render_view_helpers();           // world origin planes + axis triad (P / A toggles)
    bool m_show_planes{false};
    bool m_show_axes{false};
    std::vector<SketchPlane> m_datum_planes;
    std::vector<Vec2d>       m_datum_sizes;   // per-plane (u,v) full extent; empty -> default
    void render_mate_connectors();            // disc + roll quadrant + one-sided Z arrow
    // The face treatment of the same connector: a shaded low-poly relief of a bear's head in the
    // connector's own frame. Draws the plate, the snout tent and the marks; the caller still draws
    // the Z arrow, which is shared with the disc treatment.
    void render_mate_face(const Vec3d& origin, const Vec3d& X, const Vec3d& Y, const Vec3d& Z,
                          double R, const ColorRGBA& body);
    std::vector<MateConnectorGlyph> m_mate_connectors;
    std::vector<std::pair<Vec3d, Vec3d>> m_mate_links;
    GLModel m_mc_stroke_model;
    GLModel m_mc_fill_model;      // the face treatment's shaded facets
    GLModel m_solid_face_model;
    GLModel m_solid_edge_model;
    GLModel m_solid_vertex_model;
    int m_display_pick_region{-1}; // selected closed-region index within that feature (-1 none)

    // Visual Extrude gizmo state (C5b). GUI-only; fed by the panel each refresh_preview.
    bool        m_ex_active{false};
    SketchPlane m_ex_plane;             // profile plane (gives normal + to_world anchor)
    Vec2d       m_ex_centroid{0,0};     // arrow base in plane coords (profile centroid)
    double      m_ex_depth{0.0};        // primary depth (= m_distance)
    double      m_ex_depth2{0.0};       // second-side depth (TwoSided, = m_distance2)
    bool        m_ex_two_sided{false};
    bool        m_ex_flip{false};
    int         m_ex_drag{-1};          // 0 = primary arrow, 1 = second arrow, -1 = none
    int         m_ex_press_x{0}, m_ex_press_y{0};   // press px to tell click-to-edit from drag
    void  render_extrude_gizmo();
    bool  hit_test_extrude_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt, int& which) const;
    void  drag_extrude_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt, int which);
    void  open_extrude_editor(int which);
    GLModel m_ex_arrow_model;

    // Datum-plane resize gizmo state (C3). GUI-only; fed by the panel while the Plane card is open.
    bool        m_dz_active{false};
    SketchPlane m_dz_plane;              // resolved datum frame (origin + axes)
    double      m_dz_usize{60.0};        // current u extent (full width)
    double      m_dz_vsize{60.0};        // current v extent (full height)
    int         m_dz_drag{-1};           // 0=+u,1=-u,2=+v,3=-v handle, 4=offset tip, -1 none
    int         m_dz_press_x{0}, m_dz_press_y{0};
    Vec3d       m_dz_anchor{Vec3d::Zero()};   // base-plane origin (offset arrow tail)
    Vec3d       m_dz_normal{0.0, 0.0, 1.0};   // base normal (offset arrow direction)
    double      m_dz_offset{0.0};             // current signed offset along the base normal
    bool        m_dz_offset_on{false};        // draw/allow the offset arrow (Offset-from-base only)
    void  render_datum_gizmo();
    bool  hit_test_datum_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int& which) const;
    void  drag_datum_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int which);
    // Helix gizmo state (plane-anchored curve + 3 drag handles). Fed by the panel while the
    // Helix card is open (sketch tool NOT active); the tool draws the live helix plus a handle
    // on each length parameter (radius/height/pitch). Taper and handedness stay on the card.
    bool        m_hx_active{false};
    SketchPlane m_hx_plane;                  // axis = plane normal, base circle in the plane
    double      m_hx_radius{10.0};
    double      m_hx_pitch{2.0};
    double      m_hx_height{20.0};
    double      m_hx_taper{0.0};             // DEGREES (cone half-angle), as the kernel reads it
    bool        m_hx_left{false};
    int         m_hx_drag{-1};               // 0=radius, 1=height, 2=pitch, -1 none
    int         m_hx_press_x{0}, m_hx_press_y{0};
    Vec3d helix_point(double t) const;       // curve point at parameter t (shared render/hit/drag)
    void  render_helix_gizmo();
    bool  hit_test_helix_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int& which) const;
    void  drag_helix_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int which);
    // Rib thickness gizmo state (plane-anchored slab footprint + 2 drag handles). Fed by the
    // panel while the Rib card is open (sketch tool NOT active); the tool draws the rib's
    // footprint outline and a handle on each side of the line at half-thickness. Dragging either
    // handle sets the full thickness (the slab is centred on the line).
    bool        m_rb_active{false};
    SketchPlane m_rb_plane;
    Vec2d       m_rb_p0{Vec2d::Zero()};      // rib line endpoints, in plane coords
    Vec2d       m_rb_p1{Vec2d::Zero()};
    double      m_rb_thickness{2.0};
    int         m_rb_drag{-1};               // 0 = +perp handle, 1 = -perp handle, -1 none
    void  render_rib_gizmo();
    bool  hit_test_rib_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int& which) const;
    void  drag_rib_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int which);
    // Datum base picker (translucent clickable origin/datum planes)
    bool        m_dbp_active{false};
    std::vector<SketchPlane>  m_dbp_planes;
    std::vector<int>          m_dbp_base;
    std::vector<std::string>  m_dbp_labels;
    int         m_dbp_hover{-1};
    double      dbp_half_extent() const;   // bed-derived: reference planes are larger than the bed
    void render_base_pick();
    int  hit_test_base_pick(GLCanvas3D& canvas, const wxMouseEvent& evt) const;

    // Move-body gizmo state: 3 world-axis translate arrows + 3 world-axis rotate rings.
    // Delta model: offset/rot are deltas about a fixed pivot, composed onto m_mv_base_xform
    // (the body's pose when Move opened) so rotation works even on an already-placed body.
    bool        m_mv_active{false};
    int         m_mv_body{-1};
    Vec3d       m_mv_base{Vec3d::Zero()};      // pivot = body's world centroid at Move-open
    Vec3d       m_mv_offset{Vec3d::Zero()};    // delta translation along world X/Y/Z
    Transform3d m_mv_base_xform{Transform3d::Identity()};  // pose when Move opened
    Eigen::Matrix3d m_mv_rot{Eigen::Matrix3d::Identity()}; // accumulated delta rotation (world, about pivot)
    Eigen::Matrix3d m_mv_rot_start{Eigen::Matrix3d::Identity()}; // rot snapshot at arc-drag start
    double      m_mv_arc_a0{0.0};              // mouse angle on the ring at drag start
    int         m_mv_drag{-1};                 // 0..2 = X/Y/Z arrow, 3..5 = X/Y/Z ring, -1 none
    double      m_mv_radius{0.0};              // body bounding-sphere radius (mm); 0 = unknown
    int         m_mv_press_x{0}, m_mv_press_y{0};
    Transform3d compose_move_xform() const;    // T(offset)*T(pivot)*rot*T(-pivot)*base_xform
    void  ring_basis(int axis, Vec3d& e, Vec3d& u, Vec3d& v) const;  // world axis + in-plane basis
    void  render_move_gizmo();
    // Gizmo arm length (world mm): scales with the body so the rings clear its surface.
    double move_gizmo_arm(const Camera& cam) const;
    bool  hit_test_move_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt, int& axis) const;
    bool  hit_test_move_arc(GLCanvas3D& canvas, const wxMouseEvent& evt, int& axis) const;
    void  drag_move_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt, int axis);
    void  drag_move_arc(GLCanvas3D& canvas, const wxMouseEvent& evt, int axis);
    bool  arc_mouse_angle(GLCanvas3D& canvas, const wxMouseEvent& evt, int axis, double& ang) const;
    void  open_move_editor(int axis);
    GLModel m_mv_arrow_model;

    // Fillet/Chamfer radius gizmo state (single world-space arrow at the picked edge midpoint).
    bool        m_fl_active{false};
    Vec3d       m_fl_anchor{Vec3d::Zero()};    // edge midpoint (world, already body-transformed)
    Vec3d       m_fl_dir{Vec3d::UnitZ()};      // unit radius direction (perp to edge, outward)
    double      m_fl_radius{1.0};              // current radius (= dressup size)
    bool        m_fl_drag{false};
    int         m_fl_press_x{0}, m_fl_press_y{0};
    double      m_fl_grab_proj{0.0};           // axis projection at grab (relative drag reference)
    double      m_fl_grab_radius{1.0};         // radius at grab (relative drag reference)
    void  render_fillet_gizmo();
    bool  hit_test_fillet_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt) const;
    double fillet_axis_proj(GLCanvas3D& canvas, const wxMouseEvent& evt) const;  // NaN if camera∥axis
    void  start_fillet_drag(GLCanvas3D& canvas, const wxMouseEvent& evt);
    void  drag_fillet_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt);
    void  open_fillet_editor();
    GLModel m_fl_arrow_model;

    // Hole gizmo state. The hole is a positioned circular cut on m_hl_plane at (m_hl_x, m_hl_y);
    // the footprint circle is drawn on the plane, the diameter arrow runs along the plane u-axis,
    // the depth arrow along +normal (matching the kernel's make_extrude). Three draggable handles:
    // 0 = centre (reposition in plane u/v), 1 = diameter, 2 = depth (only shown when !through).
    bool        m_hl_active{false};
    SketchPlane m_hl_plane;
    double      m_hl_x{0.0}, m_hl_y{0.0};      // centre on the plane (u/v mm)
    double      m_hl_diameter{6.0};
    double      m_hl_depth{10.0};
    bool        m_hl_through{true};
    // #2 Part B: face (u,v) bounds, so the construction dims read as distance from the face SIDES
    // (umin/vmin = two adjacent edges) rather than from the centre. Off for a dropdown-plane hole.
    bool        m_hl_has_bounds{false};
    double      m_hl_umin{0}, m_hl_umax{0}, m_hl_vmin{0}, m_hl_vmax{0};
    int         m_hl_drag{-1};                 // 0=centre, 1=diameter, 2=depth, 3=X-dim, 4=Y-dim, -1=none
    int         m_hl_press_x{0}, m_hl_press_y{0};
    double      m_hl_grab_proj{0.0};           // diameter/depth axis projection at grab (relative)
    double      m_hl_grab_val{0.0};            // radius (diameter drag) or depth at grab
    Vec2d       m_hl_grab_uv{0.0, 0.0};        // centre drag: plane-projected grab point
    double      m_hl_grab_x{0.0}, m_hl_grab_y{0.0};   // centre drag: x/y at grab
    void   render_hole_gizmo();
    int    hit_test_hole_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const;  // 0/1/2/-1
    double hole_axis_proj(GLCanvas3D& canvas, const wxMouseEvent& evt,
                          const Vec3d& anchor, const Vec3d& dir) const;   // NaN if camera∥axis
    void   start_hole_drag(GLCanvas3D& canvas, const wxMouseEvent& evt, int which);
    void   drag_hole_handle(GLCanvas3D& canvas, const wxMouseEvent& evt);
    void   open_hole_editor(int which);
    GLModel m_hl_stroke_model;

    // Thread gizmo state (mirrors the hole gizmo; radius arrow uses an R label, length arrow is
    // always shown). Handles: 0 = centre (thread_x/y), 1 = radius, 2 = length.
    bool        m_th_active{false};
    SketchPlane m_th_plane;
    double      m_th_x{0.0}, m_th_y{0.0};
    double      m_th_radius{5.0};
    double      m_th_height{10.0};
    int         m_th_drag{-1};                 // 0=centre, 1=radius, 2=length, -1=none
    int         m_th_press_x{0}, m_th_press_y{0};
    double      m_th_grab_proj{0.0};
    double      m_th_grab_val{0.0};
    Vec2d       m_th_grab_uv{0.0, 0.0};
    double      m_th_grab_x{0.0}, m_th_grab_y{0.0};
    void   render_thread_gizmo();
    int    hit_test_thread_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const;  // 0/1/2/-1
    void   start_thread_drag(GLCanvas3D& canvas, const wxMouseEvent& evt, int which);
    void   drag_thread_handle(GLCanvas3D& canvas, const wxMouseEvent& evt);
    void   open_thread_editor(int which);
    GLModel m_th_stroke_model;

    // Shell gizmo state (single inward thickness arrow at the picked face centroid).
    bool        m_sh_active{false};
    Vec3d       m_sh_anchor{Vec3d::Zero()};    // picked face centroid (world)
    Vec3d       m_sh_dir{Vec3d::UnitZ()};      // inward unit direction (-outward normal)
    double      m_sh_thickness{2.0};
    bool        m_sh_drag{false};
    int         m_sh_press_x{0}, m_sh_press_y{0};
    double      m_sh_grab_proj{0.0};
    double      m_sh_grab_val{2.0};
    void   render_shell_gizmo();
    bool   hit_test_shell_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt) const;
    void   start_shell_drag(GLCanvas3D& canvas, const wxMouseEvent& evt);
    void   drag_shell_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt);
    void   open_shell_editor();
    GLModel m_sh_stroke_model;

    // Revolve gizmo state (arc center = projection of the profile centroid onto the axis).
    bool        m_rv_active{false};
    Vec3d       m_rv_center{Vec3d::Zero()};   // arc center on the axis (world)
    Vec3d       m_rv_axis{Vec3d::UnitX()};    // revolve axis unit dir (world)
    Vec3d       m_rv_ref{Vec3d::UnitY()};     // angle-0 reference dir (perp to axis, toward profile)
    double      m_rv_radius{10.0};            // arc radius = profile perpendicular distance (world)
    double      m_rv_angle{360.0};            // current sweep magnitude (deg, 1..360)
    bool        m_rv_flip{false};             // sweep sense (matches the kernel's negative-angle flip)
    bool        m_rv_drag{false};
    int         m_rv_press_x{0}, m_rv_press_y{0};
    void   render_revolve_gizmo();
    bool   hit_test_revolve_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const;
    void   drag_revolve_arc(GLCanvas3D& canvas, const wxMouseEvent& evt);
    void   open_revolve_editor();
    GLModel m_rv_stroke_model;

    // Draft gizmo state (arc center = picked face centroid; axis = world +Z, taper pull direction).
    bool        m_dr_active{false};
    Vec3d       m_dr_center{Vec3d::Zero()};   // arc center = face centroid (world)
    Vec3d       m_dr_axis{Vec3d::UnitZ()};    // draft axis = world +Z (pull direction)
    Vec3d       m_dr_ref{Vec3d::UnitX()};     // angle-0 reference dir (perp to axis)
    double      m_dr_radius{10.0};            // arc radius (world)
    double      m_dr_angle{5.0};              // current sweep magnitude (deg, [-89, 89])
    bool        m_dr_drag{false};
    int         m_dr_press_x{0}, m_dr_press_y{0};
    void   render_draft_gizmo();
    bool   hit_test_draft_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const;
    void   drag_draft_arc(GLCanvas3D& canvas, const wxMouseEvent& evt);
    GLModel m_dr_stroke_model;
    std::function<void(double)> m_on_draft_angle_changed;

    // Cut gizmo state (plane normal arrow + wire rectangle at the current offset).
    bool        m_ct_active{false};
    Vec3d       m_ct_base{Vec3d::Zero()};       // body centre projected into the cut plane
    Vec3d       m_ct_n{Vec3d::UnitZ()};         // cut plane normal (unit)
    Vec3d       m_ct_u{Vec3d::UnitX()};         // cut plane U axis (unit)
    Vec3d       m_ct_v{Vec3d::UnitY()};         // cut plane V axis (unit)
    double      m_ct_offset{0.0};
    double      m_ct_half{10.0};
    bool        m_ct_drag{false};
    double      m_ct_grab_val{0.0};
    double      m_ct_grab_proj{0.0};
    void   render_cut_gizmo();
    bool   hit_test_cut_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt) const;
    void   start_cut_drag(GLCanvas3D& canvas, const wxMouseEvent& evt);
    void   drag_cut_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt);
    GLModel m_ct_stroke_model;
    GLModel m_ct_rect_model;
    std::function<void(double)> m_on_cut_offset_changed;

    // Pattern gizmo state. Linear arrow along m_pt_dirw from m_pt_base; circular arc like Revolve
    // but axis = m_pt_normal through m_pt_origin (the world XY plane by default).
    bool   m_pt_active{false};
    bool   m_pt_circular{false};
    Vec3d  m_pt_base{Vec3d::Zero()};     // target body centroid (world): linear anchor / radius ref
    Vec3d  m_pt_dirw{Vec3d::UnitX()};    // linear march direction (world)
    Vec3d  m_pt_origin{Vec3d::Zero()};   // circular rotation axis origin (world)
    Vec3d  m_pt_normal{Vec3d::UnitZ()};  // circular rotation axis (world)
    Vec3d  m_pt_cref{Vec3d::UnitX()};    // circular angle-0 reference dir (perp to normal, toward body)
    Vec3d  m_pt_ccenter{Vec3d::Zero()};  // circular arc center (foot of body centroid on the axis)
    double m_pt_radius{10.0};            // circular arc radius (world)
    int    m_pt_count{3};
    double m_pt_spacing{20.0};
    double m_pt_angle{360.0};
    bool   m_pt_drag{false};
    int    m_pt_press_x{0}, m_pt_press_y{0};
    void   render_pattern_gizmo();
    bool   hit_test_pattern_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const;
    void   drag_pattern_handle(GLCanvas3D& canvas, const wxMouseEvent& evt);
    void   open_pattern_editor();
    GLModel m_pt_stroke_model;
};

}} // namespace Slic3r::GUI

#endif // slic3r_DesignSketchTool_hpp_
