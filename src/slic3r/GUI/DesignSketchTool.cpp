#include "DesignSketchTool.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "ImGuiWrapper.hpp"
#include "Plater.hpp"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "libslic3r/BuildVolume.hpp"
#include "Camera.hpp"
#include "3DScene.hpp"
#include "GLShader.hpp"
#include "libslic3r/GeometryEngine.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <GL/glew.h>
#include <wx/gdicmn.h>
#include <algorithm>
#include <climits>
#include <limits>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>

namespace Slic3r {
namespace GUI {

// Positioning helpers (defined lower down, used by the dimension methods above them).
static bool entity_ref_point(const SketchEntity& e, Vec2d& out);
static void translate_entity(SketchEntity& e, const Vec2d& d);
// Pick-distance helpers (defined lower down; used earlier by the edit-op gizmo).
static double point_segment_dist(const Vec2d& p, const Vec2d& a, const Vec2d& b);
static double entity_pick_dist(const Vec2d& p, const SketchEntity& e);
static bool   point_in_poly(const Vec2d& q, const std::vector<Vec2d>& poly);
static bool   ray_triangle(const Vec3d& ro, const Vec3d& rd, const Vec3d& v0, const Vec3d& v1,
                           const Vec3d& v2, double& t);
static double ray_segment_dist3(const Vec3d& ro, const Vec3d& rd, const Vec3d& a, const Vec3d& b);

// Project a world-space point to canvas screen pixels (device px, GL viewport units;
// origin top-left after the GL y-flip). Mirrors GLCanvas3D's world->screen pattern:
// projection * view, perspective divide, NDC -> viewport. Returns (-1,-1) if behind.
// (Retained for auto-emitted dimensions in Phase B, which have no click to anchor to;
// needs the design canvas's own camera/viewport, not the plater's.)
[[maybe_unused]] static wxPoint world_to_screen_px(const Camera& cam, const Vec3d& world)
{
    // NB: multiply the raw 4x4 matrices, NOT the Transform3d objects — the projection is not
    // affine, so Transform*Transform mangles it (Eigen assumes affine) and yields garbage w.
    const Eigen::Matrix4d m = cam.get_projection_matrix().matrix() * cam.get_view_matrix().matrix();
    const Eigen::Vector4d clip = m * world.homogeneous();
    if (std::abs(clip.w()) < 1e-9) return wxPoint(-1, -1);
    const Vec3d ndc = clip.head<3>() / clip.w();
    const std::array<int, 4>& vp = cam.get_viewport();
    const double sx = vp[0] + (ndc.x() * 0.5 + 0.5) * vp[2];
    const double sy = vp[1] + (1.0 - (ndc.y() * 0.5 + 0.5)) * vp[3];   // GL y-up -> wx y-down
    return wxPoint(int(sx + 0.5), int(sy + 0.5));
}

void DesignSketchTool::begin(const SketchPlane& plane, Mode mode)
{
    m_plane = plane;
    m_mode = mode;
    m_points.clear();
    m_entities.clear();
    m_construction = false;
    m_has_cursor = false;
    m_sel_a = m_sel_b = -1;
    m_constrain_entities = false;
    m_pick0 = m_pick1 = m_pick2 = -1;
    m_constraint_hl.clear();
    m_constrain_cons.clear();
    m_awaiting_length = false;
    m_autoedit_seen = 0;          // baseline: no entities yet; first commit triggers edit
    m_autoedit_pending = false;
    m_selection.clear();
    m_point_sel.clear();
    m_constraints.clear();
    m_dimensions.clear();
    m_dim_has0 = false;
    m_pending_dim = -1;
    m_dof = -1; m_solve_ok = true; m_entity_conflict.clear();
    m_features.clear();
    m_open_feature = -1;
    m_active = true;
}

// Re-open a committed entity sketch for editing: mirror begin() (fresh Select session),
// then load the geometry + driving constraints, re-detect feature groups, and live-solve
// so handles/quotes/regular-drag work exactly as in the original draw session.
void DesignSketchTool::begin_edit(const std::vector<SketchEntity>& entities,
                                  const std::vector<SketchEntityConstraintDef>& constraints,
                                  const SketchPlane& plane)
{
    begin(plane, Mode::Select);
    m_entities    = entities;
    m_constraints = constraints;
    rebuild_features_from_entities();
    resolve_live();
}

// Walk the entity list grouping each consecutive CLOSED chain (entities are stored in
// gesture order, so a polygon/rect/slot's members are contiguous and end-to-end linked)
// and classify it: L,A,L,A -> Slot; 4 arcs (2 concentric + 2 caps) -> ArcSlot;
// L,A×4 (4 equal-radius corner arcs) -> RoundedRect; 4 right-angled lines -> Rect;
// N equal-length lines -> Polygon. Single/irregular entities stay ungrouped (they fall
// back to per-entity quotes). This reconstructs m_features for a re-opened sketch.
void DesignSketchTool::rebuild_features_from_entities()
{
    m_features.clear();
    m_open_feature = -1;
    const int n = int(m_entities.size());
    using T = SketchEntity::Type;
    auto start = [&](int i) { return m_entities[i].p0; };
    auto end   = [&](int i) { const SketchEntity& e = m_entities[i];
        return (e.type == T::Line || e.type == T::Arc) ? e.p1 : e.p0; };
    auto is_near  = [](const Vec2d& a, const Vec2d& b) {
        return (a - b).norm() <= 0.05 + 1e-3 * std::max(a.norm(), b.norm()); };

    int i = 0;
    while (i < n) {
        int j = i; bool closed = false;            // greedily extend a consecutive chain
        while (j + 1 < n && is_near(end(j), start(j + 1))) {
            ++j;
            if ((j - i) >= 2 && is_near(end(j), start(i))) { closed = true; break; }
        }
        const int cnt = j - i + 1;
        if (!closed || cnt < 3) { ++i; continue; }

        int arcs = 0; bool all_line = true;
        for (int k = i; k <= j; ++k) {
            if (m_entities[k].type == T::Arc)       ++arcs;
            if (m_entities[k].type != T::Line)      all_line = false;
        }
        Vec2d c(0, 0); for (int k = i; k <= j; ++k) c += start(k); c /= double(cnt);
        Feature f; f.begin = i; f.end = j + 1; f.c0 = c;

        if (cnt == 4 && arcs == 2 &&
            m_entities[i].type   == T::Line && m_entities[i + 1].type == T::Arc &&
            m_entities[i + 2].type == T::Line && m_entities[i + 3].type == T::Arc) {
            f.kind  = FeatureKind::Slot;          // make_slot: top, cap@c1, bottom, cap@c0
            f.c0    = m_entities[i + 3].center;
            f.c1    = m_entities[i + 1].center;
            f.param = m_entities[i + 1].radius;
            m_features.push_back(f);
        } else if (cnt == 4 && arcs == 4 &&
                   is_near(m_entities[i].center, m_entities[i + 2].center) &&
                   std::abs(m_entities[i + 1].radius - m_entities[i + 3].radius) <=
                       0.02 * std::max(m_entities[i + 1].radius, 1e-6) &&
                   m_entities[i].radius > m_entities[i + 2].radius) {
            // make_arc_slot: [0]outer(Rc+w), [1]cap@E(w), [2]inner(Rc-w), [3]cap@S(w).
            // c0=main centre, c1=centreline start (cap@S centre = Sc), param=half-width.
            f.kind  = FeatureKind::ArcSlot;
            f.c0    = m_entities[i].center;
            f.c1    = m_entities[i + 3].center;
            f.param = m_entities[i + 3].radius;
            m_features.push_back(f);
        } else if (cnt == 8 && arcs == 4 &&
                   m_entities[i].type     == T::Line && m_entities[i + 1].type == T::Arc &&
                   m_entities[i + 2].type == T::Line && m_entities[i + 3].type == T::Arc &&
                   m_entities[i + 4].type == T::Line && m_entities[i + 5].type == T::Arc &&
                   m_entities[i + 6].type == T::Line && m_entities[i + 7].type == T::Arc) {
            // rounded_rect_entities: 4 corner arcs (equal radius r) at the inset corners.
            // Recover the axis-aligned bounds from the arc centres ± r.
            const double r = m_entities[i + 1].radius;
            bool equal_r = true;
            for (int k : {3, 5, 7})
                if (std::abs(m_entities[i + k].radius - r) > 0.02 * std::max(r, 1e-6))
                    equal_r = false;
            if (equal_r && r > 1e-6) {
                double cxmin = 1e18, cxmax = -1e18, cymin = 1e18, cymax = -1e18;
                for (int k : {1, 3, 5, 7}) {
                    const Vec2d& o = m_entities[i + k].center;
                    cxmin = std::min(cxmin, o.x()); cxmax = std::max(cxmax, o.x());
                    cymin = std::min(cymin, o.y()); cymax = std::max(cymax, o.y());
                }
                f.kind  = FeatureKind::RoundedRect;
                f.c0    = Vec2d(cxmin - r, cymin - r);   // (xmin,ymin)
                f.c1    = Vec2d(cxmax + r, cymax + r);   // (xmax,ymax)
                f.param = r;
                m_features.push_back(f);
            }
        } else if (all_line) {
            std::vector<double> sidelen(cnt);
            for (int k = 0; k < cnt; ++k) sidelen[k] = (m_entities[i + k].p1 - m_entities[i + k].p0).norm();
            const double lmin = *std::min_element(sidelen.begin(), sidelen.end());
            const double lmax = *std::max_element(sidelen.begin(), sidelen.end());
            const bool equal_sides = lmin > 1e-6 && (lmax - lmin) / lmax < 0.02;
            bool all_right = true;
            for (int k = 0; k < cnt && all_right; ++k) {
                Vec2d u = m_entities[i + k].p1 - m_entities[i + k].p0;
                Vec2d v = m_entities[i + (k + 1) % cnt].p1 - m_entities[i + (k + 1) % cnt].p0;
                if (u.norm() < 1e-9 || v.norm() < 1e-9) { all_right = false; break; }
                if (std::abs(u.normalized().dot(v.normalized())) > 0.06) all_right = false; // ~3.4°
            }
            if (cnt == 4 && all_right) {
                f.kind = FeatureKind::CornerRect; f.c0 = start(i); f.c1 = start(i + 2);
                m_features.push_back(f);
            } else if (equal_sides) {
                f.kind = FeatureKind::Polygon; f.c0 = c; f.c1 = start(i);
                f.sides = cnt; f.param = (start(i) - c).norm();
                m_features.push_back(f);
            }
        }
        i = j + 1;
    }
}

void DesignSketchTool::set_tool(Mode mode)
{
    // A READY edit-op carries the user's typed or dragged value, so switching tools commits it
    // rather than dropping it — the same rule Tab follows in the dimension editor. Discarding it
    // here is most of why Fillet looked like it simply did not work: every documented route (type
    // the radius, or drag the arrow) left the op ready-but-pending, and the next tool click threw
    // the value away and reverted the corner to sharp, with nothing on screen saying so.
    // This MUST run before m_mode is reassigned: op_ready() and confirm_op() both switch on
    // m_mode, so after the assignment they would test the tool being switched TO. That read
    // op_ready()==0 with a=0 b=3 val=28.205 sitting right there — picked, valued, and dropped.
    if (op_ready()) confirm_op();

    // An OPEN inline value field freezes the canvas (on_mouse_impl returns early while
    // m_awaiting_length) and blocks every keyboard shortcut (in_text includes inline_busy()).
    // Drawing a rectangle opens one automatically for its Width/Height, so after a rectangle the
    // sketch was STUCK: pressing C did nothing because the key never reached this function, and
    // clicking on the canvas did nothing because the canvas was frozen. Committing here accepts
    // the typed value and closes the field, which is the same rule the ready-edit-op above
    // follows — leaving a tool must not silently discard what the user entered.
    if (on_inline_commit) on_inline_commit();

    // Switch the active drawing tool without dropping accumulated entities.
    m_mode = mode;
    m_points.clear();
    m_has_cursor = false;
    m_awaiting_length = false;
    m_autoedit_seen = int(m_entities.size());   // resync baseline so a switch never fires
    m_autoedit_pending = false;
    // A READY edit-op carries the user's typed or dragged value, so switching tools commits it
    // rather than dropping it — the same rule Tab follows in the dimension editor. Discarding it
    // here is most of why Fillet looked like it simply did not work: every documented route (type
    // the radius, or drag the arrow) left the op ready-but-pending, and the next tool click threw
    // the value away and reverted the corner to sharp, with nothing on screen saying so.
    reset_op();                 // drop any in-progress (not yet ready) edit-op gizmo
    reset_tf();                 // drop any in-progress transform gizmo
    m_selection.clear();
    if (on_selection_changed) on_selection_changed(0);
}

void DesignSketchTool::cancel()
{
    close_session_chrome();     // same orphaned-field freeze as finish() — see snaporca-yce
    m_active = false;
    m_points.clear();
    m_entities.clear();
    m_construction = false;
    m_has_cursor = false;
    m_sel_a = m_sel_b = -1;
    m_constrain_entities = false;
    m_pick0 = m_pick1 = m_pick2 = -1;
    m_constraint_hl.clear();
    m_constrain_cons.clear();
    m_awaiting_length = false;
    m_selection.clear();
    m_point_sel.clear();
    m_constraints.clear();
    m_dimensions.clear();
    m_dim_has0 = false;
    m_pending_dim = -1;
    m_dof = -1; m_solve_ok = true; m_entity_conflict.clear();
    m_features.clear();
    m_open_feature = -1;
    reset_op();
    reset_xform();
    reset_tf();
}

// Esc while active: layered exit (Onshape-like). Abort an in-progress entity first, then
// drop a draw tool back to Select; only an idle Select session exits to Feature mode.
void DesignSketchTool::request_exit()
{
    if (!m_points.empty()) { m_points.clear(); m_has_cursor = false; return; }
    // Drop any pending edit-op BEFORE the downgrade: set_tool commits a ready one, and Esc must
    // cancel it, never apply it. Right-click already discards it through its own branch.
    if (m_mode != Mode::Select) { reset_op(); set_tool(Mode::Select); return; }
    if (on_exit) on_exit(); else cancel();
}

void DesignSketchTool::request_undo_redo(bool redo)
{
    if (on_undo_redo) on_undo_redo(redo);
}

void DesignSketchTool::clear_selection()
{
    if (m_selection.empty() && m_point_sel.empty()) return;
    m_selection.clear();
    m_point_sel.clear();
    if (on_selection_changed) on_selection_changed(0);
}

void DesignSketchTool::delete_selected()
{
    if (m_selection.empty()) return;
    const int n = int(m_entities.size());
    std::vector<bool> del(n, false);
    for (int i : m_selection)
        if (i >= 0 && i < n) del[i] = true;
    // old index -> new index (or -1 if deleted), to fix up constraint references.
    std::vector<int> remap(n, -1);
    int next = 0;
    for (int i = 0; i < n; ++i)
        if (!del[i]) remap[i] = next++;
    for (int i = n - 1; i >= 0; --i)
        if (del[i]) m_entities.erase(m_entities.begin() + i);
    // Drop constraints touching a deleted entity; remap the survivors.
    std::vector<SketchEntityConstraintDef> kept;
    auto live = [&](int e) { return e < 0 || (e < n && remap[e] >= 0); };
    auto map  = [&](int e) { return e < 0 ? -1 : remap[e]; };
    for (SketchEntityConstraintDef c : m_constraints) {
        if (!live(c.ea) || !live(c.eb) || !live(c.ec)) continue;
        c.ea = map(c.ea); c.eb = map(c.eb); c.ec = map(c.ec);
        kept.push_back(c);
    }
    m_constraints.swap(kept);
    m_selection.clear();
    m_point_sel.clear();
    // v1: placed quotes reference entity indices that have shifted; drop them rather
    // than risk a dangling reference (the driving constraints survive, reindexed).
    m_dimensions.clear();
    m_dim_has0 = false;
    m_pending_dim = -1;
    if (on_selection_changed) on_selection_changed(0);
}

bool DesignSketchTool::selection_valid() const
{
    for (int i : m_selection)
        if (i < 0 || i >= int(m_entities.size())) return false;
    return true;
}

DesignSketchTool::DimType DesignSketchTool::dimension_kind() const
{
    if (!selection_valid()) return DimType::None;
    if (m_selection.size() == 1) {
        switch (m_entities[m_selection[0]].type) {
        case SketchEntity::Type::Line:   return DimType::Length;
        case SketchEntity::Type::Circle: return DimType::Diameter;
        case SketchEntity::Type::Arc:    return DimType::Radius;
        default: return DimType::None;
        }
    }
    if (m_selection.size() == 2) {
        const SketchEntity& a = m_entities[m_selection[0]];
        const SketchEntity& b = m_entities[m_selection[1]];
        const bool aLine = (a.type == SketchEntity::Type::Line);
        const bool bLine = (b.type == SketchEntity::Type::Line);
        Vec2d tmp(0, 0);
        if (aLine && bLine)                                   return DimType::Angle;
        // one line + one point-like (point / circle-centre / arc-centre)
        if (aLine && entity_ref_point(b, tmp))                return DimType::DistanceToLine;
        if (bLine && entity_ref_point(a, tmp))                return DimType::DistanceToLine;
        // two point-likes -> centre/point distance (0 = coincident/concentric)
        if (entity_ref_point(a, tmp) && entity_ref_point(b, tmp)) return DimType::Distance;
    }
    return DimType::None;
}

double DesignSketchTool::dimension_current() const
{
    switch (dimension_kind()) {
    case DimType::Length:   { const auto& e = m_entities[m_selection[0]]; return (e.p1 - e.p0).norm(); }
    case DimType::Diameter: return 2.0 * m_entities[m_selection[0]].radius;
    case DimType::Radius:   return m_entities[m_selection[0]].radius;
    case DimType::Angle: {
        const auto& a = m_entities[m_selection[0]];
        const auto& b = m_entities[m_selection[1]];
        const Vec2d da = a.p1 - a.p0, db = b.p1 - b.p0;
        const double na = da.norm(), nb = db.norm();
        if (na < 1e-9 || nb < 1e-9) return 0.0;
        const double c = std::max(-1.0, std::min(1.0, da.dot(db) / (na * nb)));
        return std::acos(c) * 180.0 / M_PI;
    }
    case DimType::Distance: {
        Vec2d ra(0, 0), rb(0, 0);
        entity_ref_point(m_entities[m_selection[0]], ra);
        entity_ref_point(m_entities[m_selection[1]], rb);
        return (rb - ra).norm();
    }
    case DimType::DistanceToLine: {
        const SketchEntity& a = m_entities[m_selection[0]];
        const SketchEntity& b = m_entities[m_selection[1]];
        const bool aLine = (a.type == SketchEntity::Type::Line);
        const SketchEntity& L = aLine ? a : b;
        const SketchEntity& P = aLine ? b : a;
        Vec2d rp(0, 0); entity_ref_point(P, rp);
        Vec2d dir = L.p1 - L.p0;
        const double n = dir.norm();
        if (n < 1e-9) return 0.0;
        const Vec2d nrm(-dir.y() / n, dir.x() / n);   // unit normal to the line
        return std::abs((rp - L.p0).dot(nrm));
    }
    default: return 0.0;
    }
}

void DesignSketchTool::apply_angle_between(int ia, int ib, double deg)
{
    SketchEntity& A = m_entities[ia];
    SketchEntity& B = m_entities[ib];
    const Vec2d aE[2] = { A.p0, A.p1 };
    const Vec2d bE[2] = { B.p0, B.p1 };
    int si = -1, sj = -1;
    double best = 1e-6;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            const double d = (aE[i] - bE[j]).squaredNorm();
            if (d < best) { best = d; si = i; sj = j; }
        }
    Vec2d pivot, refDir, bMoving;
    bool moveP1;
    if (si >= 0) {                       // shared vertex: pivot there, A's arm is the reference
        pivot   = aE[si];
        refDir  = aE[1 - si] - pivot;
        moveP1  = (sj == 0);             // move the B end that is NOT at the pivot
        bMoving = moveP1 ? B.p1 : B.p0;
    } else {                             // no shared vertex: pivot B.p0, A's direction is reference
        pivot   = B.p0;
        refDir  = A.p1 - A.p0;
        moveP1  = true;
        bMoving = B.p1;
    }
    double nr = refDir.norm();
    if (nr < 1e-9) return;
    refDir /= nr;
    const Vec2d db = bMoving - pivot;
    const double Lb = db.norm();
    if (Lb < 1e-9) return;
    const double cross = refDir.x() * db.y() - refDir.y() * db.x();
    const double sign  = (cross >= 0.0) ? 1.0 : -1.0;   // keep B on its current side
    const double rad   = sign * deg * M_PI / 180.0;
    const Vec2d ndir(refDir.x() * std::cos(rad) - refDir.y() * std::sin(rad),
                     refDir.x() * std::sin(rad) + refDir.y() * std::cos(rad));
    const Vec2d nb = pivot + Lb * ndir;
    if (moveP1) B.p1 = nb; else B.p0 = nb;
}

void DesignSketchTool::apply_dimension(double v)
{
    switch (dimension_kind()) {
    case DimType::Length: {
        SketchEntity& e = m_entities[m_selection[0]];
        const Vec2d d = e.p1 - e.p0;
        const double r = d.norm();
        if (r > 1e-9 && v > 1e-9) e.p1 = e.p0 + (v / r) * d;
        break;
    }
    case DimType::Diameter: if (v > 1e-9) m_entities[m_selection[0]].radius = 0.5 * v; break;
    case DimType::Radius:   if (v > 1e-9) m_entities[m_selection[0]].radius = v;       break;
    case DimType::Angle:    apply_angle_between(m_selection[0], m_selection[1], v);     break;
    case DimType::Distance: {
        // Move the 2nd selection so its reference point sits at distance v from the
        // 1st (v == 0 -> coincident / concentric). Translate the whole entity.
        if (v < 0.0) break;
        Vec2d ra(0, 0), rb(0, 0);
        entity_ref_point(m_entities[m_selection[0]], ra);
        SketchEntity& b = m_entities[m_selection[1]];
        entity_ref_point(b, rb);
        const Vec2d d = rb - ra;
        const double r = d.norm();
        Vec2d target = ra;
        if (r > 1e-9) target = ra + (v / r) * d;
        else          target = ra + Vec2d(v, 0.0);
        translate_entity(b, target - rb);
        break;
    }
    case DimType::DistanceToLine: {
        // Move the point-like selection perpendicular to the line so its reference
        // point is at distance v (v == 0 -> on the line / on the axis).
        if (v < 0.0) break;
        const bool aLine = (m_entities[m_selection[0]].type == SketchEntity::Type::Line);
        const SketchEntity& L = m_entities[m_selection[aLine ? 0 : 1]];
        SketchEntity& P = m_entities[m_selection[aLine ? 1 : 0]];
        Vec2d rp(0, 0); entity_ref_point(P, rp);
        Vec2d dir = L.p1 - L.p0;
        const double n = dir.norm();
        if (n < 1e-9) break;
        const Vec2d nrm(-dir.y() / n, dir.x() / n);
        const double d0 = (rp - L.p0).dot(nrm);          // current signed distance
        const double sign = (d0 >= 0.0) ? 1.0 : -1.0;    // keep the point on its side
        translate_entity(P, (sign * v - d0) * nrm);
        break;
    }
    default: break;
    }
    record_dimension_constraint(v);          // store a driving constraint for this dimension
    resolve_live();                          // live-solve so the viewport shows the solved sketch
    m_selection.clear();
    if (on_selection_changed) on_selection_changed(0);
}

// Append the SketchEntityConstraintDef that makes the just-applied dimension a
// driving constraint (enforced by the kernel live and at commit). DistanceToLine
// records a PointOnLine constraint so "centre onto axis" persists through re-solve.
void DesignSketchTool::record_dimension_constraint(double v)
{
    const DimType k = dimension_kind();
    auto role = [&](int i) {
        return (m_entities[i].type == SketchEntity::Type::Point) ? SketchPointRole::P0
                                                                 : SketchPointRole::Center;
    };
    SketchEntityConstraintDef c;
    switch (k) {
    case DimType::Length:
        c.type = SketchConstraintType::Distance;
        c.ea = m_selection[0]; c.ra = SketchPointRole::P0;
        c.eb = m_selection[0]; c.rb = SketchPointRole::P1;
        c.value = v; m_constraints.push_back(c); break;
    case DimType::Diameter:
        c.type = SketchConstraintType::Diameter; c.ea = m_selection[0]; c.value = v;
        m_constraints.push_back(c); break;
    case DimType::Radius:
        c.type = SketchConstraintType::Radius; c.ea = m_selection[0]; c.value = v;
        m_constraints.push_back(c); break;
    case DimType::Angle:
        c.type = SketchConstraintType::Angle;
        c.ea = m_selection[0]; c.eb = m_selection[1]; c.value = v;
        m_constraints.push_back(c); break;
    case DimType::Distance: {
        const int ia = m_selection[0], ib = m_selection[1];
        if (v < 1e-9) c.type = SketchConstraintType::Coincident;
        else        { c.type = SketchConstraintType::Distance; c.value = v; }
        c.ea = ia; c.ra = role(ia); c.eb = ib; c.rb = role(ib);
        m_constraints.push_back(c); break;
    }
    case DimType::DistanceToLine: {
        // Point-on-line driving constraint: hold the point-like entity at unsigned
        // perpendicular distance v from the line (v == 0 -> on the axis).
        const bool aLine = (m_entities[m_selection[0]].type == SketchEntity::Type::Line);
        const int ip = m_selection[aLine ? 1 : 0];   // point-like (Point/Circle/Arc)
        const int il = m_selection[aLine ? 0 : 1];   // line
        c.type = SketchConstraintType::PointOnLine;
        c.ea = ip; c.ra = role(ip); c.eb = il; c.value = v;
        m_constraints.push_back(c); break;
    }
    default: break;   // None: no driving constraint recorded
    }
}

// Onshape-style live solve: enforce all accumulated driving constraints on the
// in-session entities immediately, so the viewport reflects the solved sketch as
// each dimension/constraint is added (not only at commit). The pre-edit geometry
// is the solver's initial guess, keeping convergence local and side-preserving.
void DesignSketchTool::resolve_live()
{
    resolve_live_drag(-1, SketchPointRole::P0);
}

void DesignSketchTool::resolve_live_drag(int dragged_ei, SketchPointRole dragged_role)
{
    const bool has = !m_constraints.empty();
    m_entity_conflict.assign(m_entities.size(), 0);
    if (has) {
        // Dragging a line endpoint must move ONLY that endpoint (changing the line's angle +
        // length), anchoring the other end — Onshape behaviour. Without this, a constraint on
        // the line (e.g. a length dimension or an inferred H/V) lets the solver relocate the
        // un-dragged endpoint too, so the whole line appears to shift. Temporarily Fix the
        // opposite endpoint for this drag-solve only (set_point already moved just the grabbed
        // point, so the other end's current coord is its anchor).
        std::vector<SketchEntityConstraintDef> cons = m_constraints;
        const bool line_end = dragged_ei >= 0 && dragged_ei < int(m_entities.size()) &&
                              m_entities[dragged_ei].type == SketchEntity::Type::Line &&
                              (dragged_role == SketchPointRole::P0 || dragged_role == SketchPointRole::P1);
        if (line_end) {
            SketchEntityConstraintDef fix;
            fix.type = SketchConstraintType::Fix;
            fix.ea   = dragged_ei;
            fix.ra   = (dragged_role == SketchPointRole::P0) ? SketchPointRole::P1
                                                             : SketchPointRole::P0;
            cons.push_back(fix);
        }
        const SketchSolveResult r = (dragged_ei >= 0)
            ? sketch_solve_drag(m_entities, cons, dragged_ei, dragged_role)
            : sketch_solve(m_entities, cons);
        m_dof      = r.dof;
        m_solve_ok = r.ok;
        // Flag every entity referenced by a conflicting constraint so render() can
        // tint it red (Onshape/SolveSpace over-constrained feedback).
        for (int bi : r.bad) {
            if (bi < 0 || bi >= int(m_constraints.size())) continue;
            const SketchEntityConstraintDef& c = m_constraints[bi];
            for (int e : {c.ea, c.eb, c.ec})
                if (e >= 0 && e < int(m_entity_conflict.size())) m_entity_conflict[e] = 1;
        }
    } else {
        m_dof = -1; m_solve_ok = true;
    }
    if (on_solve_state) on_solve_state(m_dof, m_solve_ok, has);
}

// ---- Onshape-style visual editing: feature grouping + handles -----------------

// Index of the Feature whose entity span contains ei, or -1 (last match wins so a
// later, tighter gesture shadows an earlier one if they ever overlap).
int DesignSketchTool::feature_of(int ei) const
{
    for (int i = int(m_features.size()) - 1; i >= 0; --i)
        if (ei >= m_features[i].begin && ei < m_features[i].end) return i;
    return -1;
}

// Open a Feature spanning the entities a single gesture is about to append. The
// [begin,end) range is closed in end_feature() once the gesture's entities are in.
void DesignSketchTool::begin_feature(FeatureKind kind)
{
    Feature f;
    f.kind  = kind;
    f.begin = int(m_entities.size());
    f.end   = f.begin;
    m_features.push_back(f);
    m_open_feature = int(m_features.size()) - 1;
}

// Close the open Feature: record its entity span end + the gesture's parametric
// anchors (centres / corners / half-width / sides) for later handle + dim rebuild.
void DesignSketchTool::end_feature(const Vec2d& c0, const Vec2d& c1, double param, int sides)
{
    if (m_open_feature < 0 || m_open_feature >= int(m_features.size())) return;
    Feature& f = m_features[m_open_feature];
    f.end   = int(m_entities.size());
    f.c0    = c0;
    f.c1    = c1;
    f.param = param;
    f.sides = sides;
    // Drop a degenerate feature (gesture appended nothing).
    if (f.end <= f.begin) m_features.pop_back();
    m_open_feature = -1;
}

// Forward decls: these ellipse helpers are defined further down but used by the
// handle/drag code above their definition.
static Vec2d  ellipse_point(const Vec2d& c, double a, double b, double phi, double t);
static double ellipse_param_of(const Vec2d& center, double a, double b, double phi, const Vec2d& q);

// Live handle set (A3: Line + Circle). Recomputed from solved geometry every frame,
// never persisted — so handles always track the current solve. Derived roles (here
// the circle RadiusHandle, which is NOT a serialized SketchPointRole) are what let a
// tool expose a parametric control its raw entity points don't carry. A4 routes a
// Select-mode drag through hit_test_handle + set_handle; later phases add the
// slot/rect/polygon/ellipse roles off the Feature groups.
std::vector<DesignSketchTool::Handle> DesignSketchTool::build_handles() const
{
    std::vector<Handle> hs;
    hs.reserve(m_entities.size() * 2);
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const SketchEntity& e = m_entities[i];
        switch (e.type) {
        case SketchEntity::Type::Line: {
            Handle a; a.role = HandleRole::P0; a.ei = int(i); a.pos = e.p0; hs.push_back(a);
            Handle b; b.role = HandleRole::P1; b.ei = int(i); b.pos = e.p1; hs.push_back(b);
            break;
        }
        case SketchEntity::Type::Circle: {
            Handle c; c.role = HandleRole::Center; c.ei = int(i); c.pos = e.center; hs.push_back(c);
            // RadiusHandle sits on the circle to the +x side of its centre; dragging it
            // (A4) edits the radius. Pure geometry, no constraint of its own.
            Handle r; r.role = HandleRole::RadiusHandle; r.ei = int(i);
            r.pos = e.center + Vec2d(e.radius, 0.0); hs.push_back(r);
            break;
        }
        case SketchEntity::Type::Ellipse:
        case SketchEntity::Type::EllipseArc: {
            // 3 grips: centre (translate), major-axis end (semi-major a + orientation phi),
            // minor-axis end (semi-minor b). a=e.radius, b=e.rminor, phi=e.rotation.
            // (EllipseArc also exposes its two endpoints via hit_test_point for sweep.)
            const Vec2d um(std::cos(e.rotation), std::sin(e.rotation));   // major dir
            const Vec2d un(-um.y(), um.x());                              // minor dir
            Handle c; c.role = HandleRole::Center; c.ei = int(i); c.pos = e.center; hs.push_back(c);
            Handle ma; ma.role = HandleRole::MajorAxis; ma.ei = int(i);
            ma.pos = e.center + um * e.radius; hs.push_back(ma);
            Handle mi; mi.role = HandleRole::MinorAxis; mi.ei = int(i);
            mi.pos = e.center + un * e.rminor; hs.push_back(mi);
            break;
        }
        case SketchEntity::Type::BSpline: {
            // One draggable grip per control pole; dragging a pole reshapes the curve.
            for (size_t k = 0; k < e.ctrl.size(); ++k) {
                Handle h; h.role = HandleRole::BSplineCtrl; h.ei = int(i);
                h.ctrl_index = int(k); h.pos = e.ctrl[k]; hs.push_back(h);
            }
            break;
        }
        default: break;   // Arc derived handles land in later chunks
        }
    }
    return hs;
}

// Nearest handle to plane-point p within tol. Ties broken by smallest distance.
bool DesignSketchTool::hit_test_handle(const Vec2d& p, double tol, Handle& out) const
{
    bool found = false;
    double best = tol;
    for (const Handle& h : build_handles()) {
        const double d = (h.pos - p).norm();
        if (d <= best) { best = d; out = h; out.hovered = true; found = true; }
    }
    return found;
}

// Recompute the hovered handle on a plain (no-button) move. Returns true ONLY when the
// hovered handle changes, so on_mouse forces a single repaint per transition rather than
// re-rendering on every motion event. A no-op (false) for non-Moving events.
bool DesignSketchTool::update_hover(GLCanvas3D& canvas, wxMouseEvent& evt)
{
    if (!evt.Moving()) return false;
    Vec2d p;
    screen_to_plane(canvas, evt, p);
    // Zoom-aware pick tolerance: project a point a few px away and measure in plane units.
    const Linef3 r2 = canvas.mouse_ray(Point(evt.GetX() + 6, evt.GetY()));
    const double tol = std::max(1e-3, (m_plane.project(r2.a, r2.vector()) - p).norm());
    const bool   had  = m_has_hover_handle;
    const Handle prev = m_hover_handle;
    Handle h;
    m_has_hover_handle = hit_test_handle(p, tol, h);
    if (m_has_hover_handle) m_hover_handle = h;
    return (had != m_has_hover_handle) ||
           (m_has_hover_handle && (prev.ei != h.ei || prev.role != h.role));
}

// Apply a handle drag (A4). Point handles (P0/P1/Center) move the entity point and
// pin it in the drag-solve so constraints settle around the cursor. The derived
// RadiusHandle isn't a solver point — it edits the circle radius directly, then a
// full re-solve lets any driving Radius/Diameter constraint reassert (an unconstrained
// radius is a free DoF, so the solver keeps the new value). Slot/rect/polygon/ellipse
// roles land in later phases (they rebuild their Feature group).
void DesignSketchTool::set_handle(const Handle& h, const Vec2d& target)
{
    if (h.ei < 0 || h.ei >= int(m_entities.size())) return;
    SketchEntity& e = m_entities[h.ei];
    switch (h.role) {
    case HandleRole::P0:
        set_point(h.ei, SketchPointRole::P0, target);     resolve_live_drag(h.ei, SketchPointRole::P0);     break;
    case HandleRole::P1:
        set_point(h.ei, SketchPointRole::P1, target);     resolve_live_drag(h.ei, SketchPointRole::P1);     break;
    case HandleRole::Center:
        set_point(h.ei, SketchPointRole::Center, target); resolve_live_drag(h.ei, SketchPointRole::Center); break;
    case HandleRole::RadiusHandle: {
        const double r = (target - e.center).norm();
        if (r > 1e-6) e.radius = r;
        resolve_live();
        break;
    }
    case HandleRole::MajorAxis: {
        // The major grip defines the major-axis vector: sets semi-major a + orientation phi.
        const Vec2d d = target - e.center;
        const double a = d.norm();
        if (a > 1e-6) {
            e.rotation = std::atan2(d.y(), d.x());
            e.radius   = std::max(a, e.rminor);   // keep OCCT invariant a >= b
        }
        if (e.type == SketchEntity::Type::EllipseArc) {   // endpoints ride the reshaped frame
            e.p0 = ellipse_point(e.center, e.radius, e.rminor, e.rotation, e.start_angle);
            e.p1 = ellipse_point(e.center, e.radius, e.rminor, e.rotation, e.end_angle);
        }
        resolve_live();
        break;
    }
    case HandleRole::MinorAxis: {
        // The minor grip sets semi-minor b = perpendicular distance to the major axis.
        const Vec2d um(std::cos(e.rotation), std::sin(e.rotation));
        const Vec2d un(-um.y(), um.x());
        const double b = std::abs((target - e.center).dot(un));
        if (b > 1e-6) e.rminor = std::min(b, e.radius);
        if (e.type == SketchEntity::Type::EllipseArc) {
            e.p0 = ellipse_point(e.center, e.radius, e.rminor, e.rotation, e.start_angle);
            e.p1 = ellipse_point(e.center, e.radius, e.rminor, e.rotation, e.end_angle);
        }
        resolve_live();
        break;
    }
    case HandleRole::BSplineCtrl: {
        // Move one control pole; the end poles mirror p0/p1 (kept in sync for picking).
        const int k = h.ctrl_index;
        if (k >= 0 && k < int(e.ctrl.size())) {
            e.ctrl[k] = target;
            if (k == 0)                       e.p0 = target;
            if (k == int(e.ctrl.size()) - 1)  e.p1 = target;
        }
        resolve_live();
        break;
    }
    default: break;
    }
}

// ---- Dimension tool (Mode::Dimension): click-to-place driving quotes ----------

bool DesignSketchTool::point_at(int ei, SketchPointRole role, Vec2d& out) const
{
    if (ei < 0 || ei >= int(m_entities.size())) return false;
    const SketchEntity& e = m_entities[ei];
    switch (role) {
    case SketchPointRole::P0:     out = e.p0;     return true;
    case SketchPointRole::P1:     out = e.p1;     return true;
    case SketchPointRole::Center: out = e.center; return true;
    }
    return false;
}

// Move an entity point to v. Lines/Points set the coordinate directly; a circle/
// arc centre translates the whole entity (arc endpoints ride along). Dragging an
// arc's endpoint is intentionally a no-op (it would redefine radius + angles).
void DesignSketchTool::set_point(int ei, SketchPointRole role, const Vec2d& v)
{
    if (ei < 0 || ei >= int(m_entities.size())) return;
    SketchEntity& e = m_entities[ei];
    switch (e.type) {
    case SketchEntity::Type::Line:
        if (role == SketchPointRole::P0)      e.p0 = v;
        else if (role == SketchPointRole::P1) e.p1 = v;
        break;
    case SketchEntity::Type::Point:
        e.p0 = v;
        break;
    case SketchEntity::Type::Circle:
        if (role == SketchPointRole::Center)  e.center = v;
        break;
    case SketchEntity::Type::Arc:
    case SketchEntity::Type::EllipseArc:
        if (role == SketchPointRole::Center) {
            const Vec2d d = v - e.center;     // rigid translate, keep radius/angles
            e.center = v; e.p0 += d; e.p1 += d;
        }
        break;
    case SketchEntity::Type::Ellipse:
        if (role == SketchPointRole::Center) { e.center = v; e.p0 = v; }
        break;
    case SketchEntity::Type::BSpline:
        // P0/P1 drag the end poles; Center rigidly translates the whole curve.
        if (role == SketchPointRole::P0 && !e.ctrl.empty()) { e.ctrl.front() = v; e.p0 = v; }
        else if (role == SketchPointRole::P1 && !e.ctrl.empty()) { e.ctrl.back() = v; e.p1 = v; }
        else if (role == SketchPointRole::Center) {
            const Vec2d d = v - (e.ctrl.empty() ? e.p0 : e.ctrl.front());
            for (auto& cp : e.ctrl) cp += d;
            e.p0 += d; e.p1 += d;
        }
        break;
    }
}

// Nearest entity *point* (endpoint / centre) within tol, with its role.
bool DesignSketchTool::hit_test_point(const Vec2d& p, double tol, int& ei, SketchPointRole& role) const
{
    double best = tol;
    bool found = false;
    auto consider = [&](int i, SketchPointRole r, const Vec2d& q) {
        const double d = (q - p).norm();
        if (d < best) { best = d; ei = i; role = r; found = true; }
    };
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const SketchEntity& e = m_entities[i];
        switch (e.type) {
        case SketchEntity::Type::Line:
            consider(int(i), SketchPointRole::P0, e.p0);
            consider(int(i), SketchPointRole::P1, e.p1);
            break;
        case SketchEntity::Type::Arc:
        case SketchEntity::Type::EllipseArc:
            consider(int(i), SketchPointRole::P0, e.p0);
            consider(int(i), SketchPointRole::P1, e.p1);
            consider(int(i), SketchPointRole::Center, e.center);
            break;
        case SketchEntity::Type::Circle:
        case SketchEntity::Type::Ellipse:
            consider(int(i), SketchPointRole::Center, e.center);
            break;
        case SketchEntity::Type::BSpline:
            consider(int(i), SketchPointRole::P0, e.p0);
            consider(int(i), SketchPointRole::P1, e.p1);
            break;
        case SketchEntity::Type::Point:
            consider(int(i), SketchPointRole::P0, e.p0);
            break;
        }
    }
    return found;
}

double DesignSketchTool::measure_dim(const DimAnnot& a) const
{
    Vec2d pa, pb;
    switch (a.kind) {
    case DimType::Length:
        return (a.ea >= 0 && a.ea < int(m_entities.size()))
                   ? (m_entities[a.ea].p1 - m_entities[a.ea].p0).norm() : 0.0;
    case DimType::Diameter:
        return (a.ea >= 0 && a.ea < int(m_entities.size())) ? 2.0 * m_entities[a.ea].radius : 0.0;
    case DimType::Radius:
        return (a.ea >= 0 && a.ea < int(m_entities.size())) ? m_entities[a.ea].radius : 0.0;
    case DimType::Angle: {
        // Single line: angle to the +X axis, normalised to [0,360). (Line-to-line angle
        // dimensions use the legacy dialog path.)
        if (a.ea < 0 || a.ea >= int(m_entities.size())) return 0.0;
        const SketchEntity& e = m_entities[a.ea];
        if (e.type != SketchEntity::Type::Line) return 0.0;
        const Vec2d d = e.p1 - e.p0;
        if (d.squaredNorm() < 1e-18) return 0.0;
        double deg = std::atan2(d.y(), d.x()) * 180.0 / M_PI;
        if (deg < 0.0) deg += 360.0;
        return deg;
    }
    case DimType::Distance:
        return (point_at(a.ea, a.ra, pa) && point_at(a.eb, a.rb, pb)) ? (pb - pa).norm() : 0.0;
    case DimType::DistanceToLine: {
        if (!point_at(a.ea, a.ra, pa) || a.eb < 0 || a.eb >= int(m_entities.size())) return 0.0;
        const SketchEntity& L = m_entities[a.eb];
        const Vec2d d = L.p1 - L.p0;
        const double n = d.norm();
        if (n < 1e-9) return 0.0;
        const Vec2d nrm(-d.y() / n, d.x() / n);
        return std::abs((pa - L.p0).dot(nrm));
    }
    default: return 0.0;
    }
}

// One driving constraint per (kind, operands) — update it in place rather than appending a
// second one every time its value is edited.
//
// Re-typing a quote used to push a duplicate alongside the original: draw a line and accept its
// length, and you get Distance(P0,P1)=64.9; click the quote later and type 30, and you get a
// SECOND Distance on the same two points asking for 30. That is over-constrained by
// construction, so the solver reported "Conflicting constraints" and refused to move anything —
// the number on screen changed and the geometry did not, which reads as the edit being ignored.
// A line carrying no dimension yet was unaffected, which is what made it look intermittent.
//
// Operand order is ignored: a Distance from A to B is the same constraint as B to A, and so is
// an Angle. Returns the index, so callers can keep the annotation's `con` link pointing at the
// constraint that is actually live — which is what makes the next edit an update too.
int DesignSketchTool::upsert_constraint(const SketchEntityConstraintDef& c)
{
    auto same_operands = [&](const SketchEntityConstraintDef& o) {
        if (o.ea == c.ea && o.ra == c.ra && o.eb == c.eb && o.rb == c.rb) return true;
        return o.ea == c.eb && o.ra == c.rb && o.eb == c.ea && o.rb == c.ra;
    };
    for (int i = 0; i < int(m_constraints.size()); ++i)
        if (m_constraints[i].type == c.type && same_operands(m_constraints[i])) {
            m_constraints[i] = c;
            return i;
        }
    m_constraints.push_back(c);
    return int(m_constraints.size()) - 1;
}

// The same rule for the visible annotation: one quote per (kind, operands), so repeated edits
// do not stack labels on top of each other reading different values.
int DesignSketchTool::upsert_dimension(const DimAnnot& a)
{
    for (int i = 0; i < int(m_dimensions.size()); ++i) {
        const DimAnnot& o = m_dimensions[i];
        if (o.kind == a.kind && ((o.ea == a.ea && o.eb == a.eb) || (o.ea == a.eb && o.eb == a.ea))) {
            const Vec2d keep = m_dimensions[i].label_pos;   // don't teleport a placed label
            m_dimensions[i] = a;
            m_dimensions[i].label_pos = keep;
            return i;
        }
    }
    m_dimensions.push_back(a);
    return int(m_dimensions.size()) - 1;
}

SketchEntityConstraintDef DesignSketchTool::constraint_for(const DimAnnot& a) const
{
    SketchEntityConstraintDef c;
    switch (a.kind) {
    case DimType::Length:
        c.type = SketchConstraintType::Distance;
        c.ea = a.ea; c.ra = SketchPointRole::P0;
        c.eb = a.ea; c.rb = SketchPointRole::P1; c.value = a.value;
        break;
    case DimType::Diameter: c.type = SketchConstraintType::Diameter; c.ea = a.ea; c.value = a.value; break;
    case DimType::Radius:   c.type = SketchConstraintType::Radius;   c.ea = a.ea; c.value = a.value; break;
    case DimType::Distance:
        if (a.value < 1e-9) c.type = SketchConstraintType::Coincident;
        else              { c.type = SketchConstraintType::Distance; c.value = a.value; }
        c.ea = a.ea; c.ra = a.ra; c.eb = a.eb; c.rb = a.rb;
        break;
    case DimType::DistanceToLine:
        c.type = SketchConstraintType::PointOnLine;
        c.ea = a.ea; c.ra = a.ra; c.eb = a.eb; c.value = a.value;
        break;
    default: break;
    }
    return c;
}

// Measure the just-picked dimension, append its driving constraint, live-solve, and
// fire the value-card callback so the user can override the value.
int DesignSketchTool::place_dimension(DimAnnot a)
{
    a.value = measure_dim(a);
    a.con   = upsert_constraint(constraint_for(a));
    const int di = upsert_dimension(a);
    resolve_live();
    open_value_editor(di);
    return m_pending_dim;
}

// Nearest placed-dimension label within tol (uses the centre cached by render).
int DesignSketchTool::hit_test_dimension(const Vec2d& p, double tol) const
{
    double best = tol;
    int bi = -1;
    for (size_t i = 0; i < m_dimensions.size(); ++i) {
        const double d = (m_dimensions[i].label_pos - p).norm();
        if (d < best) { best = d; bi = int(i); }
    }
    return bi;
}

// Reopen the value editor on an existing dimension (click/double-click its label).
void DesignSketchTool::edit_dimension(int di)
{
    if (di < 0 || di >= int(m_dimensions.size())) return;
    open_value_editor(di);
}

// Representative plane anchor for a dimension's value editor: the cached label centre
// once render has computed it, otherwise a geometric midpoint (length/distance) or the
// entity centre (radius/diameter).
Vec2d DesignSketchTool::dim_anchor(const DimAnnot& a) const
{
    if (a.label_pos.squaredNorm() > 1e-12) return a.label_pos;
    Vec2d pa, pb;
    if ((a.kind == DimType::Radius || a.kind == DimType::Diameter) &&
        point_at(a.ea, SketchPointRole::Center, pa))
        return pa;
    const bool ga = point_at(a.ea, a.ra, pa);
    const bool gb = point_at(a.eb, a.rb, pb);
    if (ga && gb) return 0.5 * (pa + pb);
    if (ga)       return pa;
    if (gb)       return pb;
    return Vec2d(0, 0);
}

// Open the in-canvas value editor on dimension `di`. Projects the dimension's anchor
// to screen pixels and hands the host (DesignCanvas) a commit/cancel pair that drive
// the value through the existing set/cancel_dimension_value path. Falls back to the
// modal pick-complete callback when no inline-edit host is wired.
void DesignSketchTool::open_value_editor(int di)
{
    if (di < 0 || di >= int(m_dimensions.size())) return;
    m_pending_dim = di;
    if (!on_inline_edit) {
        if (on_dimension_pick_complete) on_dimension_pick_complete(m_dimensions[di].value);
        return;
    }
    const DimAnnot& a = m_dimensions[di];
    // Anchor the field OVER the dimension (project its label/anchor to the viewport), same as
    // the draw-then-edit tools and Constrain mode; fall back to the click point if it projects
    // off-screen.
    wxPoint px(m_last_mouse_x, m_last_mouse_y);
    const Camera& cam = wxGetApp().plater()->get_camera();
    const wxPoint lp = world_to_screen_px(cam, m_plane.to_world(dim_anchor(a)));
    const std::array<int, 4>& vp = cam.get_viewport();
    if (lp.x >= vp[0] && lp.y >= vp[1] && lp.x <= vp[0] + vp[2] && lp.y <= vp[1] + vp[3])
        px = lp;
    on_inline_edit(px, a.value, dimtype_title(a.kind),
                   [this](double v) { set_dimension_value(v); },
                   [this]()         { cancel_dimension_value(); });
}

// In-canvas editor for a line's angle-to-horizontal. Unlike length/radius, a single
// line's angle has no libslvs constraint here (SLVS_C_ANGLE is line-to-line), so the
// commit rotates the segment GEOMETRICALLY about P0 to the typed degrees, then re-solves
// — the angle is a free DoF, so the solver keeps the new orientation (mirrors the radius
// handle). Length-constrained lines keep their length.
void DesignSketchTool::open_angle_editor(int ei)
{
    if (ei < 0 || ei >= int(m_entities.size())) return;
    if (m_entities[ei].type != SketchEntity::Type::Line) return;
    if (!on_inline_edit) return;
    DimAnnot a; a.kind = DimType::Angle; a.ea = ei;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, measure_dim(a), "Angle",
                   [this, ei](double deg) { set_line_angle(ei, deg); },
                   []()                   {});
}

void DesignSketchTool::set_line_angle(int ei, double deg)
{
    if (ei < 0 || ei >= int(m_entities.size())) return;
    SketchEntity& e = m_entities[ei];
    if (e.type != SketchEntity::Type::Line) return;
    const double L = (e.p1 - e.p0).norm();
    if (L < 1e-9) return;
    drop_orientation_constraints(ei, ei + 1);   // a typed angle overrides an inferred H/V
    const double r = deg * M_PI / 180.0;
    e.p1 = e.p0 + Vec2d(std::cos(r), std::sin(r)) * L;   // rotate about P0, keep length
    resolve_live();
}

// Open the queued scalar quote at m_autoedit_dim_idx. Commit appends the driving dimension
// AND advances to the next queued quote (deferred via CallAfter so the single SketchInlineEditor
// fully unwinds its Enter handler before being reopened). Cancel (Esc) aborts the whole chain —
// the shape is kept as drawn. This is what lets a rectangle edit Width THEN Height, a slot its
// centre-distance THEN width, etc., instead of only the first dimension.
void DesignSketchTool::open_next_autoedit_dim()
{
    if (!on_inline_edit || !m_active) { m_autoedit_dim_idx = -1; return; }
    if (m_autoedit_dim_idx < 0 || m_autoedit_dim_idx >= int(m_autoedit_dims.size())) {
        m_autoedit_dim_idx = -1;
        return;
    }
    const AutoEditStep step = m_autoedit_dims[m_autoedit_dim_idx];
    // Anchor the field OVER this dimension's label (project its plane-coords centre to the
    // viewport), not at the last cursor spot — otherwise each field pops up in an unrelated
    // screen position. Fall back to the cursor if the label projects off-screen.
    // Anchor the field OVER this dimension's label (project its plane-coords centre to the
    // viewport). A label can project OFF-screen (near-degenerate perspective divide when the
    // sketch plane is viewed at a grazing angle), in which case we fall back to the cursor —
    // but staggered by step index, so a shape's successive fields (rrect W/H/R) don't all
    // stack on the exact same pixel and hide each other.
    const Camera& cam = wxGetApp().plater()->get_camera();
    const wxPoint lp = world_to_screen_px(cam, m_plane.to_world(step.label));
    const std::array<int, 4>& vp = cam.get_viewport();
    wxPoint px(m_last_mouse_x, m_last_mouse_y + m_autoedit_dim_idx * 34);
    if (lp.x >= vp[0] && lp.y >= vp[1] && lp.x <= vp[0] + vp[2] && lp.y <= vp[1] + vp[3])
        px = lp;
    on_inline_edit(px, step.value, step.title,
        [this, step](double v) {                 // commit: apply this dimension, then next
            if (step.apply) step.apply(v);
            ++m_autoedit_dim_idx;
            wxGetApp().CallAfter([this] { open_next_autoedit_dim(); });
        },
        [this]() { m_autoedit_dim_idx = -1; });   // cancel: keep as drawn, stop the chain
}

// Polyline draw-then-edit: after each click places a chain vertex, refine THAT segment's
// Length then Angle through the same AutoEditStep queue. The polyline batch-creates its
// entities only when the chain ends, so here we edit the pending m_points vertex directly
// (geometric): Length rescales it along the segment, Angle rotates it about the previous
// vertex. The next click continues from the adjusted vertex. Same field/anchor/focus path as
// every other tool — Enter advances Length->Angle, Esc keeps the segment as clicked.
void DesignSketchTool::arm_polyline_segment_edit()
{
    const int k = int(m_points.size()) - 1;          // index of the just-placed vertex
    if (k < 1 || !on_inline_edit) return;
    const Vec2d a   = m_points[k - 1];               // segment anchor (previous vertex)
    const Vec2d mid = 0.5 * (a + m_points[k]);       // label anchor = segment midpoint
    const Vec2d d   = m_points[k] - a;
    const double L  = d.norm();
    if (L < 1e-9) return;
    double deg = std::atan2(d.y(), d.x()) * 180.0 / M_PI; if (deg < 0.0) deg += 360.0;

    m_autoedit_dims.clear();
    m_autoedit_dims.push_back({ mid, L, [this, k, a](double len) {     // Length
        if (k < int(m_points.size())) {
            Vec2d dd = m_points[k] - a; const double n = dd.norm();
            if (n > 1e-9 && len > 1e-9) m_points[k] = a + (len / n) * dd;
        }
    }, {}, "Length" });
    m_autoedit_dims.push_back({ mid, deg, [this, k, a](double dg) {     // Angle
        if (k < int(m_points.size())) {
            const double len = (m_points[k] - a).norm();
            const double r   = dg * M_PI / 180.0;
            m_points[k] = a + len * Vec2d(std::cos(r), std::sin(r));
        }
    }, {}, "Angle" });
    m_autoedit_dim_idx = 0;
    wxGetApp().CallAfter([this] { open_next_autoedit_dim(); });
}

std::string DesignSketchTool::dimtype_title(DimType k) const {
    switch (k) {
        case DimType::Length:         return "Length";
        case DimType::Diameter:       return "Diameter";
        case DimType::Radius:         return "Radius";
        case DimType::Angle:          return "Angle";
        case DimType::Distance:       return "Distance";
        case DimType::DistanceToLine: return "Distance";
        default:                      return "Value";
    }
}

// Draw-then-edit dispatcher: mirror the Select-mode quote-click logic, but target the
// freshly-drawn selection's PRIMARY value and use the tentative (clean-cancel) path for
// scalar quotes. Runs after render_live_quotes, so the live-quote state is populated.
void DesignSketchTool::open_primary_autoedit()
{
    if (!on_inline_edit || m_awaiting_length) return;   // no host, or a field is already open
    if (!m_active) return;                              // session ended before the deferred tick

    // Build ONE ordered list of edit steps covering EVERY characteristic dimension of the
    // freshly-drawn shape — scalar quotes (constraint-based) AND geometric editors — so every
    // 2D tool behaves like the rectangle: a linear sequence of value fields, each over its own
    // label, Enter advances to the next, Esc keeps the shape as drawn. (Line keeps its own
    // dedicated length field; Polyline/BSpline/Point have no two-click dimension set.)
    m_autoedit_dims.clear();

    // (1) Scalar quotes: rect Width+Height, slot Distance+Radius, circle/arc Radius, line
    //     Length. The lone Angle quote (only a single Line emits one) becomes a GEOMETRIC
    //     orientation step (set_line_angle, like the polygon angle) — so the Line tool gets
    //     Length THEN Angle, same as the rectangle gets W then H.
    for (const DimAnnot& q : m_live_quotes) {
        if (q.kind == DimType::Angle) {
            const int ei = q.ea;
            m_autoedit_dims.push_back({ q.label_pos, measure_dim(q),
                [this, ei](double v) { set_line_angle(ei, v); }, { ei }, "Angle" });
            continue;
        }
        DimAnnot a = q;
        a.value = measure_dim(a);
        m_autoedit_dims.push_back({ a.label_pos, a.value,
            [this, a](double v) mutable {
                a.value = v;
                a.con   = upsert_constraint(constraint_for(a));
                upsert_dimension(a);
                resolve_live();
            }, { a.ea, a.eb }, dimtype_title(a.kind) });
    }

    // (2) Geometric editors (mutate geometry directly, no constraint). Each reads the CURRENT
    //     feature/entity state inside apply(), so sequential edits compose correctly.
    // Entities to highlight = the feature's whole [begin,end) span, so editing any of its
    // characteristic dims lights up the shape it drives.
    auto span = [this](int fi) {
        std::vector<int> v;
        if (fi >= 0 && fi < int(m_features.size()))
            for (int k = m_features[fi].begin; k < m_features[fi].end; ++k) v.push_back(k);
        return v;
    };
    if (m_live_poly_fi >= 0) {
        const Feature& f = m_features[m_live_poly_fi];
        const int fi = m_live_poly_fi;
        if (f.begin >= 0 && f.begin < int(m_entities.size())) {
            const double side = (m_entities[f.begin].p1 - m_entities[f.begin].p0).norm();
            const Vec2d  sp   = m_entities[f.begin].p0 - f.c0;
            double deg = std::atan2(sp.y(), sp.x()) * 180.0 / M_PI; if (deg < 0.0) deg += 360.0;
            m_autoedit_dims.push_back({ m_live_poly_side_label,  side, [this, fi](double v){ set_polygon_side(fi, v); }, span(fi), "Side" });
            m_autoedit_dims.push_back({ m_live_poly_angle_label, deg,  [this, fi](double v){ set_polygon_angle(fi, v); }, span(fi), "Angle" });
        }
    }
    if (m_live_rrect_fi >= 0) {
        const Feature& f = m_features[m_live_rrect_fi];
        const int fi = m_live_rrect_fi;
        const double w = std::abs(f.c1.x() - f.c0.x()), h = std::abs(f.c1.y() - f.c0.y()), r = f.param;
        auto rr_w = [this, fi](double v){ const Feature& g = m_features[fi]; set_rounded_rect(fi, v, std::abs(g.c1.y()-g.c0.y()), g.param); };
        auto rr_h = [this, fi](double v){ const Feature& g = m_features[fi]; set_rounded_rect(fi, std::abs(g.c1.x()-g.c0.x()), v, g.param); };
        auto rr_r = [this, fi](double v){ const Feature& g = m_features[fi]; set_rounded_rect(fi, std::abs(g.c1.x()-g.c0.x()), std::abs(g.c1.y()-g.c0.y()), v); };
        m_autoedit_dims.push_back({ m_live_rrect_w_label, w, rr_w, span(fi), "Width" });
        m_autoedit_dims.push_back({ m_live_rrect_h_label, h, rr_h, span(fi), "Height" });
        m_autoedit_dims.push_back({ m_live_rrect_r_label, r, rr_r, span(fi), "Radius" });
    }
    if (m_live_aslot_fi >= 0) {
        const Feature& f = m_features[m_live_aslot_fi];
        const int fi = m_live_aslot_fi;
        const double Rc = (f.c1 - f.c0).norm(), fw = 2.0 * f.param;
        m_autoedit_dims.push_back({ m_live_aslot_r_label, Rc, [this, fi](double v){ const Feature& g = m_features[fi]; set_arc_slot(fi, v, g.param); }, span(fi), "Radius" });
        m_autoedit_dims.push_back({ m_live_aslot_w_label, fw, [this, fi](double v){ const Feature& g = m_features[fi]; set_arc_slot(fi, (g.c1-g.c0).norm(), std::max(1e-3, v*0.5)); }, span(fi), "Width" });
    }
    if (m_live_slot_fi >= 0) {
        const Feature& f = m_features[m_live_slot_fi];
        const int fi = m_live_slot_fi;
        // Slot dims, in order: (1) inter-centre distance, (2) radius (= half-width), (3) angle.
        const Vec2d  d  = f.c1 - f.c0;
        const double Lc = d.norm();
        double deg = std::atan2(d.y(), d.x()) * 180.0 / M_PI; if (deg < 0.0) deg += 360.0;
        m_autoedit_dims.push_back({ m_live_slot_len_label, Lc, [this, fi](double v){ const Feature& g = m_features[fi]; set_slot(fi, v, g.param); }, span(fi), "Length" });
        m_autoedit_dims.push_back({ m_live_slot_w_label, f.param, [this, fi](double v){ const Feature& g = m_features[fi]; set_slot(fi, (g.c1-g.c0).norm(), std::max(1e-3, v)); }, span(fi), "Radius" });
        m_autoedit_dims.push_back({ m_live_slot_angle_label, deg, [this, fi](double v){ set_slot_angle(fi, v); }, span(fi), "Angle" });
    }
    if (m_live_arc_ei >= 0) {   // arc Radius is already a scalar step above; add its sweep angle
        const int ei = m_live_arc_ei;
        const SketchEntity& e = m_entities[ei];
        const double swdeg = std::abs(e.end_angle - e.start_angle) * 180.0 / M_PI;
        m_autoedit_dims.push_back({ m_live_arc_angle_label, swdeg, [this, ei](double v){ set_arc_sweep(ei, v); }, { ei }, "Angle" });
    }
    if (m_live_ellipse_ei >= 0) {
        const int ei = m_live_ellipse_ei;
        const SketchEntity& e = m_entities[ei];
        m_autoedit_dims.push_back({ m_live_ellipse_major_label, e.radius, [this, ei](double v){ set_ellipse_axis(ei, true,  v); }, { ei }, "Major" });
        m_autoedit_dims.push_back({ m_live_ellipse_minor_label, e.rminor, [this, ei](double v){ set_ellipse_axis(ei, false, v); }, { ei }, "Minor" });
        if (e.type == SketchEntity::Type::EllipseArc) {   // + included sweep
            const double swdeg = std::abs(e.end_angle - e.start_angle) * 180.0 / M_PI;
            m_autoedit_dims.push_back({ m_live_ellipsearc_sweep_label, swdeg,
                [this, ei](double v){ set_ellipsearc_sweep(ei, v); }, { ei }, "Angle" });
        }
    }
    if (m_live_obrect_fi >= 0) {   // oblique rect: W,H already added as scalars; + orientation
        const int fi = m_live_obrect_fi;
        const Feature& f = m_features[fi];
        const SketchEntity& e0 = m_entities[f.begin];
        double adeg = std::atan2(e0.p1.y() - e0.p0.y(), e0.p1.x() - e0.p0.x()) * 180.0 / M_PI;
        if (adeg < 0.0) adeg += 360.0;
        m_autoedit_dims.push_back({ m_live_obrect_angle_label, adeg,
            [this, fi](double v){ set_rect_angle(fi, v); }, span(fi), "Angle" });
    }

    if (!m_autoedit_dims.empty()) {
        m_autoedit_dim_idx = 0;
        open_next_autoedit_dim();
    }
}

// A regular polygon is N raw lines with no centre entity, so (like the line angle) its
// side and orientation edits transform the whole loop GEOMETRICALLY about its centre.
void DesignSketchTool::open_polygon_side_editor(int fi)
{
    if (fi < 0 || fi >= int(m_features.size()) || !on_inline_edit) return;
    const Feature& f = m_features[fi];
    if (f.begin < 0 || f.begin >= int(m_entities.size())) return;
    const double side = (m_entities[f.begin].p1 - m_entities[f.begin].p0).norm();
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, side, "Side",
                   [this, fi](double v) { set_polygon_side(fi, v); },
                   []()                 {});
}

void DesignSketchTool::open_polygon_angle_editor(int fi)
{
    if (fi < 0 || fi >= int(m_features.size()) || !on_inline_edit) return;
    const Feature& f = m_features[fi];
    if (f.begin < 0 || f.begin >= int(m_entities.size())) return;
    const Vec2d sp = m_entities[f.begin].p0 - f.c0;          // centre -> vertex0 spoke
    double deg = std::atan2(sp.y(), sp.x()) * 180.0 / M_PI;
    if (deg < 0.0) deg += 360.0;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, deg, "Angle",
                   [this, fi](double v) { set_polygon_angle(fi, v); },
                   []()                 {});
}

// Scale the loop uniformly about its centre so an edge equals `side`. For a regular
// n-gon, circumradius R = side / (2 sin(pi/n)).
void DesignSketchTool::set_polygon_side(int fi, double side)
{
    if (fi < 0 || fi >= int(m_features.size()) || side < 1e-6) return;
    const int n = std::max(3, m_features[fi].sides);
    const double R = side / (2.0 * std::sin(M_PI / double(n)));
    set_polygon_radius(fi, R);
}

// Remove orientation constraints touching [begin,end). A pure rotation makes inferred
// per-edge Horizontal/Vertical (and Parallel/Perp/Angle/Lock) inconsistent, so leaving
// them in would make resolve_live collapse the shape to satisfy them.
void DesignSketchTool::drop_orientation_constraints(int begin, int end)
{
    using CT = SketchConstraintType;
    auto orient = [](CT t) {
        return t == CT::Horizontal || t == CT::Vertical || t == CT::Parallel ||
               t == CT::Perpendicular || t == CT::Angle || t == CT::LockX || t == CT::LockY;
    };
    auto in = [&](int e) { return e >= begin && e < end; };
    std::vector<int> remap(m_constraints.size(), -1);
    std::vector<SketchEntityConstraintDef> kept;
    kept.reserve(m_constraints.size());
    for (int i = 0; i < int(m_constraints.size()); ++i) {
        const SketchEntityConstraintDef& c = m_constraints[i];
        if (orient(c.type) && (in(c.ea) || in(c.eb))) continue;   // drop
        remap[i] = int(kept.size());
        kept.push_back(c);
    }
    if (kept.size() == m_constraints.size()) return;             // nothing dropped
    m_constraints.swap(kept);
    for (DimAnnot& a : m_dimensions)                              // fix cached con indices
        if (a.con >= 0) a.con = (a.con < int(remap.size())) ? remap[a.con] : -1;
}

void DesignSketchTool::drop_constraints_referencing(int ei)
{
    std::vector<int> remap(m_constraints.size(), -1);
    std::vector<SketchEntityConstraintDef> kept;
    kept.reserve(m_constraints.size());
    for (int i = 0; i < int(m_constraints.size()); ++i) {
        const SketchEntityConstraintDef& c = m_constraints[i];
        if (c.ea == ei || c.eb == ei || c.ec == ei) continue;    // drop refs to the cut entity
        remap[i] = int(kept.size());
        kept.push_back(c);
    }
    if (kept.size() == m_constraints.size()) return;
    m_constraints.swap(kept);
    for (DimAnnot& a : m_dimensions)
        if (a.con >= 0) a.con = (a.con < int(remap.size())) ? remap[a.con] : -1;
}

// Onshape scissors on the live sketch: cut the picked entity at its nearest intersection.
// trim_entity/extend_entity mutate the subject in place (slide one endpoint) given the other
// entities + the pick point — no entity is added/removed, so indices stay stable.
bool DesignSketchTool::apply_live_trim(const Vec2d& p, double tol, bool extend)
{
    double best = 1e30; int bi = -1;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const double d = entity_pick_dist(p, m_entities[i]);
        if (d < best) { best = d; bi = int(i); }
    }
    if (bi < 0 || best > tol) return false;
    using Ty = SketchEntity::Type;
    const Ty st = m_entities[bi].type;
    const bool subject_ok = extend ? (st == Ty::Line || st == Ty::Arc)
                                   : (st == Ty::Line || st == Ty::Arc || st == Ty::Circle);
    if (!subject_ok) return false;
    std::vector<SketchEntity> others;
    others.reserve(m_entities.size());
    for (size_t i = 0; i < m_entities.size(); ++i)
        if (int(i) != bi) others.push_back(m_entities[i]);
    const bool ok = extend ? SketchEngine::extend_entity(m_entities[bi], others, p)
                           : SketchEngine::trim_entity(m_entities[bi], others, p);
    if (!ok) return false;
    drop_constraints_referencing(bi);   // the slid endpoint invalidates this entity's constraints
    return true;
}

// Hover preview for the Trim/Extend scissors: replay apply_live_trim's pick and the engine
// cut on a COPY of the subject, then diff the copy against the original to recover the exact
// sub-portion a click would remove (Trim) / add (Extend). Pure computation, mutates nothing.
bool DesignSketchTool::compute_trim_preview(const Vec2d& p, double tol, bool extend,
                                            int& subject_ei, std::vector<Vec2d>& removed_poly) const
{
    subject_ei = -1;
    removed_poly.clear();

    double best = 1e30; int bi = -1;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const double d = entity_pick_dist(p, m_entities[i]);
        if (d < best) { best = d; bi = int(i); }
    }
    if (bi < 0 || best > tol) return false;

    using Ty = SketchEntity::Type;
    const Ty st = m_entities[bi].type;
    const bool subject_ok = extend ? (st == Ty::Line || st == Ty::Arc)
                                   : (st == Ty::Line || st == Ty::Arc || st == Ty::Circle);
    if (!subject_ok) return false;

    std::vector<SketchEntity> others;
    others.reserve(m_entities.size());
    for (size_t i = 0; i < m_entities.size(); ++i)
        if (int(i) != bi) others.push_back(m_entities[i]);

    const SketchEntity& orig = m_entities[bi];
    SketchEntity trimmed = orig;                          // cut on the copy, never the live entity
    const bool ok = extend ? SketchEngine::extend_entity(trimmed, others, p)
                           : SketchEngine::trim_entity(trimmed, others, p);
    if (!ok) return false;

    // The highlighted portion is where `trimmed` differs from `orig`: the dropped sub-segment
    // (Trim) or the grown one (Extend). Rebuild it as a temp entity and sample its polyline.
    const double EPS2 = 1e-14;     // squared plane-unit endpoint tolerance
    const double AEPS = 1e-7;      // radian tolerance
    bool closed = false;

    if (orig.type == Ty::Line) {
        SketchEntity seg = orig;
        if ((trimmed.p0 - orig.p0).squaredNorm() > EPS2) {
            seg.p0 = orig.p0; seg.p1 = trimmed.p0;        // start endpoint moved
        } else if ((trimmed.p1 - orig.p1).squaredNorm() > EPS2) {
            seg.p0 = trimmed.p1; seg.p1 = orig.p1;        // end endpoint moved
        } else {
            return false;                                  // nothing changed
        }
        removed_poly = entity_polyline(seg, closed);
    } else if (orig.type == Ty::Arc) {
        SketchEntity arc = orig;                           // same centre/radius
        if (std::abs(trimmed.start_angle - orig.start_angle) > AEPS) {
            arc.start_angle = orig.start_angle; arc.end_angle = trimmed.start_angle;
        } else if (std::abs(trimmed.end_angle - orig.end_angle) > AEPS) {
            arc.start_angle = trimmed.end_angle; arc.end_angle = orig.end_angle;
        } else {
            return false;
        }
        removed_poly = entity_polyline(arc, closed);
    } else if (orig.type == Ty::Circle) {
        // Trim opens the Circle into the kept Arc [start,end]; the removed gap is its
        // complement, swept from the kept arc's end round to its start.
        if (trimmed.type != Ty::Arc) return false;
        SketchEntity gap = orig;
        gap.type        = Ty::Arc;
        gap.start_angle = trimmed.end_angle;
        gap.end_angle   = trimmed.start_angle + 2.0 * M_PI;
        removed_poly = entity_polyline(gap, closed);
    } else {
        return false;
    }

    if (removed_poly.size() < 2) return false;
    subject_ei = bi;
    return true;
}

// Rotate the whole loop about its centre so the centre->vertex0 spoke points at `deg`
// (degrees from +X).
void DesignSketchTool::set_polygon_angle(int fi, double deg)
{
    if (fi < 0 || fi >= int(m_features.size())) return;
    Feature& f = m_features[fi];
    if (f.begin < 0 || f.begin >= int(m_entities.size())) return;
    const Vec2d c = f.c0;
    const Vec2d sp = m_entities[f.begin].p0 - c;          // centre -> vertex0
    if (sp.squaredNorm() < 1e-12) return;
    const double cur = std::atan2(sp.y(), sp.x());
    const double da  = deg * M_PI / 180.0 - cur;
    drop_orientation_constraints(f.begin, f.end);         // rotation invalidates edge H/V
    const double ca = std::cos(da), sa = std::sin(da);
    // -> Vec2d is REQUIRED: an auto return deduces an Eigen expression template that holds
    // a reference to the destroyed `c + Vec2d(...)` temporary (dangling -> garbage coords).
    auto rot = [&](const Vec2d& pt) -> Vec2d { const Vec2d r = pt - c;
        return c + Vec2d(r.x() * ca - r.y() * sa, r.x() * sa + r.y() * ca); };
    for (int i = f.begin; i < f.end && i < int(m_entities.size()); ++i) {
        SketchEntity& e = m_entities[i];
        if (e.type != SketchEntity::Type::Line) continue;
        e.p0 = rot(e.p0); e.p1 = rot(e.p1);
    }
    // Do NOT re-solve: the rotated geometry is already a correct regular polygon, and the
    // inferred loop (redundant Coincident, now with no H/V anchor) collapses to a point
    // under libslvs. We only drop the stale edge H/V (above) so a later commit-solve stays
    // sane; the display renders the mutated entities directly.
}

// Drag a polygon vertex keeping the loop regular: scale + rotate the whole polygon
// about its centroid so the grabbed vertex lands on `target`. This adjusts both the
// circumradius (|target-centroid|) and the orientation (its direction) at once.
void DesignSketchTool::drag_polygon_vertex(int fi, int ei, SketchPointRole role, const Vec2d& target)
{
    if (fi < 0 || fi >= int(m_features.size())) return;
    Feature& f = m_features[fi];
    if (f.begin < 0 || f.end > int(m_entities.size())) return;
    // Centroid of the loop = the regular polygon's centre (robust if it was moved).
    Vec2d c(0, 0); int n = 0;
    for (int i = f.begin; i < f.end; ++i)
        if (m_entities[i].type == SketchEntity::Type::Line) { c += m_entities[i].p0; ++n; }
    if (n == 0) return;
    c /= double(n);
    Vec2d vpos;
    if (!point_at(ei, role, vpos)) return;
    const Vec2d cur = vpos - c;                 // current grabbed-vertex spoke
    const Vec2d tgt = target - c;               // desired spoke
    const double curR = cur.norm(), newR = tgt.norm();
    if (curR < 1e-9 || newR < 1e-6) return;
    const double s  = newR / curR;
    const double da = std::atan2(tgt.y(), tgt.x()) - std::atan2(cur.y(), cur.x());
    const double ca = std::cos(da), sa = std::sin(da);
    auto tf = [&](const Vec2d& p) -> Vec2d { const Vec2d r = (p - c) * s;   // -> Vec2d: avoid
        return c + Vec2d(r.x() * ca - r.y() * sa, r.x() * sa + r.y() * ca); };  // Eigen dangling

    for (int i = f.begin; i < f.end; ++i) {
        SketchEntity& e = m_entities[i];
        if (e.type != SketchEntity::Type::Line) continue;
        e.p0 = tf(e.p0); e.p1 = tf(e.p1);
    }
    f.c0 = c; f.param = newR;
    drop_orientation_constraints(f.begin, f.end);   // the drag rotates -> edge H/V invalid
    // No re-solve (see set_polygon_angle): the transformed geometry is already a correct
    // regular polygon; solving the anchorless redundant loop would collapse it.
}

void DesignSketchTool::set_polygon_radius(int fi, double R)
{
    if (fi < 0 || fi >= int(m_features.size()) || R < 1e-6) return;
    Feature& f = m_features[fi];
    if (f.begin < 0 || f.begin >= int(m_entities.size())) return;
    const Vec2d  c    = f.c0;
    const double curR = (m_entities[f.begin].p0 - c).norm();
    if (curR < 1e-9) return;
    const double s = R / curR;                       // uniform scale about the centre
    for (int i = f.begin; i < f.end && i < int(m_entities.size()); ++i) {
        SketchEntity& e = m_entities[i];
        if (e.type != SketchEntity::Type::Line) continue;
        e.p0 = c + (e.p0 - c) * s;
        e.p1 = c + (e.p1 - c) * s;
    }
    f.param = R;
    // Geometric only (no solve): consistent with the rotation edits, and avoids collapsing
    // the loop if its H/V anchors were already dropped by a prior rotation.
}

void DesignSketchTool::open_arc_angle_editor(int ei)
{
    if (ei < 0 || ei >= int(m_entities.size()) || !on_inline_edit) return;
    const SketchEntity& e = m_entities[ei];
    if (e.type != SketchEntity::Type::Arc) return;
    double swdeg = std::abs(e.end_angle - e.start_angle) * 180.0 / M_PI;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, swdeg, "Angle",
                   [this, ei](double v) { set_arc_sweep(ei, v); },
                   []()                 {});
}

// Set the arc's included (sweep) angle to `deg`, keeping the start point and radius fixed
// and rotating the end point about the centre. Geometric (SLVS angle is line-to-line), so
// no re-solve; the mutated entity renders directly. Direction (CCW/CW) of the original
// sweep is preserved.
void DesignSketchTool::set_arc_sweep(int ei, double deg)
{
    if (ei < 0 || ei >= int(m_entities.size())) return;
    SketchEntity& e = m_entities[ei];
    if (e.type != SketchEntity::Type::Arc || e.radius < 1e-6) return;
    double sweep = std::max(1e-3, std::min(deg, 359.999)) * M_PI / 180.0;
    const double sign = (e.end_angle >= e.start_angle) ? 1.0 : -1.0;
    e.end_angle = e.start_angle + sign * sweep;
    e.p1 = e.center + e.radius * Vec2d(std::cos(e.end_angle), std::sin(e.end_angle));
    resolve_live();
}

// Drag one of the arc's three handles. Roles are split so each grip changes ONE property
// (Onshape-like): Center -> translate; START point (P0) -> radius only; END point (P1) ->
// sweep angle only. Geometric (mutates the entity directly), then re-solve for any
// coincident constraints on the arc endpoints.
void DesignSketchTool::drag_arc_handle(int ei, SketchPointRole role, const Vec2d& target)
{
    if (ei < 0 || ei >= int(m_entities.size())) return;
    SketchEntity& e = m_entities[ei];
    if (e.type != SketchEntity::Type::Arc) return;
    if (role == SketchPointRole::Center) {
        const Vec2d d = target - e.center;            // rigid translate, keep R + angles
        e.center = target; e.p0 += d; e.p1 += d;
    } else if (role == SketchPointRole::P0) {         // start = RADIUS handle (keep angles)
        const double R = (target - e.center).norm();
        if (R < 1e-6) return;
        e.radius = R;
        e.p0 = e.center + R * Vec2d(std::cos(e.start_angle), std::sin(e.start_angle));
        e.p1 = e.center + R * Vec2d(std::cos(e.end_angle),   std::sin(e.end_angle));
    } else if (role == SketchPointRole::P1) {         // end = ANGLE handle (keep radius)
        const Vec2d d = target - e.center;
        if (d.squaredNorm() < 1e-12) return;
        // Keep the CCW sweep continuous (0,2pi) so the arc never flips to its complement.
        double da = std::atan2(d.y(), d.x()) - e.start_angle;
        while (da < 0.0)            da += 2.0 * M_PI;
        while (da >= 2.0 * M_PI)    da -= 2.0 * M_PI;
        e.end_angle = e.start_angle + da;
        e.p1 = e.center + e.radius * Vec2d(std::cos(e.end_angle), std::sin(e.end_angle));
    }
    resolve_live();
}

// Drag an elliptical-arc grip. Center rigidly translates (endpoints + frame move with it);
// P0/P1 set the sweep start/end to the cursor's parametric angle on the ellipse, keeping
// the ellipse shape (a/b/phi). Geometric, then resolve_live().
void DesignSketchTool::drag_ellipsearc_handle(int ei, SketchPointRole role, const Vec2d& target)
{
    if (ei < 0 || ei >= int(m_entities.size())) return;
    SketchEntity& e = m_entities[ei];
    if (e.type != SketchEntity::Type::EllipseArc) return;
    if (role == SketchPointRole::Center) {
        const Vec2d d = target - e.center;
        e.center = target; e.p0 += d; e.p1 += d;
    } else if (role == SketchPointRole::P0 || role == SketchPointRole::P1) {
        const double t = ellipse_param_of(e.center, e.radius, e.rminor, e.rotation, target);
        if (role == SketchPointRole::P0) {
            e.start_angle = t;
            e.p0 = ellipse_point(e.center, e.radius, e.rminor, e.rotation, t);
        } else {
            // Keep the CCW sweep (end strictly after start) so the arc never inverts.
            double t1 = t; while (t1 <= e.start_angle) t1 += 2.0 * M_PI;
            e.end_angle = t1;
            e.p1 = ellipse_point(e.center, e.radius, e.rminor, e.rotation, t1);
        }
    }
    resolve_live();
}

void DesignSketchTool::open_ellipse_axis_editor(int ei, bool major)
{
    if (ei < 0 || ei >= int(m_entities.size()) || !on_inline_edit) return;
    const SketchEntity& e = m_entities[ei];
    if (e.type != SketchEntity::Type::Ellipse && e.type != SketchEntity::Type::EllipseArc) return;
    const double v = major ? e.radius : e.rminor;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, v, major ? "Major" : "Minor",
                   [this, ei, major](double nv) { set_ellipse_axis(ei, major, nv); },
                   []()                         {});
}

// Set a semi-axis to `v`: major -> e.radius, minor -> e.rminor; keep OCCT a >= b.
void DesignSketchTool::set_ellipse_axis(int ei, bool major, double v)
{
    if (ei < 0 || ei >= int(m_entities.size()) || v < 1e-6) return;
    SketchEntity& e = m_entities[ei];
    if (e.type != SketchEntity::Type::Ellipse && e.type != SketchEntity::Type::EllipseArc) return;
    if (major) e.radius = std::max(v, e.rminor);
    else       e.rminor = std::min(v, e.radius);
    if (e.type == SketchEntity::Type::EllipseArc) {   // endpoints ride the reshaped frame
        e.p0 = ellipse_point(e.center, e.radius, e.rminor, e.rotation, e.start_angle);
        e.p1 = ellipse_point(e.center, e.radius, e.rminor, e.rotation, e.end_angle);
    }
    resolve_live();
}

// Set an elliptical arc's included (parametric) sweep, keeping the start fixed and moving the
// end. Geometric (mirrors set_arc_sweep), then resolve_live for any endpoint coincidences.
void DesignSketchTool::set_ellipsearc_sweep(int ei, double deg)
{
    if (ei < 0 || ei >= int(m_entities.size())) return;
    SketchEntity& e = m_entities[ei];
    if (e.type != SketchEntity::Type::EllipseArc) return;
    const double sweep = std::max(1e-3, std::min(deg, 359.999)) * M_PI / 180.0;
    const double sign  = (e.end_angle >= e.start_angle) ? 1.0 : -1.0;
    e.end_angle = e.start_angle + sign * sweep;
    e.p1 = ellipse_point(e.center, e.radius, e.rminor, e.rotation, e.end_angle);
    resolve_live();
}

// Rotate an oblique rectangle to an absolute orientation (angle of edge0 to +X), pivoting on
// its anchor corner f.c0. Geometric, mirrors set_polygon_angle: drop the now-inconsistent edge
// H/V first, rotate every member point + the opposite corner, and DON'T re-solve (the rotated
// loop is already consistent; a length Distance the user may have set is rotation-invariant).
void DesignSketchTool::set_rect_angle(int fi, double deg)
{
    if (fi < 0 || fi >= int(m_features.size())) return;
    Feature& f = m_features[fi];
    if (f.begin < 0 || f.end > int(m_entities.size()) || f.end <= f.begin) return;
    const SketchEntity& e0 = m_entities[f.begin];
    const Vec2d d0 = e0.p1 - e0.p0;
    if (d0.squaredNorm() < 1e-12) return;
    const double da = deg * M_PI / 180.0 - std::atan2(d0.y(), d0.x());
    drop_orientation_constraints(f.begin, f.end);
    const Vec2d pivot = f.c0;
    const double ca = std::cos(da), sa = std::sin(da);
    // -> Vec2d REQUIRED (see set_polygon_angle): an auto return deduces an Eigen expression
    // template referencing the destroyed temporary -> dangling.
    auto rot = [&](const Vec2d& pt) -> Vec2d { const Vec2d r = pt - pivot;
        return pivot + Vec2d(r.x() * ca - r.y() * sa, r.x() * sa + r.y() * ca); };
    for (int i = f.begin; i < f.end && i < int(m_entities.size()); ++i) {
        SketchEntity& e = m_entities[i];
        e.p0 = rot(e.p0); e.p1 = rot(e.p1);
    }
    f.c1 = rot(f.c1);   // keep the opposite corner consistent for later W/H quotes
}

// Screen anchor for a Constrain-mode value field: over the picked geometry (its representative
// point — circle/arc centre, else segment midpoint; averaged when two entities are picked),
// projected to the viewport. Lets a dimensional constraint's field open ON the geometry like
// the draw-then-edit tools, instead of floating at viewport centre. False if no valid pick or
// it projects off-screen (caller falls back to centre).
bool DesignSketchTool::constrain_value_anchor(wxPoint& out) const
{
    if (m_pick0 < 0 || m_pick0 >= int(m_entities.size())) return false;
    auto rep = [](const SketchEntity& e) -> Vec2d {
        using T = SketchEntity::Type;
        if (e.type == T::Circle || e.type == T::Arc ||
            e.type == T::Ellipse || e.type == T::EllipseArc) return e.center;
        return 0.5 * (e.p0 + e.p1);
    };
    Vec2d p = rep(m_entities[m_pick0]);
    if (m_pick1 >= 0 && m_pick1 < int(m_entities.size()))
        p = 0.5 * (p + rep(m_entities[m_pick1]));
    const Camera& cam = wxGetApp().plater()->get_camera();
    const wxPoint sp = world_to_screen_px(cam, m_plane.to_world(p));
    const std::array<int, 4>& vp = cam.get_viewport();
    if (sp.x < vp[0] || sp.y < vp[1] || sp.x > vp[0] + vp[2] || sp.y > vp[1] + vp[3]) return false;
    out = sp;
    return true;
}

void DesignSketchTool::open_rounded_rect_editor(int fi, int which)
{
    if (fi < 0 || fi >= int(m_features.size()) || !on_inline_edit) return;
    const Feature& f = m_features[fi];
    const double w = std::abs(f.c1.x() - f.c0.x());
    const double h = std::abs(f.c1.y() - f.c0.y());
    const double r = f.param;
    const double v = (which == 0) ? w : (which == 1) ? h : r;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, v, which == 0 ? "Width" : which == 1 ? "Height" : "Radius",
        [this, fi, which](double nv) {
            const Feature& g = m_features[fi];
            double gw = std::abs(g.c1.x() - g.c0.x());
            double gh = std::abs(g.c1.y() - g.c0.y());
            double gr = g.param;
            if (which == 0) gw = nv; else if (which == 1) gh = nv; else gr = nv;
            set_rounded_rect(fi, gw, gh, gr);
        },
        []() {});
}

// Rebuild the rounded-rect's 8 entities in place for a new width/height/fillet radius,
// keeping the min corner (c0) fixed. Geometric (entity order/count preserved so constraint
// refs stay valid); fillet clamped to (0, min(w,h)/2].
void DesignSketchTool::set_rounded_rect(int fi, double w, double h, double r)
{
    if (fi < 0 || fi >= int(m_features.size())) return;
    Feature& f = m_features[fi];
    if (f.begin < 0 || f.end > int(m_entities.size()) || f.end <= f.begin) return;
    w = std::max(w, 1e-3); h = std::max(h, 1e-3);
    r = std::max(1e-3, std::min(r, std::min(w, h) * 0.5 - 1e-4));
    const double xmin = std::min(f.c0.x(), f.c1.x()), ymin = std::min(f.c0.y(), f.c1.y());
    const double xmax = xmin + w, ymax = ymin + h;
    std::vector<SketchEntity> rebuilt = rounded_rect_entities(xmin, ymin, xmax, ymax, r);
    if (int(rebuilt.size()) != f.end - f.begin) return;   // count must match to keep con refs
    for (int i = 0; i < int(rebuilt.size()); ++i) {
        rebuilt[i].construction = m_entities[f.begin + i].construction;   // preserve flag
        m_entities[f.begin + i] = rebuilt[i];
    }
    f.c0 = Vec2d(xmin, ymin); f.c1 = Vec2d(xmax, ymax); f.param = r;
    resolve_live();
}

void DesignSketchTool::open_arc_slot_editor(int fi, bool radius)
{
    if (fi < 0 || fi >= int(m_features.size()) || !on_inline_edit) return;
    const Feature& f = m_features[fi];
    const double Rc = (f.c1 - f.c0).norm();
    const double v  = radius ? Rc : (2.0 * f.param);     // width quote shows the FULL width
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, v, radius ? "Radius" : "Width",
        [this, fi, radius](double nv) {
            const Feature& g = m_features[fi];
            const double gRc = (g.c1 - g.c0).norm();
            if (radius) set_arc_slot(fi, nv, g.param);
            else        set_arc_slot(fi, gRc, std::max(1e-3, nv * 0.5));  // full width -> half
        },
        []() {});
}

// Rebuild the arc-slot's 4 arcs in place for a new centreline radius / half-width. Centre
// + the two centreline directions are kept (the end direction is recovered from the cap@E
// arc centre). Geometric; entity count preserved so constraint refs stay valid.
void DesignSketchTool::set_arc_slot(int fi, double Rc, double w)
{
    if (fi < 0 || fi >= int(m_features.size())) return;
    Feature& f = m_features[fi];
    if (f.begin < 0 || f.end > int(m_entities.size()) || f.end - f.begin != 4) return;
    const Vec2d center = f.c0;
    Vec2d dirS = f.c1 - center;
    const Vec2d Ec = m_entities[f.begin + 1].center;     // cap@E centre = centreline end
    Vec2d dirE = Ec - center;
    if (dirS.squaredNorm() < 1e-12 || dirE.squaredNorm() < 1e-12) return;
    dirS.normalize(); dirE.normalize();
    Rc = std::max(Rc, 2e-3);
    w  = std::max(1e-3, std::min(w, Rc - 1e-3));         // make_arc_slot needs w < Rc
    std::vector<SketchEntity> rebuilt =
        make_arc_slot(center, center + Rc * dirS, center + Rc * dirE, w);
    if (int(rebuilt.size()) != 4) return;
    for (int i = 0; i < 4; ++i) {
        rebuilt[i].construction = m_entities[f.begin + i].construction;
        m_entities[f.begin + i] = rebuilt[i];
    }
    f.c1 = center + Rc * dirS; f.param = w;
    resolve_live();
}

// Open the inline editor for a straight slot's dimension: which 0 = inter-centre distance,
// 1 = radius (half-width), 2 = centreline angle. Drives set_slot / set_slot_angle geometrically.
void DesignSketchTool::open_slot_editor(int fi, int which)
{
    if (fi < 0 || fi >= int(m_features.size()) || !on_inline_edit) return;
    const Feature& f = m_features[fi];
    const Vec2d  d = f.c1 - f.c0;
    double deg = std::atan2(d.y(), d.x()) * 180.0 / M_PI; if (deg < 0.0) deg += 360.0;
    const double v = (which == 0) ? d.norm() : (which == 1) ? f.param : deg;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, v, which == 0 ? "Length" : which == 1 ? "Radius" : "Angle",
        [this, fi, which](double nv) {
            const Feature& g = m_features[fi];
            if      (which == 0) set_slot(fi, nv, g.param);
            else if (which == 1) set_slot(fi, (g.c1 - g.c0).norm(), std::max(1e-3, nv));
            else                 set_slot_angle(fi, nv);
        },
        []() {});
}

// Rebuild the straight slot's 4 entities in place for a new centreline length / half-width.
// Centre c0 and the centreline direction are kept; only c1 (length) or param (width) change.
// Geometric; entity count preserved so constraint refs stay valid.
void DesignSketchTool::set_slot(int fi, double length, double w)
{
    if (fi < 0 || fi >= int(m_features.size())) return;
    Feature& f = m_features[fi];
    if (f.begin < 0 || f.end > int(m_entities.size()) || f.end - f.begin != 4) return;
    Vec2d dir = f.c1 - f.c0;
    if (dir.squaredNorm() < 1e-12) return;
    dir.normalize();
    length = std::max(length, 2e-3);
    w      = std::max(1e-3, w);
    const Vec2d c0 = f.c0, c1 = f.c0 + length * dir;
    std::vector<SketchEntity> rebuilt = make_slot(c0, c1, w);
    if (int(rebuilt.size()) != 4) return;
    for (int i = 0; i < 4; ++i) {
        rebuilt[i].construction = m_entities[f.begin + i].construction;
        m_entities[f.begin + i] = rebuilt[i];
    }
    f.c1 = c1; f.param = w;
    resolve_live();
}

// Rotate a straight slot about c0 to a new centreline angle (degrees), keeping length + radius.
void DesignSketchTool::set_slot_angle(int fi, double deg)
{
    if (fi < 0 || fi >= int(m_features.size())) return;
    Feature& f = m_features[fi];
    if (f.begin < 0 || f.end > int(m_entities.size()) || f.end - f.begin != 4) return;
    const double L = (f.c1 - f.c0).norm();
    if (L < 1e-9) return;
    const double a = deg * M_PI / 180.0;
    const Vec2d c1 = f.c0 + L * Vec2d(std::cos(a), std::sin(a));
    std::vector<SketchEntity> rebuilt = make_slot(f.c0, c1, f.param);
    if (int(rebuilt.size()) != 4) return;
    for (int i = 0; i < 4; ++i) {
        rebuilt[i].construction = m_entities[f.begin + i].construction;
        m_entities[f.begin + i] = rebuilt[i];
    }
    f.c1 = c1;
    resolve_live();
}

// Resize an axis-aligned rectangle by dragging a corner: the diagonally-opposite corner
// (captured at grab as m_drag_rect_anchor) stays fixed; the box becomes [anchor, cursor].
// Geometric rebuild in place (4 lines, same order) — edges stay axis-aligned so the
// inferred H/V + corner-coincident constraints remain satisfied (no re-solve needed).
void DesignSketchTool::drag_rect_corner(int fi, const Vec2d& cursor)
{
    if (fi < 0 || fi >= int(m_features.size())) return;
    Feature& f = m_features[fi];
    if (f.end - f.begin != 4) return;
    const Vec2d A = m_drag_rect_anchor, B = cursor;
    if (std::abs(B.x() - A.x()) < 1e-4 || std::abs(B.y() - A.y()) < 1e-4) return;  // degenerate
    const Vec2d corners[4] = { A, Vec2d(B.x(), A.y()), B, Vec2d(A.x(), B.y()) };
    for (int i = 0; i < 4; ++i) {
        SketchEntity e; e.type = SketchEntity::Type::Line;
        e.p0 = corners[i]; e.p1 = corners[(i + 1) % 4];
        e.construction = m_entities[f.begin + i].construction;
        m_entities[f.begin + i] = e;
    }
    f.c0 = A; f.c1 = B;
}

// Move one end of a slot by dragging its cap centre (which cap captured at grab); the other
// centre + half-width are kept. Rebuilds the 4-entity span via make_slot. Geometric.
void DesignSketchTool::drag_slot_handle(int fi, const Vec2d& cursor)
{
    if (fi < 0 || fi >= int(m_features.size())) return;
    Feature& f = m_features[fi];
    if (f.end - f.begin != 4) return;
    const Vec2d c0 = m_drag_slot_c1 ? f.c0 : cursor;
    const Vec2d c1 = m_drag_slot_c1 ? cursor : f.c1;
    std::vector<SketchEntity> rebuilt = make_slot(c0, c1, f.param);
    if (int(rebuilt.size()) != 4) return;
    for (int i = 0; i < 4; ++i) {
        rebuilt[i].construction = m_entities[f.begin + i].construction;
        m_entities[f.begin + i] = rebuilt[i];
    }
    f.c0 = c0; f.c1 = c1;
}

DesignSketchTool::DimType DesignSketchTool::pending_dimension_type() const
{
    return (m_pending_dim >= 0 && m_pending_dim < int(m_dimensions.size()))
               ? m_dimensions[m_pending_dim].kind : DimType::None;
}

void DesignSketchTool::set_dimension_value(double v)
{
    if (m_pending_dim < 0 || m_pending_dim >= int(m_dimensions.size())) return;
    DimAnnot& a = m_dimensions[m_pending_dim];
    a.value = v;
    if (a.con >= 0 && a.con < int(m_constraints.size()))
        m_constraints[a.con] = constraint_for(a);
    resolve_live();
    m_pending_dim = -1;
}

void DesignSketchTool::cancel_dimension_value()
{
    m_pending_dim = -1;   // keep the placed dimension at its measured value
}

std::string DesignSketchTool::dim_text(const DimAnnot& a) const
{
    char buf[32];
    const char* prefix = (a.kind == DimType::Diameter) ? "\xC3\x98"   // 'Ø'
                       : (a.kind == DimType::Radius)   ? "R" : "";
    const char* suffix = (a.kind == DimType::Angle) ? "\xC2\xB0" : ""; // '°'
    std::snprintf(buf, sizeof(buf), "%s%.1f%s", prefix, a.value, suffix);
    // Force the international (en) decimal point: wx sets LC_NUMERIC to the user
    // locale at startup, so snprintf("%.1f") can emit a comma. Normalise it.
    for (char& ch : buf)
        if (ch == ',') ch = '.';
    std::string out(buf);
    if (a.kind != DimType::Angle) {
        const bool use_in = wxGetApp().app_config->get_bool("use_inches");
        out += use_in ? " in" : " mm";
    }
    return out;
}

void DesignSketchTool::apply_segment_length(double len)
{
    if (m_points.size() == 2 && len > 1e-9) {
        const Vec2d d = m_points[1] - m_points[0];
        const double r = d.norm();
        if (r > 1e-9)
            m_points[1] = m_points[0] + (len / r) * d;
    }
    keep_segment_as_drawn();
    // Driving length constraint on the just-committed Line entity.
    if (len > 1e-9 && !m_entities.empty()) {
        const int i = int(m_entities.size()) - 1;
        if (m_entities[i].type == SketchEntity::Type::Line) {
            SketchEntityConstraintDef c;
            c.type = SketchConstraintType::Distance;
            c.ea = i; c.ra = SketchPointRole::P0;
            c.eb = i; c.rb = SketchPointRole::P1;
            c.value = len;
            m_constraints.push_back(c);
        }
    }
    resolve_live();                          // live-solve the in-session sketch
}

void DesignSketchTool::keep_segment_as_drawn()
{
    if (m_points.size() == 2) {
        const int base = int(m_entities.size());
        push_line(m_points[0], m_points[1]);  // accrues into the session's entities
        infer_auto_constraints(base);         // auto Coincident at snapped ends + H/V
    }
    m_points.clear();
    m_has_cursor = false;
    m_awaiting_length = false;
}

void DesignSketchTool::finish()
{
    // A value field still open at commit time outlives the session — the editor is a top-level
    // frame — and inline_busy stays set, so every later click is swallowed at on_mouse_impl's
    // first branch and the viewport reads as dead. Dismiss it first (keep-as-drawn, the same
    // contract as the polyline terminators) and drop any queued field with it.
    close_session_chrome();
    if (op_ready()) confirm_op();      // apply a pending edit-op gizmo before committing
    if (tf_ready()) confirm_transform(); // apply a pending transform gizmo before committing
    auto cb = on_commit_entities;
    std::vector<SketchEntity> ents = m_entities;
    std::vector<SketchEntityConstraintDef> cons = m_constraints;
    SketchPlane pl = m_plane;
    m_active = false;
    m_points.clear();
    m_entities.clear();
    m_constraints.clear();
    m_dimensions.clear();
    m_point_sel.clear();
    m_dim_has0 = false;
    m_pending_dim = -1;
    m_has_cursor = false;
    m_features.clear();
    m_open_feature = -1;
    if (cb)
        cb(ents, cons, pl);
}

void DesignSketchTool::begin_constrain(const SketchProfile& prof, const SketchPlane& plane)
{
    m_plane = plane;
    m_mode = Mode::Constrain;
    m_points = prof.points;
    m_entities.clear();
    m_has_cursor = false;
    m_sel_a = m_sel_b = -1;
    m_constrain_entities = false;
    m_pick0 = m_pick1 = m_pick2 = -1;
    m_active = true;
}

void DesignSketchTool::begin_constrain_entities(const std::vector<SketchEntity>& ents,
                                                const SketchPlane& plane)
{
    m_plane = plane;
    m_mode = Mode::Constrain;
    m_constrain_entities = true;
    m_points.clear();
    m_entities = ents;
    m_has_cursor = false;
    m_sel_a = m_sel_b = -1;
    m_pick0 = m_pick1 = m_pick2 = -1;
    m_active = true;
}

bool DesignSketchTool::selected_segment(int& a, int& b) const
{
    if (m_sel_a < 0 || m_sel_b < 0)
        return false;
    a = m_sel_a;
    b = m_sel_b;
    return true;
}

bool DesignSketchTool::screen_to_plane(GLCanvas3D& canvas, const wxMouseEvent& evt, Vec2d& out) const
{
    Point pos(evt.GetX(), evt.GetY());
    Linef3 r = canvas.mouse_ray(pos);
    out = m_plane.project(r.a, r.vector());
    return true;
}

bool DesignSketchTool::near_first(const Vec2d& p) const
{
    return m_points.size() >= 3 && (p - m_points[0]).squaredNorm() < 4.0;
}

Vec2d DesignSketchTool::snap_dir(const Vec2d& anchor, const Vec2d& raw, bool& locked) const
{
    locked = false;
    if (m_snap_off) return raw;

    const Vec2d d = raw - anchor;
    const double r = d.norm();
    if (r < 1e-9) return raw;

    const double tol_deg = 5.0;                 // inference half-window
    double ang = std::atan2(d.y(), d.x()) * 180.0 / M_PI;  // (-180,180]
    if (ang < 0.0) ang += 360.0;                            // [0,360)

    // Base angles within one quadrant, replicated every 90 deg up to 360.
    static const double base[] = {0.0, 30.0, 45.0, 60.0};
    double best_cand = ang, best_diff = 1e30;
    for (int q = 0; q < 4; ++q) {
        for (double b : base) {
            const double cand = b + 90.0 * q;
            double diff = std::abs(ang - cand);
            if (diff > 180.0) diff = 360.0 - diff;
            if (diff < best_diff) { best_diff = diff; best_cand = cand; }
        }
    }
    if (best_diff > tol_deg) return raw;

    locked = true;
    const double rad = best_cand * M_PI / 180.0;
    return anchor + r * Vec2d(std::cos(rad), std::sin(rad));
}

double DesignSketchTool::screen_tol(GLCanvas3D& canvas, const wxMouseEvent& evt,
                                    const Vec2d& at, double px) const
{
    // Project a point `px` screen pixels away and measure the gap in plane units.
    const Linef3 r2 = canvas.mouse_ray(Point(evt.GetX() + int(px), evt.GetY()));
    const Vec2d  p2 = m_plane.project(r2.a, r2.vector());
    return std::max(1e-3, (p2 - at).norm());
}

InferenceSnap DesignSketchTool::infer_at(GLCanvas3D& canvas, const wxMouseEvent& evt,
                                         const Vec2d& raw) const
{
    if (evt.ShiftDown()) { InferenceSnap s; s.point = raw; return s; }   // Shift suppresses
    const double tol = screen_tol(canvas, evt, raw);
    return infer_point_snap(m_entities, raw, tol);
}

Vec2d DesignSketchTool::snap_vertex(GLCanvas3D& canvas, const wxMouseEvent& evt,
                                    const Vec2d& raw, bool& snapped) const
{
    const InferenceSnap s = infer_at(canvas, evt, raw);
    // Cache the target so render() can draw a snap hint; only endpoint/centre/origin
    // count as a "vertex" snap for the callers that gate angle inference on it.
    const_cast<DesignSketchTool*>(this)->m_cursor_snap = s;
    snapped = s.snapped();   // any hard snap moves the cursor + suppresses angle lock
    return s.point;
}

bool DesignSketchTool::has_coincident(int ea, SketchPointRole ra, int eb, SketchPointRole rb) const
{
    for (const auto& c : m_constraints) {
        if (c.type != SketchConstraintType::Coincident) continue;
        if ((c.ea == ea && c.ra == ra && c.eb == eb && c.rb == rb) ||
            (c.ea == eb && c.ra == rb && c.eb == ea && c.rb == ra))
            return true;
    }
    return false;
}

bool DesignSketchTool::try_add_constraints(const std::vector<SketchEntityConstraintDef>& cands)
{
    if (cands.empty()) return true;
    const size_t mark = m_constraints.size();
    for (const auto& c : cands) m_constraints.push_back(c);
    if (solve_sketch_entities(m_entities, m_constraints))
        return true;
    m_constraints.resize(mark);                 // roll back the conflicting batch
    // No re-solve to "restore": a failed solve no longer touches the geometry
    // (SketchSolver.cpp only writes back on success), so m_entities still holds the
    // prior solved state exactly. snaporca-pl5.
    return false;
}

void DesignSketchTool::infer_auto_constraints(int base)
{
    const int n = int(m_entities.size());
    if (base < 0 || base >= n) return;

    // Endpoint roles an entity exposes for coincidence matching.
    auto roles_of = [](const SketchEntity& e, SketchPointRole out[2]) -> int {
        switch (e.type) {
        case SketchEntity::Type::Line:   out[0] = SketchPointRole::P0; out[1] = SketchPointRole::P1; return 2;
        case SketchEntity::Type::Arc:    out[0] = SketchPointRole::P0; out[1] = SketchPointRole::P1; return 2;
        case SketchEntity::Type::BSpline:out[0] = SketchPointRole::P0; out[1] = SketchPointRole::P1; return 2;
        case SketchEntity::Type::Point:  out[0] = SketchPointRole::P0; return 1;
        default: return 0;   // circle: centre coincidence handled by Concentric, not here
        }
    };

    // 1) Coincident between a new endpoint and any (co-located) endpoint of another
    //    entity. snap_vertex already drove the coordinates together; this records it
    //    so a re-solve keeps the loop closed.
    std::vector<SketchEntityConstraintDef> coincs;
    for (int i = base; i < n; ++i) {
        SketchPointRole ir[2]; const int ni = roles_of(m_entities[i], ir);
        for (int a = 0; a < ni; ++a) {
            Vec2d pa; if (!point_at(i, ir[a], pa)) continue;
            for (int j = 0; j < n; ++j) {
                if (j == i) continue;
                SketchPointRole jr[2]; const int nj = roles_of(m_entities[j], jr);
                for (int b = 0; b < nj; ++b) {
                    if (j >= base && j < i) continue;          // avoid duplicate (i,j)/(j,i)
                    Vec2d pb; if (!point_at(j, jr[b], pb)) continue;
                    if ((pa - pb).squaredNorm() > 1e-6) continue;
                    if (has_coincident(i, ir[a], j, jr[b])) continue;
                    SketchEntityConstraintDef c;
                    c.type = SketchConstraintType::Coincident;
                    c.ea = i; c.ra = ir[a]; c.eb = j; c.rb = jr[b];
                    coincs.push_back(c);
                }
            }
        }
    }
    try_add_constraints(coincs);   // co-located points: consistent by construction

    // 2) Horizontal / Vertical on axis-aligned new line segments (added one at a time
    //    so a single conflict never drops the others).
    for (int i = base; i < n; ++i) {
        if (m_entities[i].type != SketchEntity::Type::Line) continue;
        auto ax = infer_axis_constraint(m_entities[i].p0, m_entities[i].p1);
        if (!ax) continue;
        SketchEntityConstraintDef c;
        c.type = *ax;
        c.ea = i; c.ra = SketchPointRole::P0;
        c.eb = i; c.rb = SketchPointRole::P1;
        try_add_constraints({ c });
    }

    resolve_live();
}

static std::vector<Vec2d> circle_polygon(const Vec2d& c, double r, int n = 48)
{
    std::vector<Vec2d> v; v.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        v.push_back(Vec2d(c.x() + r * std::cos(a), c.y() + r * std::sin(a)));
    }
    return v;
}

// Point on an ellipse at parametric angle theta: center + R(phi)*(a cos t, b sin t).
static Vec2d ellipse_point(const Vec2d& c, double a, double b, double phi, double t)
{
    const double cu = std::cos(phi), su = std::sin(phi);
    const double x = a * std::cos(t), y = b * std::sin(t);
    return Vec2d(c.x() + x * cu - y * su, c.y() + x * su + y * cu);
}

// Tessellate an ellipse arc parametric range [t0,t1] into a polyline.
static std::vector<Vec2d> ellipse_polyline(const Vec2d& c, double a, double b, double phi,
                                           double t0, double t1, int n = 48)
{
    std::vector<Vec2d> v; v.reserve(n + 1);
    for (int i = 0; i <= n; ++i)
        v.push_back(ellipse_point(c, a, b, phi, t0 + (t1 - t0) * double(i) / double(n)));
    return v;
}

// Clamped uniform B-spline (degree min(3, n-1)) through control poles. The knot
// construction mirrors SketchEngine::entities_to_wire's OCCT Geom_BSplineCurve so
// the previewed/extruded curve match. de Boor evaluation.
static int bspline_degree(int n) { return n >= 4 ? 3 : (n >= 2 ? n - 1 : 0); }

static std::vector<double> bspline_knots(int n, int p)
{
    std::vector<double> U;                     // full knot vector, length n+p+1
    const int interior = n - p - 1;
    for (int i = 0; i <= p; ++i) U.push_back(0.0);
    for (int i = 1; i <= interior; ++i) U.push_back(double(i));
    const double last = double(interior + 1);
    for (int i = 0; i <= p; ++i) U.push_back(last);
    return U;
}

static Vec2d bspline_eval(const std::vector<Vec2d>& P, const std::vector<double>& U, int p, double u)
{
    const int n = int(P.size());
    if (u <= U[p])            return P.front();
    if (u >= U[n])            return P.back();   // U[n] == domain max (clamped)
    int k = p;
    while (k < n - 1 && U[k + 1] <= u) ++k;       // span: U[k] <= u < U[k+1]
    std::vector<Vec2d> d(p + 1);
    for (int j = 0; j <= p; ++j) d[j] = P[j + k - p];
    for (int r = 1; r <= p; ++r)
        for (int j = p; j >= r; --j) {
            const double denom = U[j + 1 + k - r] - U[j + k - p];
            const double a = denom > 1e-12 ? (u - U[j + k - p]) / denom : 0.0;
            d[j] = (1.0 - a) * d[j - 1] + a * d[j];
        }
    return d[p];
}

static std::vector<Vec2d> bspline_polyline(const std::vector<Vec2d>& ctrl, int samples = 0)
{
    const int n = int(ctrl.size());
    if (n < 2) return ctrl;
    const int p = bspline_degree(n);
    const std::vector<double> U = bspline_knots(n, p);
    const double umax = double(n - p);
    if (samples <= 0) samples = std::max(24, 14 * (n - 1));
    std::vector<Vec2d> out; out.reserve(samples + 1);
    for (int i = 0; i <= samples; ++i)
        out.push_back(bspline_eval(ctrl, U, p, umax * double(i) / double(samples)));
    return out;
}

// ---- entity builders --------------------------------------------------------

void DesignSketchTool::push_line(const Vec2d& a, const Vec2d& b)
{
    if ((b - a).squaredNorm() < 1e-9)
        return;
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0 = a;
    e.p1 = b;
    e.construction = m_construction;
    m_entities.push_back(e);
}

bool DesignSketchTool::add_imported_regions(
    const std::vector<std::vector<std::vector<Vec2d>>>& regions)
{
    if (!m_active) return false;          // no session to draw into; caller makes a feature
    const bool saved = m_construction;
    m_construction = false;               // art is real geometry, never construction lines
    size_t before = m_entities.size();
    for (const auto& region : regions)
        for (const auto& loop : region)
            push_closed_lines(loop);      // every glyph contour, holes included
    m_construction = saved;
    if (m_entities.size() == before) return false;   // nothing importable — say so, do not lie
    // Art is not "just drawn", so it must NOT enter the draw-then-edit queue. Without this the
    // glyph contours are treated as fresh entities and a Length field opens on the first of
    // them — on a word, that is one value editor per segment, and an open field freezes the
    // canvas (snaporca-yce). reset_autoedit() marks every entity as already seen.
    reset_autoedit();
    // The new lines carry no constraints, so the solver has nothing to move; resolve anyway so
    // the degrees-of-freedom readout counts them instead of going stale.
    resolve_live();
    return true;
}

void DesignSketchTool::push_closed_lines(const std::vector<Vec2d>& corners)
{
    const size_t n = corners.size();
    if (n < 2) return;
    for (size_t i = 0; i < n; ++i)
        push_line(corners[i], corners[(i + 1) % n]);
}

void DesignSketchTool::push_open_chain(const std::vector<Vec2d>& pts)
{
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        push_line(pts[i], pts[i + 1]);
}

void DesignSketchTool::push_circle(const Vec2d& center, double radius)
{
    if (radius < 1e-3) return;
    SketchEntity e;
    e.type = SketchEntity::Type::Circle;
    e.center = center;
    e.p0 = center;
    e.radius = radius;
    e.construction = m_construction;
    m_entities.push_back(e);
}

void DesignSketchTool::push_point(const Vec2d& p)
{
    SketchEntity e;
    e.type = SketchEntity::Type::Point;
    e.p0 = p;
    e.center = p;
    e.construction = m_construction;
    m_entities.push_back(e);
}

void DesignSketchTool::append_entities(const std::vector<SketchEntity>& ents)
{
    for (const SketchEntity& e : ents)
        m_entities.push_back(e);
}

static double wrap_2pi(double a)
{
    while (a < 0.0)        a += 2.0 * M_PI;
    while (a >= 2.0 * M_PI) a -= 2.0 * M_PI;
    return a;
}

// Circumcircle of 3 points. Returns false if (nearly) collinear.
static bool circumcircle(const Vec2d& a, const Vec2d& b, const Vec2d& c,
                         Vec2d& center, double& radius)
{
    const double d = 2.0 * (a.x() * (b.y() - c.y()) +
                            b.x() * (c.y() - a.y()) +
                            c.x() * (a.y() - b.y()));
    if (std::abs(d) < 1e-9)
        return false;
    const double a2 = a.squaredNorm(), b2 = b.squaredNorm(), c2 = c.squaredNorm();
    const double ux = (a2 * (b.y() - c.y()) + b2 * (c.y() - a.y()) + c2 * (a.y() - b.y())) / d;
    const double uy = (a2 * (c.x() - b.x()) + b2 * (a.x() - c.x()) + c2 * (b.x() - a.x())) / d;
    center = Vec2d(ux, uy);
    radius = (a - center).norm();
    return true;
}

// Build an Arc entity that sweeps start -> end passing through `through`. The
// kernel reconstructs the mid from (start_angle+end_angle)/2, so the angle pair
// must bracket `through` on the correct side of the circle.
static SketchEntity make_arc_through(const Vec2d& center, double radius,
                                     const Vec2d& start, const Vec2d& end,
                                     const Vec2d& through, bool construction)
{
    const double a_start = std::atan2(start.y()  - center.y(), start.x()  - center.x());
    const double a_end   = std::atan2(end.y()    - center.y(), end.x()    - center.x());
    const double a_thru  = std::atan2(through.y() - center.y(), through.x() - center.x());
    const double de = wrap_2pi(a_end  - a_start);   // CCW sweep to end (0,2π)
    const double d3 = wrap_2pi(a_thru - a_start);   // CCW position of through
    SketchEntity e;
    e.type   = SketchEntity::Type::Arc;
    e.center = center;
    e.radius = radius;
    e.p0     = start;
    e.p1     = end;
    e.start_angle = a_start;
    e.end_angle   = (d3 <= de) ? (a_start + de) : (a_start + de - 2.0 * M_PI);
    e.construction = construction;
    return e;
}

std::vector<SketchEntity> DesignSketchTool::make_three_point_circle(const Vec2d& a, const Vec2d& b, const Vec2d& c) const
{
    Vec2d center; double radius;
    if (!circumcircle(a, b, c, center, radius))
        return {};
    SketchEntity e;
    e.type = SketchEntity::Type::Circle;
    e.center = center;
    e.p0 = center;
    e.radius = radius;
    e.construction = m_construction;
    return { e };
}

std::vector<SketchEntity> DesignSketchTool::make_three_point_arc(const Vec2d& start, const Vec2d& end, const Vec2d& on_arc) const
{
    Vec2d center; double radius;
    if (!circumcircle(start, end, on_arc, center, radius))
        return {};
    return { make_arc_through(center, radius, start, end, on_arc, m_construction) };
}

std::vector<SketchEntity> DesignSketchTool::make_tangent_arc(const Vec2d& start, const Vec2d& end) const
{
    // Tangent direction at `start` = exit direction of the previous entity.
    Vec2d t(1, 0);
    bool have_t = false;
    if (!m_entities.empty()) {
        const SketchEntity& prev = m_entities.back();
        if (prev.type == SketchEntity::Type::Line) {
            t = prev.p1 - prev.p0; have_t = (t.squaredNorm() > 1e-12);
        } else if (prev.type == SketchEntity::Type::Arc) {
            // Tangent at the arc end p1 is perpendicular to its radius, in the
            // sweep direction.
            const Vec2d r = prev.p1 - prev.center;
            const double sweep = prev.end_angle - prev.start_angle;
            t = (sweep >= 0.0) ? Vec2d(-r.y(), r.x()) : Vec2d(r.y(), -r.x());
            have_t = (r.squaredNorm() > 1e-12);
        }
    }
    const Vec2d se = end - start;
    if (!have_t || se.squaredNorm() < 1e-12) {
        // No tangent reference or zero length: fall back to a straight line.
        SketchEntity e; e.type = SketchEntity::Type::Line; e.p0 = start; e.p1 = end;
        e.construction = m_construction;
        return { e };
    }
    t.normalize();
    const Vec2d n(-t.y(), t.x());            // unit normal to the tangent
    const double denom = 2.0 * n.dot(se);
    if (std::abs(denom) < 1e-9) {            // end lies along the tangent: line
        SketchEntity e; e.type = SketchEntity::Type::Line; e.p0 = start; e.p1 = end;
        e.construction = m_construction;
        return { e };
    }
    const double R = se.squaredNorm() / denom;   // signed radius along n
    const Vec2d center = start + n * R;
    const double radius = std::abs(R);
    // Mid of the tangent arc: project the chord midpoint outward onto the circle.
    const Vec2d chord_mid = (start + end) * 0.5;
    Vec2d to_mid = chord_mid - center;
    if (to_mid.squaredNorm() < 1e-12) to_mid = n;
    to_mid.normalize();
    const Vec2d through = center + to_mid * radius;
    return { make_arc_through(center, radius, start, end, through, m_construction) };
}

std::vector<SketchEntity> DesignSketchTool::make_center_arc(const Vec2d& center, const Vec2d& start, const Vec2d& end_dir) const
{
    const double radius = (start - center).norm();
    if (radius < 1e-9)
        return {};
    const double a_start = std::atan2(start.y()   - center.y(), start.x()   - center.x());
    const double a_end   = std::atan2(end_dir.y() - center.y(), end_dir.x() - center.x());
    const double de      = wrap_2pi(a_end - a_start);    // CCW sweep start -> end (0,2π)
    const Vec2d  end      = center + radius * Vec2d(std::cos(a_end),  std::sin(a_end));
    const double a_mid    = a_start + de * 0.5;           // bisector brackets the sweep
    const Vec2d  through   = center + radius * Vec2d(std::cos(a_mid), std::sin(a_mid));
    return { make_arc_through(center, radius, start, end, through, m_construction) };
}

std::vector<SketchEntity> DesignSketchTool::make_slot(const Vec2d& c0, const Vec2d& c1, double half_width) const
{
    std::vector<SketchEntity> out;
    Vec2d u = c1 - c0;
    if (u.squaredNorm() < 1e-12 || half_width < 1e-6)
        return out;
    u.normalize();
    const Vec2d nrm(-u.y(), u.x());
    const double w = half_width;
    // Names avoid termios macros (B0 is a baud-rate #define pulled in transitively).
    const Vec2d top0 = c0 + nrm * w, top1 = c1 + nrm * w;   // upper side (c0 -> c1)
    const Vec2d bot1 = c1 - nrm * w, bot0 = c0 - nrm * w;   // lower side (c1 -> c0)

    auto line = [&](const Vec2d& p0, const Vec2d& p1) {
        SketchEntity e; e.type = SketchEntity::Type::Line; e.p0 = p0; e.p1 = p1;
        e.construction = m_construction; return e;
    };
    out.push_back(line(top0, top1));                                   // top
    out.push_back(make_arc_through(c1, w, top1, bot1, c1 + u * w, m_construction)); // cap @c1 (+u)
    out.push_back(line(bot1, bot0));                                   // bottom
    out.push_back(make_arc_through(c0, w, bot0, top0, c0 - u * w, m_construction)); // cap @c0 (-u)
    return out;
}

// Arc slot: a slot whose centerline is a circular arc (center, start, end_dir on
// the same radius). Bounded by an outer arc (Rc+w), an inner arc (Rc-w) and two
// semicircular end caps. CCW closed loop, mirroring make_slot's structure.
std::vector<SketchEntity> DesignSketchTool::make_arc_slot(const Vec2d& center, const Vec2d& start,
                                                          const Vec2d& end_dir, double half_width) const
{
    std::vector<SketchEntity> out;
    const double Rc = (start - center).norm();
    const double w  = half_width;
    if (Rc < 1e-6 || w < 1e-6 || w >= Rc) return out;
    const Vec2d dirS = (start - center) / Rc;
    Vec2d de = end_dir - center;
    if (de.squaredNorm() < 1e-12) return out;
    const Vec2d dirE = de.normalized();
    const double aS = std::atan2(dirS.y(), dirS.x());
    const double aE = std::atan2(dirE.y(), dirE.x());
    const double sweep = wrap_2pi(aE - aS);            // CCW start -> end
    const double aMid  = aS + sweep * 0.5;
    const Vec2d  uMid(std::cos(aMid), std::sin(aMid));
    const Vec2d  Sc = center + Rc * dirS;              // centerline start point (cap centre)
    const Vec2d  Ec = center + Rc * dirE;              // centerline end point   (cap centre)
    const Vec2d  S_out = center + (Rc + w) * dirS, S_in = center + (Rc - w) * dirS;
    const Vec2d  E_out = center + (Rc + w) * dirE, E_in = center + (Rc - w) * dirE;
    const Vec2d  tE(-dirE.y(), dirE.x());              // CCW travel-forward tangent at E
    const Vec2d  tS(-dirS.y(), dirS.x());              // CCW travel-forward tangent at S
    out.push_back(make_arc_through(center, Rc + w, S_out, E_out, center + (Rc + w) * uMid, m_construction)); // outer
    out.push_back(make_arc_through(Ec, w, E_out, E_in, Ec + w * tE, m_construction));                        // cap @E (forward)
    out.push_back(make_arc_through(center, Rc - w, E_in, S_in, center + (Rc - w) * uMid, m_construction));   // inner
    out.push_back(make_arc_through(Sc, w, S_in, S_out, Sc - w * tS, m_construction));                        // cap @S (backward)
    return out;
}

// Rounded rectangle: axis-aligned box (a,b opposite corners) with filleted
// corners. radius_pt's distance to the nearest corner sets the fillet radius.
// 4 straight edges + 4 quarter arcs, CCW.
// Build the 8 entities (4 lines + 4 corner arcs, CCW) of an axis-aligned rounded box
// from explicit bounds + fillet radius. Shared by make_rounded_rect (gesture) and
// set_rounded_rect (label/handle edit) so the entity order/count is identical → a rebuild
// in place keeps constraint indices into the feature span valid.
std::vector<SketchEntity> DesignSketchTool::rounded_rect_entities(double xmin, double ymin,
                                                                  double xmax, double ymax, double r) const
{
    std::vector<SketchEntity> out;
    auto line = [&](const Vec2d& p0, const Vec2d& p1) {
        SketchEntity e; e.type = SketchEntity::Type::Line; e.p0 = p0; e.p1 = p1;
        e.construction = m_construction; return e; };
    auto corner = [&](const Vec2d& O, const Vec2d& sharp, const Vec2d& start, const Vec2d& end) {
        const Vec2d thr = O + r * (sharp - O).normalized();
        return make_arc_through(O, r, start, end, thr, m_construction); };
    out.push_back(line({xmin + r, ymin}, {xmax - r, ymin}));                                  // bottom
    out.push_back(corner({xmax - r, ymin + r}, {xmax, ymin}, {xmax - r, ymin}, {xmax, ymin + r})); // BR
    out.push_back(line({xmax, ymin + r}, {xmax, ymax - r}));                                  // right
    out.push_back(corner({xmax - r, ymax - r}, {xmax, ymax}, {xmax, ymax - r}, {xmax - r, ymax})); // TR
    out.push_back(line({xmax - r, ymax}, {xmin + r, ymax}));                                  // top
    out.push_back(corner({xmin + r, ymax - r}, {xmin, ymax}, {xmin + r, ymax}, {xmin, ymax - r})); // TL
    out.push_back(line({xmin, ymax - r}, {xmin, ymin + r}));                                  // left
    out.push_back(corner({xmin + r, ymin + r}, {xmin, ymin}, {xmin, ymin + r}, {xmin + r, ymin})); // BL
    return out;
}

std::vector<SketchEntity> DesignSketchTool::make_rounded_rect(const Vec2d& a, const Vec2d& b, const Vec2d& radius_pt) const
{
    std::vector<SketchEntity> out;
    const double xmin = std::min(a.x(), b.x()), xmax = std::max(a.x(), b.x());
    const double ymin = std::min(a.y(), b.y()), ymax = std::max(a.y(), b.y());
    const double bw = xmax - xmin, bh = ymax - ymin;
    if (bw < 1e-6 || bh < 1e-6) return out;
    const Vec2d cs[4] = { {xmin,ymin}, {xmax,ymin}, {xmax,ymax}, {xmin,ymax} };
    double r = 1e18;
    for (const Vec2d& c : cs) r = std::min(r, (radius_pt - c).norm());
    r = std::min(r, std::min(bw, bh) * 0.5);
    if (r < 1e-6) {   // degenerate -> plain rectangle
        auto line = [&](const Vec2d& p0, const Vec2d& p1) {
            SketchEntity e; e.type = SketchEntity::Type::Line; e.p0 = p0; e.p1 = p1;
            e.construction = m_construction; return e; };
        for (int i = 0; i < 4; ++i) out.push_back(line(cs[i], cs[(i + 1) % 4]));
        return out;
    }
    return rounded_rect_entities(xmin, ymin, xmax, ymax, r);
}

std::vector<SketchEntity> DesignSketchTool::make_polygon(const Vec2d& center, const Vec2d& vertex, int sides) const
{
    std::vector<SketchEntity> out;
    if (sides < 3) sides = 3;
    const Vec2d rv = vertex - center;
    const double d = rv.norm();
    if (d < 1e-6) return out;
    // Inscribed: cursor is a vertex (circumradius = d). Circumscribed: cursor is an
    // edge midpoint (apothem = d) → circumradius R = d / cos(pi/n), rotated by half
    // a step so an edge midpoint points at the cursor.
    double R = d, a0 = std::atan2(rv.y(), rv.x());
    if (m_polygon_circumscribed) {
        R  = d / std::cos(M_PI / double(sides));
        a0 = std::atan2(rv.y(), rv.x()) - M_PI / double(sides);
    }
    std::vector<Vec2d> verts; verts.reserve(sides);
    for (int i = 0; i < sides; ++i) {
        const double a = a0 + 2.0 * M_PI * double(i) / double(sides);
        verts.push_back(Vec2d(center.x() + R * std::cos(a), center.y() + R * std::sin(a)));
    }
    for (int i = 0; i < sides; ++i) {
        SketchEntity e; e.type = SketchEntity::Type::Line;
        e.p0 = verts[i]; e.p1 = verts[(i + 1) % sides];
        e.construction = m_construction;
        out.push_back(e);
    }
    return out;
}

// Derive (a, b, phi) of an ellipse from the 3 defining clicks. First axis click =
// major (a, phi); the minor point's perpendicular distance to the major axis = b,
// clamped to a so OCCT's a >= b holds.
static void ellipse_axes(const Vec2d& center, const Vec2d& major_end, const Vec2d& minor_pt,
                         double& a, double& b, double& phi)
{
    const Vec2d maj = major_end - center;
    a   = std::max(maj.norm(), 1e-6);
    phi = std::atan2(maj.y(), maj.x());
    const Vec2d n(-std::sin(phi), std::cos(phi));        // minor-axis direction
    b   = std::min(std::abs((minor_pt - center).dot(n)), a);
}

// Parametric angle on an ellipse of the point nearest `q` (q projected onto the frame).
static double ellipse_param_of(const Vec2d& center, double a, double b, double phi, const Vec2d& q)
{
    const Vec2d d = q - center;
    const double cu = std::cos(phi), su = std::sin(phi);
    const double u =  d.x() * cu + d.y() * su;            // along major
    const double v = -d.x() * su + d.y() * cu;            // along minor
    return std::atan2(v / std::max(b, 1e-9), u / std::max(a, 1e-9));
}

std::vector<SketchEntity> DesignSketchTool::make_ellipse(const Vec2d& center, const Vec2d& major_end,
                                                         const Vec2d& minor_pt) const
{
    double a, b, phi;
    ellipse_axes(center, major_end, minor_pt, a, b, phi);
    if (b < 1e-6) return {};
    SketchEntity e;
    e.type = SketchEntity::Type::Ellipse;
    e.center = center; e.p0 = center;
    e.radius = a; e.rminor = b; e.rotation = phi;
    e.start_angle = 0.0; e.end_angle = 2.0 * M_PI;
    e.construction = m_construction;
    return { e };
}

std::vector<SketchEntity> DesignSketchTool::make_ellipse_arc(const Vec2d& center, const Vec2d& major_end,
                                                             const Vec2d& minor_pt, const Vec2d& start_pt,
                                                             const Vec2d& end_pt) const
{
    double a, b, phi;
    ellipse_axes(center, major_end, minor_pt, a, b, phi);
    if (b < 1e-6) return {};
    double t0 = ellipse_param_of(center, a, b, phi, start_pt);
    double t1 = ellipse_param_of(center, a, b, phi, end_pt);
    // CCW sweep from t0 to t1.
    while (t1 <= t0) t1 += 2.0 * M_PI;
    SketchEntity e;
    e.type = SketchEntity::Type::EllipseArc;
    e.center = center;
    e.radius = a; e.rminor = b; e.rotation = phi;
    e.start_angle = t0; e.end_angle = t1;
    e.p0 = ellipse_point(center, a, b, phi, t0);
    e.p1 = ellipse_point(center, a, b, phi, t1);
    e.construction = m_construction;
    return { e };
}

std::vector<SketchEntity> DesignSketchTool::make_bspline(const std::vector<Vec2d>& ctrl) const
{
    if (ctrl.size() < 2) return {};
    SketchEntity e;
    e.type = SketchEntity::Type::BSpline;
    e.ctrl = ctrl;
    e.p0 = ctrl.front();
    e.p1 = ctrl.back();
    e.construction = m_construction;
    return { e };
}

std::vector<Vec2d> DesignSketchTool::entity_polyline(const SketchEntity& e, bool& closed) const
{
    closed = false;
    switch (e.type) {
    case SketchEntity::Type::Line:
        return { e.p0, e.p1 };
    case SketchEntity::Type::Circle:
        closed = true;
        return circle_polygon(e.center, e.radius);
    case SketchEntity::Type::Arc: {
        const int n = 24;
        std::vector<Vec2d> pts; pts.reserve(n + 1);
        for (int i = 0; i <= n; ++i) {
            const double a = e.start_angle + (e.end_angle - e.start_angle) * double(i) / double(n);
            pts.push_back(Vec2d(e.center.x() + e.radius * std::cos(a),
                                e.center.y() + e.radius * std::sin(a)));
        }
        return pts;
    }
    case SketchEntity::Type::Ellipse:
        closed = true;
        return ellipse_polyline(e.center, e.radius, e.rminor, e.rotation, 0.0, 2.0 * M_PI);
    case SketchEntity::Type::EllipseArc:
        return ellipse_polyline(e.center, e.radius, e.rminor, e.rotation, e.start_angle, e.end_angle);
    case SketchEntity::Type::BSpline:
        return bspline_polyline(e.ctrl);
    case SketchEntity::Type::Point:
        return { e.p0 };
    }
    return {};
}

std::vector<std::vector<Vec2d>> DesignSketchTool::closed_regions() const
{
    return closed_regions(m_entities);
}

std::vector<std::vector<Vec2d>> DesignSketchTool::closed_regions(const std::vector<SketchEntity>& ents) const
{
    std::vector<std::vector<Vec2d>> out;
    for (RegionLoop& r : region_loops(ents)) out.push_back(std::move(r.poly));
    return out;
}

std::vector<std::vector<int>>
DesignSketchTool::region_entity_indices(const std::vector<SketchEntity>& ents) const
{
    std::vector<std::vector<int>> out;
    for (RegionLoop& r : region_loops(ents)) out.push_back(std::move(r.ents));
    return out;
}

std::vector<std::vector<int>>
DesignSketchTool::region_entity_indices_with_holes(const std::vector<SketchEntity>& ents) const
{
    const std::vector<RegionLoop> loops = region_loops(ents);
    std::vector<std::vector<int>> out;
    out.reserve(loops.size());
    for (const RegionLoop& r : loops) {
        std::vector<int> ids = r.ents;
        for (int h : r.holes)
            if (h >= 0 && h < int(loops.size()))
                ids.insert(ids.end(), loops[h].ents.begin(), loops[h].ents.end());
        out.push_back(std::move(ids));
    }
    return out;
}

// Nearest stroke, and the enclosing region if the cursor is inside one, for ONE committed
// sketch. Factored out so the single-click and double-click paths cannot drift apart: they must
// agree about what is under the pointer or one of them will act on something else.
//
// region_loops exists to find EXTRUDABLE regions, and by design it discards open chains — its
// own walk comment says so. Using it as the pick index meant a committed sketch of open lines
// had no pickable geometry whatsoever: the strokes drew, and not one of them could be clicked,
// so there was no way to select it and therefore none to edit or delete it. Whether a stroke
// bounds a region has nothing to do with whether the user can point at it. Region membership
// decides what a hit REPORTS, not whether the hit can happen.
static void dp_pick_trace(const char* fmt, ...);   // defined below; used by the diagnostics here
static bool dp_pick_trace_on();                    // ditto — lets callers skip building a message
void DesignSketchTool::hit_display_sketch(const DisplaySketch& d, const Vec2d& p, double tol,
                                          int& edge_feat, int& edge_reg, int& edge_ent,
                                          double& edge_d, int& face_feat, int& face_reg) const
{
    const std::vector<RegionLoop> loops = region_loops(d.entities);
    // What did the sketch decompose into, and what is under the click? This is the trace that
    // settled snaporca-txp8 — it prints the loop table with each loop's hole count, so
    // "containment is wrong" and "the click landed elsewhere" stop being indistinguishable.
    // Guarded rather than merely silent: hit_display_sketch runs on every pick, and the message
    // costs a string build and a heap allocation per loop even when nothing consumes it.
    if (dp_pick_trace_on()) {
        std::string h;
        for (size_t r = 0; r < loops.size(); ++r) {
            h += " loop" + std::to_string(r) + "(ents=" + std::to_string(loops[r].ents.size())
               + ",poly=" + std::to_string(loops[r].poly.size())
               + ",holes=" + std::to_string(loops[r].holes.size()) + ")";
        }
        dp_pick_trace("sketch feat=%d entities=%zu loops=%zu:%s", d.feature, d.entities.size(),
                      loops.size(), h.c_str());
    }
    std::vector<int> ent_region(d.entities.size(), -1);
    for (int r = 0; r < int(loops.size()); ++r)
        for (int ei : loops[r].ents)
            if (ei >= 0 && ei < int(ent_region.size())) ent_region[ei] = r;

    for (int ei = 0; ei < int(d.entities.size()); ++ei) {
        if (d.entities[ei].construction) continue;   // as region_loops filters it
        const double ed = entity_pick_dist(p, d.entities[ei]);
        if (ed <= tol * 3.0 && ed < edge_d) {
            edge_d = ed; edge_feat = d.feature; edge_reg = ent_region[ei]; edge_ent = ei;
        }
    }
    // Pick the region the point is REALLY in: inside its boundary and not inside any of its
    // holes. Clicking the middle of a plate-with-hole must select the plate; clicking inside
    // the hole must select the disc, not the plate. Previously the first containing polygon
    // won, so a click inside the circle selected the rectangle.
    for (int r = 0; r < int(loops.size()); ++r) {
        if (face_feat >= 0) break;
        if (!point_in_poly(p, loops[r].poly)) continue;
        bool in_hole = false;
        for (int h : loops[r].holes)
            if (h >= 0 && h < int(loops.size()) && point_in_poly(p, loops[h].poly)) { in_hole = true; break; }
        if (!in_hole) { face_feat = d.feature; face_reg = r; }
    }
    dp_pick_trace("region hit -> feat=%d reg=%d (edge_feat=%d edge_reg=%d)",
                  face_feat, face_reg, edge_feat, edge_reg);
}

std::vector<SketchEntity> DesignSketchTool::selected_loop_entities() const
{
    if (m_display_pick < 0 || m_display_pick_region < 0) return {};
    for (const DisplaySketch& d : m_display_sketches) {
        if (d.feature != m_display_pick) continue;
        const std::vector<RegionLoop> loops = region_loops(d.entities);
        if (m_display_pick_region >= int(loops.size())) return {};
        // The region's OWN boundary plus every loop nested in it. Handing over only the outer
        // loop is what made a rectangle-with-a-circle extrude to a plain box: the circle was
        // never passed to the kernel, so build_sketch_face had one loop to work with and the
        // multi-loop path never ran.
        std::vector<SketchEntity> out;
        auto take = [&](int region) {
            if (region < 0 || region >= int(loops.size())) return;
            for (int ei : loops[region].ents)
                if (ei >= 0 && ei < int(d.entities.size())) out.push_back(d.entities[ei]);
        };
        take(m_display_pick_region);
        for (int h : loops[m_display_pick_region].holes) take(h);
        return out;
    }
    return {};
}

// ---- Solid topology selection (whole -> face -> edge cycle) ----

void DesignSketchTool::set_solid_pick(const std::vector<CadBody>* bodies, const TriangleMesh* mesh,
                                      const std::vector<int>* tri_face, const std::vector<int>* tri_body,
                                      const std::vector<bool>* visible,
                                      const std::vector<Transform3d>* xform)
{
    // Treat no bodies or an empty mesh as "no solid" so has_display()/picking stay off.
    if (bodies == nullptr || bodies->empty() || mesh == nullptr || mesh->its.indices.empty()) {
        m_solid_bodies = nullptr; m_solid_mesh = nullptr; m_solid_tri_face = nullptr; m_solid_tri_body = nullptr;
        m_solid_visible = nullptr; m_solid_xform = nullptr;
    } else {
        m_solid_bodies = bodies; m_solid_mesh = mesh; m_solid_tri_face = tri_face; m_solid_tri_body = tri_body;
        m_solid_visible = visible; m_solid_xform = xform;
    }
    clear_solid_selection();
}

// Map a point sampled from the (untransformed) OCCT body shape through the body's display
// transform, so edge picking/highlight track a moved body. The pick MESH is already
// transformed by the host; only OCCT-sampled edges need this.
Vec3d DesignSketchTool::body_xform_pt(int body, const Vec3d& p) const
{
    if (m_solid_xform != nullptr && body >= 0 && body < int(m_solid_xform->size()))
        return (*m_solid_xform)[body] * p;
    return p;
}

// A body is pickable unless an explicit visibility vector marks it hidden.
bool DesignSketchTool::body_pickable(int b) const
{
    if (b < 0) return false;
    // Body-focus mode. The focus is an INDEX held by the panel across recomputes, so it can
    // outlive the body it names — delete a body and the stored index may point past the end.
    // A restriction to a body that no longer exists rejects EVERY body, which is a viewport
    // that silently accepts no clicks at all: the worst possible failure for a picking mode,
    // because nothing on screen says why. Out of range therefore means NO restriction — fail
    // open, never dead.
    const bool focus_live = m_pick_only_body >= 0 && m_solid_bodies != nullptr
                            && m_pick_only_body < int(m_solid_bodies->size());
    if (focus_live && b != m_pick_only_body) return false;
    if (m_solid_visible == nullptr || b >= int(m_solid_visible->size())) return true;
    return (*m_solid_visible)[b];
}

void DesignSketchTool::clear_solid_selection()
{
    m_solid_sel = SolidSel::None;
    m_sel_body = m_sel_face = m_sel_edge = -1;
    m_sel_edge_pts.clear();
    // The pre-highlight names a face/edge/vertex by index into a shape that a recompute has just
    // rebuilt, so it expires with the selection it was a promise about. Left behind it would keep
    // glowing on whatever now sits at those indices — a real entity, but not the one meant.
    m_pre = SolidPick{};
}

void DesignSketchTool::select_body(int body)
{
    // Hidden bodies aren't highlighted (the tint overlay would otherwise draw over a
    // body whose GLVolume is off, leaving a ghost after a hide).
    if (m_solid_bodies == nullptr || body < 0 || body >= int(m_solid_bodies->size())
        || !body_pickable(body)) {
        clear_solid_selection();
        return;
    }
    m_sel_body  = body;
    m_sel_face  = m_sel_edge = -1;
    m_sel_edge_pts.clear();
    m_solid_sel = SolidSel::Whole;   // render_solid_highlight tints just this body
}

// Pick tracing. Selection failures on a real desktop have repeatedly turned out to be an
// event that never arrived rather than a ray that missed, and the two look identical from
// the UI. Set SNAPORCA_PICK_TRACE=1 and the whole press->release->ray path narrates itself
// on stderr. Off by default: no cost, no noise.
static bool dp_pick_trace_on()
{
    static const bool on = ::getenv("SNAPORCA_PICK_TRACE") != nullptr;
    return on;
}

static void dp_pick_trace(const char* fmt, ...)
{
    if (!dp_pick_trace_on()) return;
    va_list ap; va_start(ap, fmt);
    std::fputs("[pick] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    std::fflush(stderr);
}

// Resolve a swept rubber band into a whole-body selection.
//
// Sample points are the display mesh's triangle vertices plus each triangle's centroid. That
// mesh is already in world coordinates — the very points the ray pick tests — so no per-body
// transform is needed here. A body counts as swept when any of its samples lands inside the
// rectangle (crossing semantics: touching selects, which is the forgiving reading of a sweep),
// and the body with the most samples inside wins because the selection callback downstream
// carries exactly one body.
//
// ponytail: crossing over a triangle sample set. A rectangle small enough to sit entirely
// inside one flat triangle selects nothing — drag a bigger one, or click. Real multi-body
// selection (and the homogeneous-set rule that goes with it) is snaporca-9xw.
void DesignSketchTool::pick_bodies_in_rectangle()
{
    if (m_solid_mesh == nullptr || m_solid_tri_body == nullptr || m_solid_bodies == nullptr)
        return;
    const indexed_triangle_set& its = m_solid_mesh->its;
    std::vector<Vec3d> pts;
    std::vector<int>   owner;
    pts.reserve(its.indices.size() * 4);
    owner.reserve(its.indices.size() * 4);
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const int b = (i < m_solid_tri_body->size()) ? (*m_solid_tri_body)[i] : -1;
        if (!body_pickable(b)) continue;          // hidden bodies aren't swept either
        const auto& idx = its.indices[i];
        Vec3d c = Vec3d::Zero();
        for (int k = 0; k < 3; ++k) {
            const Vec3d p = its.vertices[idx(k)].cast<double>();
            pts.push_back(p); owner.push_back(b); c += p;
        }
        pts.push_back(c / 3.0); owner.push_back(b);
    }

    std::vector<int> hits(m_solid_bodies->size(), 0);
    if (!pts.empty())
        for (unsigned int i : m_rubber.contains(pts))
            if (i < owner.size() && owner[i] >= 0 && owner[i] < int(hits.size()))
                ++hits[owner[i]];

    int best = -1, best_n = 0;
    for (int b = 0; b < int(hits.size()); ++b)
        if (hits[b] > best_n) { best_n = hits[b]; best = b; }

    if (best < 0) clear_solid_selection();        // swept empty space -> drop the selection
    else          select_body(best);
    dp_pick_trace("rubber band -> body=%d (%d samples)", best, best_n);
    if (on_solid_selection_changed)
        on_solid_selection_changed(int(m_solid_sel), m_sel_body, m_sel_face, m_sel_edge);
}

// Resolve what a pick at (mx,my) would take. CONST, and it writes only into `out`: the hover
// path calls this many times a second and must not disturb the committed selection by doing so.
bool DesignSketchTool::resolve_solid_pick(GLCanvas3D& canvas, int mx, int my, SolidPick& out) const
{
    out = SolidPick{};
    if (m_solid_bodies == nullptr || m_solid_mesh == nullptr) {
        dp_pick_trace("no solid data (bodies=%p mesh=%p)",
                      (const void*) m_solid_bodies, (const void*) m_solid_mesh);
        return false;
    }
    const Linef3 r = canvas.mouse_ray(Point(mx, my));
    const Vec3d ro = r.a, rd = r.b - r.a;

    // 1) nearest solid face under the cursor (ray vs display-mesh triangles). Resolve WHICH
    //    body and which face-within-that-body via the per-triangle (tri_body, tri_face) tags.
    const indexed_triangle_set& its = m_solid_mesh->its;
    int best_face = -1, best_body = -1; double best_t = 1e30;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const auto& idx = its.indices[i];
        const Vec3d v0 = its.vertices[idx(0)].cast<double>();
        const Vec3d v1 = its.vertices[idx(1)].cast<double>();
        const Vec3d v2 = its.vertices[idx(2)].cast<double>();
        double t;
        if (ray_triangle(ro, rd, v0, v1, v2, t) && t < best_t) {
            const int cand_body = (m_solid_tri_body && i < m_solid_tri_body->size()) ? (*m_solid_tri_body)[i] : -1;
            if (!body_pickable(cand_body)) continue;   // hidden bodies don't catch clicks
            best_t = t;
            best_face = (m_solid_tri_face && i < m_solid_tri_face->size()) ? (*m_solid_tri_face)[i] : -1;
            best_body = cand_body;
        }
    }
    dp_pick_trace("ray tris=%zu -> body=%d face=%d t=%.3f", its.indices.size(),
                  best_body, best_face, best_t);
    if (best_face < 0 || best_body < 0 || best_body >= int(m_solid_bodies->size()))
        return false;   // missed the solid

    // ---- one click, one deterministic result ---------------------------------------------
    // NO CYCLE. A click selects the SMALLEST thing under the cursor: the edge if the pointer is
    // within tolerance of one, otherwise the face. The WHOLE body is taken by a left-drag
    // rubber band (pick_bodies_in_rectangle) — a different gesture for a different scale.
    //
    // What this replaces: click 1 = whole body, click 2 = face, click 3 = nearest edge, click 4
    // = back to whole. That made "click a face" a two-click gesture and "click an edge" a
    // three-click one, neither discoverable — the L5 violation §10 of the charter already
    // listed, and the real reason sketching on a face kept reading as broken however often the
    // plane resolution was fixed. Selection is the foundation the tool offer stands on: the
    // offer can only ever be as truthful as the selection beneath it.
    //
    // Tolerance is measured in SCREEN PIXELS. The old edge step compared a ray-to-segment
    // distance in millimetres, so the same gesture meant different things at different zooms —
    // the pointer is a screen object and its tolerance has to be one too.
    const Camera& cam = wxGetApp().plater()->get_camera();
    const wxPoint cursor(mx, my);
    // Vertex beats edge beats face, and the vertex tolerance is the larger of the two: a corner
    // sits ON its edges, so an equal radius would make vertices unreachable — every click near
    // one would resolve to the edge it lies on.
    const double  kVertexTolPx = 11.0;
    const double  kEdgeTolPx   = 8.0;

    auto seg_px = [](const wxPoint& p, const wxPoint& a, const wxPoint& b) {   // 2D point→segment, px
        const double vx = b.x - a.x, vy = b.y - a.y;
        const double wx = p.x - a.x, wy = p.y - a.y;
        const double L2 = vx * vx + vy * vy;
        double t = (L2 > 1e-12) ? (wx * vx + wy * vy) / L2 : 0.0;
        t = std::max(0.0, std::min(1.0, t));
        return std::hypot(wx - t * vx, wy - t * vy);
    };

    out.body = best_body;
    out.face = best_face;

    {
        const TopoDS_Shape& bshape = (*m_solid_bodies)[out.body].shape;
        const TopoDS_Face  face    = GeometryEngine::face_by_index(bshape, out.face);
        double best_ed = 1e30; std::vector<Vec3d> ed_pts; TopoDS_Edge ed_edge; bool have_edge = false;
        double best_vd = 1e30; Vec3d vtx = Vec3d::Zero(); bool have_vtx = false;
        // Screen extent of the face, accumulated from the same edge samples the loop already
        // takes. A FIXED edge tolerance makes a narrow face unreachable: a 3 mm-wide plate is
        // barely wider on screen than the 8 px budget, so every point on it is "on an edge" and
        // the face level can never be picked — which also blocks the face-based Coord Sys that a
        // mate needs. The tolerances below shrink with the face so its middle stays its own.
        int fx0 = INT_MAX, fy0 = INT_MAX, fx1 = INT_MIN, fy1 = INT_MIN;
        if (!face.IsNull()) {
            for (const TopoDS_Edge& e : GeometryEngine::edges_of_face(face)) {
                std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(e);
                for (Vec3d& q : pts) q = body_xform_pt(out.body, q);   // follow a moved body
                if (pts.size() < 2) continue;
                // The polyline ends ARE the edge's vertices; every corner of the face is the
                // end of one of its edges, so this covers them without a separate topology walk.
                for (const Vec3d& v : {pts.front(), pts.back()}) {
                    const wxPoint sp = world_to_screen_px(cam, v);
                    if (sp.x < 0) continue;
                    const double d = std::hypot(double(sp.x - cursor.x), double(sp.y - cursor.y));
                    if (d < best_vd) { best_vd = d; vtx = v; have_vtx = true; }
                }
                double d = 1e30;
                for (size_t s = 1; s < pts.size(); ++s) {
                    const wxPoint a = world_to_screen_px(cam, pts[s - 1]);
                    const wxPoint b = world_to_screen_px(cam, pts[s]);
                    if (a.x < 0 || b.x < 0) continue;                    // behind the camera
                    fx0 = std::min({fx0, a.x, b.x}); fx1 = std::max({fx1, a.x, b.x});
                    fy0 = std::min({fy0, a.y, b.y}); fy1 = std::max({fy1, a.y, b.y});
                    d = std::min(d, seg_px(cursor, a, b));
                }
                if (d < best_ed) { best_ed = d; ed_pts = pts; ed_edge = e; have_edge = true; }
            }
        }
        // A third of the face's SHORTER on-screen side, so the two tolerances can never meet in
        // the middle. Only ever shrinks: a face big enough keeps the full budget.
        double vtol = kVertexTolPx, etol = kEdgeTolPx;
        if (fx1 > fx0 && fy1 > fy0) {
            const double narrow = double(std::min(fx1 - fx0, fy1 - fy0)) / 3.0;
            vtol = std::min(vtol, narrow);
            etol = std::min(etol, narrow);
        }
        if (have_vtx && best_vd <= vtol) {
            out.vertex_pt = vtx;
            out.kind      = SolidSel::Vertex;
        } else if (have_edge && best_ed <= etol) {
            // Promote the face-relative pick to a STABLE GLOBAL edge id so dress-up ops
            // (fillet/chamfer) can target this exact edge across recomputes.
            out.edge     = GeometryEngine::edge_index_of(bshape, ed_edge);
            out.edge_pts = std::move(ed_pts);
            out.kind     = SolidSel::Edge;
        } else {
            out.kind = SolidSel::Face;
        }
    }
    return true;
}

// A click on the solid takes the smallest thing under the cursor (vertex/edge/face). Returns
// true if the click hit the solid (consumed); false otherwise so the caller can try
// committed-sketch loop picking. Whole bodies are taken by the rubber band, not by clicking.
bool DesignSketchTool::handle_solid_click(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    SolidPick p;
    if (!resolve_solid_pick(canvas, evt.GetX(), evt.GetY(), p))
        return false;   // missed the solid, or no solid data — same two exits as before

    // What was selected BEFORE this pick — the escalation below is the only thing that reads
    // it, and everything from here on overwrites it.
    const SolidSel prev_kind = m_solid_sel;
    const int      prev_body = m_sel_body, prev_face = m_sel_face, prev_edge = m_sel_edge;
    const Vec3d    prev_vtx  = m_sel_vertex_pt;

    m_sel_body      = p.body;
    m_sel_face      = p.face;
    m_sel_edge      = p.edge;
    m_sel_edge_pts  = std::move(p.edge_pts);
    m_sel_vertex_pt = p.vertex_pt;
    m_solid_sel     = p.kind;

    // CLICK AGAIN ON THE SAME THING -> THE WHOLE BODY (snaporca-gem). Pointing at a face and
    // pointing at its body are different intents, and until now only the rubber band could
    // express the second one — so the status line said "face 0 selected" while the user
    // believed they had taken the body, and every body verb had to opt into the face kinds to
    // stay reachable. One more click on the SAME sub-element escalates.
    //
    // This is not the pick cycle that was removed (bc2b741ce9). That one was silent and three
    // deep, so no click had a predictable meaning. Here the escalation is announced by the
    // status line BEFORE you make the click, and a further click just takes the face under the
    // cursor again — the ordinary meaning of clicking a face, which needs no teaching.
    //
    // Double-click is safe: wx sends Down/Up/DClick/Up, and only the first Up carries a
    // pending press, so a fast double-click zooms to fit and picks ONCE. Escalation needs two
    // separate clicks, the same "click, pause, click" distinction a file manager uses.
    //
    // "The same thing" is compared AT THE LEVEL THAT WAS PICKED, and nothing else. Requiring
    // every field to match looked stricter and was simply wrong: an edge pick leaves m_sel_face
    // set to whichever face the ray happened to hit, and a shared edge is reached through a
    // different face depending on which side of the body you are looking from. So picking an
    // edge, orbiting, and clicking that same edge from the other side left m_sel_edge equal and
    // m_sel_face different, and the escalation the status line had just promised did not happen.
    // The edge id here is already the STABLE GLOBAL one (edge_index_of, a few lines up) — it
    // identifies the edge on its own and does not need the face to disambiguate it.
    const bool same_pick = m_solid_sel == prev_kind && m_sel_body == prev_body
        && (m_solid_sel == SolidSel::Vertex ? (m_sel_vertex_pt - prev_vtx).norm() < 1e-9
          : m_solid_sel == SolidSel::Edge   ? m_sel_edge == prev_edge
          : m_solid_sel == SolidSel::Face   ? m_sel_face == prev_face
                                            : true);
    if (same_pick && m_escalate_repick) {
        select_body(m_sel_body);   // clears face/edge/vertex, tints the whole body
        dp_pick_trace("re-pick -> escalated to whole body %d", m_sel_body);
    }
    dp_pick_trace("pick -> sel=%d body=%d face=%d edge=%d",
                  int(m_solid_sel), m_sel_body, m_sel_face, m_sel_edge);
    if (on_solid_selection_changed)
        on_solid_selection_changed(int(m_solid_sel), m_sel_body, m_sel_face, m_sel_edge);
    return true;
}

// Recompute what the pointer is over. Returns true only when the answer CHANGED — this runs on
// every motion event, and a repaint per event would cost far more than the pick itself.
bool DesignSketchTool::update_solid_hover(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    SolidPick p;
    if (!resolve_solid_pick(canvas, evt.GetX(), evt.GetY(), p))
        p = SolidPick{};   // off the solid: no promise to make
    // Compared AT THE LEVEL THAT WAS PICKED, for the same reason the click's escalation is
    // (see same_pick above): an edge pick carries whichever face the ray happened to cross, and
    // that face changes as the pointer slides along the edge without the answer changing at all.
    const bool same = p.kind == m_pre.kind && p.body == m_pre.body
        && (p.kind == SolidSel::Vertex ? (p.vertex_pt - m_pre.vertex_pt).norm() < 1e-9
          : p.kind == SolidSel::Edge   ? p.edge == m_pre.edge
          : p.kind == SolidSel::Face   ? p.face == m_pre.face
                                       : true);
    if (same) return false;
    m_pre = std::move(p);
    return true;
}

// One highlight, drawn from explicit arguments rather than from the selection members, so the
// committed selection and the hover pre-highlight cannot drift apart in how they look. alpha_mul
// scales every layer at once: the pre-highlight is the same shape in the same place, quieter.
void DesignSketchTool::render_solid_sel(SolidSel kind, int body, int face,
                                        const std::vector<Vec3d>& edge_pts,
                                        const Vec3d& vertex_pt,
                                        const ColorRGBA& rgb, float alpha_mul)
{
    using EPT = GLModel::Geometry::EPrimitiveType;
    using EVL = GLModel::Geometry::EVertexLayout;
    // Opaque: the edge ribbon and the vertex square render with GL_BLEND OFF, so an alpha below 1
    // here would be silently ignored. Those two are quietened by a MUTED rgb from the caller
    // instead; alpha_mul only reaches the face fill, which is the one layer that is blended.
    const ColorRGBA cyan(rgb.r(), rgb.g(), rgb.b(), 1.0f);

    // Whole tints the picked BODY (all its triangles, lighter alpha); Face tints just the
    // picked face on that body. Both filter by `body` so other bodies stay untinted.
    if ((kind == SolidSel::Face || kind == SolidSel::Whole)
        && m_solid_mesh != nullptr && m_solid_tri_body != nullptr && body >= 0) {
        const bool face_only = (kind == SolidSel::Face);
        const indexed_triangle_set& its = m_solid_mesh->its;
        GLModel::Geometry g; g.format = { EPT::Triangles, EVL::P3 };
        unsigned int base = 0;
        for (size_t i = 0; i < its.indices.size(); ++i) {
            if (i >= m_solid_tri_body->size() || (*m_solid_tri_body)[i] != body) continue;
            if (face_only && (m_solid_tri_face == nullptr || i >= m_solid_tri_face->size()
                              || (*m_solid_tri_face)[i] != face)) continue;
            const auto& idx = its.indices[i];
            for (int j = 0; j < 3; ++j) g.add_vertex(its.vertices[idx(j)]);
            g.add_triangle(base, base + 1, base + 2); base += 3;
        }
        if (base > 0) {
            glsafe(::glEnable(GL_DEPTH_TEST));
            glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
            glsafe(::glPolygonOffset(-2.0f, -2.0f));
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            m_solid_face_model.reset();
            m_solid_face_model.init_from(std::move(g));
            m_solid_face_model.set_color(ColorRGBA(rgb.r(), rgb.g(), rgb.b(),
                                                  (face_only ? 0.40f : 0.22f) * alpha_mul));
            m_solid_face_model.render();
            glsafe(::glDisable(GL_BLEND));
            glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
            glsafe(::glDisable(GL_DEPTH_TEST));
        }
    } else if (kind == SolidSel::Edge && edge_pts.size() >= 2) {
        const Camera& cam = wxGetApp().plater()->get_camera();
        const Vec3d vd = cam.get_dir_forward();
        const double hw = 2.0 / std::max(cam.get_zoom(), 1e-6);   // ~2 px ribbon half-width
        GLModel::Geometry g; g.format = { EPT::Triangles, EVL::P3 };
        unsigned int base = 0;
        for (size_t s = 1; s < edge_pts.size(); ++s) {
            const Vec3d a = edge_pts[s - 1], b = edge_pts[s];
            Vec3d dir = b - a; if (dir.norm() < 1e-9) continue; dir.normalize();
            Vec3d off = dir.cross(vd);
            if (off.norm() < 1e-9) off = dir.cross(cam.get_dir_up());
            if (off.norm() < 1e-9) continue;
            off.normalize(); off *= hw;
            g.add_vertex((Vec3f)(a + off).cast<float>());
            g.add_vertex((Vec3f)(b + off).cast<float>());
            g.add_vertex((Vec3f)(b - off).cast<float>());
            g.add_vertex((Vec3f)(a - off).cast<float>());
            g.add_triangle(base, base + 1, base + 2);
            g.add_triangle(base, base + 2, base + 3); base += 4;
        }
        if (base > 0) {
            glsafe(::glDisable(GL_DEPTH_TEST));
            m_solid_edge_model.reset();
            m_solid_edge_model.init_from(std::move(g));
            m_solid_edge_model.set_color(cyan);
            m_solid_edge_model.render();
        }
    } else if (kind == SolidSel::Vertex) {
        // A camera-facing square at the picked corner, sized in screen terms (the same
        // 1/zoom trick the edge ribbon uses) so it stays a constant dot at every zoom. A
        // selection you cannot see is not a selection (L5), which is why vertex picking waited
        // for this rather than shipping without a highlight.
        const Camera& cam = wxGetApp().plater()->get_camera();
        Vec3d right = cam.get_dir_right(), up = cam.get_dir_up();
        const double h = 4.5 / std::max(cam.get_zoom(), 1e-6);   // ~4.5 px half-size
        right *= h; up *= h;
        const Vec3d c = vertex_pt;
        GLModel::Geometry g; g.format = { EPT::Triangles, EVL::P3 };
        g.add_vertex((Vec3f)(c - right - up).cast<float>());
        g.add_vertex((Vec3f)(c + right - up).cast<float>());
        g.add_vertex((Vec3f)(c + right + up).cast<float>());
        g.add_vertex((Vec3f)(c - right + up).cast<float>());
        g.add_triangle(0, 1, 2);
        g.add_triangle(0, 2, 3);
        glsafe(::glDisable(GL_DEPTH_TEST));
        m_solid_vertex_model.reset();
        m_solid_vertex_model.init_from(std::move(g));
        m_solid_vertex_model.set_color(cyan);
        m_solid_vertex_model.render();
    }
}

// Cyan overlay for the picked face / edge / vertex, plus the quieter pre-highlight of whatever
// the pointer is currently over. Whole-solid tint is the panel's job (set_body_highlight).
// Called from render() while no sketch session is active.
void DesignSketchTool::render_solid_highlight()
{
    const ColorRGBA sel_cyan = design_selection_color();

    // The pre-highlight goes FIRST so the committed selection paints over it where the two
    // overlap — what you HAVE outranks what you would get. Suppressed entirely when they are the
    // same thing: two coats of the same colour on the same face reads as a rendering fault, and
    // a promise about a click that would change nothing is not worth making.
    const bool pre_is_sel = m_pre.kind == m_solid_sel && m_pre.body == m_sel_body
        && (m_pre.kind == SolidSel::Vertex ? (m_pre.vertex_pt - m_sel_vertex_pt).norm() < 1e-9
          : m_pre.kind == SolidSel::Edge   ? m_pre.edge == m_sel_edge
          : m_pre.kind == SolidSel::Face   ? m_pre.face == m_sel_face
                                           : true);
    if (m_pre.kind != SolidSel::None && !pre_is_sel)
        // Desaturated toward white rather than a second hue: a distinct colour would read as a
        // distinct KIND of selection, when it is the same selection one moment earlier.
        render_solid_sel(m_pre.kind, m_pre.body, m_pre.face, m_pre.edge_pts, m_pre.vertex_pt,
                         design_selection_color(), 0.45f);   // hover = the same colour, quieter

    render_solid_sel(m_solid_sel, m_sel_body, m_sel_face, m_sel_edge_pts, m_sel_vertex_pt,
                     sel_cyan, 1.0f);
}

// Datum/reference planes (Plane feature) have no solid; draw each as a translucent indigo
// rectangle + border so it is visible in the viewport (Onshape-style finite plane). World
// space, depth-test off so it reads over the bed; indigo to stay distinct from the cyan
// solid-selection tint, orange sketches and amber feature ghosts.
void DesignSketchTool::render_view_helpers()
{
    if (!m_show_planes && !m_show_axes) return;
    using EPT = GLModel::Geometry::EPrimitiveType;
    using EVL = GLModel::Geometry::EVertexLayout;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d vd = cam.get_dir_forward();
    const double hw = 1.5 / std::max(cam.get_zoom(), 1e-6);   // billboard ribbon half-width (px)

    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    if (m_show_planes) {
        const double H = 40.0;   // half-extent (mm)
        SketchPlane pl[3];       // XY / XZ / YZ through the world origin
        pl[0].origin = Vec3d(0,0,0); pl[0].x_axis = Vec3d(1,0,0); pl[0].y_axis = Vec3d(0,1,0); pl[0].normal = Vec3d(0,0,1);
        pl[1].origin = Vec3d(0,0,0); pl[1].x_axis = Vec3d(1,0,0); pl[1].y_axis = Vec3d(0,0,1); pl[1].normal = Vec3d(0,1,0);
        pl[2].origin = Vec3d(0,0,0); pl[2].x_axis = Vec3d(0,1,0); pl[2].y_axis = Vec3d(0,0,1); pl[2].normal = Vec3d(1,0,0);
        GLModel::Geometry fill; fill.format = { EPT::Triangles, EVL::P3 };
        unsigned int fb = 0;
        for (const SketchPlane& p : pl) {
            const Vec3d c[4] = { p.to_world(Vec2d(-H,-H)), p.to_world(Vec2d(H,-H)),
                                 p.to_world(Vec2d(H,H)),   p.to_world(Vec2d(-H,H)) };
            fill.add_vertex((Vec3f)c[0].cast<float>()); fill.add_vertex((Vec3f)c[1].cast<float>());
            fill.add_vertex((Vec3f)c[2].cast<float>()); fill.add_triangle(fb, fb+1, fb+2); fb += 3;
            fill.add_vertex((Vec3f)c[0].cast<float>()); fill.add_vertex((Vec3f)c[2].cast<float>());
            fill.add_vertex((Vec3f)c[3].cast<float>()); fill.add_triangle(fb, fb+1, fb+2); fb += 3;
        }
        if (fb > 0) { GLModel fm; fm.init_from(std::move(fill));
            fm.set_color(ColorRGBA(0.42f, 0.52f, 0.78f, 0.20f)); fm.render(); }
    }

    if (m_show_axes) {
        const double L = 60.0;   // axis length (mm)
        struct Ax { Vec3d dir; ColorRGBA col; };
        const Ax axes[3] = { { Vec3d(1,0,0), ColorRGBA(0.92f, 0.28f, 0.28f, 0.9f) },
                             { Vec3d(0,1,0), ColorRGBA(0.30f, 0.80f, 0.34f, 0.9f) },
                             { Vec3d(0,0,1), ColorRGBA(0.32f, 0.55f, 0.95f, 0.9f) } };
        for (const Ax& ax : axes) {
            // Lines don't rasterise under the reused software GL context, so each axis is a
            // thin view-facing ribbon (two triangles), like the datum-plane border.
            const Vec3d a = Vec3d(0,0,0), b = ax.dir * L;
            Vec3d dir = (b - a).normalized();
            Vec3d off = dir.cross(vd);
            if (off.norm() < 1e-9) off = dir.cross(cam.get_dir_up());
            if (off.norm() < 1e-9) continue;
            off.normalize(); off *= hw * 1.5;
            GLModel::Geometry g; g.format = { EPT::Triangles, EVL::P3 };
            g.add_vertex((Vec3f)(a + off).cast<float>()); g.add_vertex((Vec3f)(b + off).cast<float>());
            g.add_vertex((Vec3f)(b - off).cast<float>()); g.add_vertex((Vec3f)(a - off).cast<float>());
            g.add_triangle(0, 1, 2); g.add_triangle(0, 2, 3);
            GLModel m; m.init_from(std::move(g)); m.set_color(ax.col); m.render();
        }
    }
    glsafe(::glDisable(GL_BLEND));
}

void DesignSketchTool::render_datum_planes()
{
    if (m_datum_planes.empty()) return;
    using EPT = GLModel::Geometry::EPrimitiveType;
    using EVL = GLModel::Geometry::EVertexLayout;
    const double H = 40.0;   // default half-extent when no per-plane size is given (mm)
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d vd = cam.get_dir_forward();
    const double hw = 1.5 / std::max(cam.get_zoom(), 1e-6);   // ~1.5 px border ribbon

    GLModel::Geometry fill;   fill.format   = { EPT::Triangles, EVL::P3 };
    GLModel::Geometry border; border.format = { EPT::Triangles, EVL::P3 };
    unsigned int fb = 0, bb = 0;
    for (size_t pi = 0; pi < m_datum_planes.size(); ++pi) {
        const SketchPlane& p = m_datum_planes[pi];
        // Per-plane u/v half-extent (the GUI Size U/V + drag handles drive these); fall
        // back to the square default when no size was supplied.
        const double hu = (pi < m_datum_sizes.size() && m_datum_sizes[pi].x() > 1e-6)
                              ? m_datum_sizes[pi].x() * 0.5 : H;
        const double hv = (pi < m_datum_sizes.size() && m_datum_sizes[pi].y() > 1e-6)
                              ? m_datum_sizes[pi].y() * 0.5 : H;
        const Vec3d c[4] = { p.to_world(Vec2d(-hu, -hv)), p.to_world(Vec2d(hu, -hv)),
                             p.to_world(Vec2d(hu, hv)),   p.to_world(Vec2d(-hu, hv)) };
        fill.add_vertex((Vec3f)c[0].cast<float>()); fill.add_vertex((Vec3f)c[1].cast<float>());
        fill.add_vertex((Vec3f)c[2].cast<float>()); fill.add_triangle(fb, fb + 1, fb + 2); fb += 3;
        fill.add_vertex((Vec3f)c[0].cast<float>()); fill.add_vertex((Vec3f)c[2].cast<float>());
        fill.add_vertex((Vec3f)c[3].cast<float>()); fill.add_triangle(fb, fb + 1, fb + 2); fb += 3;
        for (int s = 0; s < 4; ++s) {
            const Vec3d a = c[s], b = c[(s + 1) & 3];
            Vec3d dir = b - a; if (dir.norm() < 1e-9) continue; dir.normalize();
            Vec3d off = dir.cross(vd);
            if (off.norm() < 1e-9) off = dir.cross(cam.get_dir_up());
            if (off.norm() < 1e-9) continue;
            off.normalize(); off *= hw;
            border.add_vertex((Vec3f)(a + off).cast<float>());
            border.add_vertex((Vec3f)(b + off).cast<float>());
            border.add_vertex((Vec3f)(b - off).cast<float>());
            border.add_vertex((Vec3f)(a - off).cast<float>());
            border.add_triangle(bb, bb + 1, bb + 2);
            border.add_triangle(bb, bb + 2, bb + 3); bb += 4;
        }
    }
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    if (fb > 0) {
        GLModel fm; fm.init_from(std::move(fill));
        fm.set_color(ColorRGBA(0.62f, 0.52f, 0.95f, 0.10f));
        fm.render();
    }
    if (bb > 0) {
        GLModel bm; bm.init_from(std::move(border));
        bm.set_color(ColorRGBA(0.70f, 0.60f, 1.0f, 0.85f));
        bm.render();
    }
    glsafe(::glDisable(GL_BLEND));
}

// ---- Visual Extrude gizmo (C5b) -------------------------------------------------------
void DesignSketchTool::set_extrude_gizmo(const SketchPlane& plane, const Vec2d& centroid,
                                         double depth, double depth2, bool two_sided, bool flip)
{
    m_ex_active    = true;
    m_ex_plane     = plane;
    m_ex_centroid  = centroid;
    m_ex_depth     = std::max(0.0, depth);
    m_ex_depth2    = std::max(0.0, depth2);
    m_ex_two_sided = two_sided;
    m_ex_flip      = flip;
}

void DesignSketchTool::clear_extrude_gizmo()
{
    m_ex_active = false;
    m_ex_drag   = -1;
}

// Camera-billboarded depth arrow(s) along the profile normal, drawn in WORLD via a billboard
// SketchPlane at the centroid (draw_strokes/draw_text lift 2D coords through m_plane.to_world,
// so swapping m_plane to a screen-facing frame renders a flat, screen-aligned arrow + label).
// The mate-connector glyph. Three elements, each answering one question, and every dimension is in
// SCREEN PIXELS via upp (= 1/zoom) like every other gizmo here — a connector is a symbol, not a part,
// so it must not shrink with the model.
//
//   disc      the XY plane        "I am a frame, and this is the plane I sit in"
//   quadrant  the +x/+y sector    "this is where X is"  -- the roll, otherwise invisible
//   Z arrow   +Z only, never -Z   "this is the way I point"  -- the VERSE
//
// POLARITY (which one is anchored, which one is about to move) is carried by the head: a filled
// cone travels, an open collar receives. No surveyed CAD system encodes this at all; both ends of
// their mates are drawn identically, which is why "which part moves?" is a standing complaint.
//
// SNAPORCA_GLYPH=A|B selects the treatment while this is being judged on the rig:
//   A  three short axis arms, no head differentiation  (the Onshape baseline)
//   B  one-sided Z arrow, filled vs open head          (the proposal)          -- default
void DesignSketchTool::render_mate_connectors()
{
    if (m_mate_connectors.empty()) return;
    static const bool style_A = [] {
        const char* s = ::getenv("SNAPORCA_GLYPH");
        return s && (*s == 'A' || *s == 'a');
    }();

    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double R    = 22.0 * upp;                     // disc radius, ~22 px
    const double lw   = std::max(0.9 * upp, 1e-4);

    const ColorRGBA gold (0.93f, 0.66f, 0.09f, 1.0f);
    const ColorRGBA blue (0.18f, 0.44f, 0.93f, 1.0f);
    const ColorRGBA grey (0.42f, 0.46f, 0.52f, 1.0f);
    // F5: roll-undefined was a loud red — the strongest colour in the viewport spent on the LEAST
    // important connector, which pulled the eye away from the mate being made. It is a "this one
    // could not be derived" mark, not an error: muted amber says look-here without shouting.
    const ColorRGBA warn (0.72f, 0.55f, 0.22f, 1.0f);

    const SketchPlane saved = m_plane;

    for (const MateConnectorGlyph& g : m_mate_connectors) {
        const Vec3d X = g.x.normalized();
        const Vec3d Y = g.y.normalized();
        const Vec3d Z = X.cross(Y).normalized();
        const ColorRGBA body = (g.role == 2) ? blue : grey;

        // ---- disc + quadrant, drawn IN the connector's own plane. This is the part that must not
        // be billboarded: a disc that always faces the camera cannot show the frame's orientation,
        // which is the only reason it is a disc and not a dot.
        // Lifted off the surface by a sub-pixel epsilon. A connector's disc is EXACTLY coplanar with
        // the face it sits on, so depth-testing it z-fights: on the rig the disc came out as a broken
        // dotted arc that flickered with the camera. Depth off floats it through solids, depth on and
        // coplanar tears it — the lift is what buys both. Scaled by upp so it stays sub-pixel at any
        // zoom instead of becoming a visible gap when you zoom in.
        SketchPlane cp; cp.origin = g.origin + Z * (0.7 * upp);
        cp.x_axis = X; cp.y_axis = Y; cp.normal = Z;
        m_plane = cp;
        {
            std::vector<std::pair<Vec2d, Vec2d>> segs;
            const int N = 40;
            for (int i = 0; i < N; ++i) {
                const double a0 = (2.0 * M_PI * i) / N, a1 = (2.0 * M_PI * (i + 1)) / N;
                segs.emplace_back(Vec2d(R * std::cos(a0), R * std::sin(a0)),
                                  Vec2d(R * std::cos(a1), R * std::sin(a1)));
            }
            // Depth test ON for the disc, deliberately. With it off, a connector on a face pointing
            // AWAY from the camera still drew its disc over the solid, so the part looked covered in
            // frames that were really on its back — and a disc floating over an edge read as
            // detached rather than planted. render_hole_gizmo learned the same thing for its cube.
            glsafe(::glEnable(GL_DEPTH_TEST));
            draw_strokes(m_mc_stroke_model, segs, lw, g.roll_undefined ? warn : body);

            // The quadrant: hatched when the roll could not be derived (a full circular face or a
            // seam offers no in-plane direction), so the fallback is a mark you cannot miss rather
            // than a silent guess.
            std::vector<std::pair<Vec2d, Vec2d>> q;
            const double qr = R * 0.84;
            const int    QN = 10;
            for (int i = 0; i < QN; ++i) {
                const double a0 = (M_PI_2 * i) / QN, a1 = (M_PI_2 * (i + 1)) / QN;
                q.emplace_back(Vec2d(qr * std::cos(a0), qr * std::sin(a0)),
                               Vec2d(qr * std::cos(a1), qr * std::sin(a1)));
            }
            q.emplace_back(Vec2d(0, 0), Vec2d(qr, 0));
            q.emplace_back(Vec2d(0, 0), Vec2d(0, qr));
            if (!g.roll_undefined) {
                for (int i = 1; i < 5; ++i) {          // fan lines read as "filled" at any angle
                    const double a = (M_PI_2 * i) / 5.0;
                    q.emplace_back(Vec2d(0, 0), Vec2d(qr * std::cos(a), qr * std::sin(a)));
                }
                // F4: the quadrant collapses to a blob at grazing angles — exactly when the roll
                // is hardest to read. A radial tick along +X, extending PAST the disc rim, is what
                // survives that: as the disc flattens to a line the sector loses all area, but a
                // radial spoke keeps its length and its direction along the one axis that still
                // projects.
                //
                // The alternative on the issue was billboarding the quadrant. Rejected, and not
                // on taste: at true grazing the view direction lies IN the connector's plane, so
                // every in-plane direction projects onto the same screen line and the roll is
                // geometrically unrecoverable. A billboarded quadrant would not recover it — it
                // would face the camera and read as a definite orientation that is not the frame's.
                // Better to degrade to a direction you can still trust than to draw a confident
                // lie. Judge the tick on the rig at a true grazing view before calling F4 closed.
                q.emplace_back(Vec2d(R, 0), Vec2d(R * 1.28, 0));
            }
            draw_strokes(m_mc_stroke_model, q, lw, g.roll_undefined ? warn : gold);
        }

        // ---- the axes. Billboarded at the origin: a 3D direction is projected onto the screen
        // frame, which is the only way an arrow keeps a readable head at any viewing angle.
        SketchPlane bb; bb.origin = g.origin; bb.x_axis = right; bb.y_axis = up;
        bb.normal = cam.get_dir_forward().normalized();
        m_plane = bb;

        auto arrow = [&](const Vec3d& dir, double len, const ColorRGBA& col, bool filled_head) {
            const Vec3d tipw = g.origin + dir * len;
            const Vec2d tip2((tipw - g.origin).dot(right), (tipw - g.origin).dot(up));
            std::vector<std::pair<Vec2d, Vec2d>> segs;
            // Foreshortening: when the axis points at (or away from) the camera it projects to
            // nothing and an arrow degenerates into a dot. Draw a ring instead — "pointing at you"
            // — rather than silently vanishing, which is what a naive projection does.
            if (tip2.norm() < R * 0.28) {
                const int N = 24;
                const double rr = R * 0.30;
                for (int i = 0; i < N; ++i) {
                    const double a0 = (2.0 * M_PI * i) / N, a1 = (2.0 * M_PI * (i + 1)) / N;
                    segs.emplace_back(Vec2d(rr * std::cos(a0), rr * std::sin(a0)),
                                      Vec2d(rr * std::cos(a1), rr * std::sin(a1)));
                }
                draw_strokes(m_mc_stroke_model, segs, lw, col);
                return;
            }
            const Vec2d u = tip2.normalized();
            const Vec2d n(-u.y(), u.x());
            const double as = R * 0.42;
            const Vec2d back = tip2 - u * as;
            segs.emplace_back(Vec2d(0, 0), back);
            segs.emplace_back(tip2, back + n * (as * 0.45));
            segs.emplace_back(tip2, back - n * (as * 0.45));
            if (filled_head) {
                // A closed head reads solid; the open one is left as two barbs. At 20-odd pixels
                // this is the difference that says "this body travels".
                segs.emplace_back(back + n * (as * 0.45), back - n * (as * 0.45));
                segs.emplace_back(back + n * (as * 0.22), tip2);
                segs.emplace_back(back - n * (as * 0.22), tip2);
            } else {
                segs.emplace_back(back + n * (as * 0.45), back - n * (as * 0.45));
            }
            glsafe(::glDisable(GL_DEPTH_TEST));
            draw_strokes(m_mc_stroke_model, segs, lw, col);
        };

        if (style_A) {
            arrow(X, R * 1.15, ColorRGBA(0.85f, 0.29f, 0.24f, 1.0f), true);
            arrow(Y, R * 1.15, ColorRGBA(0.23f, 0.65f, 0.35f, 1.0f), true);
            arrow(Z, R * 1.60, blue, true);
        } else {
            arrow(Z, R * 2.10, body, g.role == 2);   // +Z only. Nothing is ever drawn on -Z.
        }
    }
    m_plane = saved;
}

void DesignSketchTool::render_extrude_gizmo()
{
    if (!m_ex_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const Vec3d fwd   = cam.get_dir_forward().normalized();
    const Vec3d base  = m_ex_plane.to_world(m_ex_centroid);
    const Vec3d ndir  = (m_ex_flip ? -1.0 : 1.0) * m_ex_plane.normal.normalized();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th   = std::max(15.0 * upp, 1e-4);

    // Express everything in the billboard frame (origin=base) so it always faces the camera.
    const SketchPlane saved = m_plane;
    SketchPlane bb; bb.origin = base; bb.x_axis = right; bb.y_axis = up; bb.normal = fwd;
    m_plane = bb;
    const ColorRGBA arrowc(1.0f, 0.62f, 0.16f, 1.0f);   // CAD amber

    auto draw_arrow = [&](double depth, bool flip_side) {
        if (depth <= 1e-6) return;
        const Vec3d dirw = (flip_side ? -1.0 : 1.0) * ndir;      // world arrow direction
        const Vec3d tipw = base + dirw * depth;
        const Vec2d tip2((tipw - base).dot(right), (tipw - base).dot(up));
        if (tip2.norm() < 1e-6) return;                          // axis ~ parallel to view: no arrow
        const Vec2d u = tip2.normalized();
        const Vec2d nrm(-u.y(), u.x());
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        segs.emplace_back(Vec2d(0, 0), tip2);
        const double as = std::max(tip2.norm() * 0.18, th * 0.9);   // arrowhead size
        const Vec2d back = tip2 - u * as;
        segs.emplace_back(tip2, back + nrm * (as * 0.5));
        segs.emplace_back(tip2, back - nrm * (as * 0.5));
        draw_strokes(m_ex_arrow_model, segs, std::max(0.7 * upp, 1e-4), arrowc);
        DimAnnot a; a.kind = DimType::Distance; a.value = depth;
        draw_text(m_line_model, dim_text(a), tip2 + u * (th * 1.4), th, arrowc);
    };

    glsafe(::glDisable(GL_DEPTH_TEST));
    draw_arrow(m_ex_depth, false);
    if (m_ex_two_sided) draw_arrow(m_ex_depth2, true);
    m_plane = saved;
}

// Ray vs the arrow segment(s) in world space; `which` = 0 primary, 1 second side.
bool DesignSketchTool::hit_test_extrude_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt, int& which) const
{
    if (!m_ex_active) return false;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double tol = 7.0 / std::max(cam.get_zoom(), 1e-6);    // ~7 px in world units
    const Vec3d ndir = (m_ex_flip ? -1.0 : 1.0) * m_ex_plane.normal.normalized();
    const Vec3d base = m_ex_plane.to_world(m_ex_centroid);
    double dA = 1e30, dB = 1e30;
    if (m_ex_depth  > 1e-6) dA = ray_segment_dist3(ro, rd, base, base + ndir * m_ex_depth);
    if (m_ex_two_sided && m_ex_depth2 > 1e-6)
        dB = ray_segment_dist3(ro, rd, base, base - ndir * m_ex_depth2);
    if (dA <= tol && dA <= dB) { which = 0; return true; }
    if (dB <= tol)             { which = 1; return true; }
    return false;
}

// Drag the arrow handle: closest point on the world arrow axis to the mouse ray (skew-line
// closest-point), projected onto the axis direction -> signed depth (clamped positive).
void DesignSketchTool::drag_extrude_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt, int which)
{
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Vec3d nd = (m_ex_flip ? -1.0 : 1.0) * m_ex_plane.normal.normalized();
    const Vec3d e  = (which == 1 ? -1.0 : 1.0) * nd;            // axis dir for this arrow
    const Vec3d base = m_ex_plane.to_world(m_ex_centroid);
    const Vec3d w0 = base - ro;
    const double a = e.dot(e), b = e.dot(rd), c = rd.dot(rd), dd = e.dot(w0), ee = rd.dot(w0);
    const double denom = a * c - b * b;
    if (std::abs(denom) < 1e-7) return;                        // camera ∥ axis: leave depth as-is
    const double depth = std::max(0.01, (b * ee - c * dd) / denom);
    if (which == 1) m_ex_depth2 = depth; else m_ex_depth = depth;
    if (on_extrude_depth_changed) on_extrude_depth_changed(depth, which == 1);
}

void DesignSketchTool::open_extrude_editor(int which)
{
    if (!on_inline_edit) return;
    const double cur = (which == 1) ? m_ex_depth2 : m_ex_depth;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, cur, "",
        [this, which](double v) {
            const double d = std::max(0.01, v);
            if (which == 1) m_ex_depth2 = d; else m_ex_depth = d;
            if (on_extrude_depth_changed) on_extrude_depth_changed(d, which == 1);
        },
        []() {});
}

// ---- Datum-plane resize gizmo (C3) ----------------------------------------------------
void DesignSketchTool::set_datum_gizmo(const SketchPlane& plane, double usize, double vsize,
                                       const Vec3d& base_origin, const Vec3d& base_normal,
                                       double offset, bool offset_on)
{
    m_dz_active = true;
    m_dz_plane  = plane;
    m_dz_usize  = std::max(1.0, usize);
    m_dz_vsize  = std::max(1.0, vsize);
    m_dz_anchor = base_origin;
    m_dz_normal = base_normal.normalized();
    m_dz_offset = offset;
    m_dz_offset_on = offset_on;
}

void DesignSketchTool::clear_datum_gizmo()
{
    m_dz_active = false;
    m_dz_drag   = -1;
}

// Draw the datum rectangle outline + 4 camera-billboarded edge-midpoint handles. Self-contained
// so it shows even for an uncommitted (not-yet-Confirmed) datum that render_datum_planes can't draw.
void DesignSketchTool::render_datum_gizmo()
{
    if (!m_dz_active) return;
    using EPT = GLModel::Geometry::EPrimitiveType;
    using EVL = GLModel::Geometry::EVertexLayout;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const Vec3d vd    = cam.get_dir_forward();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double hs   = 6.0 * upp;                       // handle half-size (~6 px)
    const double hw   = 1.5 * upp;                       // outline ribbon half-width
    const double hu   = m_dz_usize * 0.5, hv = m_dz_vsize * 0.5;
    const SketchPlane& p = m_dz_plane;

    // Rectangle outline (4 thin camera-facing ribbons).
    const Vec3d c[4] = { p.to_world(Vec2d(-hu, -hv)), p.to_world(Vec2d(hu, -hv)),
                         p.to_world(Vec2d(hu, hv)),   p.to_world(Vec2d(-hu, hv)) };
    GLModel::Geometry border; border.format = { EPT::Triangles, EVL::P3 };
    unsigned int bb = 0;
    for (int s = 0; s < 4; ++s) {
        const Vec3d a = c[s], b = c[(s + 1) & 3];
        Vec3d dir = b - a; if (dir.norm() < 1e-9) continue; dir.normalize();
        Vec3d off = dir.cross(vd);
        if (off.norm() < 1e-9) off = dir.cross(up);
        if (off.norm() < 1e-9) continue;
        off.normalize(); off *= hw;
        border.add_vertex((Vec3f)(a + off).cast<float>()); border.add_vertex((Vec3f)(b + off).cast<float>());
        border.add_vertex((Vec3f)(b - off).cast<float>()); border.add_vertex((Vec3f)(a - off).cast<float>());
        border.add_triangle(bb, bb + 1, bb + 2); border.add_triangle(bb, bb + 2, bb + 3); bb += 4;
    }

    // 4 edge-midpoint handle squares.
    const Vec2d hpos[4] = { Vec2d(hu, 0), Vec2d(-hu, 0), Vec2d(0, hv), Vec2d(0, -hv) };
    GLModel::Geometry handles; handles.format = { EPT::Triangles, EVL::P3 };
    unsigned int hb = 0;
    for (int i = 0; i < 4; ++i) {
        const Vec3d ctr = p.to_world(hpos[i]);
        const Vec3d q0 = ctr - right * hs - up * hs, q1 = ctr + right * hs - up * hs,
                    q2 = ctr + right * hs + up * hs, q3 = ctr - right * hs + up * hs;
        handles.add_vertex((Vec3f)q0.cast<float>()); handles.add_vertex((Vec3f)q1.cast<float>());
        handles.add_vertex((Vec3f)q2.cast<float>()); handles.add_vertex((Vec3f)q3.cast<float>());
        handles.add_triangle(hb, hb + 1, hb + 2); handles.add_triangle(hb, hb + 2, hb + 3); hb += 4;
    }

    glsafe(::glDisable(GL_DEPTH_TEST));
    if (bb > 0) {
        GLModel m; m.init_from(std::move(border));
        m.set_color(ColorRGBA(1.0f, 0.62f, 0.16f, 0.9f));   // CAD amber
        m.render();
    }
    if (hb > 0) {
        GLModel m; m.init_from(std::move(handles));
        m.set_color(ColorRGBA(1.0f, 0.72f, 0.28f, 1.0f));
        m.render();
    }

    // Offset arrow: a camera-facing ribbon from the base origin along the base normal to the
    // datum origin, with a grabbable square at the tip. Drag the tip to set the offset distance.
    if (m_dz_offset_on) {
        const Vec3d tip = m_dz_anchor + m_dz_normal * m_dz_offset;
        Vec3d off = m_dz_normal.cross(vd);
        if (off.norm() < 1e-9) off = m_dz_normal.cross(up);
        GLModel::Geometry shaft; shaft.format = { EPT::Triangles, EVL::P3 };
        if (off.norm() > 1e-9 && (tip - m_dz_anchor).norm() > 1e-9) {
            off.normalize(); off *= hw;
            shaft.add_vertex((Vec3f)(m_dz_anchor + off).cast<float>());
            shaft.add_vertex((Vec3f)(tip + off).cast<float>());
            shaft.add_vertex((Vec3f)(tip - off).cast<float>());
            shaft.add_vertex((Vec3f)(m_dz_anchor - off).cast<float>());
            shaft.add_triangle(0, 1, 2); shaft.add_triangle(0, 2, 3);
            GLModel sm; sm.init_from(std::move(shaft));
            sm.set_color(ColorRGBA(0.30f, 0.78f, 1.0f, 0.95f));   // cyan offset axis
            sm.render();
        }
        GLModel::Geometry tipsq; tipsq.format = { EPT::Triangles, EVL::P3 };
        const Vec3d t0 = tip - right * hs - up * hs, t1 = tip + right * hs - up * hs,
                    t2 = tip + right * hs + up * hs, t3 = tip - right * hs + up * hs;
        tipsq.add_vertex((Vec3f)t0.cast<float>()); tipsq.add_vertex((Vec3f)t1.cast<float>());
        tipsq.add_vertex((Vec3f)t2.cast<float>()); tipsq.add_vertex((Vec3f)t3.cast<float>());
        tipsq.add_triangle(0, 1, 2); tipsq.add_triangle(0, 2, 3);
        GLModel tm; tm.init_from(std::move(tipsq));
        tm.set_color(ColorRGBA(0.45f, 0.86f, 1.0f, 1.0f));
        tm.render();
    }
}

bool DesignSketchTool::hit_test_datum_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int& which) const
{
    if (!m_dz_active) return false;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double tol = 9.0 / std::max(cam.get_zoom(), 1e-6);   // ~9 px in world units
    const double hu = m_dz_usize * 0.5, hv = m_dz_vsize * 0.5;
    const Vec2d hpos[4] = { Vec2d(hu, 0), Vec2d(-hu, 0), Vec2d(0, hv), Vec2d(0, -hv) };
    double best = tol; which = -1;
    for (int i = 0; i < 4; ++i) {
        const Vec3d pt = m_dz_plane.to_world(hpos[i]);
        const Vec3d w  = pt - ro;
        const double t = w.dot(rd) / std::max(rd.dot(rd), 1e-12);
        const double d = (w - rd * t).norm();
        if (d < best) { best = d; which = i; }
    }
    if (m_dz_offset_on) {                                       // offset arrow tip = handle 4
        const Vec3d pt = m_dz_anchor + m_dz_normal * m_dz_offset;
        const Vec3d w  = pt - ro;
        const double t = w.dot(rd) / std::max(rd.dot(rd), 1e-12);
        const double d = (w - rd * t).norm();
        if (d < best) { best = d; which = 4; }
    }
    return which >= 0;
}

// Drag a handle: closest point of the mouse ray to the plane axis (u or v) through the origin,
// |param| -> new half-extent, doubled to the full size.
void DesignSketchTool::drag_datum_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int which)
{
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    if (which == 4) {                                          // offset arrow: drag along base normal
        const Vec3d e = m_dz_normal;                           // signed offset, may go negative
        const Vec3d w0 = m_dz_anchor - ro;
        const double a = e.dot(e), b = e.dot(rd), c = rd.dot(rd), dd = e.dot(w0), ee = rd.dot(w0);
        const double denom = a * c - b * b;
        if (std::abs(denom) < 1e-7) return;                    // camera ∥ normal: leave offset as-is
        m_dz_offset = (b * ee - c * dd) / denom;               // signed distance along the normal
        m_dz_plane.origin = m_dz_anchor + m_dz_normal * m_dz_offset;   // rectangle follows live
        if (on_datum_offset_changed) on_datum_offset_changed(m_dz_offset);
        return;
    }
    const bool  uaxis = (which == 0 || which == 1);
    const Vec3d e   = (uaxis ? m_dz_plane.x_axis : m_dz_plane.y_axis).normalized();
    const Vec3d base = m_dz_plane.origin;
    const Vec3d w0 = base - ro;
    const double a = e.dot(e), b = e.dot(rd), c = rd.dot(rd), dd = e.dot(w0), ee = rd.dot(w0);
    const double denom = a * c - b * b;
    if (std::abs(denom) < 1e-7) return;                        // camera ∥ axis: leave size as-is
    const double s    = (b * ee - c * dd) / denom;             // signed coord along e of closest pt
    const double size = std::max(2.0, 2.0 * std::abs(s));
    if (uaxis) m_dz_usize = size; else m_dz_vsize = size;
    if (on_datum_size_changed) on_datum_size_changed(m_dz_usize, m_dz_vsize);
}

// ---- Helix gizmo (plane-anchored curve + 3 drag handles) --------------------------------
void DesignSketchTool::set_helix_gizmo(const SketchPlane& plane, double radius, double pitch,
                                       double height, double taper, bool left_handed)
{
    m_hx_active = true;
    m_hx_plane  = plane;
    m_hx_radius = radius;
    m_hx_pitch  = pitch;
    m_hx_height = height;
    m_hx_taper  = taper;
    m_hx_left   = left_handed;
}

void DesignSketchTool::clear_helix_gizmo()
{
    m_hx_active = false;
    m_hx_drag   = -1;
}

// Curve point at parameter t (t in turns): angle 2*pi*t, negated when left-handed, rising one
// pitch per turn along the plane normal.
//
// Taper is an ANGLE IN DEGREES and must be read the way the kernel reads it. CadDocument's
// helix_spine() builds a Geom_ConicalSurface of half-angle taper and computes the top radius as
// R + H*tan(taper), so the radius grows with the HEIGHT RISEN, not as a fraction of R consumed
// over the turn count. Getting that wrong draws a preview that collapses to a point for any
// non-zero taper while the committed feature is fine — a preview that lies is worse than none.
Vec3d DesignSketchTool::helix_point(double t) const
{
    const Vec3d O = m_hx_plane.origin;
    const Vec3d n = m_hx_plane.normal.normalized();
    const Vec3d u = m_hx_plane.x_axis.normalized();
    const Vec3d v = m_hx_plane.y_axis.normalized();
    const double a = (m_hx_left ? -1.0 : 1.0) * 2.0 * M_PI * t;
    const double z = m_hx_pitch * t;                      // height risen at this parameter
    double rt = m_hx_radius + z * std::tan(m_hx_taper * M_PI / 180.0);
    // The kernel REFUSES a taper that drives the radius negative before the full height; the
    // preview shows it collapsing instead, so the user can see which value did it.
    if (rt < 0.0) rt = 0.0;
    return O + u * (rt * std::cos(a)) + v * (rt * std::sin(a)) + n * z;
}

// Draw the live helix as a connected camera-facing ribbon, a dim axis line, and three square
// handles (radius on the base circle, height on the axis top, pitch at the end of the first turn).
void DesignSketchTool::render_helix_gizmo()
{
    if (!m_hx_active) return;
    using EPT = GLModel::Geometry::EPrimitiveType;
    using EVL = GLModel::Geometry::EVertexLayout;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const Vec3d vd    = cam.get_dir_forward();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double hs   = 6.0 * upp;                       // handle half-size (~6 px)
    const double hw   = 1.5 * upp;                       // ribbon half-width
    const Vec3d O     = m_hx_plane.origin;
    const Vec3d n     = m_hx_plane.normal.normalized();
    const double turns = m_hx_pitch > 1e-9 ? m_hx_height / m_hx_pitch : 0.0;

    // Curve ribbon: connected thin camera-facing quads over t in [0, turns].
    const int segs = std::min(2048, std::max(48, int(turns * 32.0)));
    GLModel::Geometry curve; curve.format = { EPT::Triangles, EVL::P3 };
    unsigned int cb = 0;
    auto add_seg = [&](const Vec3d& a, const Vec3d& b) {
        Vec3d dir = b - a; if (dir.norm() < 1e-9) return; dir.normalize();
        Vec3d off = dir.cross(vd);
        if (off.norm() < 1e-9) off = dir.cross(up);
        if (off.norm() < 1e-9) return;
        off.normalize(); off *= hw;
        curve.add_vertex((Vec3f)(a + off).cast<float>()); curve.add_vertex((Vec3f)(b + off).cast<float>());
        curve.add_vertex((Vec3f)(b - off).cast<float>()); curve.add_vertex((Vec3f)(a - off).cast<float>());
        curve.add_triangle(cb, cb + 1, cb + 2); curve.add_triangle(cb, cb + 2, cb + 3); cb += 4;
    };
    for (int i = 0; i < segs; ++i)
        add_seg(helix_point(turns * i / segs), helix_point(turns * (i + 1) / segs));

    // Axis: a dim line from the plane origin to the top of the helix.
    const Vec3d top = O + n * m_hx_height;
    GLModel::Geometry axis; axis.format = { EPT::Triangles, EVL::P3 };
    {
        Vec3d dir = top - O;
        if (dir.norm() > 1e-9) {
            dir.normalize();
            Vec3d off = dir.cross(vd);
            if (off.norm() < 1e-9) off = dir.cross(up);
            if (off.norm() > 1e-9) {
                off.normalize(); off *= hw * 0.6;
                axis.add_vertex((Vec3f)(O + off).cast<float>()); axis.add_vertex((Vec3f)(top + off).cast<float>());
                axis.add_vertex((Vec3f)(top - off).cast<float>()); axis.add_vertex((Vec3f)(O - off).cast<float>());
                axis.add_triangle(0, 1, 2); axis.add_triangle(0, 2, 3);
            }
        }
    }

    // Three handles: 0=radius (t=0), 1=height (axis top), 2=pitch (end of first turn, or the
    // whole curve if shorter than one turn so the handle never floats off a missing curve).
    const Vec3d hpts[3] = { helix_point(0.0), top,
                            helix_point(m_hx_height < m_hx_pitch ? turns : 1.0) };
    const ColorRGBA hcol[3] = { ColorRGBA(1.0f, 0.72f, 0.28f, 1.0f),   // radius — amber
                                ColorRGBA(0.45f, 0.86f, 1.0f, 1.0f),   // height — cyan
                                ColorRGBA(0.30f, 0.80f, 0.34f, 1.0f) }; // pitch — green
    const ColorRGBA hot(1.0f, 0.85f, 0.2f, 1.0f);

    glsafe(::glDisable(GL_DEPTH_TEST));
    if (cb > 0) {
        GLModel m; m.init_from(std::move(curve));
        m.set_color(ColorRGBA(1.0f, 0.62f, 0.16f, 0.9f));   // CAD amber helix curve
        m.render();
    }
    if (!axis.vertices.empty()) {
        GLModel m; m.init_from(std::move(axis));
        m.set_color(ColorRGBA(0.42f, 0.46f, 0.52f, 0.55f));   // dim grey axis
        m.render();
    }
    for (int i = 0; i < 3; ++i) {
        const Vec3d ctr = hpts[i];
        const Vec3d q0 = ctr - right * hs - up * hs, q1 = ctr + right * hs - up * hs,
                    q2 = ctr + right * hs + up * hs, q3 = ctr - right * hs + up * hs;
        GLModel::Geometry sq; sq.format = { EPT::Triangles, EVL::P3 };
        sq.add_vertex((Vec3f)q0.cast<float>()); sq.add_vertex((Vec3f)q1.cast<float>());
        sq.add_vertex((Vec3f)q2.cast<float>()); sq.add_vertex((Vec3f)q3.cast<float>());
        sq.add_triangle(0, 1, 2); sq.add_triangle(0, 2, 3);
        GLModel m; m.init_from(std::move(sq));
        m.set_color(m_hx_drag == i ? hot : hcol[i]);
        m.render();
    }
}

bool DesignSketchTool::hit_test_helix_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int& which) const
{
    if (!m_hx_active) return false;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double tol = 9.0 / std::max(cam.get_zoom(), 1e-6);   // ~9 px in world units
    const Vec3d O = m_hx_plane.origin;
    const Vec3d n = m_hx_plane.normal.normalized();
    const double turns = m_hx_pitch > 1e-9 ? m_hx_height / m_hx_pitch : 0.0;
    const Vec3d hpts[3] = { helix_point(0.0), O + n * m_hx_height,
                            helix_point(m_hx_height < m_hx_pitch ? turns : 1.0) };
    double best = tol; which = -1;
    for (int i = 0; i < 3; ++i) {
        const Vec3d w  = hpts[i] - ro;
        const double t = w.dot(rd) / std::max(rd.dot(rd), 1e-12);
        const double d = (w - rd * t).norm();
        if (d < best) { best = d; which = i; }
    }
    return which >= 0;
}

// Drag a handle: radius = cursor's in-plane distance from the origin, height/pitch = the signed
// distance of the cursor's closest axis point. All fire the full (radius, pitch, height) triple.
void DesignSketchTool::drag_helix_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int which)
{
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const SketchPlane& p = m_hx_plane;
    const Vec3d O = p.origin, n = p.normal.normalized();

    if (which == 0) {                                          // radius: in-plane distance from O
        const Vec2d lp = p.project(ro, rd);
        m_hx_radius = std::max(0.01, lp.norm());
    } else {                                                   // height/pitch: signed distance along n
        const Vec3d e   = n;
        const Vec3d w0  = O - ro;
        const double a  = e.dot(e), b = e.dot(rd), c = rd.dot(rd), dd = e.dot(w0), ee = rd.dot(w0);
        const double denom = a * c - b * b;
        if (std::abs(denom) < 1e-7) return;                    // camera ∥ axis: leave value as-is
        const double s = (b * ee - c * dd) / denom;            // signed distance along the axis
        if (which == 1) m_hx_height = std::max(0.0, s);
        else            m_hx_pitch  = std::max(0.01, s);       // zero/negative pitch divides by zero
    }
    if (on_helix_changed) on_helix_changed(m_hx_radius, m_hx_pitch, m_hx_height);
}

// ---- Rib thickness gizmo (in-plane slab footprint + 2 symmetric handles) -----------------
void DesignSketchTool::set_rib_gizmo(const SketchPlane& plane, const Vec2d& p0, const Vec2d& p1,
                                     double thickness)
{
    m_rb_active    = true;
    m_rb_plane     = plane;
    m_rb_p0        = p0;
    m_rb_p1        = p1;
    m_rb_thickness = thickness;
}

void DesignSketchTool::clear_rib_gizmo()
{
    m_rb_active = false;
    m_rb_drag   = -1;
}

// Resolve the rib line's in-plane frame: unit direction d, perpendicular perp, midpoint, and
// half-thickness. A degenerate line (zero length) has no direction to grow a slab perpendicular
// to — return false so the caller draws nothing and never divides by zero.
static bool rib_frame(const Vec2d& p0, const Vec2d& p1, double thickness,
                      Vec2d& d, Vec2d& perp, Vec2d& mid, double& half)
{
    const Vec2d seg = p1 - p0;
    const double len = seg.norm();
    if (len < 1e-9) return false;
    d    = seg / len;
    perp = Vec2d(-d.y(), d.x());
    mid  = 0.5 * (p0 + p1);
    half = thickness * 0.5;
    return true;
}

// Draw the rib slab's actual footprint (the rectangle p0±perp·half, p1±perp·half) as a thin
// closed ribbon plus two square handles at mid ± perp·half. Both handles sit at half-thickness,
// so a drag on either expresses the full thickness symmetrically.
void DesignSketchTool::render_rib_gizmo()
{
    if (!m_rb_active) return;
    Vec2d d, perp, mid; double half;
    if (!rib_frame(m_rb_p0, m_rb_p1, m_rb_thickness, d, perp, mid, half)) return;
    using EPT = GLModel::Geometry::EPrimitiveType;
    using EVL = GLModel::Geometry::EVertexLayout;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const Vec3d vd    = cam.get_dir_forward();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double hs   = 6.0 * upp;                       // handle half-size (~6 px)
    const double hw   = 1.5 * upp;                       // ribbon half-width

    // Slab footprint outline (4 thin camera-facing ribbons).
    const Vec3d c[4] = { m_rb_plane.to_world(m_rb_p0 + perp * half),
                         m_rb_plane.to_world(m_rb_p1 + perp * half),
                         m_rb_plane.to_world(m_rb_p1 - perp * half),
                         m_rb_plane.to_world(m_rb_p0 - perp * half) };
    GLModel::Geometry border; border.format = { EPT::Triangles, EVL::P3 };
    unsigned int bb = 0;
    for (int s = 0; s < 4; ++s) {
        const Vec3d a = c[s], b = c[(s + 1) & 3];
        Vec3d dir = b - a; if (dir.norm() < 1e-9) continue; dir.normalize();
        Vec3d off = dir.cross(vd);
        if (off.norm() < 1e-9) off = dir.cross(up);
        if (off.norm() < 1e-9) continue;
        off.normalize(); off *= hw;
        border.add_vertex((Vec3f)(a + off).cast<float>()); border.add_vertex((Vec3f)(b + off).cast<float>());
        border.add_vertex((Vec3f)(b - off).cast<float>()); border.add_vertex((Vec3f)(a - off).cast<float>());
        border.add_triangle(bb, bb + 1, bb + 2); border.add_triangle(bb, bb + 2, bb + 3); bb += 4;
    }

    // Two handles: 0 = +perp side, 1 = -perp side (both at half-thickness).
    const Vec3d hpts[2] = { m_rb_plane.to_world(mid + perp * half),
                            m_rb_plane.to_world(mid - perp * half) };
    const ColorRGBA hot(1.0f, 0.85f, 0.2f, 1.0f);

    glsafe(::glDisable(GL_DEPTH_TEST));
    if (bb > 0) {
        GLModel m; m.init_from(std::move(border));
        m.set_color(ColorRGBA(1.0f, 0.62f, 0.16f, 0.9f));   // CAD amber slab footprint
        m.render();
    }
    for (int i = 0; i < 2; ++i) {
        const Vec3d ctr = hpts[i];
        const Vec3d q0 = ctr - right * hs - up * hs, q1 = ctr + right * hs - up * hs,
                    q2 = ctr + right * hs + up * hs, q3 = ctr - right * hs + up * hs;
        GLModel::Geometry sq; sq.format = { EPT::Triangles, EVL::P3 };
        sq.add_vertex((Vec3f)q0.cast<float>()); sq.add_vertex((Vec3f)q1.cast<float>());
        sq.add_vertex((Vec3f)q2.cast<float>()); sq.add_vertex((Vec3f)q3.cast<float>());
        sq.add_triangle(0, 1, 2); sq.add_triangle(0, 2, 3);
        GLModel m; m.init_from(std::move(sq));
        m.set_color(m_rb_drag == i ? hot : ColorRGBA(0.30f, 0.80f, 0.34f, 1.0f));   // green
        m.render();
    }
}

bool DesignSketchTool::hit_test_rib_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int& which) const
{
    if (!m_rb_active) return false;
    Vec2d d, perp, mid; double half;
    if (!rib_frame(m_rb_p0, m_rb_p1, m_rb_thickness, d, perp, mid, half)) return false;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double tol = 9.0 / std::max(cam.get_zoom(), 1e-6);   // ~9 px in world units
    const Vec3d hpts[2] = { m_rb_plane.to_world(mid + perp * half),
                            m_rb_plane.to_world(mid - perp * half) };
    double best = tol; which = -1;
    for (int i = 0; i < 2; ++i) {
        const Vec3d w  = hpts[i] - ro;
        const double t = w.dot(rd) / std::max(rd.dot(rd), 1e-12);
        const double dd = (w - rd * t).norm();
        if (dd < best) { best = dd; which = i; }
    }
    return which >= 0;
}

// Drag a handle: project the cursor ray onto the rib plane (the same call the helix radius drag
// uses), take the perpendicular distance from the rib LINE to that point, and set the thickness
// to twice that distance — the slab is centred on the line and the handle sits at half-thickness.
void DesignSketchTool::drag_rib_handle(GLCanvas3D& canvas, const wxMouseEvent& evt, int which)
{
    (void)which;   // both handles behave identically: a drag on either sets the full thickness
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    Vec2d d, perp, mid; double half;
    if (!rib_frame(m_rb_p0, m_rb_p1, m_rb_thickness, d, perp, mid, half)) return;
    const Vec2d lp = m_rb_plane.project(ro, rd);
    const Vec2d w  = lp - m_rb_p0;
    const double dist = std::abs(w.x() * d.y() - w.y() * d.x());   // |cross(w, d)| = perp distance
    m_rb_thickness = std::max(0.01, 2.0 * dist);                   // ×2: centred slab, handle at half
    if (on_rib_thickness_changed) on_rib_thickness_changed(m_rb_thickness);
}

// ---- Reference/base planes (Onshape-style default planes) -----------------------------
void DesignSketchTool::set_base_pick(std::vector<SketchPlane> planes, std::vector<int> bases,
                                     std::vector<std::string> labels)
{
    m_dbp_active = !planes.empty();
    m_dbp_planes = std::move(planes);
    m_dbp_base   = std::move(bases);
    m_dbp_labels = std::move(labels);
    if (m_dbp_hover >= int(m_dbp_planes.size())) m_dbp_hover = -1;
}

void DesignSketchTool::clear_base_pick()
{
    m_dbp_active = false;
    m_dbp_planes.clear();
    m_dbp_base.clear();
    m_dbp_labels.clear();
    m_dbp_hover = -1;
}

// Reference planes are larger than the bed (Onshape default-plane feel). Half-extent = 0.6 * the
// bed's larger side, so the square fully overhangs the print area. Falls back to 150mm if the bed
// isn't queryable yet.
double DesignSketchTool::dbp_half_extent() const
{
    double half = 150.0;
    if (auto* pl = wxGetApp().plater()) {
        const BoundingBoxf bb = pl->build_volume().bounding_volume2d();
        const double w = bb.max.x() - bb.min.x(), d = bb.max.y() - bb.min.y();
        if (w > 1.0 && d > 1.0) half = 0.6 * std::max(w, d);
    }
    return half;
}

// Draw the reference planes as large translucent labelled squares; the hovered one brightens.
void DesignSketchTool::render_base_pick()
{
    if (!m_dbp_active || m_dbp_planes.empty()) return;
    using EPT = GLModel::Geometry::EPrimitiveType;
    using EVL = GLModel::Geometry::EVertexLayout;
    const double H = dbp_half_extent();
    // Onshape-ish per-plane tints: XY blue, XZ green, YZ red (keyed by base index 0/1/2; datums grey).
    auto tint = [](int base, bool hot) -> ColorRGBA {
        float a = hot ? 0.10f : 0.047f;   // base planes kept faint (reduced ~2/3 from 0.30/0.14)
        if (base == 0) return ColorRGBA(0.30f, 0.55f, 0.95f, a);
        if (base == 1) return ColorRGBA(0.35f, 0.80f, 0.45f, a);
        if (base == 2) return ColorRGBA(0.92f, 0.42f, 0.42f, a);
        return ColorRGBA(0.70f, 0.72f, 0.78f, a);
    };
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));                                // alpha is ignored without this
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    const SketchPlane saved_plane = m_plane;
    for (size_t i = 0; i < m_dbp_planes.size(); ++i) {
        const SketchPlane& p = m_dbp_planes[i];
        const Vec3d q0 = p.to_world(Vec2d(-H, -H)), q1 = p.to_world(Vec2d(H, -H)),
                    q2 = p.to_world(Vec2d(H, H)),   q3 = p.to_world(Vec2d(-H, H));
        GLModel::Geometry quad; quad.format = { EPT::Triangles, EVL::P3 };
        quad.add_vertex((Vec3f)q0.cast<float>()); quad.add_vertex((Vec3f)q1.cast<float>());
        quad.add_vertex((Vec3f)q2.cast<float>()); quad.add_vertex((Vec3f)q3.cast<float>());
        quad.add_triangle(0, 1, 2); quad.add_triangle(0, 2, 3);
        GLModel m; m.init_from(std::move(quad));
        const bool hot = (int(i) == m_dbp_hover);
        const int  base = (i < m_dbp_base.size()) ? m_dbp_base[i] : -1;
        m.set_color(tint(base, hot));
        m.render();

        // Label near the top-left corner, drawn in the plane (draw_text lifts through m_plane).
        if (i < m_dbp_labels.size() && !m_dbp_labels[i].empty()) {
            m_plane = p;
            const double th = H * 0.10;
            const ColorRGBA lc = tint(base, true); ColorRGBA lcs(lc.r(), lc.g(), lc.b(), 1.0f);
            draw_text(m_line_model, m_dbp_labels[i], Vec2d(-H + th * 2.0, H - th * 1.6), th, lcs);
        }
    }
    m_plane = saved_plane;   // draw_text renders each label immediately (draw_strokes self-renders)
    glsafe(::glDisable(GL_BLEND));
}

// Ray-pick the reference planes: intersect the mouse ray with each plane, keep hits inside the
// square, return the index of the nearest by |t|. -1 on miss.
int DesignSketchTool::hit_test_base_pick(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    if (!m_dbp_active) return -1;
    const double H = dbp_half_extent();

    // THE LABEL WINS, and it has to. Each plane's name is a screen-space chip centred on its
    // own in-plane anchor, and it is the one part of a base plane a user aims at deliberately —
    // the quads are near-transparent and overlap everywhere. Ray-casting the quads alone made
    // the labels pure decoration: on a fresh document at 1920x1060, clicking "XY" reported
    // "XZ plane selected", because the XZ quad happens to sit in front at that pixel. Nothing
    // about the click was ambiguous to the user; they clicked the word XY.
    // Anchor and text height must track render_base_pick's, which is where they are drawn.
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double  th  = H * 0.10;
    const Vec2d   anchor(-H + th * 2.0, H - th * 1.6);
    int lbest = -1; double lbest_d = 1e30;
    for (size_t i = 0; i < m_dbp_planes.size(); ++i) {
        if (i >= m_dbp_labels.size() || m_dbp_labels[i].empty()) continue;
        const wxPoint sp = world_to_screen_px(cam, m_dbp_planes[i].to_world(anchor));
        if (sp.x < 0 && sp.y < 0) continue;                    // behind the camera
        const double dx = std::abs(double(evt.GetX() - sp.x));
        const double dy = std::abs(double(evt.GetY() - sp.y));
        // Chip half-extents in px, scaled like the label itself. Generous rather than tight:
        // missing the text and silently selecting a different plane is the failure being fixed.
        const double hw = (9.0 + 5.0 * double(m_dbp_labels[i].size())) * double(m_render_scale);
        const double hh = 11.0 * double(m_render_scale);
        if (dx > hw || dy > hh) continue;
        const double d = dx * dx + dy * dy;                    // nearest label if chips overlap
        if (d < lbest_d) { lbest_d = d; lbest = int(i); }
    }
    if (lbest >= 0) return lbest;

    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    int best = -1; double best_t = 1e30;
    for (size_t i = 0; i < m_dbp_planes.size(); ++i) {
        const SketchPlane& p = m_dbp_planes[i];
        const double dn = rd.dot(p.normal);
        if (std::abs(dn) < 1e-9) continue;                      // ray parallel to plane
        const double t = (p.origin - ro).dot(p.normal) / dn;
        if (t < 0) continue;                                    // behind the camera
        const Vec3d hit = ro + rd * t;
        const Vec3d d = hit - p.origin;
        if (std::abs(d.dot(p.x_axis)) > H || std::abs(d.dot(p.y_axis)) > H) continue;
        if (t < best_t) { best_t = t; best = int(i); }
    }
    return best;
}

// ---- Move-body gizmo (M5) -------------------------------------------------------------
void DesignSketchTool::set_move_gizmo(int body, const Vec3d& pivot, const Transform3d& base_xform,
                                      double body_radius)
{
    m_mv_active     = true;
    m_mv_body       = body;
    m_mv_base       = pivot;          // body's world centroid at Move-open = rotation pivot
    m_mv_base_xform = base_xform;     // pose the deltas compose onto
    m_mv_offset     = Vec3d::Zero();
    m_mv_rot        = Eigen::Matrix3d::Identity();
    m_mv_drag       = -1;
    m_mv_radius     = std::max(body_radius, 0.0);
}

// Gizmo arm length in world mm. Orca's Prepare gizmos size themselves from the selection's
// bounding sphere (GLGizmoRotate3D: m_radius = Offset + sphere radius) so the handles always sit
// clear of the object; a fixed screen-size arm instead collapsed into a tangle buried inside a
// large solid, which is why the rotation rings read as "missing". Same idea here, with a
// screen-space floor so the gizmo stays grabbable on a tiny body or when zoomed far out.
double DesignSketchTool::move_gizmo_arm(const Camera& cam) const
{
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double screen_min = 70.0 * upp;              // never smaller than the old fixed size
    return std::max(screen_min, m_mv_radius * 1.25);   // 25% clear of the body surface
}

void DesignSketchTool::clear_move_gizmo()
{
    m_mv_active = false;
    m_mv_drag   = -1;
    m_mv_body   = -1;
    m_mv_offset = Vec3d::Zero();
    m_mv_rot    = Eigen::Matrix3d::Identity();
}

// Final body transform = translate(delta) then rotate(delta, about pivot) on the open pose.
Transform3d DesignSketchTool::compose_move_xform() const
{
    const Vec3d p = m_mv_base;
    Transform3d R = Transform3d::Identity();
    R.linear() = m_mv_rot;
    const Transform3d rot_about_pivot =
        Transform3d(Eigen::Translation3d(p)) * R * Transform3d(Eigen::Translation3d(-p));
    return Transform3d(Eigen::Translation3d(m_mv_offset)) * rot_about_pivot * m_mv_base_xform;
}

// World axis `e` of ring `axis` plus an in-plane orthonormal basis (u,v).
void DesignSketchTool::ring_basis(int axis, Vec3d& e, Vec3d& u, Vec3d& v) const
{
    switch (axis) {
    case 0: e = Vec3d::UnitX(); u = Vec3d::UnitY(); v = Vec3d::UnitZ(); break;
    case 1: e = Vec3d::UnitY(); u = Vec3d::UnitZ(); v = Vec3d::UnitX(); break;
    default:e = Vec3d::UnitZ(); u = Vec3d::UnitX(); v = Vec3d::UnitY(); break;
    }
}

// Three world-axis arrows (X red / Y green / Z blue) from the body centroid + current
// offset, billboarded into a screen-facing frame like the extrude depth arrow.
void DesignSketchTool::render_move_gizmo()
{
    if (!m_mv_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const Vec3d fwd   = cam.get_dir_forward().normalized();
    const Vec3d anchor = m_mv_base + m_mv_offset;
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th   = std::max(15.0 * upp, 1e-4);
    const double L    = move_gizmo_arm(cam);        // scales with the body (see move_gizmo_arm)

    const SketchPlane saved = m_plane;
    SketchPlane bb; bb.origin = anchor; bb.x_axis = right; bb.y_axis = up; bb.normal = fwd;
    m_plane = bb;

    const Vec3d axes[3]   = { Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ() };
    const ColorRGBA cols[3] = { ColorRGBA(0.92f, 0.28f, 0.28f, 1.0f),   // X red
                                ColorRGBA(0.30f, 0.80f, 0.34f, 1.0f),   // Y green
                                ColorRGBA(0.32f, 0.55f, 0.95f, 1.0f) }; // Z blue

    glsafe(::glDisable(GL_DEPTH_TEST));
    for (int a = 0; a < 3; ++a) {
        const Vec3d tipw = anchor + axes[a] * L;
        const Vec2d tip2((tipw - anchor).dot(right), (tipw - anchor).dot(up));
        if (tip2.norm() < 1e-6) continue;           // axis ~parallel to view: skip
        const Vec2d u = tip2.normalized();
        const Vec2d nrm(-u.y(), u.x());
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        segs.emplace_back(Vec2d(0, 0), tip2);
        const double as = std::max(tip2.norm() * 0.20, th * 0.9);
        const Vec2d back = tip2 - u * as;
        segs.emplace_back(tip2, back + nrm * (as * 0.5));
        segs.emplace_back(tip2, back - nrm * (as * 0.5));
        draw_strokes(m_mv_arrow_model, segs, std::max(0.7 * upp, 1e-4), cols[a]);
        const double off = m_mv_offset[a];
        if (std::abs(off) > 1e-4) {
            DimAnnot da; da.kind = DimType::Distance; da.value = std::abs(off);
            draw_text(m_line_model, dim_text(da), tip2 + u * (th * 1.4), th, cols[a]);
        }
    }
    m_plane = saved;

    // Three world-axis rotation rings (X/Y/Z), each a circle in the plane perpendicular to
    // its axis through the gizmo anchor — drag a ring to rotate the body about that axis.
    const double R = 0.83 * move_gizmo_arm(cam);   // rings just inside the arrow tips
    for (int a = 0; a < 3; ++a) {
        Vec3d e, u, v; ring_basis(a, e, u, v);
        SketchPlane rp; rp.origin = anchor; rp.x_axis = u; rp.y_axis = v; rp.normal = e;
        m_plane = rp;
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        const int N = 48;
        Vec2d prev(R, 0.0);
        for (int i = 1; i <= N; ++i) {
            const double t = 2.0 * M_PI * double(i) / double(N);
            const Vec2d cur(R * std::cos(t), R * std::sin(t));
            segs.emplace_back(prev, cur); prev = cur;
        }
        draw_strokes(m_mv_arrow_model, segs, std::max(0.55 * upp, 1e-4), cols[a]);
    }
    m_plane = saved;
}

// Ray vs each world-axis arrow segment; nearest within ~7 px wins.
bool DesignSketchTool::hit_test_move_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt, int& axis) const
{
    if (!m_mv_active) return false;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double L   = move_gizmo_arm(cam);         // must match render_move_gizmo
    const Vec3d anchor = m_mv_base + m_mv_offset;
    const Vec3d axes[3] = { Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ() };
    int best = -1; double bestd = 7.0 * upp;        // ~7 px tolerance
    for (int a = 0; a < 3; ++a) {
        const double d = ray_segment_dist3(ro, rd, anchor, anchor + axes[a] * L);
        if (d <= bestd) { bestd = d; best = a; }
    }
    if (best < 0) return false;
    axis = best; return true;
}

// Skew-line closest point of the mouse ray to the axis line through the ORIGINAL centroid
// -> signed offset along that axis (no clamp; a body can move either way).
void DesignSketchTool::drag_move_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt, int axis)
{
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Vec3d axes[3] = { Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ() };
    const Vec3d e = axes[axis];
    const Vec3d w0 = m_mv_base - ro;
    const double a = e.dot(e), b = e.dot(rd), c = rd.dot(rd), dd = e.dot(w0), ee = rd.dot(w0);
    const double denom = a * c - b * b;
    if (std::abs(denom) < 1e-7) return;             // camera ∥ axis: leave offset as-is
    m_mv_offset[axis] = (b * ee - c * dd) / denom;
    if (on_body_move_changed) on_body_move_changed(m_mv_body, compose_move_xform());
}

void DesignSketchTool::open_move_editor(int axis)
{
    if (!on_inline_edit) return;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, m_mv_offset[axis], "",
        [this, axis](double v) {
            m_mv_offset[axis] = v;
            if (on_body_move_changed) on_body_move_changed(m_mv_body, compose_move_xform());
        },
        []() {});
}

// Ray vs each rotation ring (sampled polyline); nearest within ~7 px wins -> axis 0/1/2.
bool DesignSketchTool::hit_test_move_arc(GLCanvas3D& canvas, const wxMouseEvent& evt, int& axis) const
{
    if (!m_mv_active) return false;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double R = 0.83 * move_gizmo_arm(cam);   // rings just inside the arrow tips
    const Vec3d anchor = m_mv_base + m_mv_offset;
    int best = -1; double bestd = 7.0 * upp;
    const int N = 48;
    for (int a = 0; a < 3; ++a) {
        Vec3d e, u, v; ring_basis(a, e, u, v);
        Vec3d prev = anchor + R * u;
        for (int i = 1; i <= N; ++i) {
            const double t = 2.0 * M_PI * double(i) / double(N);
            const Vec3d cur = anchor + R * (std::cos(t) * u + std::sin(t) * v);
            const double d = ray_segment_dist3(ro, rd, prev, cur);
            if (d <= bestd) { bestd = d; best = a; }
            prev = cur;
        }
    }
    if (best < 0) return false;
    axis = best; return true;
}

// Intersect the mouse ray with ring `axis`'s plane through the anchor -> in-plane angle.
bool DesignSketchTool::arc_mouse_angle(GLCanvas3D& canvas, const wxMouseEvent& evt, int axis, double& ang) const
{
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    Vec3d e, u, v; ring_basis(axis, e, u, v);
    const Vec3d p = m_mv_base + m_mv_offset;
    const double denom = rd.dot(e);
    if (std::abs(denom) < 1e-9) return false;          // ray ∥ ring plane
    const Vec3d hit = ro + ((p - ro).dot(e) / denom) * rd;
    const Vec3d d = hit - p;
    ang = std::atan2(d.dot(v), d.dot(u));
    return true;
}

// Drag a ring -> rotate the body about that world axis by (mouse angle - grab angle).
void DesignSketchTool::drag_move_arc(GLCanvas3D& canvas, const wxMouseEvent& evt, int axis)
{
    double ang;
    if (!arc_mouse_angle(canvas, evt, axis, ang)) return;
    Vec3d e, u, v; ring_basis(axis, e, u, v);
    const double delta = ang - m_mv_arc_a0;
    m_mv_rot = Eigen::AngleAxisd(delta, e).toRotationMatrix() * m_mv_rot_start;
    if (on_body_move_changed) on_body_move_changed(m_mv_body, compose_move_xform());
}

// ---- Fillet/Chamfer radius gizmo ------------------------------------------------------
// Anchor a single radius arrow at the picked edge midpoint (m_sel_edge_pts is already in world
// space, body-transformed), perpendicular to the edge and pointing away from the body centroid —
// the natural outward direction a fillet/chamfer grows.
bool DesignSketchTool::set_fillet_gizmo(const Vec3d& body_centroid, double radius)
{
    if (m_sel_edge_pts.size() < 2) { m_fl_active = false; return false; }
    const size_t n = m_sel_edge_pts.size();
    // True geometric midpoint along the edge: a straight edge often samples to just its two
    // endpoints, so the middle INDEX would land on an end. Walk the polyline to its half-length.
    double total = 0.0;
    for (size_t i = 1; i < n; ++i) total += (m_sel_edge_pts[i] - m_sel_edge_pts[i - 1]).norm();
    m_fl_anchor = m_sel_edge_pts[0];
    Vec3d t = m_sel_edge_pts[n - 1] - m_sel_edge_pts[0];
    const double half = 0.5 * total;
    double acc = 0.0;
    for (size_t i = 1; i < n; ++i) {
        const Vec3d seg = m_sel_edge_pts[i] - m_sel_edge_pts[i - 1];
        const double L = seg.norm();
        if (acc + L >= half && L > 1e-12) {
            m_fl_anchor = m_sel_edge_pts[i - 1] + seg * ((half - acc) / L);
            t = seg;
            break;
        }
        acc += L;
    }
    if (t.norm() < 1e-9) return false;
    t.normalize();
    Vec3d r = m_fl_anchor - body_centroid;           // radial offset from the body centre
    r -= r.dot(t) * t;                               // strip the along-edge component
    if (r.norm() < 1e-6) {                           // edge passes through the centroid
        r = t.cross(Vec3d::UnitZ());
        if (r.norm() < 1e-6) r = t.cross(Vec3d::UnitX());
    }
    m_fl_dir    = r.normalized();
    m_fl_radius = std::max(0.01, radius);
    if (!m_fl_active) m_fl_drag = false;   // re-anchored every preview: preserve an in-progress drag
    m_fl_active = true;
    return true;
}

void DesignSketchTool::clear_fillet_gizmo()
{
    m_fl_active = false;
    m_fl_drag   = false;
}

// Single billboarded radius arrow from the edge midpoint along m_fl_dir; length = radius (world),
// floored to a grabbable screen size. Label shows the true radius (R-prefixed).
void DesignSketchTool::render_fillet_gizmo()
{
    if (!m_fl_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th   = std::max(15.0 * upp, 1e-4);
    const double L    = std::max(m_fl_radius, 40.0 * upp);   // WYSIWYG, floored to a comfortable handle
    const Vec3d tipw  = m_fl_anchor + m_fl_dir * L;

    const SketchPlane saved = m_plane;
    SketchPlane bb; bb.origin = m_fl_anchor; bb.x_axis = right; bb.y_axis = up; bb.normal = cam.get_dir_forward().normalized();
    m_plane = bb;
    const ColorRGBA arrowc(1.0f, 0.62f, 0.16f, 1.0f);       // CAD amber

    const Vec2d tip2((tipw - m_fl_anchor).dot(right), (tipw - m_fl_anchor).dot(up));
    if (tip2.norm() > 1e-6) {
        const Vec2d u = tip2.normalized();
        const Vec2d nrm(-u.y(), u.x());
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        segs.emplace_back(Vec2d(0, 0), tip2);
        const double as = std::max(tip2.norm() * 0.20, th * 0.9);
        const Vec2d back = tip2 - u * as;
        segs.emplace_back(tip2, back + nrm * (as * 0.5));
        segs.emplace_back(tip2, back - nrm * (as * 0.5));
        glsafe(::glDisable(GL_DEPTH_TEST));
        draw_strokes(m_fl_arrow_model, segs, std::max(0.7 * upp, 1e-4), arrowc);
        DimAnnot da; da.kind = DimType::Radius; da.value = m_fl_radius;
        draw_text(m_line_model, dim_text(da), tip2 + u * (th * 1.4), th, arrowc);
    }
    m_plane = saved;
}

// Ray vs the radius arrow segment; ~12 px tolerance over the (floored) handle length.
bool DesignSketchTool::hit_test_fillet_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    if (!m_fl_active) return false;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double L   = std::max(m_fl_radius, 40.0 * upp);
    return ray_segment_dist3(ro, rd, m_fl_anchor, m_fl_anchor + m_fl_dir * L) <= 12.0 * upp;
}

// Skew-line closest point of the mouse ray to the radius axis -> signed distance along m_fl_dir
// from the anchor. NaN when the camera is ~parallel to the axis (no meaningful projection).
double DesignSketchTool::fillet_axis_proj(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Vec3d e = m_fl_dir;
    const Vec3d w0 = m_fl_anchor - ro;
    const double a = e.dot(e), b = e.dot(rd), c = rd.dot(rd), dd = e.dot(w0), ee = rd.dot(w0);
    const double denom = a * c - b * b;
    if (std::abs(denom) < 1e-7) return std::nan("");
    return (b * ee - c * dd) / denom;
}

// Record the grab reference so the drag is RELATIVE (grab anywhere on the handle without the
// radius snapping to the grab point — important since the handle is floored to a min size).
void DesignSketchTool::start_fillet_drag(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    m_fl_drag        = true;
    m_fl_press_x     = evt.GetX();
    m_fl_press_y     = evt.GetY();
    m_fl_grab_radius = m_fl_radius;
    const double p   = fillet_axis_proj(canvas, evt);
    m_fl_grab_proj   = std::isnan(p) ? 0.0 : p;
}

void DesignSketchTool::drag_fillet_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    const double proj = fillet_axis_proj(canvas, evt);
    if (std::isnan(proj)) return;                   // camera ∥ axis: leave radius as-is
    m_fl_radius = std::max(0.01, m_fl_grab_radius + (proj - m_fl_grab_proj));
    if (on_fillet_radius_changed) on_fillet_radius_changed(m_fl_radius);
}

void DesignSketchTool::open_fillet_editor()
{
    if (!on_inline_edit) return;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, m_fl_radius, "",
        [this](double v) {
            m_fl_radius = std::max(0.01, v);
            if (on_fillet_radius_changed) on_fillet_radius_changed(m_fl_radius);
        },
        []() {});
}

// ---- Hole gizmo -----------------------------------------------------------------------------
// Positioned circular cut: footprint circle on the plane + a radial diameter arrow (plane u-axis)
// + a normal-axis depth arrow (drawn only when !through, matching the kernel's blind cut) + a
// draggable centre marker. The panel re-pushes this every preview, so (like the fillet gizmo) we
// must NOT reset an in-progress drag when already active.
void DesignSketchTool::set_hole_gizmo(const SketchPlane& plane, double x, double y,
                                      double diameter, double depth, bool through)
{
    m_hl_plane    = plane;
    m_hl_x        = x;
    m_hl_y        = y;
    m_hl_diameter = std::max(0.01, diameter);
    m_hl_depth    = std::max(0.01, depth);
    m_hl_through  = through;
    if (!m_hl_active) m_hl_drag = -1;   // re-pushed every preview: preserve an in-progress drag
    m_hl_active   = true;
}

void DesignSketchTool::clear_hole_gizmo()
{
    m_hl_active = false;
    m_hl_drag   = -1;
}

void DesignSketchTool::set_hole_face_bounds(bool has, double umin, double umax, double vmin, double vmax)
{
    m_hl_has_bounds = has;
    m_hl_umin = umin; m_hl_umax = umax; m_hl_vmin = vmin; m_hl_vmax = vmax;
}

void DesignSketchTool::render_hole_gizmo()
{
    if (!m_hl_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th   = std::max(15.0 * upp, 1e-4);

    const Vec3d centre = m_hl_plane.to_world(Vec2d(m_hl_x, m_hl_y));
    const Vec3d nrm    = m_hl_plane.normal.normalized();
    const Vec3d ddir   = m_hl_plane.x_axis.normalized();     // diameter arrow runs along plane u
    const double r     = std::max(0.01, m_hl_diameter * 0.5);

    const SketchPlane saved = m_plane;

    // (1) Footprint circle drawn ON the plane (lifts plane u/v -> world through to_world).
    {
        m_plane = m_hl_plane;
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        const int N = 48;
        for (int i = 0; i < N; ++i) {
            const double a0 = (2.0 * M_PI * i) / N, a1 = (2.0 * M_PI * (i + 1)) / N;
            segs.emplace_back(Vec2d(m_hl_x + r * std::cos(a0), m_hl_y + r * std::sin(a0)),
                              Vec2d(m_hl_x + r * std::cos(a1), m_hl_y + r * std::sin(a1)));
        }
        glsafe(::glDisable(GL_DEPTH_TEST));
        draw_strokes(m_hl_stroke_model, segs, std::max(0.6 * upp, 1e-4), ColorRGBA(1.0f, 0.62f, 0.16f, 1.0f));
        m_plane = saved;
    }

    // (2) Billboarded handles (centre marker + diameter arrow + depth arrow), screen-facing frame
    // at the centre so draw_strokes/draw_text read on top regardless of orientation.
    SketchPlane bb; bb.origin = centre; bb.x_axis = right; bb.y_axis = up; bb.normal = cam.get_dir_forward().normalized();
    m_plane = bb;
    const ColorRGBA amber(1.0f, 0.62f, 0.16f, 1.0f);
    const ColorRGBA blue (0.30f, 0.55f, 1.0f, 1.0f);

    auto arrow_to = [&](const Vec3d& tipw, const ColorRGBA& col, const DimAnnot& da) {
        const Vec2d tip2((tipw - centre).dot(right), (tipw - centre).dot(up));
        if (tip2.norm() <= 1e-6) return;
        const Vec2d u = tip2.normalized();
        const Vec2d nrm2(-u.y(), u.x());
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        segs.emplace_back(Vec2d(0, 0), tip2);
        const double as = std::max(tip2.norm() * 0.20, th * 0.9);
        const Vec2d back = tip2 - u * as;
        segs.emplace_back(tip2, back + nrm2 * (as * 0.5));
        segs.emplace_back(tip2, back - nrm2 * (as * 0.5));
        glsafe(::glDisable(GL_DEPTH_TEST));
        draw_strokes(m_hl_stroke_model, segs, std::max(0.7 * upp, 1e-4), col);
        draw_text(m_line_model, dim_text(da), tip2 + u * (th * 1.4), th, col);
    };

    // Diameter arrow: WYSIWYG radius, floored to a grabbable handle; label shows Ø (full diameter).
    {
        const double L = std::max(r, 40.0 * upp);
        DimAnnot da; da.kind = DimType::Diameter; da.value = m_hl_diameter;
        arrow_to(centre + ddir * L, amber, da);
    }
    // Depth arrow along +normal (blind cut). Through cuts ignore depth, so skip the arrow then.
    if (!m_hl_through) {
        const double L = std::max(m_hl_depth, 40.0 * upp);
        DimAnnot da; da.kind = DimType::Distance; da.value = m_hl_depth;
        arrow_to(centre + nrm * L, blue, da);
    }

    m_plane = saved;

    // Move handle: a small 3D CUBE at the hole centre (Orca text/SVG-on-face feel) — grab and drag
    // it to slide the hole across the face. Built from the plane axes so it sits flat on the face.
    {
        using EPT = GLModel::Geometry::EPrimitiveType;
        using EVL = GLModel::Geometry::EVertexLayout;
        const double hs = 9.0 * upp;   // cube half-size (~9 px); hit-test uses the same below
        const Vec3d U = ddir * hs, V = m_hl_plane.y_axis.normalized() * hs, Nn = nrm * hs;
        // Cube centred EXACTLY on the surface point (= the hole). Depth-test ON occludes the inner
        // half inside the solid, so the visible half-cube reads as planted at the hole — no depth-off
        // float that looked offset from the on-surface footprint. Hit-test targets the same `centre`.
        Vec3d c8[8];
        for (int i = 0; i < 8; ++i)
            c8[i] = centre + ((i & 1) ? U : -U) + ((i & 2) ? V : -V) + ((i & 4) ? Nn : -Nn);
        // 6 faces (CCW), each as 2 triangles, lightly shaded so the box reads as a cube.
        const int faces[6][4] = { {0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3} };
        const float shade[6]  = { 0.78f, 1.0f, 0.86f, 0.92f, 0.70f, 0.96f };
        glsafe(::glEnable(GL_DEPTH_TEST));
        glsafe(::glDisable(GL_CULL_FACE));
        for (int f = 0; f < 6; ++f) {
            GLModel::Geometry g; g.format = { EPT::Triangles, EVL::P3 };
            for (int k = 0; k < 4; ++k) g.add_vertex((Vec3f)c8[faces[f][k]].cast<float>());
            g.add_triangle(0, 1, 2); g.add_triangle(0, 2, 3);
            GLModel m; m.init_from(std::move(g));
            m.set_color(ColorRGBA(1.0f * shade[f], 0.62f * shade[f], 0.16f * shade[f], 1.0f));
            m.render();
        }
    }
}

// Best-matching hole handle under the cursor: centre (0) / diameter (1) / depth (2), or -1.
// Tested by ray-to-segment distance in world; ~12 px tolerance. Centre wins at the shared base.
int DesignSketchTool::hit_test_hole_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    if (!m_hl_active) return -1;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);

    const Vec3d centre = m_hl_plane.to_world(Vec2d(m_hl_x, m_hl_y));
    const Vec3d nrm    = m_hl_plane.normal.normalized();
    const Vec3d ddir   = m_hl_plane.x_axis.normalized();
    const double rad   = std::max(m_hl_diameter * 0.5, 40.0 * upp);
    const double depL  = std::max(m_hl_depth, 40.0 * upp);
    const double tol   = 12.0 * upp;

    // Centre wins at the shared base: the move cube straddles the centre, so a grab within the cube
    // half-size is a reposition. The arrows are only grabbable along the shaft extending outward,
    // which also keeps an edge-on arrow (e.g. depth in top view) from stealing the reposition grab.
    if (ray_segment_dist3(ro, rd, centre, centre) <= 9.0 * upp) return 0;   // cube half-size

    int best = -1; double bestd = tol;
    const double dD = ray_segment_dist3(ro, rd, centre, centre + ddir * rad);
    if (dD < bestd) { bestd = dD; best = 1; }
    if (!m_hl_through) {
        const double dZ = ray_segment_dist3(ro, rd, centre, centre + nrm * depL);
        if (dZ < bestd) { bestd = dZ; best = 2; }
    }
    return best;
}

// Skew-line closest point of the mouse ray to an axis (anchor + t*dir) -> signed distance along
// dir. NaN when the camera is ~parallel to the axis (no meaningful projection).
double DesignSketchTool::hole_axis_proj(GLCanvas3D& canvas, const wxMouseEvent& evt,
                                        const Vec3d& anchor, const Vec3d& dir) const
{
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Vec3d e = dir;
    const Vec3d w0 = anchor - ro;
    const double a = e.dot(e), b = e.dot(rd), c = rd.dot(rd), dd = e.dot(w0), ee = rd.dot(w0);
    const double denom = a * c - b * b;
    // Relative near-parallel guard: when the camera ray is ~along the axis (e.g. the depth axis in
    // top view) denom collapses; a tiny absolute floor lets a huge, unstable projection through.
    if (std::abs(denom) < 1e-4 * std::max(a * c, 1e-12)) return std::nan("");
    return (b * ee - c * dd) / denom;
}

void DesignSketchTool::start_hole_drag(GLCanvas3D& canvas, const wxMouseEvent& evt, int which)
{
    m_hl_drag    = which;
    m_hl_press_x = evt.GetX();
    m_hl_press_y = evt.GetY();
    const Vec3d centre = m_hl_plane.to_world(Vec2d(m_hl_x, m_hl_y));
    if (which == 0) {
        const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
        m_hl_grab_uv = m_hl_plane.project(r.a, r.b - r.a);
        m_hl_grab_x  = m_hl_x;
        m_hl_grab_y  = m_hl_y;
    } else if (which == 1 || which == 2) {
        const Vec3d dir = (which == 1) ? m_hl_plane.x_axis.normalized() : m_hl_plane.normal.normalized();
        const double p  = hole_axis_proj(canvas, evt, centre, dir);
        m_hl_grab_proj  = std::isnan(p) ? 0.0 : p;
        m_hl_grab_val   = (which == 1) ? m_hl_diameter * 0.5 : m_hl_depth;
    }
    // which == 3/4 (X/Y dim labels) are edit-only: a stationary click opens the inline editor.
}

void DesignSketchTool::drag_hole_handle(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    if (m_hl_drag >= 3) return;                    // X/Y dim labels are click-to-edit, not drag
    if (m_hl_drag == 0) {                          // reposition centre on the plane
        const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
        const Vec2d uv = m_hl_plane.project(r.a, r.b - r.a);
        m_hl_x = m_hl_grab_x + (uv.x() - m_hl_grab_uv.x());
        m_hl_y = m_hl_grab_y + (uv.y() - m_hl_grab_uv.y());
    } else {                                       // diameter (1) or depth (2): relative axis drag
        const Vec3d centre = m_hl_plane.to_world(Vec2d(m_hl_x, m_hl_y));
        const Vec3d dir = (m_hl_drag == 1) ? m_hl_plane.x_axis.normalized() : m_hl_plane.normal.normalized();
        const double proj = hole_axis_proj(canvas, evt, centre, dir);
        if (std::isnan(proj)) return;              // camera ∥ axis: leave value as-is
        const double v = std::max(0.01, m_hl_grab_val + (proj - m_hl_grab_proj));
        if (m_hl_drag == 1) m_hl_diameter = 2.0 * v;
        else                m_hl_depth    = v;
    }
    if (on_hole_changed) on_hole_changed(m_hl_x, m_hl_y, m_hl_diameter, m_hl_depth);
}

void DesignSketchTool::open_hole_editor(int which)
{
    if (!on_inline_edit) return;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    if (which == 1) {
        on_inline_edit(px, m_hl_diameter, "",
            [this](double v) {
                m_hl_diameter = std::max(0.01, v);
                if (on_hole_changed) on_hole_changed(m_hl_x, m_hl_y, m_hl_diameter, m_hl_depth);
            },
            []() {});
    } else if (which == 2) {
        on_inline_edit(px, m_hl_depth, "",
            [this](double v) {
                m_hl_depth = std::max(0.01, v);
                if (on_hole_changed) on_hole_changed(m_hl_x, m_hl_y, m_hl_diameter, m_hl_depth);
            },
            []() {});
    } else if (which == 3) {                       // #2 Part B: edit the distance from the u-side
        const double ru = m_hl_has_bounds ? m_hl_umin : 0.0;
        on_inline_edit(px, m_hl_x - ru, "",
            [this, ru](double v) {
                m_hl_x = ru + v;
                if (on_hole_changed) on_hole_changed(m_hl_x, m_hl_y, m_hl_diameter, m_hl_depth);
            },
            []() {});
    } else if (which == 4) {                       // edit the distance from the v-side
        const double rv = m_hl_has_bounds ? m_hl_vmin : 0.0;
        on_inline_edit(px, m_hl_y - rv, "",
            [this, rv](double v) {
                m_hl_y = rv + v;
                if (on_hole_changed) on_hole_changed(m_hl_x, m_hl_y, m_hl_diameter, m_hl_depth);
            },
            []() {});
    }
    // which == 0 (centre): no scalar to edit inline — it's a drag-only reposition handle.
}

// ---- Thread gizmo ---------------------------------------------------------------------------
// Mirrors the hole gizmo: footprint circle on the plane at the nominal radius + a radial radius
// arrow (R label) + a normal-axis length arrow (always shown) + a draggable centre marker.
// Reuses hole_axis_proj() for the relative axis drags.
void DesignSketchTool::set_thread_gizmo(const SketchPlane& plane, double x, double y,
                                        double radius, double height)
{
    m_th_plane  = plane;
    m_th_x      = x;
    m_th_y      = y;
    m_th_radius = std::max(0.01, radius);
    m_th_height = std::max(0.01, height);
    if (!m_th_active) m_th_drag = -1;   // re-pushed every preview: preserve an in-progress drag
    m_th_active = true;
}

void DesignSketchTool::clear_thread_gizmo()
{
    m_th_active = false;
    m_th_drag   = -1;
}

void DesignSketchTool::render_thread_gizmo()
{
    if (!m_th_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th   = std::max(15.0 * upp, 1e-4);

    const Vec3d centre = m_th_plane.to_world(Vec2d(m_th_x, m_th_y));
    const Vec3d nrm    = m_th_plane.normal.normalized();
    const Vec3d ddir   = m_th_plane.x_axis.normalized();     // radius arrow runs along plane u
    const double r     = std::max(0.01, m_th_radius);

    const SketchPlane saved = m_plane;

    // (1) Footprint circle on the plane at the nominal radius.
    {
        m_plane = m_th_plane;
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        const int N = 48;
        for (int i = 0; i < N; ++i) {
            const double a0 = (2.0 * M_PI * i) / N, a1 = (2.0 * M_PI * (i + 1)) / N;
            segs.emplace_back(Vec2d(m_th_x + r * std::cos(a0), m_th_y + r * std::sin(a0)),
                              Vec2d(m_th_x + r * std::cos(a1), m_th_y + r * std::sin(a1)));
        }
        glsafe(::glDisable(GL_DEPTH_TEST));
        draw_strokes(m_th_stroke_model, segs, std::max(0.6 * upp, 1e-4), ColorRGBA(0.55f, 0.80f, 0.30f, 1.0f));
        m_plane = saved;
    }

    // (2) Billboarded handles.
    SketchPlane bb; bb.origin = centre; bb.x_axis = right; bb.y_axis = up; bb.normal = cam.get_dir_forward().normalized();
    m_plane = bb;
    const ColorRGBA green(0.55f, 0.80f, 0.30f, 1.0f);
    const ColorRGBA blue (0.30f, 0.55f, 1.0f, 1.0f);

    auto arrow_to = [&](const Vec3d& tipw, const ColorRGBA& col, const DimAnnot& da) {
        const Vec2d tip2((tipw - centre).dot(right), (tipw - centre).dot(up));
        if (tip2.norm() <= 1e-6) return;
        const Vec2d u = tip2.normalized();
        const Vec2d nrm2(-u.y(), u.x());
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        segs.emplace_back(Vec2d(0, 0), tip2);
        const double as = std::max(tip2.norm() * 0.20, th * 0.9);
        const Vec2d back = tip2 - u * as;
        segs.emplace_back(tip2, back + nrm2 * (as * 0.5));
        segs.emplace_back(tip2, back - nrm2 * (as * 0.5));
        glsafe(::glDisable(GL_DEPTH_TEST));
        draw_strokes(m_th_stroke_model, segs, std::max(0.7 * upp, 1e-4), col);
        draw_text(m_line_model, dim_text(da), tip2 + u * (th * 1.4), th, col);
    };

    {   // Radius arrow (R label), floored to a grabbable handle.
        const double L = std::max(r, 40.0 * upp);
        DimAnnot da; da.kind = DimType::Radius; da.value = m_th_radius;
        arrow_to(centre + ddir * L, green, da);
    }
    {   // Length arrow along +normal.
        const double L = std::max(m_th_height, 40.0 * upp);
        DimAnnot da; da.kind = DimType::Distance; da.value = m_th_height;
        arrow_to(centre + nrm * L, blue, da);
    }
    {   // Centre marker.
        const double s = 7.0 * upp;
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        segs.emplace_back(Vec2d(-s, -s), Vec2d(s, -s));
        segs.emplace_back(Vec2d( s, -s), Vec2d(s,  s));
        segs.emplace_back(Vec2d( s,  s), Vec2d(-s, s));
        segs.emplace_back(Vec2d(-s,  s), Vec2d(-s, -s));
        glsafe(::glDisable(GL_DEPTH_TEST));
        draw_strokes(m_th_stroke_model, segs, std::max(0.7 * upp, 1e-4), green);
    }

    m_plane = saved;
}

int DesignSketchTool::hit_test_thread_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    if (!m_th_active) return -1;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);

    const Vec3d centre = m_th_plane.to_world(Vec2d(m_th_x, m_th_y));
    const Vec3d nrm    = m_th_plane.normal.normalized();
    const Vec3d ddir   = m_th_plane.x_axis.normalized();
    const double rad   = std::max(m_th_radius, 40.0 * upp);
    const double hL    = std::max(m_th_height, 40.0 * upp);
    const double tol   = 12.0 * upp;

    if (ray_segment_dist3(ro, rd, centre, centre) <= tol) return 0;   // centre wins at the base
    int best = -1; double bestd = tol;
    const double dR = ray_segment_dist3(ro, rd, centre, centre + ddir * rad);
    if (dR < bestd) { bestd = dR; best = 1; }
    const double dH = ray_segment_dist3(ro, rd, centre, centre + nrm * hL);
    if (dH < bestd) { bestd = dH; best = 2; }
    return best;
}

void DesignSketchTool::start_thread_drag(GLCanvas3D& canvas, const wxMouseEvent& evt, int which)
{
    m_th_drag    = which;
    m_th_press_x = evt.GetX();
    m_th_press_y = evt.GetY();
    const Vec3d centre = m_th_plane.to_world(Vec2d(m_th_x, m_th_y));
    if (which == 0) {
        const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
        m_th_grab_uv = m_th_plane.project(r.a, r.b - r.a);
        m_th_grab_x  = m_th_x;
        m_th_grab_y  = m_th_y;
    } else {
        const Vec3d dir = (which == 1) ? m_th_plane.x_axis.normalized() : m_th_plane.normal.normalized();
        const double p  = hole_axis_proj(canvas, evt, centre, dir);
        m_th_grab_proj  = std::isnan(p) ? 0.0 : p;
        m_th_grab_val   = (which == 1) ? m_th_radius : m_th_height;
    }
}

void DesignSketchTool::drag_thread_handle(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    if (m_th_drag == 0) {
        const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
        const Vec2d uv = m_th_plane.project(r.a, r.b - r.a);
        m_th_x = m_th_grab_x + (uv.x() - m_th_grab_uv.x());
        m_th_y = m_th_grab_y + (uv.y() - m_th_grab_uv.y());
    } else {
        const Vec3d centre = m_th_plane.to_world(Vec2d(m_th_x, m_th_y));
        const Vec3d dir = (m_th_drag == 1) ? m_th_plane.x_axis.normalized() : m_th_plane.normal.normalized();
        const double proj = hole_axis_proj(canvas, evt, centre, dir);
        if (std::isnan(proj)) return;
        const double v = std::max(0.01, m_th_grab_val + (proj - m_th_grab_proj));
        if (m_th_drag == 1) m_th_radius = v;
        else                m_th_height = v;
    }
    if (on_thread_changed) on_thread_changed(m_th_x, m_th_y, m_th_radius, m_th_height);
}

void DesignSketchTool::open_thread_editor(int which)
{
    if (!on_inline_edit) return;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    if (which == 1) {
        on_inline_edit(px, m_th_radius, "",
            [this](double v) {
                m_th_radius = std::max(0.01, v);
                if (on_thread_changed) on_thread_changed(m_th_x, m_th_y, m_th_radius, m_th_height);
            },
            []() {});
    } else if (which == 2) {
        on_inline_edit(px, m_th_height, "",
            [this](double v) {
                m_th_height = std::max(0.01, v);
                if (on_thread_changed) on_thread_changed(m_th_x, m_th_y, m_th_radius, m_th_height);
            },
            []() {});
    }
}

// ---- Shell gizmo ----------------------------------------------------------------------------
// A single inward thickness arrow at the picked open-face centroid (mirrors the fillet radius
// arrow). RELATIVE drag (like fillet), reusing hole_axis_proj for the projection.
void DesignSketchTool::set_shell_gizmo(const Vec3d& face_centroid, const Vec3d& inward_dir,
                                       double thickness)
{
    m_sh_anchor    = face_centroid;
    if (inward_dir.norm() > 1e-9) m_sh_dir = inward_dir.normalized();
    m_sh_thickness = std::max(0.01, thickness);
    if (!m_sh_active) m_sh_drag = false;   // re-pushed every preview: preserve an in-progress drag
    m_sh_active = true;
}

void DesignSketchTool::clear_shell_gizmo()
{
    m_sh_active = false;
    m_sh_drag   = false;
}

void DesignSketchTool::render_shell_gizmo()
{
    if (!m_sh_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th   = std::max(15.0 * upp, 1e-4);
    const double L    = std::max(m_sh_thickness, 40.0 * upp);   // WYSIWYG, floored to a handle
    const Vec3d tipw  = m_sh_anchor + m_sh_dir * L;

    const SketchPlane saved = m_plane;
    SketchPlane bb; bb.origin = m_sh_anchor; bb.x_axis = right; bb.y_axis = up; bb.normal = cam.get_dir_forward().normalized();
    m_plane = bb;
    const ColorRGBA teal(0.20f, 0.80f, 0.75f, 1.0f);

    const Vec2d tip2((tipw - m_sh_anchor).dot(right), (tipw - m_sh_anchor).dot(up));
    if (tip2.norm() > 1e-6) {
        const Vec2d u = tip2.normalized();
        const Vec2d nrm(-u.y(), u.x());
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        segs.emplace_back(Vec2d(0, 0), tip2);
        const double as = std::max(tip2.norm() * 0.20, th * 0.9);
        const Vec2d back = tip2 - u * as;
        segs.emplace_back(tip2, back + nrm * (as * 0.5));
        segs.emplace_back(tip2, back - nrm * (as * 0.5));
        glsafe(::glDisable(GL_DEPTH_TEST));
        draw_strokes(m_sh_stroke_model, segs, std::max(0.7 * upp, 1e-4), teal);
        DimAnnot da; da.kind = DimType::Distance; da.value = m_sh_thickness;
        draw_text(m_line_model, dim_text(da), tip2 + u * (th * 1.4), th, teal);
    }
    m_plane = saved;
}

bool DesignSketchTool::hit_test_shell_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    if (!m_sh_active) return false;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double L   = std::max(m_sh_thickness, 40.0 * upp);
    return ray_segment_dist3(ro, rd, m_sh_anchor, m_sh_anchor + m_sh_dir * L) <= 12.0 * upp;
}

void DesignSketchTool::start_shell_drag(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    m_sh_drag      = true;
    m_sh_press_x   = evt.GetX();
    m_sh_press_y   = evt.GetY();
    m_sh_grab_val  = m_sh_thickness;
    const double p = hole_axis_proj(canvas, evt, m_sh_anchor, m_sh_dir);
    m_sh_grab_proj = std::isnan(p) ? 0.0 : p;
}

void DesignSketchTool::drag_shell_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    const double proj = hole_axis_proj(canvas, evt, m_sh_anchor, m_sh_dir);
    if (std::isnan(proj)) return;
    m_sh_thickness = std::max(0.01, m_sh_grab_val + (proj - m_sh_grab_proj));
    if (on_shell_thickness_changed) on_shell_thickness_changed(m_sh_thickness);
}

void DesignSketchTool::open_shell_editor()
{
    if (!on_inline_edit) return;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, m_sh_thickness, "",
        [this](double v) {
            m_sh_thickness = std::max(0.01, v);
            if (on_shell_thickness_changed) on_shell_thickness_changed(m_sh_thickness);
        },
        []() {});
}

// ---- Revolve angle-arc gizmo ----------------------------------------------------------------
// Arc in the revolve plane (perpendicular to the axis) at the profile's radius. The arc center is
// the projection of the profile centroid onto the axis, so the arc rides at the profile's height.
// The yaxis sense follows m_rv_flip, matching the kernel's negative-angle reversed sweep.
static Vec3d rv_yaxis(const Vec3d& axis, const Vec3d& ref, bool flip)
{
    Vec3d y = axis.cross(ref);
    if (y.norm() < 1e-9) return ref;            // degenerate; never used (ref ⟂ axis by construction)
    y.normalize();
    return flip ? -y : y;
}

// Arc in the draft plane (perpendicular to the world +Z axis through the face centroid).
// The arc sweeps from angle 0 to m_dr_angle, representing the taper amount.
static Vec3d dr_yaxis(const Vec3d& axis, const Vec3d& ref)
{
    Vec3d y = axis.cross(ref);
    if (y.norm() < 1e-9) return ref;
    y.normalize();
    return y;
}

void DesignSketchTool::set_draft_gizmo(const Vec3d& face_centroid, const Vec3d& face_normal, double angle)
{
    m_dr_center = face_centroid;
    m_dr_angle  = std::min(89.0, std::max(-89.0, angle));
    m_dr_axis   = Vec3d::UnitZ();                    // pull direction is world +Z
    Vec3d ref = face_normal - face_normal.dot(m_dr_axis) * m_dr_axis;  // horizontal component
    if (ref.norm() < 1e-6) ref = Vec3d::UnitX();     // fallback: face normal is parallel to Z
    m_dr_ref    = ref / ref.norm();
    m_dr_radius = 12.0;                              // fixed world-size manipulator
    if (!m_dr_active) m_dr_drag = false;
    m_dr_active = true;
}

void DesignSketchTool::clear_draft_gizmo()
{
    m_dr_active = false;
    m_dr_drag   = false;
}

void DesignSketchTool::render_draft_gizmo()
{
    if (!m_dr_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th  = std::max(15.0 * upp, 1e-4);
    const Vec3d yax  = dr_yaxis(m_dr_axis, m_dr_ref);

    const SketchPlane saved = m_plane;
    SketchPlane rp; rp.origin = m_dr_center; rp.x_axis = m_dr_ref; rp.y_axis = yax; rp.normal = m_dr_axis;
    m_plane = rp;
    const ColorRGBA arcc(0.15f, 0.92f, 1.0f, 1.0f);
    const double r = m_dr_radius;
    // Draft angle can be negative: sweep counter-clockwise (positive) or clockwise (negative)
    const double a = m_dr_angle * M_PI / 180.0;
    const int    N = std::max(8, int(std::abs(a) / (M_PI / 32.0)));   // ~ every 5.6°

    std::vector<std::pair<Vec2d, Vec2d>> segs;
    Vec2d prev(r, 0.0);
    for (int i = 1; i <= N; ++i) {
        const double t = a * double(i) / double(N);
        const Vec2d cur(r * std::cos(t), r * std::sin(t));
        segs.emplace_back(prev, cur);
        prev = cur;
    }
    const Vec2d tip(r * std::cos(a), r * std::sin(a));
    segs.emplace_back(Vec2d(0, 0), Vec2d(r, 0));   // spoke at angle 0
    segs.emplace_back(Vec2d(0, 0), tip);           // spoke at the swept angle (the handle)
    const Vec2d tang(-std::sin(a), std::cos(a));
    const Vec2d radial = tip.normalized();
    const double as = std::max(r * 0.14, th);
    segs.emplace_back(tip, tip - tang * as - radial * (as * 0.5));
    segs.emplace_back(tip, tip - tang * as + radial * (as * 0.5));
    const double hs = std::max(th * 0.8, r * 0.05);
    const Vec2d du = radial * hs, dv = Vec2d(-radial.y(), radial.x()) * hs;
    segs.emplace_back(tip + du, tip + dv);
    segs.emplace_back(tip + dv, tip - du);
    segs.emplace_back(tip - du, tip - dv);
    segs.emplace_back(tip - dv, tip + du);

    glsafe(::glDisable(GL_DEPTH_TEST));
    draw_strokes(m_dr_stroke_model, segs, std::max(0.8 * upp, 1e-4), arcc);
    DimAnnot da; da.kind = DimType::Angle; da.value = m_dr_angle;
    draw_text(m_line_model, dim_text(da), tip * 1.14, th, arcc);
    m_plane = saved;
}

bool DesignSketchTool::hit_test_draft_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    if (!m_dr_active) return false;
    const Linef3 ray = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = ray.a, rd = ray.b - ray.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const Vec3d yax = dr_yaxis(m_dr_axis, m_dr_ref);
    const double a  = m_dr_angle * M_PI / 180.0;
    const double tol = 16.0 * upp;
    auto ray_pt = [&](const Vec3d& p) {
        const double t = (p - ro).dot(rd) / std::max(rd.dot(rd), 1e-12);
        return (p - (ro + t * rd)).norm();
    };
    const Vec3d tip = m_dr_center + m_dr_radius * (std::cos(a) * m_dr_ref + std::sin(a) * yax);
    const Vec3d ref = m_dr_center + m_dr_radius * m_dr_ref;   // angle-0 spoke end
    const int N = 48;
    for (int i = 0; i <= N; ++i) {
        const double f = double(i) / double(N);
        const double th = a * f;
        const Vec3d arc = m_dr_center + m_dr_radius * (std::cos(th) * m_dr_ref + std::sin(th) * yax);
        if (ray_pt(arc) <= tol) return true;
        if (ray_pt(m_dr_center + f * (tip - m_dr_center)) <= tol) return true;
        if (ray_pt(m_dr_center + f * (ref - m_dr_center)) <= tol) return true;
    }
    return false;
}

void DesignSketchTool::drag_draft_arc(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    const Linef3 ray = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = ray.a, rd = ray.b - ray.a;
    const double denom = rd.dot(m_dr_axis);
    if (std::abs(denom) < 1e-9) return;
    const double t = (m_dr_center - ro).dot(m_dr_axis) / denom;
    const Vec3d  p = ro + t * rd;
    const Vec3d yax = dr_yaxis(m_dr_axis, m_dr_ref);
    const double u = (p - m_dr_center).dot(m_dr_ref);
    const double v = (p - m_dr_center).dot(yax);
    double deg = std::atan2(v, u) * 180.0 / M_PI;
    deg = std::min(89.0, std::max(-89.0, deg));
    m_dr_angle = deg;
    if (m_on_draft_angle_changed) m_on_draft_angle_changed(deg);
}

// ---- Cut gizmo (plane normal arrow + wire rectangle preview) --------------------------------
// Arrow: shaft + arrowhead billboarded along the cut-plane normal from the projected body
// centre, with a signed Distance label = offset. Drag is RELATIVE via hole_axis_proj.
// Rectangle: 4 segments in the cut plane at the current offset, sized to the target body.

void DesignSketchTool::set_cut_gizmo(const SketchPlane& plane, double offset, const Vec3d& body_center, double half_extent)
{
    m_ct_n  = plane.normal.normalized();
    m_ct_u  = plane.x_axis.normalized();
    m_ct_v  = plane.y_axis.normalized();
    Vec3d rel = body_center - plane.origin;
    m_ct_base = plane.origin + (rel - rel.dot(m_ct_n) * m_ct_n);  // body center projected into the cut plane
    m_ct_offset = offset;
    m_ct_half   = std::max(half_extent, 10.0);
    if (!m_ct_active) m_ct_drag = false;
    m_ct_active = true;
}

void DesignSketchTool::clear_cut_gizmo()
{
    m_ct_active = false;
    m_ct_drag   = false;
}

void DesignSketchTool::render_cut_gizmo()
{
    if (!m_ct_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const Vec3d right = cam.get_dir_right().normalized();
    const Vec3d up    = cam.get_dir_up().normalized();
    const double upp  = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th   = std::max(15.0 * upp, 1e-4);
    const ColorRGBA teal(0.20f, 0.80f, 0.75f, 1.0f);

    // ---- Wire rectangle in the cut plane  ----
    {
        const Vec3d cutpos = m_ct_base + m_ct_offset * m_ct_n;
        const SketchPlane saved = m_plane;
        SketchPlane rp; rp.origin = cutpos; rp.x_axis = m_ct_u; rp.y_axis = m_ct_v; rp.normal = m_ct_n;
        m_plane = rp;
        const double h = m_ct_half;
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        segs.emplace_back(Vec2d(-h, -h), Vec2d( h, -h));
        segs.emplace_back(Vec2d( h, -h), Vec2d( h,  h));
        segs.emplace_back(Vec2d( h,  h), Vec2d(-h,  h));
        segs.emplace_back(Vec2d(-h,  h), Vec2d(-h, -h));
        glsafe(::glDisable(GL_DEPTH_TEST));
        draw_strokes(m_ct_rect_model, segs, std::max(0.7 * upp, 1e-4), teal);
        m_plane = saved;
    }

    // ---- Offset arrow (billboarded, clone of render_shell_gizmo) ----
    {
        const double L_sign = (std::abs(m_ct_offset) < 40.0 * upp)
            ? std::copysign(40.0 * upp, (m_ct_offset == 0.0 ? 1.0 : m_ct_offset))
            : m_ct_offset;
        const Vec3d tipw = m_ct_base + m_ct_n * L_sign;

        const SketchPlane saved = m_plane;
        SketchPlane bb; bb.origin = m_ct_base; bb.x_axis = right; bb.y_axis = up;
        bb.normal = cam.get_dir_forward().normalized();
        m_plane = bb;

        const Vec2d tip2((tipw - m_ct_base).dot(right), (tipw - m_ct_base).dot(up));
        if (tip2.norm() > 1e-6) {
            const Vec2d u = tip2.normalized();
            const Vec2d nrm(-u.y(), u.x());
            std::vector<std::pair<Vec2d, Vec2d>> segs;
            segs.emplace_back(Vec2d(0, 0), tip2);
            const double as = std::max(tip2.norm() * 0.20, th * 0.9);
            const Vec2d back = tip2 - u * as;
            segs.emplace_back(tip2, back + nrm * (as * 0.5));
            segs.emplace_back(tip2, back - nrm * (as * 0.5));
            glsafe(::glDisable(GL_DEPTH_TEST));
            draw_strokes(m_ct_stroke_model, segs, std::max(0.7 * upp, 1e-4), teal);
            DimAnnot da; da.kind = DimType::Distance; da.value = m_ct_offset;
            draw_text(m_line_model, dim_text(da), tip2 + u * (th * 1.4), th, teal);
        }
        m_plane = saved;
    }
}

bool DesignSketchTool::hit_test_cut_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    if (!m_ct_active) return false;
    const Linef3 r = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = r.a, rd = r.b - r.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double L_sign = (std::abs(m_ct_offset) < 40.0 * upp)
        ? std::copysign(40.0 * upp, (m_ct_offset == 0.0 ? 1.0 : m_ct_offset))
        : m_ct_offset;
    return ray_segment_dist3(ro, rd, m_ct_base, m_ct_base + m_ct_n * L_sign) <= 12.0 * upp;
}

void DesignSketchTool::start_cut_drag(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    m_ct_drag      = true;
    m_ct_grab_val  = m_ct_offset;
    const double p = hole_axis_proj(canvas, evt, m_ct_base, m_ct_n);
    m_ct_grab_proj = std::isnan(p) ? 0.0 : p;
}

void DesignSketchTool::drag_cut_arrow(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    const double proj = hole_axis_proj(canvas, evt, m_ct_base, m_ct_n);
    if (std::isnan(proj)) return;
    m_ct_offset = m_ct_grab_val + (proj - m_ct_grab_proj);
    if (m_on_cut_offset_changed) m_on_cut_offset_changed(m_ct_offset);
}

void DesignSketchTool::set_revolve_gizmo(const SketchPlane& plane, const Vec2d& centroid,
                                         int axis_sel, double angle, bool flip)
{
    const Vec3d ax = (axis_sel == 1 ? plane.y_axis : plane.x_axis).normalized();
    const Vec3d cw = plane.to_world(centroid);
    const double axial = (cw - plane.origin).dot(ax);
    m_rv_center = plane.origin + axial * ax;     // foot of the centroid on the axis line
    Vec3d ref = cw - m_rv_center;                // perpendicular to ax by construction
    double r = ref.norm();
    if (r < 1e-6) { ref = plane.normal.normalized(); r = std::max(plane.normal.norm(), 1.0); }
    m_rv_axis   = ax;
    m_rv_ref    = ref / ref.norm();
    m_rv_radius = std::max(r, 1.0);
    m_rv_angle  = std::min(360.0, std::max(1.0, std::abs(angle)));
    m_rv_flip   = flip;
    if (!m_rv_active) m_rv_drag = false;         // re-pushed every preview: keep an in-progress drag
    m_rv_active = true;
}

void DesignSketchTool::clear_revolve_gizmo()
{
    m_rv_active = false;
    m_rv_drag   = false;
}

void DesignSketchTool::render_revolve_gizmo()
{
    if (!m_rv_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th  = std::max(15.0 * upp, 1e-4);
    const Vec3d yax  = rv_yaxis(m_rv_axis, m_rv_ref, m_rv_flip);

    // The arc lives in the true revolve plane (NOT billboarded) so the sweep reads in 3D.
    const SketchPlane saved = m_plane;
    SketchPlane rp; rp.origin = m_rv_center; rp.x_axis = m_rv_ref; rp.y_axis = yax; rp.normal = m_rv_axis;
    m_plane = rp;
    // Vivid cyan: the revolve ghost is amber, so the arc manipulator must contrast with it
    // (it overlaps the solid, unlike the extrude depth-arrow which points away into empty space).
    const ColorRGBA arcc(0.15f, 0.92f, 1.0f, 1.0f);
    const double r = m_rv_radius;
    const double a = m_rv_angle * M_PI / 180.0;
    const int    N = std::max(8, int(a / (M_PI / 32.0)));   // ~ every 5.6°

    std::vector<std::pair<Vec2d, Vec2d>> segs;
    Vec2d prev(r, 0.0);
    for (int i = 1; i <= N; ++i) {
        const double t = a * double(i) / double(N);
        const Vec2d cur(r * std::cos(t), r * std::sin(t));
        segs.emplace_back(prev, cur);
        prev = cur;
    }
    const Vec2d tip(r * std::cos(a), r * std::sin(a));
    segs.emplace_back(Vec2d(0, 0), Vec2d(r, 0));   // spoke at angle 0
    segs.emplace_back(Vec2d(0, 0), tip);           // spoke at the swept angle (the handle)
    // Arrowhead at the tip, pointing along the sweep tangent (-sin,cos) rotated by a.
    const Vec2d tang(-std::sin(a), std::cos(a));
    const Vec2d radial = tip.normalized();
    const double as = std::max(r * 0.14, th);
    segs.emplace_back(tip, tip - tang * as - radial * (as * 0.5));
    segs.emplace_back(tip, tip - tang * as + radial * (as * 0.5));
    // A diamond grab-handle at the tip so the draggable target is unmistakable.
    const double hs = std::max(th * 0.8, r * 0.05);
    const Vec2d du = radial * hs, dv = Vec2d(-radial.y(), radial.x()) * hs;
    segs.emplace_back(tip + du, tip + dv);
    segs.emplace_back(tip + dv, tip - du);
    segs.emplace_back(tip - du, tip - dv);
    segs.emplace_back(tip - dv, tip + du);

    glsafe(::glDisable(GL_DEPTH_TEST));
    draw_strokes(m_rv_stroke_model, segs, std::max(0.8 * upp, 1e-4), arcc);
    DimAnnot da; da.kind = DimType::Angle; da.value = m_rv_angle;
    draw_text(m_line_model, dim_text(da), tip * 1.14, th, arcc);
    m_plane = saved;
}

bool DesignSketchTool::hit_test_revolve_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    if (!m_rv_active) return false;
    const Linef3 ray = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = ray.a, rd = ray.b - ray.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const Vec3d yax = rv_yaxis(m_rv_axis, m_rv_ref, m_rv_flip);
    const double a  = m_rv_angle * M_PI / 180.0;
    const double tol = 16.0 * upp;
    // Grab anywhere on the whole gizmo (Onshape-style): the arc curve OR either radial spoke.
    // Take the nearest ray-to-point distance over a dense sampling.
    auto ray_pt = [&](const Vec3d& p) {
        const double t = (p - ro).dot(rd) / std::max(rd.dot(rd), 1e-12);
        return (p - (ro + t * rd)).norm();
    };
    const Vec3d tip = m_rv_center + m_rv_radius * (std::cos(a) * m_rv_ref + std::sin(a) * yax);
    const Vec3d ref = m_rv_center + m_rv_radius * m_rv_ref;   // angle-0 spoke end
    const int N = 48;
    for (int i = 0; i <= N; ++i) {
        const double f = double(i) / double(N);
        const double th = a * f;
        const Vec3d arc = m_rv_center + m_rv_radius * (std::cos(th) * m_rv_ref + std::sin(th) * yax);
        if (ray_pt(arc) <= tol) return true;                 // on the arc curve
        if (ray_pt(m_rv_center + f * (tip - m_rv_center)) <= tol) return true;  // on the swept spoke
        if (ray_pt(m_rv_center + f * (ref - m_rv_center)) <= tol) return true;  // on the ref spoke
    }
    return false;
}

// Intersect the mouse ray with the revolve plane, read its angle around the center -> sweep angle.
void DesignSketchTool::drag_revolve_arc(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    const Linef3 ray = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = ray.a, rd = ray.b - ray.a;
    const double denom = rd.dot(m_rv_axis);
    if (std::abs(denom) < 1e-9) return;            // ray ∥ revolve plane: leave angle as-is
    const double t = (m_rv_center - ro).dot(m_rv_axis) / denom;
    const Vec3d  p = ro + t * rd;
    const Vec3d yax = rv_yaxis(m_rv_axis, m_rv_ref, m_rv_flip);
    const double u = (p - m_rv_center).dot(m_rv_ref);
    const double v = (p - m_rv_center).dot(yax);
    double deg = std::atan2(v, u) * 180.0 / M_PI;
    if (deg < 0.0) deg += 360.0;
    deg = std::min(360.0, std::max(1.0, deg));
    m_rv_angle = deg;
    if (on_revolve_angle_changed) on_revolve_angle_changed(deg);
}

void DesignSketchTool::open_revolve_editor()
{
    if (!on_inline_edit) return;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, m_rv_angle, "",
        [this](double v) {
            m_rv_angle = std::min(360.0, std::max(1.0, v));
            if (on_revolve_angle_changed) on_revolve_angle_changed(m_rv_angle);
        },
        []() {});
}

// ---- Pattern gizmo (linear spacing arrow | circular angle-arc) ------------------------------
// Linear: a 3D arrow along the world march axis with a tick at each copy; the diamond at the end
// drags the spacing. Circular: a Revolve-style arc about the plane normal through the plane origin.
void DesignSketchTool::set_pattern_gizmo(const SketchPlane& plane, const Vec3d& body_centroid,
                                         bool circular, int count, int dir, double spacing, double angle)
{
    m_pt_circular = circular;
    m_pt_base     = body_centroid;
    m_pt_count    = std::max(1, count);
    m_pt_spacing  = std::max(0.01, spacing);
    m_pt_angle    = std::min(360.0, std::max(1.0, angle));
    m_pt_dirw     = (dir == 1 ? plane.y_axis : plane.x_axis).normalized();
    m_pt_origin   = plane.origin;
    m_pt_normal   = plane.normal.normalized();
    // Circular arc center = foot of the body centroid on the rotation axis; ref = perpendicular dir.
    const double axial = (body_centroid - plane.origin).dot(m_pt_normal);
    m_pt_ccenter = plane.origin + axial * m_pt_normal;
    Vec3d ref = body_centroid - m_pt_ccenter;
    double r = ref.norm();
    if (r < 1e-6) { ref = m_pt_dirw; r = 1.0; }   // body centred on the axis: nominal radius
    m_pt_cref   = ref / ref.norm();
    m_pt_radius = std::max(r, 1.0);
    if (!m_pt_active) m_pt_drag = false;          // re-pushed every preview: keep an in-progress drag
    m_pt_active = true;
}

void DesignSketchTool::clear_pattern_gizmo()
{
    m_pt_active = false;
    m_pt_drag   = false;
}

// Linear arrow span = spacing*(count-1), at least one step so count=1 still shows a direction.
static double pt_linear_len(double spacing, int count)
{
    return std::max(spacing * double(std::max(1, count) - 1), spacing);
}

void DesignSketchTool::render_pattern_gizmo()
{
    if (!m_pt_active) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    const double th  = std::max(15.0 * upp, 1e-4);
    const ColorRGBA col(0.15f, 0.92f, 1.0f, 1.0f);   // vivid cyan, matches the gizmo family
    const SketchPlane saved = m_plane;
    std::vector<std::pair<Vec2d, Vec2d>> segs;

    if (!m_pt_circular) {
        // The arrow lives in the true world plane (NOT billboarded) so the march reads in 3D.
        const Vec3d perp = m_pt_normal.cross(m_pt_dirw).normalized();
        SketchPlane fp; fp.origin = m_pt_base; fp.x_axis = m_pt_dirw; fp.y_axis = perp; fp.normal = m_pt_normal;
        m_plane = fp;
        const int copies = std::max(1, m_pt_count);
        const double L = pt_linear_len(m_pt_spacing, copies);
        segs.emplace_back(Vec2d(0, 0), Vec2d(L, 0));                 // shaft
        const double as = std::max(L * 0.12, th);                   // arrowhead
        segs.emplace_back(Vec2d(L, 0), Vec2d(L - as,  as * 0.5));
        segs.emplace_back(Vec2d(L, 0), Vec2d(L - as, -as * 0.5));
        const double tk = std::max(th, L * 0.05);                   // copy tick half-height
        for (int i = 0; i < copies; ++i) {
            const double x = m_pt_spacing * i;
            segs.emplace_back(Vec2d(x, -tk), Vec2d(x, tk));
        }
        const Vec2d E(L, 0);                                        // diamond grab-handle
        const double hs = std::max(th * 0.8, L * 0.04);
        segs.emplace_back(E + Vec2d(hs, 0), E + Vec2d(0, hs));
        segs.emplace_back(E + Vec2d(0, hs), E + Vec2d(-hs, 0));
        segs.emplace_back(E + Vec2d(-hs, 0), E + Vec2d(0, -hs));
        segs.emplace_back(E + Vec2d(0, -hs), E + Vec2d(hs, 0));
        glsafe(::glDisable(GL_DEPTH_TEST));
        draw_strokes(m_pt_stroke_model, segs, std::max(0.8 * upp, 1e-4), col);
        DimAnnot da; da.kind = DimType::Distance; da.value = m_pt_spacing;
        draw_text(m_line_model, dim_text(da), Vec2d(m_pt_spacing * 0.5, tk * 1.7), th, col);
        m_plane = saved;
        return;
    }

    // ---- Circular: clone the Revolve arc about the plane normal through the plane origin ----
    const Vec3d yax = m_pt_normal.cross(m_pt_cref).normalized();
    SketchPlane rp; rp.origin = m_pt_ccenter; rp.x_axis = m_pt_cref; rp.y_axis = yax; rp.normal = m_pt_normal;
    m_plane = rp;
    const double r = m_pt_radius;
    const double a = m_pt_angle * M_PI / 180.0;
    const int    N = std::max(8, int(a / (M_PI / 32.0)));
    Vec2d prev(r, 0.0);
    for (int i = 1; i <= N; ++i) {
        const double t = a * double(i) / double(N);
        const Vec2d cur(r * std::cos(t), r * std::sin(t));
        segs.emplace_back(prev, cur);
        prev = cur;
    }
    const Vec2d tip(r * std::cos(a), r * std::sin(a));
    segs.emplace_back(Vec2d(0, 0), Vec2d(r, 0));   // angle-0 spoke
    segs.emplace_back(Vec2d(0, 0), tip);           // swept spoke (the handle)
    const Vec2d tang(-std::sin(a), std::cos(a));
    const Vec2d radial = tip.normalized();
    const double as = std::max(r * 0.14, th);
    segs.emplace_back(tip, tip - tang * as - radial * (as * 0.5));
    segs.emplace_back(tip, tip - tang * as + radial * (as * 0.5));
    const double hs = std::max(th * 0.8, r * 0.05);
    const Vec2d du = radial * hs, dv = Vec2d(-radial.y(), radial.x()) * hs;
    segs.emplace_back(tip + du, tip + dv);
    segs.emplace_back(tip + dv, tip - du);
    segs.emplace_back(tip - du, tip - dv);
    segs.emplace_back(tip - dv, tip + du);
    glsafe(::glDisable(GL_DEPTH_TEST));
    draw_strokes(m_pt_stroke_model, segs, std::max(0.8 * upp, 1e-4), col);
    DimAnnot da; da.kind = DimType::Angle; da.value = m_pt_angle;
    draw_text(m_line_model, dim_text(da), tip * 1.14, th, col);
    m_plane = saved;
}

bool DesignSketchTool::hit_test_pattern_handle(GLCanvas3D& canvas, const wxMouseEvent& evt) const
{
    if (!m_pt_active) return false;
    const Linef3 ray = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = ray.a, rd = ray.b - ray.a;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double upp = 1.0 / std::max(cam.get_zoom(), 1e-6);
    auto ray_pt = [&](const Vec3d& p) {
        const double t = (p - ro).dot(rd) / std::max(rd.dot(rd), 1e-12);
        return (p - (ro + t * rd)).norm();
    };
    if (!m_pt_circular) {
        const double tol = 8.0 * upp;
        const double L = pt_linear_len(m_pt_spacing, m_pt_count);
        return ray_segment_dist3(ro, rd, m_pt_base, m_pt_base + m_pt_dirw * L) <= tol;
    }
    const double tol = 16.0 * upp;
    const Vec3d yax = m_pt_normal.cross(m_pt_cref).normalized();
    const double a  = m_pt_angle * M_PI / 180.0;
    const Vec3d tip = m_pt_ccenter + m_pt_radius * (std::cos(a) * m_pt_cref + std::sin(a) * yax);
    const Vec3d ref = m_pt_ccenter + m_pt_radius * m_pt_cref;
    const int N = 48;
    for (int i = 0; i <= N; ++i) {
        const double f = double(i) / double(N);
        const Vec3d arc = m_pt_ccenter + m_pt_radius * (std::cos(a * f) * m_pt_cref + std::sin(a * f) * yax);
        if (ray_pt(arc) <= tol) return true;
        if (ray_pt(m_pt_ccenter + f * (tip - m_pt_ccenter)) <= tol) return true;
        if (ray_pt(m_pt_ccenter + f * (ref - m_pt_ccenter)) <= tol) return true;
    }
    return false;
}

void DesignSketchTool::drag_pattern_handle(GLCanvas3D& canvas, const wxMouseEvent& evt)
{
    const Linef3 ray = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
    const Vec3d ro = ray.a, rd = ray.b - ray.a;
    if (!m_pt_circular) {
        // Skew-line closest point of the mouse ray to the march axis -> span -> spacing.
        const Vec3d e = m_pt_dirw;
        const Vec3d w0 = m_pt_base - ro;
        const double a = e.dot(e), b = e.dot(rd), c = rd.dot(rd), dd = e.dot(w0), ee = rd.dot(w0);
        const double denom = a * c - b * b;
        if (std::abs(denom) < 1e-7) return;            // camera ∥ axis: leave spacing as-is
        const double span = std::max(0.01, (b * ee - c * dd) / denom);
        const int div = std::max(1, m_pt_count - 1);
        m_pt_spacing = std::max(0.1, span / double(div));
        if (on_pattern_changed) on_pattern_changed(m_pt_spacing);
        return;
    }
    const double denom = rd.dot(m_pt_normal);
    if (std::abs(denom) < 1e-9) return;                // ray ∥ rotation plane: leave angle as-is
    const double t = (m_pt_ccenter - ro).dot(m_pt_normal) / denom;
    const Vec3d  p = ro + t * rd;
    const Vec3d yax = m_pt_normal.cross(m_pt_cref).normalized();
    double deg = std::atan2((p - m_pt_ccenter).dot(yax), (p - m_pt_ccenter).dot(m_pt_cref)) * 180.0 / M_PI;
    if (deg < 0.0) deg += 360.0;
    m_pt_angle = std::min(360.0, std::max(1.0, deg));
    if (on_pattern_changed) on_pattern_changed(m_pt_angle);
}

void DesignSketchTool::open_pattern_editor()
{
    if (!on_inline_edit) return;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    const double cur = m_pt_circular ? m_pt_angle : m_pt_spacing;
    on_inline_edit(px, cur, "",
        [this](double v) {
            if (m_pt_circular) m_pt_angle = std::min(360.0, std::max(1.0, v));
            else               m_pt_spacing = std::max(0.1, v);
            if (on_pattern_changed) on_pattern_changed(m_pt_circular ? m_pt_angle : m_pt_spacing);
        },
        []() {});
}

// Closed loops + the entity indices that form each one. A circle/ellipse is its own loop;
// line/arc chains are walked endpoint-to-endpoint. Entity membership lets a single loop be
// highlighted and extruded on its own.
std::vector<DesignSketchTool::RegionLoop>
DesignSketchTool::region_loops(const std::vector<SketchEntity>& ents) const
{
    std::vector<RegionLoop> regions;
    const double eps2 = 1e-3 * 1e-3;
    auto is_near = [&](const Vec2d& a, const Vec2d& b) { return (a - b).squaredNorm() < eps2; };

    // Circles are self-closed regions; lines/arcs are open segments to be chained. Each
    // Seg remembers the entity index it came from.
    struct Seg { std::vector<Vec2d> pts; int ent{-1}; bool used{false}; };
    std::vector<Seg> segs;
    for (int i = 0; i < int(ents.size()); ++i) {
        const SketchEntity& e = ents[i];
        if (e.construction) continue;
        bool closed = false;
        if (e.type == SketchEntity::Type::Circle || e.type == SketchEntity::Type::Ellipse) {
            regions.push_back({ entity_polyline(e, closed), { i } });
        } else if (e.type == SketchEntity::Type::Line || e.type == SketchEntity::Type::Arc
                   || e.type == SketchEntity::Type::EllipseArc
                   || e.type == SketchEntity::Type::BSpline) {
            std::vector<Vec2d> p = entity_polyline(e, closed);
            if (p.size() >= 2) segs.push_back({ std::move(p), i, false });
        }
    }

    // Walk each unused segment endpoint-to-endpoint until the chain returns to its
    // start (a closed loop) or stalls (an open chain, discarded).
    for (size_t s = 0; s < segs.size(); ++s) {
        if (segs[s].used) continue;
        segs[s].used = true;
        std::vector<Vec2d> loop = segs[s].pts;
        std::vector<int>   loop_ents = { segs[s].ent };
        const Vec2d start = loop.front();
        Vec2d cur = loop.back();
        bool extended = true;
        while (extended && !is_near(cur, start)) {
            extended = false;
            for (size_t t = 0; t < segs.size(); ++t) {
                if (segs[t].used) continue;
                const std::vector<Vec2d>& q = segs[t].pts;
                if (is_near(q.front(), cur)) {
                    for (size_t k = 1; k < q.size(); ++k) loop.push_back(q[k]);
                    cur = q.back();
                } else if (is_near(q.back(), cur)) {
                    for (int k = int(q.size()) - 2; k >= 0; --k) loop.push_back(q[k]);
                    cur = q.front();
                } else {
                    continue;
                }
                segs[t].used = true;
                loop_ents.push_back(segs[t].ent);
                extended = true;
                break;
            }
        }
        if (is_near(cur, start) && loop.size() >= 4) {
            loop.pop_back();          // drop the duplicate closing vertex
            regions.push_back({ std::move(loop), std::move(loop_ents), {} });
        }
    }

    // NESTING. A loop drawn inside another one is that one's HOLE. Without this a sketch is
    // just N disjoint filled polygons, so "the plate with the hole" is not expressible and the
    // multi-loop kernel path (snaporca-88v) is unreachable from the viewport — which is exactly
    // what Tommaso hit: a rectangle with a circle inside extruded to a plain box, because only
    // the rectangle loop could be picked and only its entities were passed on.
    //
    // Loops in a well-formed sketch do not cross, so testing ONE vertex decides containment.
    // Each loop is assigned to the SMALLEST loop that contains it, which is what makes a hole
    // belong to the region that actually bounds it rather than to every enclosing loop.
    auto poly_area = [](const std::vector<Vec2d>& q) {
        double a2 = 0.0;
        for (size_t i = 0, j = q.size() - 1; i < q.size(); j = i++)
            a2 += (q[j].x() + q[i].x()) * (q[j].y() - q[i].y());
        return std::abs(a2) * 0.5;
    };
    auto point_in = [](const Vec2d& pt, const std::vector<Vec2d>& q) {
        bool in = false;
        for (size_t i = 0, j = q.size() - 1; i < q.size(); j = i++) {
            const Vec2d& A = q[i]; const Vec2d& B = q[j];
            if (((A.y() > pt.y()) != (B.y() > pt.y())) &&
                (pt.x() < (B.x() - A.x()) * (pt.y() - A.y()) / (B.y() - A.y()) + A.x()))
                in = !in;
        }
        return in;
    };
    for (size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].poly.empty()) continue;
        int    best = -1;
        double best_area = 0.0;
        for (size_t j = 0; j < regions.size(); ++j) {
            if (i == j || regions[j].poly.size() < 3) continue;
            if (!point_in(regions[i].poly.front(), regions[j].poly)) continue;
            const double a2 = poly_area(regions[j].poly);
            if (best < 0 || a2 < best_area) { best = int(j); best_area = a2; }
        }
        if (best >= 0) regions[best].holes.push_back(int(i));
    }
    return regions;
}

int DesignSketchTool::region_at(const Vec2d& p) const
{
    // Walk region_loops (which already knows about holes) rather than the raw polygons: the
    // returned index must stay meaningful after the sketch is committed, so it has to use the
    // SAME numbering selected_loop_entities() and hit_display_sketch consume.
    const std::vector<RegionLoop> regions = region_loops(m_entities);
    auto inside = [](const Vec2d& q, const std::vector<Vec2d>& poly) {
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const Vec2d& a = poly[i];
            const Vec2d& b = poly[j];
            if (((a.y() > q.y()) != (b.y() > q.y())) &&
                (q.x() < (b.x() - a.x()) * (q.y() - a.y()) / (b.y() - a.y()) + a.x()))
                in = !in;
        }
        return in;
    };
    // Shoelace area (absolute): the SMALLEST loop containing p is the innermost one, which is
    // the region that actually owns the point — a bore inside a plate resolves to the disc, and
    // a click on the plate material resolves to the plate even though the disc is also inside it.
    auto poly_area = [](const std::vector<Vec2d>& q) {
        double a2 = 0.0;
        for (size_t i = 0, j = q.size() - 1; i < q.size(); j = i++)
            a2 += (q[j].x() + q[i].x()) * (q[j].y() - q[i].y());
        return std::abs(a2) * 0.5;
    };
    int    best = -1;
    double best_area = 0.0;
    for (size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].poly.size() < 3) continue;
        if (!inside(p, regions[i].poly)) continue;
        bool in_hole = false;   // p inside one of this region's holes → that hole owns it, not us
        for (int h : regions[i].holes) {
            if (h < 0 || h >= int(regions.size()) || regions[h].poly.size() < 3) continue;
            if (inside(p, regions[h].poly)) { in_hole = true; break; }
        }
        if (in_hole) continue;
        const double a = poly_area(regions[i].poly);
        if (best < 0 || a < best_area) { best = int(i); best_area = a; }
    }
    return best;
}

// ---- rendering --------------------------------------------------------------

void DesignSketchTool::draw_quad_strip(GLModel& model, const std::vector<Vec2d>& pts, bool closed, const ColorRGBA& color)
{
    if (pts.size() < 2)
        return;

    const double hw = 0.6;
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    unsigned int base = 0;

    const size_t segs = closed ? pts.size() : pts.size() - 1;
    for (size_t i = 0; i < segs; ++i) {
        const Vec2d a = pts[i];
        const Vec2d b = pts[(i + 1) % pts.size()];
        const Vec2d d = b - a;
        const double len = d.norm();
        if (len < 1e-6)
            continue;
        const Vec2d n(-d.y() / len, d.x() / len);
        const Vec2d o = n * hw;
        g.add_vertex((Vec3f)m_plane.to_world(a + o).cast<float>());
        g.add_vertex((Vec3f)m_plane.to_world(b + o).cast<float>());
        g.add_vertex((Vec3f)m_plane.to_world(b - o).cast<float>());
        g.add_vertex((Vec3f)m_plane.to_world(a - o).cast<float>());
        g.add_triangle(base, base + 1, base + 2);
        g.add_triangle(base, base + 2, base + 3);
        base += 4;
    }

    if (base > 0) {
        model.reset();
        model.init_from(std::move(g));
        model.set_color(color);
        model.render();
    }
}

namespace {
double poly_signed_area(const std::vector<Vec2d>& p)
{
    double a = 0.0;
    for (size_t i = 0, n = p.size(); i < n; ++i) {
        const Vec2d& u = p[i];
        const Vec2d& v = p[(i + 1) % n];
        a += u.x() * v.y() - v.x() * u.y();
    }
    return 0.5 * a;
}
bool pt_in_tri(const Vec2d& p, const Vec2d& a, const Vec2d& b, const Vec2d& c)
{
    auto cross = [](const Vec2d& u, const Vec2d& v, const Vec2d& w) {
        return (v.x() - u.x()) * (w.y() - u.y()) - (v.y() - u.y()) * (w.x() - u.x());
    };
    const double d1 = cross(a, b, p), d2 = cross(b, c, p), d3 = cross(c, a, p);
    const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);   // inside iff all cross products share a sign
}
// Ear-clipping triangulation of a simple polygon; returns index triples into `poly`.
std::vector<std::array<unsigned, 3>> ear_clip(const std::vector<Vec2d>& poly)
{
    std::vector<std::array<unsigned, 3>> tris;
    const size_t n = poly.size();
    if (n < 3) return tris;
    std::vector<unsigned> idx(n);
    for (unsigned i = 0; i < n; ++i) idx[i] = i;
    if (poly_signed_area(poly) < 0.0) std::reverse(idx.begin(), idx.end());   // work CCW
    int guard = 0;
    while (idx.size() > 3 && guard++ < int(10 * n)) {
        bool clipped = false;
        const int m = int(idx.size());
        for (int i = 0; i < m; ++i) {
            const unsigned i0 = idx[(i + m - 1) % m];
            const unsigned i1 = idx[i];
            const unsigned i2 = idx[(i + 1) % m];
            const Vec2d& a = poly[i0]; const Vec2d& b = poly[i1]; const Vec2d& c = poly[i2];
            const double cr = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
            if (cr <= 0.0) continue;   // reflex vertex, not an ear tip
            bool ear = true;
            for (int j = 0; j < m; ++j) {
                const unsigned ij = idx[j];
                if (ij == i0 || ij == i1 || ij == i2) continue;
                if (pt_in_tri(poly[ij], a, b, c)) { ear = false; break; }
            }
            if (!ear) continue;
            tris.push_back({ i0, i1, i2 });
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) break;   // degenerate input: bail rather than spin
    }
    if (idx.size() == 3) tris.push_back({ idx[0], idx[1], idx[2] });
    return tris;
}
} // namespace

// Fill a region that has holes with an EVEN-ODD SCANLINE fill: each horizontal band is scanned
// across every contour, the crossings sorted, and the spans between alternate crossings emitted
// as quads. A hole is just two more crossings, so any number of holes and any concavity fall out
// of the parity rule — no bridge and no triangulation to fail.
void DesignSketchTool::draw_fill_holed(GLModel& model, const std::vector<Vec2d>& outer,
                                       const std::vector<std::vector<Vec2d>>& holes,
                                       const ColorRGBA& color)
{
    if (outer.size() < 3) return;
    if (holes.empty()) { draw_fill(model, outer, color); return; }

    // EVEN-ODD SCANLINE, not a triangulation. The previous version spliced each hole into the
    // outer contour through a keyhole corridor and ear-clipped the result; the corridor did not
    // collapse to zero width and was drawn as a visible triangle running from the bore to the
    // nearest rectangle corner. Rather than tune a bridge that can always find a new shape to
    // fail on, this fills the region the way a rasteriser would: for each horizontal band, cross
    // EVERY contour, sort the crossings, and fill between alternate pairs. A hole is simply two
    // more crossings, so any number of holes and any concavity fall out of the same rule and no
    // corridor exists to leak.
    std::vector<const std::vector<Vec2d>*> contours;
    contours.push_back(&outer);
    for (const auto& h : holes) if (h.size() >= 3) contours.push_back(&h);

    double ymin = outer[0].y(), ymax = ymin, xmin = outer[0].x(), xmax = xmin;
    for (const auto* c : contours)
        for (const Vec2d& v : *c) {
            ymin = std::min(ymin, v.y()); ymax = std::max(ymax, v.y());
            xmin = std::min(xmin, v.x()); xmax = std::max(xmax, v.x());
        }
    if (ymax - ymin < 1e-9) return;

    // Enough bands that the step is far below a pixel at any sane zoom, cheap enough to rebuild
    // every frame: this is a translucent highlight, not geometry.
    const int    ROWS = 256;
    const double dy   = (ymax - ymin) / ROWS;

    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    unsigned base = 0;
    std::vector<double> xs;
    for (int row = 0; row < ROWS; ++row) {
        const double y0 = ymin + dy * row, y1 = y0 + dy, ym = 0.5 * (y0 + y1);
        xs.clear();
        for (const auto* c : contours) {
            const std::vector<Vec2d>& q = *c;
            for (size_t i = 0, j = q.size() - 1; i < q.size(); j = i++) {
                const Vec2d& A = q[i]; const Vec2d& B = q[j];
                if ((A.y() > ym) == (B.y() > ym)) continue;          // edge does not cross this row
                xs.push_back(A.x() + (ym - A.y()) * (B.x() - A.x()) / (B.y() - A.y()));
            }
        }
        if (xs.size() < 2) continue;
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {              // even-odd: fill alternate spans
            const double xa = xs[k], xb = xs[k + 1];
            if (xb - xa < 1e-9) continue;
            g.add_vertex((Vec3f)m_plane.to_world(Vec2d(xa, y0)).cast<float>());
            g.add_vertex((Vec3f)m_plane.to_world(Vec2d(xb, y0)).cast<float>());
            g.add_vertex((Vec3f)m_plane.to_world(Vec2d(xb, y1)).cast<float>());
            g.add_vertex((Vec3f)m_plane.to_world(Vec2d(xa, y1)).cast<float>());
            g.add_triangle(base, base + 1, base + 2);
            g.add_triangle(base, base + 2, base + 3);
            base += 4;
        }
    }
    if (base == 0) return;
    model.reset();
    model.init_from(std::move(g));
    model.set_color(color);
    model.render();
}

void DesignSketchTool::draw_fill(GLModel& model, const std::vector<Vec2d>& poly, const ColorRGBA& color)
{
    if (poly.size() < 3) return;
    const auto tris = ear_clip(poly);
    if (tris.empty()) return;
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    for (const Vec2d& p : poly)
        g.add_vertex((Vec3f)m_plane.to_world(p).cast<float>());
    for (const auto& t : tris)
        g.add_triangle(t[0], t[1], t[2]);
    model.reset();
    model.init_from(std::move(g));
    model.set_color(color);
    model.render();
}

void DesignSketchTool::draw_vertices(GLModel& model, const std::vector<Vec2d>& pts, const ColorRGBA& color,
                                     double half_size)
{
    if (pts.empty())
        return;

    const double hs = half_size;
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    unsigned int base = 0;
    for (const Vec2d& p : pts) {
        g.add_vertex((Vec3f)m_plane.to_world(p + Vec2d(-hs, -hs)).cast<float>());
        g.add_vertex((Vec3f)m_plane.to_world(p + Vec2d( hs, -hs)).cast<float>());
        g.add_vertex((Vec3f)m_plane.to_world(p + Vec2d( hs,  hs)).cast<float>());
        g.add_vertex((Vec3f)m_plane.to_world(p + Vec2d(-hs,  hs)).cast<float>());
        g.add_triangle(base, base + 1, base + 2);
        g.add_triangle(base, base + 2, base + 3);
        base += 4;
    }

    model.reset();
    model.init_from(std::move(g));
    model.set_color(color);
    model.render();
}

// Independent thick-line segments batched into one immediate-mode draw (quote lines,
// extension lines, arrowheads, glyph strokes). Mirrors draw_quad_strip's lift-to-world.
void DesignSketchTool::draw_strokes(GLModel& model, const std::vector<std::pair<Vec2d, Vec2d>>& segs,
                                    double hw, const ColorRGBA& color)
{
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    unsigned int base = 0;
    for (const auto& s : segs) {
        const Vec2d a = s.first, b = s.second;
        const Vec2d d = b - a;
        const double len = d.norm();
        if (len < 1e-6) continue;
        const Vec2d n(-d.y() / len, d.x() / len);
        const Vec2d o = n * hw;
        g.add_vertex((Vec3f)m_plane.to_world(a + o).cast<float>());
        g.add_vertex((Vec3f)m_plane.to_world(b + o).cast<float>());
        g.add_vertex((Vec3f)m_plane.to_world(b - o).cast<float>());
        g.add_vertex((Vec3f)m_plane.to_world(a - o).cast<float>());
        g.add_triangle(base, base + 1, base + 2);
        g.add_triangle(base, base + 2, base + 3);
        base += 4;
    }
    if (base > 0) {
        model.reset();
        model.init_from(std::move(g));
        model.set_color(color);
        model.render();
    }
}

namespace {
// Smooth single-stroke (Hershey-style) vector font for dimension labels. Glyphs
// live in a 0..0.6 (x) by 0..1 (y) cell, baseline at y=0, cap height y=1; curved
// digits are sampled as short segments so they read as rounded shapes, not blocks.
// `advance` is the pen step after the glyph.
constexpr double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * kPi / 180.0; }

// Connect a list of points as a polyline.
void poly(std::vector<std::pair<Vec2d, Vec2d>>& out, std::initializer_list<Vec2d> p)
{
    auto it = p.begin();
    if (it == p.end()) return;
    Vec2d prev = *it++;
    for (; it != p.end(); ++it) { out.emplace_back(prev, *it); prev = *it; }
}
// Sample an elliptical arc (centre cx,cy; radii rx,ry) from angle a0..a1.
void arc(std::vector<std::pair<Vec2d, Vec2d>>& out, double cx, double cy, double rx, double ry,
         double a0, double a1, int n = 14)
{
    Vec2d prev(cx + rx * std::cos(a0), cy + ry * std::sin(a0));
    for (int i = 1; i <= n; ++i) {
        const double t = a0 + (a1 - a0) * (double)i / n;
        const Vec2d cur(cx + rx * std::cos(t), cy + ry * std::sin(t));
        out.emplace_back(prev, cur);
        prev = cur;
    }
}

void glyph_strokes(char c, std::vector<std::pair<Vec2d, Vec2d>>& out, double& advance)
{
    advance = 0.72;
    switch (c) {
    case '0':
        arc(out, 0.30, 0.50, 0.25, 0.48, 0.0, 2.0 * kPi);
        break;
    case '1':
        poly(out, {Vec2d(0.13, 0.76), Vec2d(0.33, 1.0), Vec2d(0.33, 0.0)});
        poly(out, {Vec2d(0.13, 0.0), Vec2d(0.53, 0.0)});
        advance = 0.52;
        break;
    case '2':
        arc(out, 0.30, 0.72, 0.25, 0.25, rad(170), rad(-45));
        poly(out, {Vec2d(0.477, 0.543), Vec2d(0.06, 0.0), Vec2d(0.56, 0.0)});
        break;
    case '3':
        arc(out, 0.30, 0.74, 0.24, 0.24, rad(160), rad(-90));
        arc(out, 0.30, 0.26, 0.26, 0.26, rad(90), rad(-160));
        break;
    case '4':
        poly(out, {Vec2d(0.42, 1.0), Vec2d(0.04, 0.32), Vec2d(0.58, 0.32)});
        poly(out, {Vec2d(0.42, 1.0), Vec2d(0.42, 0.0)});
        break;
    case '5':
        poly(out, {Vec2d(0.54, 1.0), Vec2d(0.12, 1.0), Vec2d(0.11, 0.52)});
        arc(out, 0.27, 0.30, 0.27, 0.27, rad(130), rad(-120));
        break;
    case '6':
        arc(out, 0.30, 0.28, 0.26, 0.26, 0.0, 2.0 * kPi);
        arc(out, 0.30, 0.55, 0.30, 0.45, rad(90), rad(190));
        break;
    case '7':
        poly(out, {Vec2d(0.05, 1.0), Vec2d(0.57, 1.0), Vec2d(0.22, 0.0)});
        break;
    case '8':
        arc(out, 0.30, 0.73, 0.22, 0.25, 0.0, 2.0 * kPi);
        arc(out, 0.30, 0.26, 0.26, 0.26, 0.0, 2.0 * kPi);
        break;
    case '9':
        arc(out, 0.30, 0.70, 0.26, 0.26, 0.0, 2.0 * kPi);
        arc(out, 0.28, 0.55, 0.28, 0.55, rad(15), rad(-90));
        break;
    case '.':
    case ',':  // locale (LC_NUMERIC) may format the decimal separator as a comma
        // small solid dot: crossed short strokes so the quads fill a visible disk
        poly(out, {Vec2d(0.10, 0.08), Vec2d(0.24, 0.08)});
        poly(out, {Vec2d(0.17, 0.02), Vec2d(0.17, 0.15)});
        advance = 0.30;
        break;
    case '-':
        poly(out, {Vec2d(0.10, 0.5), Vec2d(0.50, 0.5)});
        advance = 0.62;
        break;
    case 'R':
        poly(out, {Vec2d(0.08, 0.0), Vec2d(0.08, 1.0), Vec2d(0.38, 1.0)});
        arc(out, 0.38, 0.75, 0.17, 0.25, rad(90), rad(-90));
        poly(out, {Vec2d(0.38, 0.50), Vec2d(0.08, 0.50)});
        poly(out, {Vec2d(0.30, 0.50), Vec2d(0.58, 0.0)});
        advance = 0.80;
        break;
    case 'X':
        poly(out, {Vec2d(0.06, 1.0), Vec2d(0.58, 0.0)});
        poly(out, {Vec2d(0.58, 1.0), Vec2d(0.06, 0.0)});
        advance = 0.72;
        break;
    case 'Y':
        poly(out, {Vec2d(0.06, 1.0), Vec2d(0.32, 0.52)});
        poly(out, {Vec2d(0.58, 1.0), Vec2d(0.32, 0.52)});
        poly(out, {Vec2d(0.32, 0.52), Vec2d(0.32, 0.0)});
        advance = 0.72;
        break;
    case 'Z':
        poly(out, {Vec2d(0.06, 1.0), Vec2d(0.58, 1.0), Vec2d(0.06, 0.0), Vec2d(0.58, 0.0)});
        advance = 0.72;
        break;
    case ' ':
        advance = 0.5;
        break;
    default:
        advance = 0.5;
        break;
    }
}
} // namespace

void DesignSketchTool::draw_dim_label(const std::string& txt, const Vec2d& plane_center)
{
    if (txt.empty()) return;
    const Camera& cam = wxGetApp().plater()->get_camera();
    const wxPoint sp = world_to_screen_px(cam, m_plane.to_world(plane_center));
    if (sp.x < 0 && sp.y < 0) return;
    ImGuiWrapper* imgui = wxGetApp().imgui();
    // Identical to the Prepare/Preview Measure gizmo label (GLGizmoMeasure::render_dimensioning):
    // push_common_window_style sets the white text colour + font/scale (without it the text is
    // invisible); BringWindowToDisplayFront keeps the per-frame label window on top.
    ImGuiWrapper::push_common_window_style(m_render_scale);
    imgui->set_next_window_pos((float)sp.x, (float)sp.y, ImGuiCond_Always, 0.5f, 0.5f);
    imgui->set_next_window_bg_alpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.0f, 1.0f));
    const std::string win = "##sketchdim" + std::to_string(m_dim_label_seq++);
    imgui->begin(win, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration
                       | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing
                       | ImGuiWindowFlags_NoNav);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::AlignTextToFramePadding();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 ts  = ImGui::CalcTextSize(txt.c_str());
    const ImGuiStyle& st = ImGui::GetStyle();
    dl->AddRectFilled(ImVec2(pos.x - st.FramePadding.x, pos.y + st.FramePadding.y),
                      ImVec2(pos.x + ts.x + 2.0f * st.FramePadding.x,
                             pos.y + ts.y + 2.0f * st.FramePadding.y),
                      ImGuiWrapper::to_ImU32(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f)));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + st.FramePadding.x, pos.y));
    imgui->text(txt);
    imgui->end();
    ImGui::PopStyleVar(3);
    ImGuiWrapper::pop_common_window_style();
}

void DesignSketchTool::draw_text(GLModel& /*model*/, const std::string& s, const Vec2d& center,
                                 double /*height*/, const ColorRGBA& /*color*/)
{
    // ponytail: all sketch labels now render as Measure-gizmo-style ImGui labels for visual
    // parity with the Prepare/Preview tabs; the old vector-font path (glyph_strokes/draw_strokes
    // for text) is retired. Leader lines/arrows still draw via draw_strokes at the call sites.
    draw_dim_label(s, center);
}

// Draw every placed dimension: extension lines, the offset dimension line, arrowheads
// and the numeric label. Geometry is recomputed from the (solved) entities each frame
// so the quote tracks the sketch.
// Draw one dimension's quote (extension/dimension lines, arrowheads, numeric label).
// Geometry is recomputed from the (solved) entities so the quote tracks the sketch.
// Returns the label centre in out_label; false if the annot references missing/degenerate
// geometry. Shared by placed (render_dimensions) and live (render_live_quotes) quotes.
bool DesignSketchTool::draw_dim_quote(const DimAnnot& a, double th, const ColorRGBA& dimcol,
                                      Vec2d& out_label)
{
    std::vector<std::pair<Vec2d, Vec2d>> segs;
    if (a.kind == DimType::Length || a.kind == DimType::Distance) {
        Vec2d pa, pb;
        if (a.kind == DimType::Length) {
            if (a.ea < 0 || a.ea >= int(m_entities.size())) return false;
            pa = m_entities[a.ea].p0; pb = m_entities[a.ea].p1;
        } else if (!point_at(a.ea, a.ra, pa) || !point_at(a.eb, a.rb, pb)) {
            return false;
        }
        const Vec2d d = pb - pa;
        const double L = d.norm();
        if (L < 1e-6) return false;
        const Vec2d u = d / L;
        const Vec2d nrm(-u.y(), u.x());
        const double side = (a.side != 0.0) ? a.side : 1.0;
        const double off  = side * std::max(L * 0.18, 8.0);
        const Vec2d A2 = pa + nrm * off, B2 = pb + nrm * off;   // offset clear of the sketch line
        segs.emplace_back(A2, B2);                              // dimension line (not on the geometry)
        const double as = std::max(L * 0.04, 2.0);
        auto arrow = [&](const Vec2d& tip, const Vec2d& dir) {
            const Vec2d back = tip + dir * as;
            segs.emplace_back(tip, back + nrm * (as * 0.5));
            segs.emplace_back(tip, back - nrm * (as * 0.5));
        };
        arrow(A2, u); arrow(B2, -u);
        out_label = (A2 + B2) * 0.5 + nrm * (side * (th * 0.7 + 1.5));
    } else if (a.kind == DimType::Diameter || a.kind == DimType::Radius) {
        if (a.ea < 0 || a.ea >= int(m_entities.size())) return false;
        const SketchEntity& e = m_entities[a.ea];
        const Vec2d c = e.center;
        const double r = e.radius;
        if (r < 1e-6) return false;
        const Vec2d u(1.0, 0.0);
        const double as = std::max(r * 0.12, 2.0);
        if (a.kind == DimType::Diameter) {
            const Vec2d p1 = c - u * r, p2 = c + u * r;
            segs.emplace_back(p1, p2);
            segs.emplace_back(p1, p1 + u * as + Vec2d(0, 1) * (as * 0.5));
            segs.emplace_back(p1, p1 + u * as - Vec2d(0, 1) * (as * 0.5));
            segs.emplace_back(p2, p2 - u * as + Vec2d(0, 1) * (as * 0.5));
            segs.emplace_back(p2, p2 - u * as - Vec2d(0, 1) * (as * 0.5));
            out_label = c + Vec2d(0, 1) * (th * 0.8);
        } else {
            const Vec2d p2 = c + u * r;
            segs.emplace_back(c, p2);
            segs.emplace_back(p2, p2 - u * as + Vec2d(0, 1) * (as * 0.5));
            segs.emplace_back(p2, p2 - u * as - Vec2d(0, 1) * (as * 0.5));
            out_label = (c + p2) * 0.5 + Vec2d(0, 1) * (th * 0.8);
        }
    } else if (a.kind == DimType::DistanceToLine) {
        Vec2d pa;
        if (!point_at(a.ea, a.ra, pa) || a.eb < 0 || a.eb >= int(m_entities.size())) return false;
        const SketchEntity& Ln = m_entities[a.eb];
        const Vec2d ld = Ln.p1 - Ln.p0;
        const double n = ld.norm();
        if (n < 1e-9) return false;
        const Vec2d u = ld / n;
        const double t = (pa - Ln.p0).dot(u);
        const Vec2d foot = Ln.p0 + u * t;       // perpendicular foot on the line
        segs.emplace_back(pa, foot);
        out_label = (pa + foot) * 0.5 + u * (th * 0.7 + 1.5);
    } else if (a.kind == DimType::Angle) {
        if (a.ea < 0 || a.ea >= int(m_entities.size())) return false;
        const SketchEntity& e = m_entities[a.ea];
        if (e.type != SketchEntity::Type::Line) return false;
        const Vec2d d = e.p1 - e.p0;
        const double L = d.norm();
        if (L < 1e-6) return false;
        double ang = std::atan2(d.y(), d.x());            // signed, matches measure_dim sweep
        const double rr = std::max(std::min(L * 0.35, 40.0), th * 1.6);  // arc radius
        segs.emplace_back(e.p0, e.p0 + Vec2d(rr * 1.15, 0.0));  // horizontal reference leg
        const int N = 20;                                  // arc 0 -> ang about p0
        Vec2d prev = e.p0 + Vec2d(rr, 0.0);
        for (int i = 1; i <= N; ++i) {
            const double t = ang * double(i) / N;
            const Vec2d cur = e.p0 + Vec2d(rr * std::cos(t), rr * std::sin(t));
            segs.emplace_back(prev, cur); prev = cur;
        }
        const double mid = ang * 0.5;
        out_label = e.p0 + Vec2d(std::cos(mid), std::sin(mid)) * (rr + th * 1.1);
    } else {
        return false;
    }
    draw_strokes(m_highlight_model, segs, 0.2, dimcol);
    draw_text(m_line_model, dim_text(a), out_label, th, dimcol);
    return true;
}

// Draw every placed (driving) dimension; cache each label centre for picking.
void DesignSketchTool::render_dimensions(double unit_per_px)
{
    if (m_dimensions.empty()) return;
    const ColorRGBA dimcol(0.85f, 0.85f, 0.85f, 1.0f);   // neutral leader (Measure parity)
    // Label text is a CONSTANT screen size (like real CAD), not scaled to geometry,
    // so a long line doesn't get huge text. ~15 px tall in plane units at this zoom.
    const double th = std::max(15.0 * unit_per_px, 1e-4);
    for (size_t di = 0; di < m_dimensions.size(); ++di) {
        Vec2d label;
        if (draw_dim_quote(m_dimensions[di], th, dimcol, label))
            m_dimensions[di].label_pos = label;
    }
}

// Per-entity-type characteristic dimensions, drawn as live non-driving quotes for the
// entity being edited (point/handle drag, or a lone selection). This is the Onshape
// pattern that scales to every tool: each kind reports its defining dimension(s); each
// is clickable (m_live_quotes) to promote to a driving dim + open the inline editor.
// A dim already driven on the entity is skipped (render_dimensions draws that one).
void DesignSketchTool::render_live_quotes(double unit_per_px)
{
    m_live_quotes.clear();
    m_live_poly_fi = -1;
    m_live_poly_side_label = m_live_poly_angle_label = Vec2d(1e18, 1e18);
    m_live_arc_ei = -1;
    m_live_arc_angle_label = Vec2d(1e18, 1e18);
    m_live_ellipse_ei = -1;
    m_live_ellipse_major_label = m_live_ellipse_minor_label = Vec2d(1e18, 1e18);
    m_live_ellipsearc_sweep_label = Vec2d(1e18, 1e18);
    m_live_obrect_fi = -1;
    m_live_obrect_angle_label = Vec2d(1e18, 1e18);
    m_live_rrect_fi = -1;
    m_live_rrect_w_label = m_live_rrect_h_label = m_live_rrect_r_label = Vec2d(1e18, 1e18);
    m_live_aslot_fi = -1;
    m_live_aslot_r_label = m_live_aslot_w_label = Vec2d(1e18, 1e18);
    m_live_slot_fi = -1;
    m_live_slot_len_label = m_live_slot_w_label = m_live_slot_angle_label = Vec2d(1e18, 1e18);
    // Edit-op tools (Fillet/Chamfer/Offset/Mirror) put their picks in m_selection for the
    // highlight, but their own arrow/label gizmo is the value affordance — don't also draw
    // the picked entity's characteristic quotes (Length/Angle/…) or the view gets cluttered.
    if (is_edit_op_mode() || is_transform_mode()) return;
    int ei = -1;
    if (m_dragging_point && m_drag_ei >= 0)  ei = m_drag_ei;
    else if (m_dragging_handle)              ei = m_drag_handle.ei;
    else if (m_selection.size() == 1)        ei = m_selection[0];
    if (ei < 0 || ei >= int(m_entities.size())) return;
    const SketchEntity& e = m_entities[ei];

    std::vector<DimAnnot> protos;
    auto add_len = [&](int line_ei, double side) {
        if (line_ei < 0 || line_ei >= int(m_entities.size())) return;
        if (m_entities[line_ei].type != SketchEntity::Type::Line) return;
        DimAnnot a; a.kind = DimType::Length; a.ea = line_ei; a.side = side; protos.push_back(a);
    };

    // A grouped gesture (rect/slot/polygon decomposes into raw lines/arcs) exposes its
    // DERIVED characteristic dims off the Feature span, regardless of which member edge
    // was picked. Rect: Width = first edge length, Height = second edge length (the two
    // axes of the 4-line loop pushed by push_closed_lines: edge0 horizontal, edge1
    // vertical). Quotes offset to opposite sides so they don't overlap.
    const int fi = feature_of(ei);
    if (fi >= 0) {
        const Feature& f = m_features[fi];
        switch (f.kind) {
        case FeatureKind::CornerRect:
        case FeatureKind::CenterRect: {
            add_len(f.begin + 0,  1.0);   // Width
            add_len(f.begin + 1, -1.0);   // Height
            // OBLIQUE rect (drawn off-axis, also a CornerRect feature): expose its orientation
            // too. Gated to genuinely-tilted edges so an axis-aligned Corner/Center rect never
            // gets an angle quote (and its verified W/H behaviour is untouched).
            if (f.begin >= 0 && f.begin < int(m_entities.size())) {
                const SketchEntity& e0 = m_entities[f.begin];
                Vec2d d0 = e0.p1 - e0.p0;
                if (d0.squaredNorm() > 1e-12) {
                    double deg = std::atan2(d0.y(), d0.x()) * 180.0 / M_PI;
                    const double off = std::fmod(std::fmod(deg, 90.0) + 90.0, 90.0); // dist to axis
                    if (off > 2.0 && off < 88.0) {
                        const ColorRGBA dc(0.30f, 0.88f, 0.66f, 1.0f);
                        const double th = std::max(15.0 * unit_per_px, 1e-4);
                        double adeg = deg; if (adeg < 0.0) adeg += 360.0;
                        const Vec2d mid = 0.5 * (e0.p0 + e0.p1);
                        Vec2d nrm(-d0.y(), d0.x()); if (nrm.squaredNorm() > 1e-12) nrm.normalize();
                        m_live_obrect_angle_label = mid + nrm * (th * 1.4);
                        DimAnnot at; at.kind = DimType::Angle; at.value = adeg;
                        draw_text(m_line_model, dim_text(at), m_live_obrect_angle_label, th, dc);
                        m_live_obrect_fi = fi;
                    }
                }
            }
            break;
        }
        case FeatureKind::Slot: {
            // Straight slot edits GEOMETRICALLY (like the arc-slot / rounded-rect), NOT through
            // the constraint-based scalar quotes. Its dims are the centreline LENGTH (distance
            // between the two cap centres c0,c1) and the WIDTH (2*param). The old
            // Distance-between-arc-centres + Radius quotes never registered as editable, so the
            // labels did nothing on click (nde #6). Draw both labels clear of the fillable face
            // and remember the feature so open_primary_autoedit / click-to-promote drive set_slot.
            // Slot dims: (1) inter-centre distance, (2) radius (= half-width), (3) centreline angle.
            const ColorRGBA dc(0.30f, 0.88f, 0.66f, 1.0f);
            const double th = std::max(15.0 * unit_per_px, 1e-4);
            Vec2d u = f.c1 - f.c0;
            const double Lc = u.norm();
            if (Lc > 1e-9) {
                u /= Lc;
                const Vec2d n(-u.y(), u.x());
                const double w = f.param;
                DimAnnot len; len.kind = DimType::Length; len.value = Lc;
                m_live_slot_len_label = 0.5 * (f.c0 + f.c1) + n * (w + th * 2.0);
                draw_text(m_line_model, dim_text(len), m_live_slot_len_label, th, dc);
                DimAnnot rd; rd.kind = DimType::Radius; rd.value = w;
                m_live_slot_w_label = f.c1 + u * (w + th * 2.0);
                draw_text(m_line_model, dim_text(rd), m_live_slot_w_label, th, dc);
                double deg = std::atan2(f.c1.y() - f.c0.y(), f.c1.x() - f.c0.x()) * 180.0 / M_PI;
                if (deg < 0.0) deg += 360.0;
                DimAnnot an; an.kind = DimType::Angle; an.value = deg;
                m_live_slot_angle_label = f.c0 - u * (w + th * 2.0);
                draw_text(m_line_model, dim_text(an), m_live_slot_angle_label, th, dc);
                m_live_slot_fi = fi;
            }
            break;
        }
        case FeatureKind::Polygon: {
            // A regular polygon is N raw lines (no centre entity). Its natural editable
            // dims are the SIDE length and the ORIENTATION — NOT a circumradius (a polygon
            // is not a circle). Both edit the whole loop geometrically: side scales it
            // uniformly, angle rotates it. Drawn off edge0 with draw_dim_quote (Length +
            // Angle); the side quote is offset OUTWARD so its label clears the face.
            if (f.begin < int(m_entities.size()) &&
                m_entities[f.begin].type == SketchEntity::Type::Line) {
                const ColorRGBA dc(0.30f, 0.88f, 0.66f, 1.0f);
                const double th = std::max(15.0 * unit_per_px, 1e-4);
                const SketchEntity& e0 = m_entities[f.begin];
                const Vec2d m0 = 0.5 * (e0.p0 + e0.p1);
                Vec2d u0 = e0.p1 - e0.p0;
                if (u0.squaredNorm() > 1e-12) u0.normalize();
                const Vec2d n0(-u0.y(), u0.x());
                const double outsign = ((m0 + n0) - f.c0).norm() >= (m0 - f.c0).norm() ? 1.0 : -1.0;
                DimAnnot side; side.kind = DimType::Length; side.ea = f.begin; side.side = outsign;
                side.value = measure_dim(side);
                Vec2d slbl;
                if (draw_dim_quote(side, th, dc, slbl)) m_live_poly_side_label = slbl;

                // Orientation = the angle of the centre->vertex0 spoke from +X (the
                // intuitive "which way does the polygon point"), NOT the edge direction.
                // Drawn as a wedge OUTSIDE the polygon (radius just past the circumradius)
                // so its arc/label clear the fillable face.
                const Vec2d sp = m_entities[f.begin].p0 - f.c0;   // centre -> vertex0
                const double R = sp.norm();
                if (R > 1e-6) {
                    double av = std::atan2(sp.y(), sp.x());
                    double avdeg = av * 180.0 / M_PI; if (avdeg < 0.0) avdeg += 360.0;
                    const double rr = R + th * 2.5;               // wedge just outside the loop
                    std::vector<std::pair<Vec2d, Vec2d>> asegs;
                    asegs.emplace_back(f.c0, f.c0 + Vec2d(rr, 0.0));                       // +X leg
                    asegs.emplace_back(f.c0, f.c0 + Vec2d(std::cos(av), std::sin(av)) * rr); // spoke leg
                    const int N = 20; Vec2d prev = f.c0 + Vec2d(rr, 0.0);
                    for (int i = 1; i <= N; ++i) {
                        const double t = av * double(i) / N;
                        const Vec2d cur = f.c0 + Vec2d(rr * std::cos(t), rr * std::sin(t));
                        asegs.emplace_back(prev, cur); prev = cur;
                    }
                    const double mid = av * 0.5;
                    const Vec2d albl = f.c0 + Vec2d(std::cos(mid), std::sin(mid)) * (rr + th * 1.2);
                    DimAnnot at; at.kind = DimType::Angle; at.value = avdeg;   // "NN.N°"
                    draw_strokes(m_highlight_model, asegs, 0.6, dc);
                    draw_text(m_line_model, dim_text(at), albl, th, dc);
                    m_live_poly_angle_label = albl;
                }
                m_live_poly_fi = fi;
            }
            break;
        }
        case FeatureKind::RoundedRect: {
            // Width + Height (box bounds) + fillet Radius, drawn as clickable quotes that
            // rebuild the box geometrically (set_rounded_rect). c0=min corner, c1=max, param=r.
            const ColorRGBA dc(0.30f, 0.88f, 0.66f, 1.0f);
            const double th = std::max(15.0 * unit_per_px, 1e-4);
            const double xmin = std::min(f.c0.x(), f.c1.x()), xmax = std::max(f.c0.x(), f.c1.x());
            const double ymin = std::min(f.c0.y(), f.c1.y()), ymax = std::max(f.c0.y(), f.c1.y());
            const double w = xmax - xmin, h = ymax - ymin, r = f.param;
            const double off = th * 2.0;
            std::vector<std::pair<Vec2d, Vec2d>> segs;
            // Width quote below the box.
            const double yb = ymin - off;
            segs.emplace_back(Vec2d(xmin, ymin), Vec2d(xmin, yb));
            segs.emplace_back(Vec2d(xmax, ymin), Vec2d(xmax, yb));
            segs.emplace_back(Vec2d(xmin, yb),   Vec2d(xmax, yb));
            // Height quote left of the box.
            const double xl = xmin - off;
            segs.emplace_back(Vec2d(xmin, ymin), Vec2d(xl, ymin));
            segs.emplace_back(Vec2d(xmin, ymax), Vec2d(xl, ymax));
            segs.emplace_back(Vec2d(xl, ymin),   Vec2d(xl, ymax));
            // Fillet-radius leader from the TR arc centre out to the corner.
            const Vec2d rc(xmax - r, ymax - r);
            segs.emplace_back(rc, rc + Vec2d(r, r).normalized() * r);
            draw_strokes(m_highlight_model, segs, 0.6, dc);
            DimAnnot wa; wa.kind = DimType::Length; wa.value = w;
            DimAnnot ha; ha.kind = DimType::Length; ha.value = h;
            DimAnnot ra; ra.kind = DimType::Radius; ra.value = r;
            m_live_rrect_w_label = Vec2d((xmin + xmax) * 0.5, yb - th * 0.8);
            m_live_rrect_h_label = Vec2d(xl - th * 0.8, (ymin + ymax) * 0.5);
            m_live_rrect_r_label = rc + Vec2d(r, r).normalized() * (r + th * 1.2);
            draw_text(m_line_model, dim_text(wa), m_live_rrect_w_label, th, dc);
            draw_text(m_line_model, dim_text(ha), m_live_rrect_h_label, th, dc);
            draw_text(m_line_model, dim_text(ra), m_live_rrect_r_label, th, dc);
            m_live_rrect_fi = fi;
            break;
        }
        case FeatureKind::ArcSlot: {
            // Centreline Radius + slot Width quotes. centre=f.c0, centreline start=f.c1,
            // half-width=f.param; end direction from the cap@E arc centre (begin+1).
            if (f.begin + 1 < int(m_entities.size())) {
                const ColorRGBA dc(0.30f, 0.88f, 0.66f, 1.0f);
                const double th = std::max(15.0 * unit_per_px, 1e-4);
                const Vec2d  center = f.c0;
                const double Rc = (f.c1 - center).norm();
                const double w  = f.param;
                Vec2d dirS = (f.c1 - center);
                Vec2d dirE = (m_entities[f.begin + 1].center - center);
                if (dirS.squaredNorm() > 1e-12 && dirE.squaredNorm() > 1e-12 && Rc > 1e-6) {
                    dirS.normalize(); dirE.normalize();
                    const double aS = std::atan2(dirS.y(), dirS.x());
                    double sweep = std::atan2(dirE.y(), dirE.x()) - aS;
                    while (sweep < 0) sweep += 2.0 * M_PI;
                    const double aMid = aS + sweep * 0.5;
                    const Vec2d uMid(std::cos(aMid), std::sin(aMid));
                    // Centreline-radius leader: centre -> centreline midpoint.
                    std::vector<std::pair<Vec2d, Vec2d>> segs;
                    segs.emplace_back(center, center + uMid * Rc);
                    // Width tick across the slot at the start cap (outer<->inner).
                    segs.emplace_back(center + dirS * (Rc + w), center + dirS * (Rc - w));
                    draw_strokes(m_highlight_model, segs, 0.6, dc);
                    DimAnnot ra; ra.kind = DimType::Radius; ra.value = Rc;
                    DimAnnot wa; wa.kind = DimType::Length; wa.value = 2.0 * w;
                    m_live_aslot_r_label = center + uMid * (Rc * 0.5) + Vec2d(0, th);
                    m_live_aslot_w_label = center + dirS * (Rc + w) + dirS * (th * 1.2);
                    draw_text(m_line_model, dim_text(ra), m_live_aslot_r_label, th, dc);
                    draw_text(m_line_model, dim_text(wa), m_live_aslot_w_label, th, dc);
                    m_live_aslot_fi = fi;
                }
            }
            break;
        }
        default: break;                   // other features: later chunks
        }
    }

    if (protos.empty() && m_live_poly_fi < 0 && m_live_rrect_fi < 0 &&
        m_live_aslot_fi < 0 && m_live_slot_fi < 0) {  // ungrouped single entity
        switch (e.type) {
        case SketchEntity::Type::Line: {
            DimAnnot len; len.kind = DimType::Length; len.ea = ei; len.side = 1.0; protos.push_back(len);
            DimAnnot ang; ang.kind = DimType::Angle;  ang.ea = ei; ang.eb = -1;    protos.push_back(ang);
            break;                        // segment length + angle-to-horizontal
        }
        case SketchEntity::Type::Circle: {
            DimAnnot a; a.kind = DimType::Radius; a.ea = ei; protos.push_back(a); break;
        }
        case SketchEntity::Type::Arc: {
            // Arc radius is its single defining dimension (sweep angles edit via the end
            // handles). radius lives in the same .radius field measure_dim/constraint_for
            // read, so the Radius promotion path is identical to Circle.
            DimAnnot a; a.kind = DimType::Radius; a.ea = ei; protos.push_back(a); break;
        }
        default: break;                   // ellipse/bspline: later
        }
    }

    // Arc sweep-angle wedge: drawn inline (like the polygon orientation) because it is a
    // GEOMETRIC edit (SLVS angle constraints are line-to-line). A wedge spans the arc's
    // start->end angles just OUTSIDE the radius; its label shows the included angle and is
    // clickable to type a new sweep. The radius quote is still emitted via `protos`.
    if (m_live_poly_fi < 0 && m_live_rrect_fi < 0 && m_live_aslot_fi < 0 && m_live_slot_fi < 0 &&
        e.type == SketchEntity::Type::Arc && e.radius > 1e-6) {
        const ColorRGBA dc(0.30f, 0.88f, 0.66f, 1.0f);
        const double th = std::max(15.0 * unit_per_px, 1e-4);
        const Vec2d  c  = e.center;
        const double a0 = e.start_angle, a1 = e.end_angle;
        const double sweep = a1 - a0;                       // signed (CCW>0); |sweep| shown
        double swdeg = std::abs(sweep) * 180.0 / M_PI;
        const double rr = e.radius + th * 2.5;              // wedge just outside the arc
        std::vector<std::pair<Vec2d, Vec2d>> asegs;
        asegs.emplace_back(c, c + Vec2d(std::cos(a0), std::sin(a0)) * rr);   // start leg
        asegs.emplace_back(c, c + Vec2d(std::cos(a1), std::sin(a1)) * rr);   // end leg
        const int N = 24; Vec2d prev = c + Vec2d(std::cos(a0), std::sin(a0)) * rr;
        for (int i = 1; i <= N; ++i) {
            const double t = a0 + sweep * double(i) / N;
            const Vec2d cur = c + Vec2d(rr * std::cos(t), rr * std::sin(t));
            asegs.emplace_back(prev, cur); prev = cur;
        }
        const double mid = a0 + sweep * 0.5;
        const Vec2d albl = c + Vec2d(std::cos(mid), std::sin(mid)) * (rr + th * 1.2);
        DimAnnot at; at.kind = DimType::Angle; at.value = swdeg;             // "NN.N°"
        draw_strokes(m_highlight_model, asegs, 0.6, dc);
        draw_text(m_line_model, dim_text(at), albl, th, dc);
        m_live_arc_angle_label = albl;
        m_live_arc_ei = ei;
    }

    // Ellipse: two clickable axis quotes — semi-major (a) along the major direction and
    // semi-minor (b) along the minor. Both edit geometrically (a=e.radius, b=e.rminor);
    // phi (orientation) is changed by dragging the major grip, not via a label.
    if (m_live_poly_fi < 0 && m_live_rrect_fi < 0 &&
        (e.type == SketchEntity::Type::Ellipse || e.type == SketchEntity::Type::EllipseArc) &&
        e.radius > 1e-6 && e.rminor > 1e-6) {
        const ColorRGBA dc(0.30f, 0.88f, 0.66f, 1.0f);
        const double th = std::max(15.0 * unit_per_px, 1e-4);
        const Vec2d  c  = e.center;
        const Vec2d  um(std::cos(e.rotation), std::sin(e.rotation));   // major dir
        const Vec2d  un(-um.y(), um.x());                              // minor dir
        std::vector<std::pair<Vec2d, Vec2d>> segs;
        const Vec2d majEnd = c + um * e.radius, minEnd = c + un * e.rminor;
        segs.emplace_back(c, majEnd);
        segs.emplace_back(c, minEnd);
        draw_strokes(m_highlight_model, segs, 0.6, dc);
        DimAnnot ma; ma.kind = DimType::Length; ma.value = e.radius;   // plain "NN.N"
        DimAnnot mi; mi.kind = DimType::Length; mi.value = e.rminor;
        const Vec2d majLbl = c + um * (e.radius * 0.5) + un * (th * 1.0);
        const Vec2d minLbl = c + un * (e.rminor * 0.5) + um * (th * 1.0);
        draw_text(m_line_model, dim_text(ma), majLbl, th, dc);
        draw_text(m_line_model, dim_text(mi), minLbl, th, dc);
        m_live_ellipse_major_label = majLbl;
        m_live_ellipse_minor_label = minLbl;
        m_live_ellipse_ei = ei;
        // Elliptical arc also has a SWEEP (included parametric angle), drawn outside the arc
        // midpoint; the full ellipse skips this (closed).
        if (e.type == SketchEntity::Type::EllipseArc) {
            const double midp = 0.5 * (e.start_angle + e.end_angle);
            const Vec2d  mp   = ellipse_point(c, e.radius, e.rminor, e.rotation, midp);
            Vec2d outw = mp - c; if (outw.squaredNorm() > 1e-12) outw.normalize();
            m_live_ellipsearc_sweep_label = mp + outw * (th * 1.5);
            DimAnnot sw; sw.kind = DimType::Angle;
            sw.value = std::abs(e.end_angle - e.start_angle) * 180.0 / M_PI;
            draw_text(m_line_model, dim_text(sw), m_live_ellipsearc_sweep_label, th, dc);
        }
    }

    if (protos.empty()) return;

    const ColorRGBA dimcol(0.30f, 0.88f, 0.66f, 1.0f);
    const double th = std::max(15.0 * unit_per_px, 1e-4);
    for (DimAnnot a : protos) {
        bool driven = false;                       // skip if already a driving dim of this kind
        for (const DimAnnot& d : m_dimensions)
            if (d.ea == a.ea && d.kind == a.kind) { driven = true; break; }
        if (driven) continue;
        a.value = measure_dim(a);
        Vec2d label;
        if (draw_dim_quote(a, th, dimcol, label)) {
            a.label_pos = label;
            m_live_quotes.push_back(a);            // remember for click-to-promote
        }
    }
}

// Iconic constraint badges drawn near each constraint's primary entity (C3.4b).
// Each glyph is authored in a unit cell [-0.5,0.5]^2 then scaled to a constant
// on-screen size and translated to the anchor; badges on the same entity stack
// upward so multiple constraints stay legible.
void DesignSketchTool::build_constraint_glyphs(double unit_per_px,
                                               std::vector<std::pair<Vec2d, Vec2d>>& out) const
{
    if (m_constrain_cons.empty() || m_entities.empty()) return;
    using T = SketchConstraintType;
    const double s = std::max(11.0 * unit_per_px, 1e-4);   // glyph cell size in plane units
    const double step = s * 1.5;                           // vertical stacking step

    // Representative anchor point on an entity (line midpoint, round-entity centre).
    auto anchor_of = [&](int ei) -> Vec2d {
        if (ei < 0 || ei >= int(m_entities.size())) return Vec2d(0, 0);
        const SketchEntity& e = m_entities[ei];
        switch (e.type) {
        case SketchEntity::Type::Line:    return 0.5 * (e.p0 + e.p1);
        case SketchEntity::Type::Circle:
        case SketchEntity::Type::Ellipse:
        case SketchEntity::Type::Arc:
        case SketchEntity::Type::EllipseArc: return e.center;
        case SketchEntity::Type::BSpline:  return 0.5 * (e.p0 + e.p1);
        case SketchEntity::Type::Point:    return e.p0;
        }
        return e.p0;
    };

    // Unit-cell stroke authoring helpers (cell centred on origin).
    auto seg = [&](std::vector<std::pair<Vec2d, Vec2d>>& v, Vec2d a, Vec2d b) { v.emplace_back(a, b); };
    auto circ = [&](std::vector<std::pair<Vec2d, Vec2d>>& v, Vec2d c, double r) {
        const int n = 12; Vec2d prev(c.x() + r, c.y());
        for (int i = 1; i <= n; ++i) {
            const double t = 2.0 * 3.14159265358979 * i / n;
            Vec2d cur(c.x() + r * std::cos(t), c.y() + r * std::sin(t));
            v.emplace_back(prev, cur); prev = cur;
        }
    };
    // Author one glyph type into a unit-cell stroke list.
    auto unit_glyph = [&](T type, std::vector<std::pair<Vec2d, Vec2d>>& v) {
        switch (type) {
        case T::Horizontal: seg(v, {-0.5, 0}, {0.5, 0}); break;
        case T::Vertical:   seg(v, {0, -0.5}, {0, 0.5}); break;
        case T::Parallel:   seg(v, {-0.35, -0.5}, {0.0, 0.5}); seg(v, {0.05, -0.5}, {0.4, 0.5}); break;
        case T::Perpendicular: seg(v, {-0.4, 0.5}, {-0.4, -0.4}); seg(v, {-0.4, -0.4}, {0.5, -0.4}); break;
        case T::Coincident: circ(v, {0, 0}, 0.42); break;
        case T::Concentric: circ(v, {0, 0}, 0.5); circ(v, {0, 0}, 0.24); break;
        case T::EqualLength:seg(v, {-0.4, 0.16}, {0.4, 0.16}); seg(v, {-0.4, -0.16}, {0.4, -0.16}); break;
        case T::Tangent:    circ(v, {0, -0.1}, 0.35); seg(v, {-0.5, 0.42}, {0.5, 0.42}); break;
        case T::Midpoint:   seg(v, {-0.4, 0}, {0.4, 0}); seg(v, {0, -0.18}, {0, 0.18}); break;
        case T::Symmetric:  seg(v, {0, -0.5}, {0, 0.5});
                            seg(v, {-0.5, 0.4}, {-0.15, 0}); seg(v, {-0.5, -0.4}, {-0.15, 0});
                            seg(v, {0.5, 0.4}, {0.15, 0}); seg(v, {0.5, -0.4}, {0.15, 0}); break;
        case T::Fix:        seg(v, {-0.4, -0.4}, {0.4, -0.4}); seg(v, {0.4, -0.4}, {0.4, 0.4});
                            seg(v, {0.4, 0.4}, {-0.4, 0.4}); seg(v, {-0.4, 0.4}, {-0.4, -0.4}); break;
        case T::Angle:      seg(v, {-0.4, -0.4}, {0.4, -0.4}); seg(v, {-0.4, -0.4}, {0.3, 0.4}); break;
        case T::Radius:     circ(v, {0, 0}, 0.45); seg(v, {0, 0}, {0.45, 0}); break;
        case T::Diameter:   circ(v, {0, 0}, 0.45); seg(v, {-0.45, 0}, {0.45, 0}); break;
        case T::PointOnLine:
        case T::PointOnObject: seg(v, {-0.5, -0.3}, {0.5, -0.3}); circ(v, {0, 0.05}, 0.16); break;
        case T::Distance:
        case T::LockX:
        case T::LockY:      seg(v, {-0.4, 0}, {0.4, 0}); break;   // generic tick
        }
    };

    // Stack count per entity so successive badges step upward.
    std::vector<int> stack(m_entities.size(), 0);
    const Vec2d up(0.0, 1.0);     // plane-space up; offset so badge sits off the geometry
    for (const SketchEntityConstraintDef& d : m_constrain_cons) {
        if (d.ea < 0 || d.ea >= int(m_entities.size())) continue;
        const int k = stack[d.ea]++;
        const Vec2d center = anchor_of(d.ea) + up * (step * (1.0 + k));
        std::vector<std::pair<Vec2d, Vec2d>> cell;
        unit_glyph(d.type, cell);
        for (auto& sgp : cell)
            out.emplace_back(center + sgp.first * s, center + sgp.second * s);
    }
}

void DesignSketchTool::draw_entities_preview(const std::vector<SketchEntity>& ents, const ColorRGBA& color)
{
    for (const SketchEntity& e : ents) {
        if (e.type == SketchEntity::Type::Point) continue;
        bool closed = false;
        std::vector<Vec2d> poly = entity_polyline(e, closed);
        draw_quad_strip(m_highlight_model, poly, closed, color);
    }
}

// ---- In-canvas edit-op gizmo (Fillet/Chamfer/Offset/Mirror toolbar tools) ----------
// These tools replace the docked numeric card. They operate on the LIVE session's
// m_entities/m_constraints, so they work both while drawing and after begin_edit re-opens
// a committed sketch. The SketchEngine op (context-free) is reused verbatim; the
// constraint binding is ported from DesignPanel::apply_entity_constraint into m_constraints
// via try_add_constraints (append→solve→keep / rollback).

void DesignSketchTool::reset_op()
{
    m_op_a = m_op_b = -1;
    m_op_value = 0.0;
    m_op_anchor = Vec2d(0, 0);
    m_op_dir = Vec2d(0, 0);
    m_op_label = Vec2d(1e18, 1e18);
    m_op_ghost.clear();
    m_op_dragging_arrow = false;
    m_mirror_targets.clear();
}

// ---- Imported-art bounding-box transform gizmo (Mode::TransformArt) ----

void DesignSketchTool::reset_xform()
{
    m_xform_base.clear();
    m_xform_feat = -1;
    m_xform_handle = -1;
    m_xform_offset = Vec2d(0, 0);
    m_xform_sx = m_xform_sy = 1.0;
    m_xform_min = m_xform_max = Vec2d(0, 0);
}

void DesignSketchTool::begin_imported_transform(
        int feat, const std::vector<std::vector<std::vector<Vec2d>>>& base_regions,
        const SketchPlane& plane, const Vec2d& offset, double sx, double sy)
{
    cancel();                       // drop any prior session, clears state
    m_plane = plane;
    m_mode  = Mode::TransformArt;
    m_xform_base   = base_regions;
    m_xform_feat   = feat;
    m_xform_offset = offset;
    m_xform_sx = (std::abs(sx) > 1e-6) ? sx : 1.0;
    m_xform_sy = (std::abs(sy) > 1e-6) ? sy : 1.0;
    m_xform_handle = -1;
    Vec2d mn(1e30, 1e30), mx(-1e30, -1e30);
    for (const auto& region : m_xform_base)
        for (const auto& contour : region)
            for (const Vec2d& p : contour) {
                mn.x() = std::min(mn.x(), p.x()); mn.y() = std::min(mn.y(), p.y());
                mx.x() = std::max(mx.x(), p.x()); mx.y() = std::max(mx.y(), p.y());
            }
    if (mx.x() < mn.x()) { mn = Vec2d(0, 0); mx = Vec2d(0, 0); }
    m_xform_min = mn; m_xform_max = mx;
    m_active = true;
    m_has_cursor = false;
}

// 4 bbox corners in plane coords: 0=min/min, 1=max/min, 2=max/max, 3=min/max. The art
// transform is world = base*scale + offset (CadFeature import convention).
void DesignSketchTool::xform_world_corners(Vec2d out[4]) const
{
    const double x0 = m_xform_min.x() * m_xform_sx + m_xform_offset.x();
    const double x1 = m_xform_max.x() * m_xform_sx + m_xform_offset.x();
    const double y0 = m_xform_min.y() * m_xform_sy + m_xform_offset.y();
    const double y1 = m_xform_max.y() * m_xform_sy + m_xform_offset.y();
    out[0] = Vec2d(x0, y0); out[1] = Vec2d(x1, y0);
    out[2] = Vec2d(x1, y1); out[3] = Vec2d(x0, y1);
}

int DesignSketchTool::hit_test_xform_handle(const Vec2d& p, double tol) const
{
    Vec2d c[4]; xform_world_corners(c);
    int best = -1; double bd = tol;
    for (int i = 0; i < 4; ++i) { const double d = (c[i] - p).norm(); if (d < bd) { bd = d; best = i; } }
    if (best >= 0) return best;
    if ((0.5 * (c[0] + c[2]) - p).norm() <= tol) return 4;   // centre move-handle
    return -1;
}

void DesignSketchTool::drag_xform_handle(const Vec2d& target)
{
    if (m_xform_handle < 0) return;
    if (m_xform_handle == 4) {                 // centre move: translate by cursor delta
        m_xform_offset += (target - m_xform_anchor);
        m_xform_anchor  = target;
        emit_xform();
        return;
    }
    // Corner scale: hold the opposite corner (fixed world anchor O), send the grabbed
    // corner to the cursor. base coords of grabbed (bg) and opposite (ba) corners.
    auto base_corner = [&](int i) {
        return Vec2d((i == 1 || i == 2) ? m_xform_max.x() : m_xform_min.x(),
                     (i == 2 || i == 3) ? m_xform_max.y() : m_xform_min.y());
    };
    const int h = m_xform_handle;
    const Vec2d bg = base_corner(h);
    const Vec2d ba = base_corner((h + 2) % 4);
    const Vec2d O  = m_xform_anchor;
    const double dbx = bg.x() - ba.x(), dby = bg.y() - ba.y();
    if (std::abs(dbx) > 1e-9) {
        double nsx = (target.x() - O.x()) / dbx;
        if (std::abs(nsx) < 1e-4) nsx = (nsx < 0 ? -1e-4 : 1e-4);
        m_xform_sx = nsx;
        m_xform_offset.x() = O.x() - ba.x() * nsx;
    }
    if (std::abs(dby) > 1e-9) {
        double nsy = (target.y() - O.y()) / dby;
        if (std::abs(nsy) < 1e-4) nsy = (nsy < 0 ? -1e-4 : 1e-4);
        m_xform_sy = nsy;
        m_xform_offset.y() = O.y() - ba.y() * nsy;
    }
    emit_xform();
}

void DesignSketchTool::emit_xform()
{
    if (on_imported_transform)
        on_imported_transform(m_xform_feat, m_xform_offset, m_xform_sx, m_xform_sy);
}

void DesignSketchTool::render_xform_gizmo()
{
    if (m_mode != Mode::TransformArt) return;
    Vec2d c[4]; xform_world_corners(c);
    const ColorRGBA box(0.30f, 0.88f, 0.66f, 1.0f);
    std::vector<std::pair<Vec2d, Vec2d>> segs;
    for (int i = 0; i < 4; ++i) segs.emplace_back(c[i], c[(i + 1) % 4]);
    draw_strokes(m_highlight_model, segs, 0.6, box);
    const Camera& cam = wxGetApp().plater()->get_camera();
    const double hs = 7.0 / std::max(cam.get_zoom(), 1e-6);   // screen-constant half-size
    const ColorRGBA hcol(0.30f, 0.88f, 0.66f, 1.0f);
    const ColorRGBA hhot(1.0f, 0.85f, 0.2f, 1.0f);
    auto square = [&](const Vec2d& q, const ColorRGBA& col) {
        const std::vector<Vec2d> sq = { q + Vec2d(-hs, -hs), q + Vec2d(hs, -hs),
                                        q + Vec2d(hs, hs), q + Vec2d(-hs, hs) };
        draw_fill(m_fill_model, sq, col);
    };
    for (int i = 0; i < 4; ++i) square(c[i], m_xform_handle == i ? hhot : hcol);
    square(0.5 * (c[0] + c[2]), m_xform_handle == 4 ? hhot : hcol);
}

bool DesignSketchTool::op_ready() const
{
    switch (m_mode) {
    case Mode::Fillet:
    case Mode::Chamfer: return m_op_a >= 0 && m_op_b >= 0;
    case Mode::Offset:  return m_op_a >= 0;
    case Mode::Mirror:  return m_op_a >= 0 && !m_mirror_targets.empty();
    default: return false;
    }
}

// Corner vertex of two lines + the inward angle bisector (unit), pointing from the vertex
// into the fillet/chamfer interior. Mirrors SketchEngine::fillet_lines's geometry so the
// arrow tracks the op exactly.
bool DesignSketchTool::op_corner(int a, int b, Vec2d& C, Vec2d& bis, double& theta) const
{
    if (a < 0 || b < 0 || a >= int(m_entities.size()) || b >= int(m_entities.size())) return false;
    const SketchEntity& ea = m_entities[a];
    const SketchEntity& eb = m_entities[b];
    if (ea.type != SketchEntity::Type::Line || eb.type != SketchEntity::Type::Line) return false;
    const Vec2d da = ea.p1 - ea.p0, db = eb.p1 - eb.p0;
    const double denom = da.x() * db.y() - da.y() * db.x();
    if (std::abs(denom) < 1e-12) return false;                       // parallel
    const Vec2d diff = eb.p0 - ea.p0;
    const double s = (diff.x() * db.y() - diff.y() * db.x()) / denom;
    C = ea.p0 + s * da;
    Vec2d ua = ((ea.p0 - C).norm() <= (ea.p1 - C).norm()) ? (ea.p1 - C) : (ea.p0 - C);
    Vec2d ub = ((eb.p0 - C).norm() <= (eb.p1 - C).norm()) ? (eb.p1 - C) : (eb.p0 - C);
    if (ua.norm() < 1e-12 || ub.norm() < 1e-12) return false;
    ua.normalize(); ub.normalize();
    theta = std::acos(std::max(-1.0, std::min(1.0, ua.dot(ub))));
    bis = ua + ub;
    if (bis.norm() < 1e-12) return false;                            // 180° corner
    bis.normalize();
    return true;
}

void DesignSketchTool::recompute_op_ghost()
{
    m_op_ghost.clear();
    if (m_mode == Mode::Fillet || m_mode == Mode::Chamfer) {
        if (m_op_a < 0 || m_op_b < 0) return;
        Vec2d C, bis; double theta;
        if (op_corner(m_op_a, m_op_b, C, bis, theta)) { m_op_anchor = C; m_op_dir = bis; }
        SketchEntity a_out, b_out, extra;
        const bool ok = (m_mode == Mode::Fillet)
            ? SketchEngine::fillet_lines(m_entities[m_op_a], m_entities[m_op_b], m_op_value, a_out, b_out, extra)
            : SketchEngine::chamfer_lines(m_entities[m_op_a], m_entities[m_op_b], m_op_value, a_out, b_out, extra);
        if (ok) m_op_ghost = { a_out, b_out, extra };
    } else if (m_mode == Mode::Offset) {
        if (m_op_a < 0) return;
        const SketchEntity& e = m_entities[m_op_a];
        if (e.type == SketchEntity::Type::Line) {
            m_op_anchor = 0.5 * (e.p0 + e.p1);
            Vec2d u = e.p1 - e.p0; if (u.norm() > 1e-12) u.normalize();
            m_op_dir = Vec2d(-u.y(), u.x());                  // left normal = +distance side
        } else if (e.type == SketchEntity::Type::Circle || e.type == SketchEntity::Type::Arc) {
            m_op_anchor = e.center + Vec2d(e.radius, 0.0);
            m_op_dir = Vec2d(1, 0);
        }
        m_op_ghost = SketchEngine::offset_entities({ e }, m_op_value);
    } else if (m_mode == Mode::Mirror) {
        if (m_op_a < 0 || m_mirror_targets.empty()) return;
        const SketchEntity& axis = m_entities[m_op_a];
        std::vector<SketchEntity> src;
        for (int ti : m_mirror_targets)
            if (ti >= 0 && ti < int(m_entities.size())) src.push_back(m_entities[ti]);
        m_op_ghost = SketchEngine::mirror_entities(src, axis.p0, axis.p1);
    }
}

// Route an entity pick to the active op; sets an initial value + ghost once enough
// entities are picked. Highlights the running picks via m_selection.
void DesignSketchTool::op_pick(int ei)
{
    if (ei < 0 || ei >= int(m_entities.size())) return;
    const SketchEntity::Type t = m_entities[ei].type;
    switch (m_mode) {
    case Mode::Fillet:
    case Mode::Chamfer:
        if (t != SketchEntity::Type::Line) return;            // corner ops need two lines
        if (m_op_a < 0) m_op_a = ei;
        else if (ei != m_op_a) {
            m_op_b = ei;
            const double la = (m_entities[m_op_a].p1 - m_entities[m_op_a].p0).norm();
            const double lb = (m_entities[m_op_b].p1 - m_entities[m_op_b].p0).norm();
            m_op_value = std::max(0.001, 0.2 * std::min(la, lb));  // a sensible starting size
            recompute_op_ghost();
        }
        break;
    case Mode::Offset: {
        m_op_a = ei;
        const SketchEntity& e = m_entities[ei];
        const double sz = (e.type == SketchEntity::Type::Line) ? (e.p1 - e.p0).norm()
                                                               : std::max(e.radius * 2.0, 1.0);
        m_op_value = std::max(0.001, 0.1 * sz);
        recompute_op_ghost();
        break;
    }
    case Mode::Mirror:
        if (m_op_a < 0) {
            if (t != SketchEntity::Type::Line) return;        // axis must be a line
            m_op_a = ei;
        } else if (ei != m_op_a) {
            auto it = std::find(m_mirror_targets.begin(), m_mirror_targets.end(), ei);
            if (it == m_mirror_targets.end()) m_mirror_targets.push_back(ei);
            else                              m_mirror_targets.erase(it);
            recompute_op_ghost();
        }
        break;
    default: break;
    }
    // Mirror the picks into m_selection so the existing highlight shows them.
    m_selection.clear();
    if (m_op_a >= 0) m_selection.push_back(m_op_a);
    if (m_op_b >= 0) m_selection.push_back(m_op_b);
    for (int ti : m_mirror_targets) m_selection.push_back(ti);
    if (on_selection_changed) on_selection_changed(int(m_selection.size()));
}

bool DesignSketchTool::hit_test_op_arrow(const Vec2d& p, double tol) const
{
    if (!op_ready() || m_mode == Mode::Mirror) return false;
    const Vec2d tip = m_op_anchor + m_op_dir * m_op_value;
    return point_segment_dist(p, m_op_anchor, tip) <= tol * 1.5;
}

void DesignSketchTool::drag_op_arrow(const Vec2d& target)
{
    const double v = (target - m_op_anchor).dot(m_op_dir);      // project onto the arrow axis
    if (m_mode == Mode::Offset) m_op_value = v;                 // signed: chooses the side
    else                        m_op_value = std::max(0.001, v);// fillet/chamfer: positive
    recompute_op_ghost();
}

void DesignSketchTool::open_op_editor()
{
    if (!on_inline_edit || !op_ready() || m_mode == Mode::Mirror) return;
    const double sign = (m_mode == Mode::Offset && m_op_value < 0) ? -1.0 : 1.0;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, std::abs(m_op_value), "",
        [this, sign](double v) {
            m_op_value = (m_mode == Mode::Offset) ? sign * std::abs(v) : std::max(0.001, v);
            // Entering a radius IS the commit. Leaving it as a preview meant the most obvious
            // route of all — click the radius, type it, press Return — ended with the value set,
            // the ghost drawn, and no geometry written; the only paths that ever applied it were
            // finishing the whole sketch or clicking empty space, neither of which is signposted.
            if (op_ready()) confirm_op();
            else            recompute_op_ghost();
        },
        []() {});
}

void DesignSketchTool::render_op_gizmo(double unit_per_px)
{
    m_op_label = Vec2d(1e18, 1e18);
    if (!op_ready()) return;
    const ColorRGBA ghostc(0.30f, 0.88f, 0.66f, 0.55f);
    draw_entities_preview(m_op_ghost, ghostc);
    if (m_mode == Mode::Mirror) return;                         // pick-only, no arrow/label
    const ColorRGBA dc(0.30f, 0.88f, 0.66f, 1.0f);
    const double th = std::max(15.0 * unit_per_px, 1e-4);
    const Vec2d dir = (m_op_value >= 0 ? m_op_dir : -m_op_dir);
    const Vec2d tip = m_op_anchor + m_op_dir * m_op_value;      // signed length picks the side
    std::vector<std::pair<Vec2d, Vec2d>> segs;
    segs.emplace_back(m_op_anchor, tip);
    const double as = std::max(std::abs(m_op_value) * 0.18, th * 0.8);  // arrowhead size
    const Vec2d nrm(-dir.y(), dir.x());
    const Vec2d back = tip - dir * as;
    segs.emplace_back(tip, back + nrm * (as * 0.5));
    segs.emplace_back(tip, back - nrm * (as * 0.5));
    draw_strokes(m_highlight_model, segs, 0.6, dc);
    DimAnnot a;
    a.kind  = (m_mode == Mode::Fillet) ? DimType::Radius : DimType::Distance;
    a.value = std::abs(m_op_value);
    m_op_label = tip + dir * (th * 1.2);
    draw_text(m_line_model, dim_text(a), m_op_label, th, dc);
}

void DesignSketchTool::confirm_op()
{
    if (!op_ready()) return;
    using R  = SketchPointRole;
    using CT = SketchConstraintType;

    if (m_mode == Mode::Fillet || m_mode == Mode::Chamfer) {
        const bool fillet = (m_mode == Mode::Fillet);
        SketchEntity a_out, b_out, extra;
        const bool ok = fillet
            ? SketchEngine::fillet_lines(m_entities[m_op_a], m_entities[m_op_b], m_op_value, a_out, b_out, extra)
            : SketchEngine::chamfer_lines(m_entities[m_op_a], m_entities[m_op_b], m_op_value, a_out, b_out, extra);
        if (!ok) { reset_op(); return; }
        const int a = m_op_a, b = m_op_b;
        m_entities[a] = a_out; m_entities[b] = b_out;
        const int xi = int(m_entities.size());
        m_entities.push_back(extra);                 // fillet arc / chamfer segment
        auto role_near = [](const SketchEntity& ln, const Vec2d& q) -> R {
            return ((ln.p0 - q).squaredNorm() <= (ln.p1 - q).squaredNorm()) ? R::P0 : R::P1; };
        const R ra = role_near(m_entities[a], extra.p0);
        const R rb = role_near(m_entities[b], extra.p1);
        // Drop the now-stale corner Coincident + each line's own length Distance (the op
        // trimmed both legs back), then bind the new entity onto the trimmed endpoints.
        auto refs = [](const SketchEntityConstraintDef& d, int e, R r) {
            return (d.ea == e && d.ra == r) || (d.eb == e && d.rb == r); };
        auto self_len = [](const SketchEntityConstraintDef& d, int e) {
            return d.type == CT::Distance && d.ea == e && d.eb == e; };
        auto& cs = m_constraints;
        cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const SketchEntityConstraintDef& d) {
            return (d.type == CT::Coincident && refs(d, a, ra) && refs(d, b, rb))
                || self_len(d, a) || self_len(d, b);
        }), cs.end());
        auto coin = [&](R xr, int ln, R lr) {
            SketchEntityConstraintDef d; d.type = CT::Coincident; d.ea = xi; d.ra = xr; d.eb = ln; d.rb = lr; return d; };
        if (fillet) {
            auto tang = [&](int ln) {
                SketchEntityConstraintDef d; d.type = CT::Tangent; d.ea = xi; d.eb = ln; return d; };
            const std::vector<std::vector<SketchEntityConstraintDef>> ladder = {
                { coin(R::P0, a, ra), coin(R::P1, b, rb), tang(a), tang(b) },
                { coin(R::P0, a, ra), coin(R::P1, b, rb), tang(a) },
                { coin(R::P0, a, ra), coin(R::P1, b, rb) },
            };
            for (const auto& set : ladder) if (try_add_constraints(set)) break;
        } else {
            try_add_constraints({ coin(R::P0, a, ra), coin(R::P1, b, rb) });
        }
    } else if (m_mode == Mode::Offset) {
        const int a = m_op_a;
        auto out = SketchEngine::offset_entities({ m_entities[a] }, m_op_value);
        if (out.empty()) { reset_op(); return; }
        const int ni = int(m_entities.size());
        for (auto& o : out) m_entities.push_back(o);
        const SketchEntity::Type st = m_entities[a].type;
        SketchEntityConstraintDef d; d.ea = a; d.eb = ni;
        bool emit = true;
        if (st == SketchEntity::Type::Line)                                   d.type = CT::Parallel;
        else if (st == SketchEntity::Type::Arc || st == SketchEntity::Type::Circle) d.type = CT::Concentric;
        else                                                                  emit = false;
        if (emit) try_add_constraints({ d });
    } else if (m_mode == Mode::Mirror) {
        const SketchEntity axis = m_entities[m_op_a];   // by value (m_entities grows below)
        for (int ti : m_mirror_targets) {
            if (ti < 0 || ti >= int(m_entities.size())) continue;
            auto out = SketchEngine::mirror_entities({ m_entities[ti] }, axis.p0, axis.p1);
            if (out.empty()) continue;
            const int mi = int(m_entities.size());
            for (auto& m : out) m_entities.push_back(m);
            SketchEntityConstraintDef d; d.type = CT::Symmetric; d.ea = ti; d.eb = mi; d.ec = m_op_a;
            const SketchEntity::Type st = m_entities[ti].type;
            std::vector<SketchEntityConstraintDef> cand;
            if (st == SketchEntity::Type::Line) {
                d.ra = R::P0; d.rb = R::P0; cand.push_back(d);
                d.ra = R::P1; d.rb = R::P1; cand.push_back(d);
            } else if (st == SketchEntity::Type::Arc || st == SketchEntity::Type::Circle) {
                d.ra = R::Center; d.rb = R::Center; cand.push_back(d);
            } else if (st == SketchEntity::Type::Point) {
                d.ra = R::P0; d.rb = R::P0; cand.push_back(d);
            }
            if (!cand.empty()) try_add_constraints(cand);
        }
    }
    reset_op();
    m_selection.clear();
    resolve_live();
    if (on_selection_changed) on_selection_changed(0);
}

// ---- In-canvas transform gizmo (Move/Rotate/Scale/Array/PolarArray) ----
// These replace the docked numeric cards: pick subject entities in-canvas, then a single
// draggable handle drives the continuous parameter (Move/Array offset, Rotate/Polar angle,
// Scale factor) and an editable value label sets it exactly; Array/PolarArray expose a
// second label for the copy count. A live translucent ghost previews the result. Confirm
// applies the geometry and emits the per-op constraint web (mutating ops drop the classes
// the map invalidates; additive ops bind each copy to its source) into m_constraints.

void DesignSketchTool::reset_tf()
{
    m_tf_targets.clear();
    m_tf_pivot = Vec2d(0, 0);
    m_tf_delta = Vec2d(0, 0);
    m_tf_angle = 0.0;
    m_tf_scale = 1.0;
    m_tf_count = 3;
    m_tf_handle_r = 1.0;
    m_tf_ghost.clear();
    m_tf_handle = -1;
    m_tf_dragging = false;
    m_tf_label_a = Vec2d(1e18, 1e18);
    m_tf_label_b = Vec2d(1e18, 1e18);
}

bool DesignSketchTool::tf_ready() const { return !m_tf_targets.empty(); }

// Centroid of the picked subject set (the rotate/scale/polar pivot, and the array origin),
// plus a reference radius (max distance from the pivot to any subject extremum) used to
// size the rotate/polar handle ring and the scale handle's unit position.
void DesignSketchTool::compute_tf_pivot()
{
    using T = SketchEntity::Type;
    auto cen = [](const SketchEntity& e) -> Vec2d {
        switch (e.type) {
        case T::Line: return 0.5 * (e.p0 + e.p1);
        case T::Arc: case T::Circle: case T::Ellipse: case T::EllipseArc: return e.center;
        case T::BSpline:
            if (!e.ctrl.empty()) { Vec2d s(0, 0); for (const auto& p : e.ctrl) s += p; return s / double(e.ctrl.size()); }
            return 0.5 * (e.p0 + e.p1);
        default: return e.p0;
        }
    };
    Vec2d c(0, 0); int n = 0;
    for (int ti : m_tf_targets)
        if (ti >= 0 && ti < int(m_entities.size())) { c += cen(m_entities[ti]); ++n; }
    if (n == 0) { m_tf_pivot = Vec2d(0, 0); m_tf_handle_r = 1.0; return; }
    m_tf_pivot = c / double(n);
    double r = 0.0;
    for (int ti : m_tf_targets) {
        if (ti < 0 || ti >= int(m_entities.size())) continue;
        const SketchEntity& e = m_entities[ti];
        auto upd = [&](const Vec2d& p) { r = std::max(r, (p - m_tf_pivot).norm()); };
        switch (e.type) {
        case T::Line: upd(e.p0); upd(e.p1); break;
        case T::Arc: case T::Circle: case T::Ellipse: case T::EllipseArc:
            upd(e.center + Vec2d(e.radius, 0)); upd(e.center - Vec2d(e.radius, 0)); break;
        case T::BSpline: for (const auto& p : e.ctrl) upd(p); break;
        default: upd(e.p0); break;
        }
    }
    m_tf_handle_r = std::max(r, 1.0);
}

void DesignSketchTool::tf_pick(int ei)
{
    if (ei < 0 || ei >= int(m_entities.size())) return;
    auto it = std::find(m_tf_targets.begin(), m_tf_targets.end(), ei);
    if (it == m_tf_targets.end()) m_tf_targets.push_back(ei);   // toggle-select like Mirror
    else                          m_tf_targets.erase(it);
    compute_tf_pivot();
    // Seed sensible starting parameters (mirrors the retired card defaults so the ghost is
    // immediately visible). Only seed while still at the neutral value, so re-picking more
    // targets keeps a value the user already dialled in.
    if (!m_tf_targets.empty()) {
        const double step = std::max(m_tf_handle_r * 1.5, 1.0);
        switch (m_mode) {
        case Mode::Move:
            if (m_tf_delta.norm() < 1e-9) m_tf_delta = Vec2d(step, 0.0);
            break;
        case Mode::Array:
            if (m_tf_delta.norm() < 1e-9) {
                Vec2d d(step, 0.0);   // default: perpendicular to a single line, else +X
                if (m_tf_targets.size() == 1) {
                    const SketchEntity& e = m_entities[m_tf_targets[0]];
                    if (e.type == SketchEntity::Type::Line) {
                        Vec2d t = e.p1 - e.p0;
                        if (t.norm() > 1e-9) { t.normalize(); d = Vec2d(-t.y(), t.x()) * step; }
                    }
                }
                m_tf_delta = d;
            }
            break;
        case Mode::Rotate:     if (std::abs(m_tf_angle) < 1e-9) m_tf_angle = M_PI / 4.0; break;       // 45°
        case Mode::PolarArray: if (std::abs(m_tf_angle) < 1e-9) m_tf_angle = 2.0 * M_PI;  break;       // 360°
        case Mode::Scale:      if (std::abs(m_tf_scale - 1.0) < 1e-9) m_tf_scale = 2.0;   break;
        default: break;
        }
    }
    recompute_tf_ghost();
    m_selection = m_tf_targets;   // reuse the existing selection highlight
    if (on_selection_changed) on_selection_changed(int(m_selection.size()));
}

void DesignSketchTool::recompute_tf_ghost()
{
    m_tf_ghost.clear();
    if (m_tf_targets.empty()) return;
    std::vector<SketchEntity> src;
    for (int ti : m_tf_targets)
        if (ti >= 0 && ti < int(m_entities.size())) src.push_back(m_entities[ti]);
    if (src.empty()) return;
    const int count = std::max(2, m_tf_count);
    switch (m_mode) {
    case Mode::Move:
        m_tf_ghost = SketchEngine::transform_entities(src, m_tf_delta, 0.0, 1.0, Vec2d(0, 0));
        break;
    case Mode::Rotate:
        m_tf_ghost = SketchEngine::transform_entities(src, Vec2d(0, 0), m_tf_angle, 1.0, m_tf_pivot);
        break;
    case Mode::Scale:
        m_tf_ghost = SketchEngine::transform_entities(src, Vec2d(0, 0), 0.0, m_tf_scale, m_tf_pivot);
        break;
    case Mode::Array:
        m_tf_ghost = SketchEngine::array_entities(src, count, m_tf_delta, 0.0, m_tf_pivot);
        break;
    case Mode::PolarArray:
        m_tf_ghost = SketchEngine::array_entities(src, count, Vec2d(0, 0), m_tf_angle / double(count), m_tf_pivot);
        break;
    default: break;
    }
}

// World position of the single drag handle: at the translated/spacing tip for the linear
// ops, on the pivot-centred ring at the current angle for the rotational ops, and at the
// scaled unit position along +X for Scale.
Vec2d DesignSketchTool::tf_handle_pos() const
{
    switch (m_mode) {
    case Mode::Move:
    case Mode::Array:      return m_tf_pivot + m_tf_delta;
    case Mode::Rotate:
    case Mode::PolarArray: return m_tf_pivot + m_tf_handle_r * Vec2d(std::cos(m_tf_angle), std::sin(m_tf_angle));
    case Mode::Scale:      return m_tf_pivot + Vec2d(m_tf_scale * m_tf_handle_r, 0.0);
    default:               return m_tf_pivot;
    }
}

bool DesignSketchTool::hit_test_tf_handle(const Vec2d& p, double tol) const
{
    if (!tf_ready()) return false;
    return (tf_handle_pos() - p).norm() <= tol * 2.5;
}

void DesignSketchTool::drag_tf_handle(const Vec2d& target)
{
    switch (m_mode) {
    case Mode::Move:
    case Mode::Array:
        m_tf_delta = target - m_tf_pivot;
        break;
    case Mode::Rotate:
    case Mode::PolarArray: {
        const Vec2d d = target - m_tf_pivot;
        if (d.norm() > 1e-9) m_tf_angle = std::atan2(d.y(), d.x());
        break;
    }
    case Mode::Scale: {
        const double r = (target - m_tf_pivot).norm();
        m_tf_scale = std::max(1e-3, r / std::max(m_tf_handle_r, 1e-9));
        break;
    }
    default: break;
    }
    recompute_tf_ghost();
}

void DesignSketchTool::open_tf_editor_a()
{
    if (!on_inline_edit || !tf_ready()) return;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    double cur;
    if (m_mode == Mode::Rotate || m_mode == Mode::PolarArray) cur = std::abs(m_tf_angle) * 180.0 / M_PI;
    else if (m_mode == Mode::Scale)                           cur = m_tf_scale;
    else                                                       cur = m_tf_delta.norm();
    Vec2d dir = m_tf_delta; if (dir.norm() > 1e-9) dir.normalize(); else dir = Vec2d(1, 0);
    const double sgn = (m_tf_angle < 0) ? -1.0 : 1.0;
    on_inline_edit(px, cur, "",
        [this, dir, sgn](double v) {
            switch (m_mode) {
            case Mode::Move: case Mode::Array:        m_tf_delta = dir * v; break;
            case Mode::Rotate: case Mode::PolarArray: m_tf_angle = sgn * std::abs(v) * M_PI / 180.0; break;
            case Mode::Scale:                         m_tf_scale = std::max(1e-3, v); break;
            default: break;
            }
            recompute_tf_ghost();
        },
        []() {});
}

void DesignSketchTool::open_tf_editor_count()
{
    if (!on_inline_edit || !tf_ready()) return;
    if (m_mode != Mode::Array && m_mode != Mode::PolarArray) return;
    const wxPoint px(m_last_mouse_x, m_last_mouse_y);
    on_inline_edit(px, double(std::max(2, m_tf_count)), "",
        [this](double v) { m_tf_count = std::max(2, int(v + 0.5)); recompute_tf_ghost(); },
        []() {});
}

void DesignSketchTool::render_tf_gizmo(double unit_per_px)
{
    m_tf_label_a = Vec2d(1e18, 1e18);
    m_tf_label_b = Vec2d(1e18, 1e18);
    if (!tf_ready()) return;
    const ColorRGBA ghostc(0.30f, 0.88f, 0.66f, 0.55f);
    draw_entities_preview(m_tf_ghost, ghostc);

    const ColorRGBA dc(0.30f, 0.88f, 0.66f, 1.0f);
    const ColorRGBA hot(1.0f, 0.85f, 0.2f, 1.0f);
    const double th = std::max(15.0 * unit_per_px, 1e-4);
    const Vec2d handle = tf_handle_pos();

    // spoke from the pivot to the handle (+ arrowhead for the linear ops).
    std::vector<std::pair<Vec2d, Vec2d>> segs;
    segs.emplace_back(m_tf_pivot, handle);
    if (m_mode == Mode::Move || m_mode == Mode::Array || m_mode == Mode::Scale) {
        Vec2d dir = handle - m_tf_pivot; const double L = dir.norm();
        if (L > 1e-9) {
            dir /= L;
            const double as = std::max(L * 0.15, th * 0.8);
            const Vec2d nrm(-dir.y(), dir.x());
            const Vec2d back = handle - dir * as;
            segs.emplace_back(handle, back + nrm * (as * 0.5));
            segs.emplace_back(handle, back - nrm * (as * 0.5));
        }
    }
    draw_strokes(m_highlight_model, segs, 0.6, dc);

    const double hs = 6.0 * unit_per_px;   // screen-constant handle marker
    const std::vector<Vec2d> sq = { handle + Vec2d(-hs, -hs), handle + Vec2d(hs, -hs),
                                    handle + Vec2d(hs, hs),   handle + Vec2d(-hs, hs) };
    draw_fill(m_fill_model, sq, m_tf_dragging ? hot : dc);
    draw_vertices(m_vertex_model, { m_tf_pivot }, dc, std::max(3.0 * unit_per_px, 1e-4));

    // primary parameter label at the handle.
    std::string txt_a;
    if (m_mode == Mode::Rotate || m_mode == Mode::PolarArray) {
        DimAnnot a; a.kind = DimType::Angle; a.value = std::abs(m_tf_angle) * 180.0 / M_PI;
        txt_a = dim_text(a);
    } else if (m_mode == Mode::Scale) {
        char b[24]; std::snprintf(b, sizeof(b), "x%.2f", m_tf_scale);
        for (char& ch : b) if (ch == ',') ch = '.';
        txt_a = b;
    } else {
        DimAnnot a; a.kind = DimType::Distance; a.value = m_tf_delta.norm();
        txt_a = dim_text(a);
    }
    Vec2d outw = handle - m_tf_pivot; if (outw.norm() > 1e-9) outw.normalize(); else outw = Vec2d(1, 0);
    m_tf_label_a = handle + outw * (th * 1.2);
    draw_text(m_line_model, txt_a, m_tf_label_a, th, dc);

    if (m_mode == Mode::Array || m_mode == Mode::PolarArray) {
        char cb[16]; std::snprintf(cb, sizeof(cb), "x%d", std::max(2, m_tf_count));
        m_tf_label_b = m_tf_pivot + Vec2d(th * 1.5, th * 1.5);
        draw_text(m_line_model, cb, m_tf_label_b, th, dc);
    }
}

void DesignSketchTool::confirm_transform()
{
    if (!tf_ready()) { reset_tf(); return; }
    using CT = SketchConstraintType;
    const std::vector<int> targets = m_tf_targets;   // snapshot by value (m_entities grows)
    auto is_target = [&](int e) {
        return e >= 0 && std::find(targets.begin(), targets.end(), e) != targets.end();
    };

    if (m_mode == Mode::Move || m_mode == Mode::Rotate || m_mode == Mode::Scale) {
        // MUTATING: map every subject in place, then drop the constraint classes the map
        // invalidates for any constraint touching a subject. Surviving classes are
        // satisfied by construction; the re-solve folds in the new placement.
        const Mode mode = m_mode;
        for (int ti : targets) {
            if (ti < 0 || ti >= int(m_entities.size())) continue;
            std::vector<SketchEntity> out;
            if (mode == Mode::Move)
                out = SketchEngine::transform_entities({ m_entities[ti] }, m_tf_delta, 0.0, 1.0, Vec2d(0, 0));
            else if (mode == Mode::Rotate)
                out = SketchEngine::transform_entities({ m_entities[ti] }, Vec2d(0, 0), m_tf_angle, 1.0, m_tf_pivot);
            else
                out = SketchEngine::transform_entities({ m_entities[ti] }, Vec2d(0, 0), 0.0, m_tf_scale, m_tf_pivot);
            if (!out.empty()) m_entities[ti] = out[0];
        }
        auto& cs = m_constraints;
        cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const SketchEntityConstraintDef& d) {
            if (!(is_target(d.ea) || is_target(d.eb) || is_target(d.ec))) return false;
            const bool self = (d.ea == d.eb);   // self-length Distance survives translate/rotate
            if (mode == Mode::Move) {
                switch (d.type) {
                case CT::Coincident: case CT::PointOnLine: case CT::PointOnObject:
                case CT::Concentric: case CT::Symmetric:   case CT::Midpoint:
                case CT::Fix:        case CT::LockX:        case CT::LockY:   return true;
                case CT::Distance: return !self;
                default:           return false;   // orientation/length preserved by translation
                }
            } else if (mode == Mode::Rotate) {
                switch (d.type) {
                case CT::EqualLength: case CT::Radius: case CT::Diameter: return false;
                case CT::Distance: return !self;
                default:           return true;    // orientation + position broken by rotation
                }
            } else {   // Scale (uniform / conformal)
                switch (d.type) {
                case CT::Horizontal: case CT::Vertical: case CT::Parallel:
                case CT::Perpendicular: case CT::Angle: return false;
                default:                                return true;   // size + position broken
                }
            }
        }), cs.end());
    } else if (m_mode == Mode::Array || m_mode == Mode::PolarArray) {
        // ADDITIVE: append copies of each subject, then bind each copy to its source. Lines
        // get Parallel+EqualLength (linear) or EqualLength only (polar — rotation breaks
        // Parallel); arc/circle copies get a per-copy Radius (equal-radius under any rigid
        // map) plus Concentric when the polar pivot is the source's own centre. Emit each
        // web as a degrade ladder (try_add_constraints keeps the first set the solver
        // accepts, else the geometry stays unconstrained).
        const bool polar = (m_mode == Mode::PolarArray);
        const int  count = std::max(2, m_tf_count);
        for (int ti : targets) {
            if (ti < 0 || ti >= int(m_entities.size())) continue;
            const SketchEntity src = m_entities[ti];   // by value (m_entities grows below)
            std::vector<SketchEntity> copies = polar
                ? SketchEngine::array_entities({ src }, count, Vec2d(0, 0), m_tf_angle / double(count), m_tf_pivot)
                : SketchEngine::array_entities({ src }, count, m_tf_delta, 0.0, m_tf_pivot);
            if (copies.empty()) continue;
            const int base = int(m_entities.size());
            for (auto& c : copies) m_entities.push_back(c);
            const int nc = int(copies.size());
            const SketchEntity::Type st = src.type;
            std::vector<std::vector<SketchEntityConstraintDef>> ladder;
            if (st == SketchEntity::Type::Line) {
                auto mk = [&](bool eq, bool par) {
                    std::vector<SketchEntityConstraintDef> w;
                    for (int k = 0; k < nc; ++k) {
                        const int ci = base + k;
                        if (par) { SketchEntityConstraintDef dp; dp.type = CT::Parallel;    dp.ea = ti; dp.eb = ci; w.push_back(dp); }
                        if (eq)  { SketchEntityConstraintDef de; de.type = CT::EqualLength;  de.ea = ti; de.eb = ci; w.push_back(de); }
                    }
                    return w;
                };
                if (polar) ladder = { mk(true, false) };
                else       ladder = { mk(true, true), mk(false, true) };
            } else if (st == SketchEntity::Type::Arc || st == SketchEntity::Type::Circle) {
                const bool can_conc = polar && (m_tf_pivot - src.center).norm() < 1e-6;
                auto mk = [&](bool conc) {
                    std::vector<SketchEntityConstraintDef> w;
                    for (int k = 0; k < nc; ++k) {
                        const int ci = base + k;
                        SketchEntityConstraintDef dr; dr.type = CT::Radius; dr.ea = ci; dr.value = src.radius; w.push_back(dr);
                        if (conc) {
                            SketchEntityConstraintDef dco; dco.type = CT::Concentric;
                            dco.ea = ti; dco.ra = SketchPointRole::Center; dco.eb = ci; dco.rb = SketchPointRole::Center;
                            w.push_back(dco);
                        }
                    }
                    return w;
                };
                ladder = can_conc ? std::vector<std::vector<SketchEntityConstraintDef>>{ mk(true), mk(false) }
                                  : std::vector<std::vector<SketchEntityConstraintDef>>{ mk(false) };
            }
            for (auto& w : ladder) if (!w.empty() && try_add_constraints(w)) break;
        }
    }
    reset_tf();
    m_selection.clear();
    resolve_live();
    if (on_selection_changed) on_selection_changed(0);
}

const ColorRGBA* DesignSketchTool::sketch_hl_color(int feature) const
{
    for (const auto& h : m_hl_sketches) if (h.first == feature) return &h.second;
    return nullptr;
}

void DesignSketchTool::render(GLCanvas3D& canvas)
{
    m_dim_label_seq = 0;
    m_render_scale  = canvas.get_scale();
    (void)canvas;
    if (!has_display()) {
        if (on_readout) on_readout(std::string());   // nothing to show -> hide HUD
        return;
    }
    render_view_helpers();   // origin planes / world axes — drawn whenever their toggle is on
    m_rubber.render(canvas); // left-drag rubber band (no-op unless one is being swept). Drawn
                             // here, ahead of every early return below, so a band over an empty
                             // plate is still visible.
    if (m_active && m_mode != Mode::Constrain && m_entities.empty() && m_points.empty()
        && m_display_sketches.empty()) {
        if (on_readout) on_readout(std::string());
        return;
    }


    // Draw-then-edit: a creation tool that just committed a new entity/feature (gesture now
    // idle) gets its result auto-selected — so render_live_quotes below computes its quotes —
    // and the primary value editor armed (opened after those quotes exist, see service block).
    if (m_active && is_creation_autoedit_mode() && m_points.empty() &&
        m_open_feature < 0 && !m_awaiting_length) {
        const int n = int(m_entities.size());
        if (m_autoedit_seen >= 0 && n > m_autoedit_seen && n > 0) {
            m_selection.clear();
            m_selection.push_back(n - 1);   // feature_of(last) groups rect/slot/poly/ellipse
            m_autoedit_pending = true;
        }
        m_autoedit_seen = n;
    }

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    shader->start_using();
    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    // Persistent committed sketches (e.g. an un-consumed sketch left visible after its
    // extrude is removed): faces translucent, outlines orange. Each uses its own plane.
    if (!m_display_sketches.empty()) {
        const SketchPlane saved_plane = m_plane;
        // CYAN/BLUE MEANS SELECTED — nothing else may wear it. The unselected fill used to be
        // (0.30,0.60,1.0), one shade off the selected (0.30,0.80,1.0), so an ordinary region
        // read as picked and a picked one added nothing. The outlines already got this right:
        // orange for a sketch, cyan for the selection. The fill now follows the same logic, so
        // an unselected region is faint amber — the sketch's own colour — and every blue thing
        // on screen is something you selected.
        const ColorRGBA dface = design_idle_face_color();   // unselected: neutral grey, never the selection colour
        const ColorRGBA sface = design_selection_color(0.34f);   // selected region
        const ColorRGBA dwire(1.0f, 0.55f, 0.1f, 1.0f);     // normal orange outline
        const ColorRGBA swire = design_selection_color();        // selected outline
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        for (const DisplaySketch& ds : m_display_sketches) {
            m_plane = ds.plane;
            const std::vector<RegionLoop> loops = region_loops(ds.entities);
            for (int r = 0; r < int(loops.size()); ++r) {
                const bool sel = (ds.feature == m_display_pick && r == m_display_pick_region);
                const ColorRGBA* hlc = sketch_hl_color(ds.feature);
                ColorRGBA fc = sel ? sface : dface;
                if (hlc && !sel) { fc = *hlc; fc.a(0.20f); }
                std::vector<std::vector<Vec2d>> hp;
                for (int h : loops[r].holes)
                    if (h >= 0 && h < int(loops.size())) hp.push_back(loops[h].poly);
                draw_fill_holed(m_fill_model, loops[r].poly, hp, fc);
            }
        }
        glsafe(::glDisable(GL_BLEND));
        for (const DisplaySketch& ds : m_display_sketches) {
            m_plane = ds.plane;
            // Entities forming the selected loop (highlighted cyan); the rest stay orange.
            std::vector<char> sel_ent(ds.entities.size(), 0);
            if (ds.feature == m_display_pick && m_display_pick_region >= 0) {
                const std::vector<RegionLoop> loops = region_loops(ds.entities);
                if (m_display_pick_region < int(loops.size())) {
                    // The holes light up with their region. The highlight has to show what will
                    // be EXTRUDED, and a hole is part of that — a plate whose bore stays orange
                    // while its outline turns cyan would say the circle is not coming along,
                    // which is precisely the thing that used to be true and is now not.
                    auto mark = [&](int region) {
                        if (region < 0 || region >= int(loops.size())) return;
                        for (int ei : loops[region].ents)
                            if (ei >= 0 && ei < int(sel_ent.size())) sel_ent[ei] = 1;
                    };
                    mark(m_display_pick_region);
                    for (int h : loops[m_display_pick_region].holes) mark(h);
                }
            }
            for (int i = 0; i < int(ds.entities.size()); ++i) {
                const SketchEntity& e = ds.entities[i];
                if (e.type == SketchEntity::Type::Point) continue;
                bool closed = false;
                std::vector<Vec2d> poly = entity_polyline(e, closed);
                const ColorRGBA* hlc = sketch_hl_color(ds.feature);
                ColorRGBA wc = sel_ent[i] ? swire : (hlc ? *hlc : dwire);
                draw_quad_strip(m_line_model, poly, closed, wc);
            }
        }
        m_plane = saved_plane;
    }

    // Nothing else to draw when no live sketch session is active — except the solid
    // face/edge highlight overlay (whole-solid tint is handled by set_body_highlight).
    if (!m_active) {
        render_datum_planes();
        render_mate_connectors();
        render_solid_highlight();
        if (m_dbp_active) render_base_pick();
        if (m_dz_active) render_datum_gizmo();
        if (m_hx_active) render_helix_gizmo();
        if (m_rb_active) render_rib_gizmo();
        if (m_ex_active) render_extrude_gizmo();
        if (m_mv_active) render_move_gizmo();
        if (m_fl_active) render_fillet_gizmo();
        if (m_hl_active) render_hole_gizmo();
        if (m_th_active) render_thread_gizmo();
        if (m_sh_active) render_shell_gizmo();
        if (m_rv_active) render_revolve_gizmo();
        if (m_dr_active) render_draft_gizmo();
        if (m_ct_active) render_cut_gizmo();
        if (m_pt_active) render_pattern_gizmo();
        shader->stop_using();
        glsafe(::glEnable(GL_CULL_FACE));
        glsafe(::glEnable(GL_DEPTH_TEST));
        return;
    }

    // Imported-art transform: only the bbox + handles over the (display-overlay) art.
    if (m_mode == Mode::TransformArt) {
        render_xform_gizmo();
        shader->stop_using();
        glsafe(::glEnable(GL_CULL_FACE));
        glsafe(::glEnable(GL_DEPTH_TEST));
        return;
    }

    const ColorRGBA orange(1.0f, 0.55f, 0.1f, 1.0f);
    const ColorRGBA yellow(1.0f, 0.85f, 0.2f, 1.0f);
    const ColorRGBA grey(0.55f, 0.55f, 0.60f, 1.0f);

    if (m_mode == Mode::Constrain) {
        const ColorRGBA cyan(0.30f, 0.80f, 1.0f, 1.0f);
        const ColorRGBA red(1.0f, 0.25f, 0.25f, 1.0f);
        if (m_constrain_entities) {
            // Draw all entities cyan; picked Line entities highlighted red.
            std::vector<Vec2d> markers;
            for (size_t i = 0; i < m_entities.size(); ++i) {
                const SketchEntity& e = m_entities[i];
                const bool sel = (int(i) == m_pick0 || int(i) == m_pick1 || int(i) == m_pick2);
                // Constraint-manager highlight: the entities a selected constraint
                // references glow yellow (picked entities still win as red).
                const bool hl = !sel && std::find(m_constraint_hl.begin(), m_constraint_hl.end(),
                                                   int(i)) != m_constraint_hl.end();
                const ColorRGBA col = sel ? red : (hl ? yellow : cyan);
                if (e.type == SketchEntity::Type::Point) { markers.push_back(e.p0); continue; }
                bool closed = false;
                std::vector<Vec2d> poly = entity_polyline(e, closed);
                draw_quad_strip((sel || hl) ? m_highlight_model : m_line_model, poly, closed, col);
            }
            if (!markers.empty())
                draw_vertices(m_vertex_model, markers, cyan);
            // Constraint badges (C3.4b): iconic glyphs near each constraint's entity.
            {
                const double upp = 1.0 / std::max(camera.get_zoom(), 1e-6);
                std::vector<std::pair<Vec2d, Vec2d>> glyphs;
                build_constraint_glyphs(upp, glyphs);
                if (!glyphs.empty()) {
                    const ColorRGBA badge(0.45f, 0.95f, 0.70f, 1.0f);   // CAD teal-green
                    draw_strokes(m_fill_model, glyphs, std::max(0.9 * upp, 1e-4), badge);
                }
            }
            shader->stop_using();
            glsafe(::glEnable(GL_CULL_FACE));
            glsafe(::glEnable(GL_DEPTH_TEST));
            return;
        }
        draw_quad_strip(m_line_model, m_points, true, cyan);
        draw_vertices(m_vertex_model, m_points, cyan);
        if (m_sel_a >= 0 && m_sel_b >= 0 &&
            m_sel_a < int(m_points.size()) && m_sel_b < int(m_points.size())) {
            std::vector<Vec2d> seg = { m_points[m_sel_a], m_points[m_sel_b] };
            draw_quad_strip(m_highlight_model, seg, false, red);
        }
        shader->stop_using();
        glsafe(::glEnable(GL_CULL_FACE));
        glsafe(::glEnable(GL_DEPTH_TEST));
        return;
    }

    // Closed loops fill as translucent faces (the "closed loop = selectable face"
    // affordance). Drawn first so the entity outlines paint over the fill.
    {
        // region_loops(), NOT closed_regions(): the latter returns raw polygons with no notion
        // of nesting, so a circle drawn inside a rectangle was filled as its own solid disc on
        // top of a solid rectangle. That is why a live sketch still showed a filled blue circle
        // however often the COMMITTED renderer below was corrected — these are two separate
        // renderers and only one of them had been taught about holes.
        const std::vector<RegionLoop> loops = region_loops(m_entities);
        if (!loops.empty()) {
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            // A bore is not a face. Skip loops that are somebody's hole, and cut those holes out
            // of the region that owns them, so a plate with a hole LOOKS like one while drawing.
            std::vector<char> is_hole(loops.size(), 0);
            for (const RegionLoop& L : loops)
                for (int h : L.holes)
                    if (h >= 0 && h < int(is_hole.size())) is_hole[h] = 1;
            for (size_t r = 0; r < loops.size(); ++r) {
                if (is_hole[r]) continue;
                std::vector<std::vector<Vec2d>> hp;
                for (int h : loops[r].holes)
                    if (h >= 0 && h < int(loops.size())) hp.push_back(loops[h].poly);
                draw_fill_holed(m_fill_model, loops[r].poly, hp, design_idle_face_color());
            }
            glsafe(::glDisable(GL_BLEND));
        }
    }

    // Committed entities of this session. DoF feedback (P3): a fully-constrained
    // sketch (dof==0, consistent) paints green; entities touched by a conflicting
    // constraint paint red; otherwise the under-constrained default (orange / grey
    // construction). Selected entities always override to white.
    const ColorRGBA white(1.0f, 1.0f, 1.0f, 1.0f);
    const ColorRGBA green(0.30f, 0.85f, 0.42f, 1.0f);
    const ColorRGBA conflict(1.0f, 0.22f, 0.22f, 1.0f);
    const ColorRGBA editing(1.0f, 0.78f, 0.10f, 1.0f);   // amber: entity whose dim is being typed
    const bool fully = (m_dof == 0 && m_solve_ok);
    // While an auto-edit value field is open, the active step names the entities its dimension
    // drives — light them up so it's obvious WHICH feature the number (e.g. a circle's radius)
    // changes.
    const std::vector<int>* edit_hi =
        (m_autoedit_dim_idx >= 0 && m_autoedit_dim_idx < int(m_autoedit_dims.size()))
            ? &m_autoedit_dims[m_autoedit_dim_idx].hi : nullptr;
    std::vector<Vec2d> point_markers, sel_point_markers;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const SketchEntity& e = m_entities[i];
        const bool selected =
            std::find(m_selection.begin(), m_selection.end(), int(i)) != m_selection.end();
        const bool editing_this =
            edit_hi && std::find(edit_hi->begin(), edit_hi->end(), int(i)) != edit_hi->end();
        const bool bad = i < m_entity_conflict.size() && m_entity_conflict[i];
        ColorRGBA col;
        if (editing_this)        col = editing;
        else if (selected)       col = white;
        else if (bad)            col = conflict;
        else if (e.construction) col = grey;
        else                     col = fully ? green : orange;
        if (e.type == SketchEntity::Type::Point) {
            (selected ? sel_point_markers : point_markers).push_back(e.p0);
            continue;
        }
        bool closed = false;
        std::vector<Vec2d> poly = entity_polyline(e, closed);
        draw_quad_strip((selected || editing_this) ? m_highlight_model : m_line_model, poly, closed, col);
    }
    if (!point_markers.empty())
        draw_vertices(m_vertex_model, point_markers, yellow);
    if (!sel_point_markers.empty())
        draw_vertices(m_highlight_model, sel_point_markers, white);

    // Endpoint / centre handles so individual points are visible and pickable in the
    // Select and Dimension tools (a line = a segment + 2 points). Selected ones white.
    m_show_handles = (m_mode == Mode::Select || m_mode == Mode::Dimension);
    if (m_show_handles) {
        std::vector<Vec2d> handles, sel_handles;
        auto add_h = [&](int ei, SketchPointRole r, const Vec2d& q) {
            const bool s = std::find(m_point_sel.begin(), m_point_sel.end(),
                                     std::make_pair(ei, r)) != m_point_sel.end();
            (s ? sel_handles : handles).push_back(q);
        };
        for (size_t i = 0; i < m_entities.size(); ++i) {
            const SketchEntity& e = m_entities[i];
            switch (e.type) {
            case SketchEntity::Type::Line:
                add_h(int(i), SketchPointRole::P0, e.p0);
                add_h(int(i), SketchPointRole::P1, e.p1);
                break;
            case SketchEntity::Type::Arc:
            case SketchEntity::Type::EllipseArc:
                add_h(int(i), SketchPointRole::P0, e.p0);
                add_h(int(i), SketchPointRole::P1, e.p1);
                add_h(int(i), SketchPointRole::Center, e.center);
                break;
            case SketchEntity::Type::Circle:
            case SketchEntity::Type::Ellipse:
                add_h(int(i), SketchPointRole::Center, e.center);
                break;
            case SketchEntity::Type::BSpline:
                add_h(int(i), SketchPointRole::P0, e.p0);
                add_h(int(i), SketchPointRole::P1, e.p1);
                break;
            case SketchEntity::Type::Point:
                break;   // its own marker is drawn above
            }
        }
        if (!handles.empty())     draw_vertices(m_vertex_model, handles, ColorRGBA(0.65f, 0.65f, 0.30f, 1.0f));
        if (!sel_handles.empty()) draw_vertices(m_highlight_model, sel_handles, white);

        // Derived feature handles (A3): the circle RadiusHandle is not a SketchPointRole,
        // so the per-point pass above doesn't draw it. Render it (cyan) + the hovered
        // handle (white, larger) at a screen-constant size so they stay grabbable at any
        // zoom. A4 makes these draggable; later phases add slot/rect/polygon handles.
        const double upp = 1.0 / std::max(camera.get_zoom(), 1e-6);
        std::vector<Vec2d> radius_h;
        for (const Handle& h : build_handles())
            if (h.role == HandleRole::RadiusHandle || h.role == HandleRole::MajorAxis ||
                h.role == HandleRole::MinorAxis    || h.role == HandleRole::BSplineCtrl)
                radius_h.push_back(h.pos);
        if (!radius_h.empty())
            draw_vertices(m_vertex_model, radius_h, ColorRGBA(0.30f, 0.75f, 0.95f, 1.0f),
                          std::max(4.0 * upp, 1e-4));
        if (m_has_hover_handle)
            draw_vertices(m_highlight_model, { m_hover_handle.pos }, white,
                          std::max(5.5 * upp, 1e-4));
    }

    // Placed dimension quotes (drawn in every mode so they persist while sketching).
    // Pass plane-units-per-pixel so labels keep a constant on-screen size.
    render_dimensions(1.0 / std::max(camera.get_zoom(), 1e-6));
    render_live_quotes(1.0 / std::max(camera.get_zoom(), 1e-6));
    // Draw-then-edit: the selection's live quotes now exist. Open the primary value editor on
    // the next event-loop tick (NOT here inside the paint) so the floating field grabs focus
    // cleanly — the same context the Line path opens from. m_live_quotes persists until the
    // next render_live_quotes(), so the deferred open still sees this frame's values.
    if (m_autoedit_pending) {
        m_autoedit_pending = false;
        wxGetApp().CallAfter([this] { open_primary_autoedit(); });
    }
    if (is_edit_op_mode())
        render_op_gizmo(1.0 / std::max(camera.get_zoom(), 1e-6));
    if (is_transform_mode())
        render_tf_gizmo(1.0 / std::max(camera.get_zoom(), 1e-6));

    // In-progress entity preview for the active tool.
    const ColorRGBA preview = m_construction ? grey : orange;
    switch (m_mode) {

    case Mode::Select:
    case Mode::Dimension:
        break;   // selection highlight / placed quotes are drawn above; no rubber-band

    case Mode::Polyline: {
        std::vector<Vec2d> pts = m_points;
        if (m_has_cursor)
            pts.push_back(m_cursor);
        draw_quad_strip(m_highlight_model, pts, false, preview);
        draw_vertices(m_vertex_model, m_points, yellow);
        // Teal rubber-band = the new segment is locked to an inference angle.
        if (m_cursor_locked && m_has_cursor && !m_points.empty()) {
            const ColorRGBA lock(0.10f, 0.85f, 0.80f, 1.0f);
            std::vector<Vec2d> seg = { m_points.back(), m_cursor };
            draw_quad_strip(m_line_model, seg, false, lock);
        }
        break;
    }

    case Mode::Line: {
        std::vector<Vec2d> pts = m_points;
        if (m_has_cursor && m_points.size() == 1)
            pts.push_back(m_cursor);
        if (pts.size() >= 2) {
            const ColorRGBA col = (m_cursor_locked && m_points.size() == 1)
                                      ? ColorRGBA(0.10f, 0.85f, 0.80f, 1.0f) : preview;
            draw_quad_strip(m_highlight_model, pts, false, col);
        }
        draw_vertices(m_vertex_model, m_points, yellow);
        break;
    }

    case Mode::CornerRect: {
        if (m_points.size() == 1 && m_has_cursor) {
            const Vec2d A = m_points[0];
            const Vec2d B = m_cursor;
            std::vector<Vec2d> corners = { A, Vec2d(B.x(), A.y()), B, Vec2d(A.x(), B.y()) };
            draw_quad_strip(m_highlight_model, corners, true, preview);
        }
        break;
    }

    case Mode::CenterRect: {
        if (m_points.size() == 1 && m_has_cursor) {
            const Vec2d C = m_points[0];
            const Vec2d P = m_cursor;
            const double hx = std::abs(P.x() - C.x());
            const double hy = std::abs(P.y() - C.y());
            std::vector<Vec2d> corners = {
                Vec2d(C.x() - hx, C.y() - hy), Vec2d(C.x() + hx, C.y() - hy),
                Vec2d(C.x() + hx, C.y() + hy), Vec2d(C.x() - hx, C.y() + hy) };
            draw_quad_strip(m_highlight_model, corners, true, preview);
            draw_vertices(m_vertex_model, { C }, yellow);
        }
        break;
    }

    case Mode::ObliqueRect: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 2 && m_has_cursor) {
            const Vec2d A = m_points[0], B = m_points[1];
            Vec2d u = B - A;
            if (u.squaredNorm() > 1e-12) {
                u.normalize();
                const Vec2d n(-u.y(), u.x());
                const double w = n.dot(m_cursor - A);
                draw_quad_strip(m_highlight_model, { A, B, B + n * w, A + n * w }, true, preview);
            }
        }
        break;
    }

    case Mode::RoundedRect: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 1 && m_has_cursor) {
            const Vec2d A = m_points[0], B = m_cursor;   // box not yet fixed: plain rect
            draw_quad_strip(m_highlight_model, { A, Vec2d(B.x(), A.y()), B, Vec2d(A.x(), B.y()) }, true, preview);
        } else if (m_points.size() == 2 && m_has_cursor) {
            draw_entities_preview(make_rounded_rect(m_points[0], m_points[1], m_cursor), preview);
        }
        break;
    }

    case Mode::CenterCircle: {
        if (m_points.size() == 1 && m_has_cursor) {
            const Vec2d C = m_points[0];
            const double r = (m_cursor - C).norm();
            draw_quad_strip(m_highlight_model, circle_polygon(C, r), true, preview);
            draw_vertices(m_vertex_model, { C }, yellow);
        }
        break;
    }

    case Mode::TwoPointCircle: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 1 && m_has_cursor) {
            const Vec2d C = (m_points[0] + m_cursor) * 0.5;
            const double r = (m_cursor - m_points[0]).norm() * 0.5;
            draw_quad_strip(m_highlight_model, circle_polygon(C, r), true, preview);
        }
        break;
    }

    case Mode::ThreePointCircle: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 2 && m_has_cursor)
            draw_entities_preview(make_three_point_circle(m_points[0], m_points[1], m_cursor), preview);
        break;
    }

    case Mode::ThreePointArc: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 2 && m_has_cursor)
            draw_entities_preview(make_three_point_arc(m_points[0], m_points[1], m_cursor), preview);
        break;
    }

    case Mode::TangentArc: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 1 && m_has_cursor)
            draw_entities_preview(make_tangent_arc(m_points[0], m_cursor), preview);
        break;
    }

    case Mode::CenterArc: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 1 && m_has_cursor) {
            // center placed: show the radius rubber-band as a faint guide circle
            SketchEntity g; g.type = SketchEntity::Type::Circle; g.center = m_points[0];
            g.p0 = m_points[0]; g.radius = (m_cursor - m_points[0]).norm(); g.construction = true;
            draw_entities_preview({ g }, preview);
        } else if (m_points.size() == 2 && m_has_cursor) {
            draw_entities_preview(make_center_arc(m_points[0], m_points[1], m_cursor), preview);
        }
        break;
    }

    case Mode::Slot: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 1 && m_has_cursor) {
            draw_quad_strip(m_highlight_model, { m_points[0], m_cursor }, false, grey);
        } else if (m_points.size() == 2 && m_has_cursor) {
            Vec2d u = m_points[1] - m_points[0];
            if (u.squaredNorm() > 1e-12) {
                u.normalize();
                const Vec2d n(-u.y(), u.x());
                const double w = std::abs(n.dot(m_cursor - m_points[0]));
                draw_entities_preview(make_slot(m_points[0], m_points[1], w), preview);
            }
        }
        break;
    }

    case Mode::ArcSlot: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 1 && m_has_cursor) {
            // center placed: faint guide circle for the centerline radius
            SketchEntity g; g.type = SketchEntity::Type::Circle; g.center = m_points[0];
            g.p0 = m_points[0]; g.radius = (m_cursor - m_points[0]).norm(); g.construction = true;
            draw_entities_preview({ g }, preview);
        } else if (m_points.size() == 2 && m_has_cursor) {
            draw_entities_preview(make_center_arc(m_points[0], m_points[1], m_cursor), preview); // centerline arc
        } else if (m_points.size() == 3 && m_has_cursor) {
            const double Rc = (m_points[1] - m_points[0]).norm();
            const double w  = std::abs((m_cursor - m_points[0]).norm() - Rc);
            draw_entities_preview(make_arc_slot(m_points[0], m_points[1], m_points[2], w), preview);
        }
        break;
    }

    case Mode::Polygon: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 1 && m_has_cursor)
            draw_entities_preview(make_polygon(m_points[0], m_cursor, m_polygon_sides), preview);
        break;
    }

    case Mode::Ellipse: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 1 && m_has_cursor) {
            SketchEntity g; g.type = SketchEntity::Type::Line;
            g.p0 = m_points[0]; g.p1 = m_cursor; g.construction = true;   // major-axis rubber band
            draw_entities_preview({ g }, preview);
        } else if (m_points.size() == 2 && m_has_cursor) {
            draw_entities_preview(make_ellipse(m_points[0], m_points[1], m_cursor), preview);
        }
        break;
    }

    case Mode::EllipseArc: {
        draw_vertices(m_vertex_model, m_points, yellow);
        if (m_points.size() == 1 && m_has_cursor) {
            SketchEntity g; g.type = SketchEntity::Type::Line;
            g.p0 = m_points[0]; g.p1 = m_cursor; g.construction = true;
            draw_entities_preview({ g }, preview);
        } else if (m_points.size() == 2 && m_has_cursor) {
            draw_entities_preview(make_ellipse(m_points[0], m_points[1], m_cursor), preview);
        } else if (m_points.size() == 3 && m_has_cursor) {
            std::vector<SketchEntity> full = make_ellipse(m_points[0], m_points[1], m_points[2]);
            for (auto& e : full) e.construction = true;                   // faint full ellipse
            draw_entities_preview(full, preview);
        } else if (m_points.size() == 4 && m_has_cursor) {
            draw_entities_preview(make_ellipse_arc(m_points[0], m_points[1], m_points[2],
                                                   m_points[3], m_cursor), preview);
        }
        break;
    }

    case Mode::BSpline: {
        std::vector<Vec2d> poles = m_points;
        if (m_has_cursor) poles.push_back(m_cursor);
        // Faint control polygon as a placement guide.
        if (poles.size() >= 2) {
            std::vector<SketchEntity> guide;
            for (size_t i = 1; i < poles.size(); ++i) {
                SketchEntity g; g.type = SketchEntity::Type::Line;
                g.p0 = poles[i - 1]; g.p1 = poles[i]; g.construction = true;
                guide.push_back(g);
            }
            draw_entities_preview(guide, grey);
        }
        // The spline curve itself.
        if (poles.size() >= 2)
            draw_quad_strip(m_highlight_model, bspline_polyline(poles), false, preview);
        draw_vertices(m_vertex_model, m_points, yellow);
        break;
    }

    case Mode::Trim:
    case Mode::Extend: {
        // Hover preview: paint the exact sub-portion a click would cut (Trim) / add (Extend)
        // in red, recomputed from the cursor every frame (never persisted). The pick tolerance
        // matches on_mouse: ~8 px projected to plane units, x3 (here via zoom -> units/px).
        if (m_has_cursor) {
            const double upp = 1.0 / std::max(camera.get_zoom(), 1e-6);
            int subj = -1;
            std::vector<Vec2d> removed;
            const ColorRGBA cut(1.0f, 0.2f, 0.2f, 1.0f);
            if (compute_trim_preview(m_cursor, 24.0 * upp, m_mode == Mode::Extend, subj, removed))
                draw_quad_strip(m_highlight_model, removed, false, cut);
        }
        break;
    }

    case Mode::Point:
    case Mode::Constrain:
        break;
    }

    // Inference hint: highlight the snapped target under the cursor (C1.3). Colour
    // encodes what the placed point will be Coincident/PointOnObject/Fixed onto.
    if (m_has_cursor && m_mode != Mode::Constrain && m_cursor_snap.snapped()) {
        ColorRGBA hint(1.0f, 0.55f, 0.1f, 1.0f);                 // endpoint/midpoint: orange
        switch (m_cursor_snap.kind) {
        case InferenceSnap::Kind::Center: hint = ColorRGBA(0.30f, 0.80f, 1.0f, 1.0f); break; // cyan
        case InferenceSnap::Kind::Origin: hint = ColorRGBA(1.0f, 0.30f, 0.85f, 1.0f); break; // magenta
        case InferenceSnap::Kind::OnEdge: hint = ColorRGBA(0.45f, 0.70f, 1.0f, 1.0f); break; // blue
        default: break;
        }
        draw_vertices(m_highlight_model, { m_cursor_snap.point }, hint);
    }

    shader->stop_using();
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glEnable(GL_DEPTH_TEST));

    if (on_readout) on_readout(build_readout());   // bottom-right viewport HUD
}

// Compact "current values" for the bottom-right HUD: the live segment being drawn (length
// + bearing) takes priority; otherwise the selected entity's characteristic quotes (the
// same values render_live_quotes just drew on the geometry).
std::string DesignSketchTool::build_readout() const
{
    auto en = [](char* b) { for (char* c = b; *c; ++c) if (*c == ',') *c = '.'; };
    if (m_active && (m_mode == Mode::Line || m_mode == Mode::Polyline) &&
        !m_points.empty() && m_has_cursor) {
        const Vec2d d = m_cursor - m_points.back();
        double ang = std::atan2(d.y(), d.x()) * 180.0 / M_PI; if (ang < 0.0) ang += 360.0;
        char b[80]; std::snprintf(b, sizeof(b), "L %.2f mm    %.1f\xC2\xB0", d.norm(), ang);
        en(b);
        std::string out = b;
        // Tell the user how to end a polyline chain — there's no other affordance for it.
        if (m_mode == Mode::Polyline && m_points.size() >= 2)
            out += "      right-click or double-click to finish, click start to close";
        return out;
    }
    if (!m_active) return std::string();
    std::string out;
    for (const DimAnnot& q : m_live_quotes) {       // selection's Length/Radius/Width/… quotes
        if (!out.empty()) out += "    ";
        out += dim_text(q);
    }
    return out;
}

// Distance from point p to the segment [a,b] in plane (2D) coordinates.
static double point_segment_dist(const Vec2d& p, const Vec2d& a, const Vec2d& b)
{
    const Vec2d ab = b - a;
    const double len2 = ab.squaredNorm();
    if (len2 < 1e-12)
        return (p - a).norm();
    double t = (p - a).dot(ab) / len2;
    t = std::max(0.0, std::min(1.0, t));
    return (p - (a + t * ab)).norm();
}

// Even-odd point-in-polygon test (plane coords), for picking a closed-loop interior.
static bool point_in_poly(const Vec2d& q, const std::vector<Vec2d>& poly)
{
    if (poly.size() < 3) return false;
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2d& a = poly[i];
        const Vec2d& b = poly[j];
        if (((a.y() > q.y()) != (b.y() > q.y())) &&
            (q.x() < (b.x() - a.x()) * (q.y() - a.y()) / (b.y() - a.y()) + a.x()))
            in = !in;
    }
    return in;
}

// Möller–Trumbore ray/triangle intersection in 3D world space. ro=ray origin, rd=ray dir
// (not necessarily unit). Returns true + the ray parameter t (>0) of the hit.
static bool ray_triangle(const Vec3d& ro, const Vec3d& rd,
                         const Vec3d& v0, const Vec3d& v1, const Vec3d& v2, double& t)
{
    const Vec3d e1 = v1 - v0, e2 = v2 - v0;
    const Vec3d p = rd.cross(e2);
    const double det = e1.dot(p);
    if (std::abs(det) < 1e-12) return false;          // parallel
    const double inv = 1.0 / det;
    const Vec3d s = ro - v0;
    const double u = s.dot(p) * inv;
    if (u < -1e-9 || u > 1.0 + 1e-9) return false;
    const Vec3d q = s.cross(e1);
    const double v = rd.dot(q) * inv;
    if (v < -1e-9 || u + v > 1.0 + 1e-9) return false;
    t = e2.dot(q) * inv;
    return t > 1e-9;
}

// Shortest distance between an infinite ray (ro+rd) and a 3D segment [a,b].
static double ray_segment_dist3(const Vec3d& ro, const Vec3d& rd, const Vec3d& a, const Vec3d& b)
{
    const Vec3d d1 = rd, d2 = b - a, r = ro - a;
    const double A = d1.dot(d1), B = d1.dot(d2), C = d2.dot(d2), D = d1.dot(r), E = d2.dot(r);
    const double denom = A * C - B * B;
    double s = (std::abs(denom) > 1e-12) ? (A * E - B * D) / denom : 0.0;  // param on segment
    s = std::max(0.0, std::min(1.0, s));
    double tt = (B * s - D) / std::max(A, 1e-12);                          // param on ray
    tt = std::max(0.0, tt);                                                // ray is forward-only
    const Vec3d pr = ro + tt * d1, ps = a + s * d2;
    return (pr - ps).norm();
}

// Screen-plane distance from p to a sketch entity, for click picking in Constrain
// mode. Circles/arcs measure distance to the ring; points to their position.
static double entity_pick_dist(const Vec2d& p, const SketchEntity& e)
{
    switch (e.type) {
    case SketchEntity::Type::Line:   return point_segment_dist(p, e.p0, e.p1);
    case SketchEntity::Type::Point:  return (p - e.p0).norm();
    case SketchEntity::Type::Circle:
    case SketchEntity::Type::Arc:    return std::abs((p - e.center).norm() - e.radius);
    case SketchEntity::Type::Ellipse:
    case SketchEntity::Type::EllipseArc: {
        // Accurate edge pick: sample the true ellipse outline as a polyline and take the
        // min segment distance. The crude mean-radius circle mis-picks eccentric ellipses
        // (the outline at the major/minor extremes is far from that circle), which made the
        // face-fill swallow edge clicks. Full ellipse sweeps 0..2pi; an arc its param range.
        const double cu = std::cos(e.rotation), su = std::sin(e.rotation);
        const bool full = (e.type == SketchEntity::Type::Ellipse);
        const double t0 = full ? 0.0 : e.start_angle;
        const double t1 = full ? 2.0 * M_PI : e.end_angle;
        const int n = 48;
        double best = 1e30; Vec2d prev;
        for (int i = 0; i <= n; ++i) {
            const double t = t0 + (t1 - t0) * double(i) / n;
            const double lx = e.radius * std::cos(t), ly = e.rminor * std::sin(t);   // local
            const Vec2d q(e.center.x() + lx * cu - ly * su,
                          e.center.y() + lx * su + ly * cu);                          // world
            if (i > 0) best = std::min(best, point_segment_dist(p, prev, q));
            prev = q;
        }
        return best;
    }
    case SketchEntity::Type::BSpline: {
        const std::vector<Vec2d> poly = bspline_polyline(e.ctrl);
        double best = 1e30;
        for (size_t i = 1; i < poly.size(); ++i)
            best = std::min(best, point_segment_dist(p, poly[i - 1], poly[i]));
        return best;
    }
    }
    return 1e30;
}

// Adjacency endpoints for loop walking (Circles/Points have none -> stand-alone).
static void entity_endpoints(const SketchEntity& e, std::vector<Vec2d>& out)
{
    out.clear();
    if (e.type == SketchEntity::Type::Line) {
        out.push_back(e.p0);
        out.push_back(e.p1);
    } else if (e.type == SketchEntity::Type::Arc) {
        out.push_back(e.center + e.radius * Vec2d(std::cos(e.start_angle), std::sin(e.start_angle)));
        out.push_back(e.center + e.radius * Vec2d(std::cos(e.end_angle),   std::sin(e.end_angle)));
    }
}

// Reference point for positioning ops: a Point's position, or a Circle/Arc centre.
// Lines have no single reference point (return false).
static bool entity_ref_point(const SketchEntity& e, Vec2d& out)
{
    switch (e.type) {
    case SketchEntity::Type::Point:  out = e.p0;     return true;
    case SketchEntity::Type::Circle:
    case SketchEntity::Type::Arc:
    case SketchEntity::Type::Ellipse:
    case SketchEntity::Type::EllipseArc: out = e.center; return true;
    default:                         return false;   // Line
    }
}

// Rigidly translate a whole entity (keeps size/shape).
static void translate_entity(SketchEntity& e, const Vec2d& d)
{
    e.p0 += d;
    e.p1 += d;
    e.center += d;
    for (auto& cp : e.ctrl) cp += d;     // BSpline poles
}

int DesignSketchTool::hit_test(const Vec2d& p, double tol) const
{
    double best = tol;
    int bi = -1;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const double d = entity_pick_dist(p, m_entities[i]);
        if (d < best) { best = d; bi = int(i); }
    }
    return bi;
}

std::vector<int> DesignSketchTool::connected_loop(int seed) const
{
    std::vector<int> out;
    if (seed < 0 || seed >= int(m_entities.size())) return out;
    const double eps2 = 1e-6;
    std::vector<bool> vis(m_entities.size(), false);
    std::vector<int> stack = { seed };
    vis[seed] = true;
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        out.push_back(cur);
        std::vector<Vec2d> ce;
        entity_endpoints(m_entities[cur], ce);
        if (ce.empty()) continue;                 // circle/point: not part of a chain
        for (size_t j = 0; j < m_entities.size(); ++j) {
            if (vis[j]) continue;
            std::vector<Vec2d> je;
            entity_endpoints(m_entities[j], je);
            if (je.empty()) continue;
            bool adj = false;
            for (const Vec2d& a : ce)
                for (const Vec2d& b : je)
                    if ((a - b).squaredNorm() < eps2) { adj = true; break; }
            if (adj) { vis[j] = true; stack.push_back(int(j)); }
        }
    }
    return out;
}

// Right-click has two jobs in a sketch, and they were resolved by giving one of them everything:
// the offer was excluded in sketch mode wholesale so a right-click could end a polyline chain,
// abandon an anchor or exit a tool. That made every sketch row in the atlas unreachable.
// The honest test is not "which mode are we in" but "did the tool actually USE this right-click",
// and only the tool knows. Wrapping on_mouse records that once, for every terminator, instead of
// threading a flag through the twenty-odd sites that consume a RightDown.
bool DesignSketchTool::on_mouse(wxMouseEvent& evt, GLCanvas3D& canvas)
{
    const bool consumed = on_mouse_impl(evt, canvas);
    if (evt.RightDown())
        m_right_consumed = consumed;
    return consumed;
}

bool DesignSketchTool::on_mouse_impl(wxMouseEvent& evt, GLCanvas3D& canvas)
{
    // Track the cursor in canvas client px so the in-canvas value editor can open right
    // where the user clicked (Onshape places the field at the click, not via a camera
    // projection — the design canvas's viewport isn't valid outside its own paint).
    m_last_mouse_x = evt.GetX();
    m_last_mouse_y = evt.GetY();

    // Line draw-then-edit: while the length editor is open right after the second click,
    // freeze the canvas so a stray move/click can't push a third point or rubber-band a
    // segment under the floating field. The editor's Enter/Esc resolves it
    // (apply_segment_length / keep_segment_as_drawn, the latter clears this flag).
    if (m_awaiting_length) {
        // Polyline terminators must work even with a per-segment field open: right-click or
        // double-click accepts the current segment as drawn (close the field) and falls through
        // so the Polyline handler ends the chain. Without this the freeze ate every terminator.
        if (m_mode == Mode::Polyline && (evt.RightDown() || evt.LeftDClick()) && on_inline_dismiss)
            on_inline_dismiss();              // -> set_inline_busy(false), m_awaiting_length=false
        // The freeze exists so a stray click can't draw under the floating field — it was
        // never meant to trap the camera. Let drags and the wheel through, so a field that
        // opens somewhere unexpected can't leave the viewport unusable.
        else if (evt.Dragging() || evt.GetWheelRotation() != 0)
            return false;
        else
            return true;
    }

    // No live session, but committed sketches are shown as overlays on the plate: a left
    // click on a loop (its edge OR its closed interior) selects that Sketch feature. This
    // is the ONLY interaction in display-only mode; everything else (drag/move/wheel/right)
    // falls through (return false) so the camera can still orbit the plate.
    if (!m_active) {
        // Double-click on a committed sketch stroke OPENS IT FOR EDITING; on empty space it
        // still fits the view. A sketch line has to be editable from the line, not from a tree
        // row — selecting it already lit the feature, but "now go and press Edit in the panel"
        // is the side-panel dependency this tab exists to remove. Fit keeps the rest of the
        // plate, so nothing is taken away.
        if (evt.LeftDClick()) {
            int f = -1, r = -1, e = -1, ff = -1, fr = -1; double dbest = 1e30;
            const Linef3 dray  = canvas.mouse_ray(Point(evt.GetX(), evt.GetY()));
            const Linef3 dray8 = canvas.mouse_ray(Point(evt.GetX() + 8, evt.GetY()));
            for (const DisplaySketch& d : m_display_sketches) {
                const Vec2d dp  = d.plane.project(dray.a,  dray.vector());
                const Vec2d dp8 = d.plane.project(dray8.a, dray8.vector());
                hit_display_sketch(d, dp, std::max(1e-3, (dp8 - dp).norm()), f, r, e, dbest, ff, fr);
            }
            const int target = (f >= 0) ? f : ff;
            if (target >= 0 && on_display_sketch_activated) {
                dp_pick_trace("double-click -> edit sketch feature %d (entity %d)", target, e);
                on_display_sketch_activated(target);
                return true;
            }
            canvas.zoom_to_volumes();
            return true;
        }
        // Visual Extrude gizmo (C5b): while the Extrude card is open the depth arrow is
        // grabbable — drag changes the depth live; a click (no drag) on the arrow opens the
        // inline depth editor. Intercept before the early no-LeftDown bailout so Dragging/
        // LeftUp reach us; a LeftDown that misses the arrow falls through to solid/loop pick.
        // Move-body gizmo (M5): three world-axis arrows on the selected body. Drag an arrow to
        // translate live; a stationary click on it opens the inline offset editor; a right click
        // exits move mode. A LeftDown that misses the arrows falls through to solid re-pick.
        if (m_mv_active) {
            if (m_mv_drag >= 0 && evt.Dragging() && evt.LeftIsDown()) {
                if (m_mv_drag < 3) drag_move_arrow(canvas, evt, m_mv_drag);
                else               drag_move_arc(canvas, evt, m_mv_drag - 3);
                return true;
            }
            if (evt.LeftUp() && m_mv_drag >= 0) {
                const int d = m_mv_drag; m_mv_drag = -1;
                const bool moved = std::abs(evt.GetX() - m_mv_press_x) +
                                   std::abs(evt.GetY() - m_mv_press_y) > 3;
                if (!moved && d < 3) open_move_editor(d);   // stationary click on an arrow = edit offset
                return true;
            }
            if (evt.RightDown()) { clear_move_gizmo(); canvas.set_as_dirty(); if (on_move_exit) on_move_exit(); return true; }
            if (evt.LeftDown()) {
                int axis = -1;
                if (hit_test_move_arrow(canvas, evt, axis)) {   // translate arrows win over rings
                    m_mv_drag = axis; m_mv_press_x = evt.GetX(); m_mv_press_y = evt.GetY();
                    return true;
                }
                if (hit_test_move_arc(canvas, evt, axis)) {
                    m_mv_drag = 3 + axis; m_mv_press_x = evt.GetX(); m_mv_press_y = evt.GetY();
                    m_mv_rot_start = m_mv_rot;
                    double a0; if (arc_mouse_angle(canvas, evt, axis, a0)) m_mv_arc_a0 = a0;
                    return true;
                }
            }
        }
        // Datum-plane resize gizmo (C3): while the Plane card is open the 4 edge handles are
        // grabbable — drag changes the u/v extent live. A LeftDown that misses falls through.
        if (m_dz_active) {
            if (m_dz_drag >= 0 && evt.Dragging() && evt.LeftIsDown()) {
                drag_datum_handle(canvas, evt, m_dz_drag);
                return true;
            }
            if (evt.LeftUp() && m_dz_drag >= 0) { m_dz_drag = -1; return true; }
            if (evt.LeftDown()) {
                int which = -1;
                if (hit_test_datum_handle(canvas, evt, which)) {
                    m_dz_drag = which; m_dz_press_x = evt.GetX(); m_dz_press_y = evt.GetY();
                    return true;
                }
            }
        }
        // Helix gizmo: radius/height/pitch handles on the live curve, while the Helix card is open.
        if (m_hx_active) {
            if (m_hx_drag >= 0 && evt.Dragging() && evt.LeftIsDown()) {
                drag_helix_handle(canvas, evt, m_hx_drag);
                return true;
            }
            if (evt.LeftUp() && m_hx_drag >= 0) { m_hx_drag = -1; return true; }
            if (evt.LeftDown()) {
                int which = -1;
                if (hit_test_helix_handle(canvas, evt, which)) {
                    m_hx_drag = which; m_hx_press_x = evt.GetX(); m_hx_press_y = evt.GetY();
                    return true;
                }
            }
        }
        // Rib thickness gizmo: two in-plane handles on the slab footprint, while the Rib card is open.
        if (m_rb_active) {
            if (m_rb_drag >= 0 && evt.Dragging() && evt.LeftIsDown()) {
                drag_rib_handle(canvas, evt, m_rb_drag);
                return true;
            }
            if (evt.LeftUp() && m_rb_drag >= 0) { m_rb_drag = -1; return true; }
            if (evt.LeftDown()) {
                int which = -1;
                if (hit_test_rib_handle(canvas, evt, which)) {
                    m_rb_drag = which;
                    return true;
                }
            }
        }
        // Datum base picker: HOVER highlight only here. The CLICK is handled at the very end of the
        // selection fall-through (below), so picking existing geometry (committed sketch loops,
        // solid faces/edges) always wins over a base-plane click — the planes never block selection.
        if (m_dbp_active && evt.Moving() && !evt.LeftIsDown()) {
            const int h = hit_test_base_pick(canvas, evt);
            if (h != m_dbp_hover) {
                m_dbp_hover = h;
                canvas.set_as_dirty();
                if (h >= 0) return true;   // caller render()s on true -> hover repaints on software GL
            }
        }
        if (m_ex_active) {
            if (m_ex_drag >= 0 && evt.Dragging() && evt.LeftIsDown()) {
                drag_extrude_arrow(canvas, evt, m_ex_drag);
                return true;
            }
            if (evt.LeftUp() && m_ex_drag >= 0) {
                const int which = m_ex_drag; m_ex_drag = -1;
                const bool moved = std::abs(evt.GetX() - m_ex_press_x) +
                                   std::abs(evt.GetY() - m_ex_press_y) > 3;
                if (!moved) open_extrude_editor(which);   // treat a stationary click as edit
                return true;
            }
            if (evt.LeftDown()) {
                int which = -1;
                if (hit_test_extrude_arrow(canvas, evt, which)) {
                    m_ex_drag = which; m_ex_press_x = evt.GetX(); m_ex_press_y = evt.GetY();
                    return true;
                }
            }
        }
        // Revolve angle-arc gizmo: drag the arc tip to sweep the angle; a stationary click on the
        // handle opens the inline editor. A LeftDown that misses falls through to normal picking.
        if (m_rv_active) {
            if (m_rv_drag && evt.Dragging() && evt.LeftIsDown()) {
                drag_revolve_arc(canvas, evt);
                return true;
            }
            if (evt.LeftUp() && m_rv_drag) {
                m_rv_drag = false;
                const bool moved = std::abs(evt.GetX() - m_rv_press_x) +
                                   std::abs(evt.GetY() - m_rv_press_y) > 3;
                if (!moved) open_revolve_editor();
                return true;
            }
            if (evt.LeftDown() && hit_test_revolve_handle(canvas, evt)) {
                m_rv_drag = true; m_rv_press_x = evt.GetX(); m_rv_press_y = evt.GetY();
                return true;
            }
        }
        // Draft angle-arc gizmo: drag the arc tip to sweep the draft angle; a stationary click
        // edits it. A LeftDown that misses falls through to normal picking.
        if (m_dr_active) {
            if (m_dr_drag && evt.Dragging() && evt.LeftIsDown()) {
                drag_draft_arc(canvas, evt);
                return true;
            }
            if (evt.LeftUp() && m_dr_drag) {
                m_dr_drag = false;
                return true;
            }
            if (evt.LeftDown() && hit_test_draft_handle(canvas, evt)) {
                m_dr_drag = true; m_dr_press_x = evt.GetX(); m_dr_press_y = evt.GetY();
                return true;
            }
        }
        // Cut gizmo: drag the offset arrow along the cut-plane normal; no positive clamp (offset is signed).
        if (m_ct_active) {
            if (m_ct_drag && evt.Dragging() && evt.LeftIsDown()) {
                drag_cut_arrow(canvas, evt);
                return true;
            }
            if (evt.LeftUp() && m_ct_drag) {
                m_ct_drag = false;
                return true;
            }
            if (evt.LeftDown() && hit_test_cut_arrow(canvas, evt)) {
                start_cut_drag(canvas, evt);
                return true;
            }
        }
        // Pattern gizmo: drag the diamond/arc handle to set the spacing (linear) or angle (circular);
        // a stationary click opens the inline editor. A LeftDown that misses falls through to picking.
        if (m_pt_active) {
            if (m_pt_drag && evt.Dragging() && evt.LeftIsDown()) {
                drag_pattern_handle(canvas, evt);
                return true;
            }
            if (evt.LeftUp() && m_pt_drag) {
                m_pt_drag = false;
                const bool moved = std::abs(evt.GetX() - m_pt_press_x) +
                                   std::abs(evt.GetY() - m_pt_press_y) > 3;
                if (!moved) open_pattern_editor();
                return true;
            }
            if (evt.LeftDown() && hit_test_pattern_handle(canvas, evt)) {
                m_pt_drag = true; m_pt_press_x = evt.GetX(); m_pt_press_y = evt.GetY();
                return true;
            }
        }
        // Fillet/Chamfer radius gizmo: drag the edge-anchored arrow to set the radius live; a
        // stationary click opens the inline editor. A LeftDown that misses falls through to pick.
        if (m_fl_active) {
            if (m_fl_drag && evt.Dragging() && evt.LeftIsDown()) {
                drag_fillet_arrow(canvas, evt);
                return true;
            }
            if (evt.LeftUp() && m_fl_drag) {
                m_fl_drag = false;
                const bool moved = std::abs(evt.GetX() - m_fl_press_x) +
                                   std::abs(evt.GetY() - m_fl_press_y) > 3;
                if (!moved) open_fillet_editor();   // stationary click = edit
                return true;
            }
            if (evt.LeftDown() && hit_test_fillet_arrow(canvas, evt)) {
                start_fillet_drag(canvas, evt);
                return true;
            }
        }
        // Hole gizmo: drag the centre to reposition / the diameter or depth arrow to resize; a
        // stationary click on an arrow opens its inline editor. A LeftDown that misses any handle
        // falls through to solid/loop picking.
        if (m_hl_active) {
            if (m_hl_drag >= 0 && evt.Dragging() && evt.LeftIsDown()) {
                drag_hole_handle(canvas, evt);
                return true;
            }
            if (evt.LeftUp() && m_hl_drag >= 0) {
                const int which = m_hl_drag; m_hl_drag = -1;
                const bool moved = std::abs(evt.GetX() - m_hl_press_x) +
                                   std::abs(evt.GetY() - m_hl_press_y) > 3;
                if (!moved) open_hole_editor(which);   // stationary click = edit
                return true;
            }
            if (evt.LeftDown()) {
                const int which = hit_test_hole_handle(canvas, evt);
                if (which >= 0) { start_hole_drag(canvas, evt, which); return true; }
            }
        }
        // Thread gizmo: same interaction as the hole gizmo (centre / radius / length handles).
        if (m_th_active) {
            if (m_th_drag >= 0 && evt.Dragging() && evt.LeftIsDown()) {
                drag_thread_handle(canvas, evt);
                return true;
            }
            if (evt.LeftUp() && m_th_drag >= 0) {
                const int which = m_th_drag; m_th_drag = -1;
                const bool moved = std::abs(evt.GetX() - m_th_press_x) +
                                   std::abs(evt.GetY() - m_th_press_y) > 3;
                if (!moved) open_thread_editor(which);
                return true;
            }
            if (evt.LeftDown()) {
                const int which = hit_test_thread_handle(canvas, evt);
                if (which >= 0) { start_thread_drag(canvas, evt, which); return true; }
            }
        }
        // Shell gizmo: drag the inward thickness arrow; stationary click opens the inline editor.
        if (m_sh_active) {
            if (m_sh_drag && evt.Dragging() && evt.LeftIsDown()) {
                drag_shell_arrow(canvas, evt);
                return true;
            }
            if (evt.LeftUp() && m_sh_drag) {
                m_sh_drag = false;
                const bool moved = std::abs(evt.GetX() - m_sh_press_x) +
                                   std::abs(evt.GetY() - m_sh_press_y) > 3;
                if (!moved) open_shell_editor();
                return true;
            }
            if (evt.LeftDown() && hit_test_shell_arrow(canvas, evt)) {
                start_shell_drag(canvas, evt);
                return true;
            }
        }
        // Left-drag rubber band -> whole body. Past the click budget the press becomes a sweep:
        // the rectangle is anchored at the ORIGINAL press point (not at the frame where the
        // threshold was crossed, which would lose the first few pixels) and the events are
        // consumed from here on. Left-drag no longer orbits in this canvas — DesignCanvas puts
        // orbit on middle-drag and pan on right-drag, the CAD convention — so nothing downstream
        // is being starved of a gesture it used to own.
        // HOVER PRE-HIGHLIGHT (snaporca-9xw part 3): say what a click would take, before it is
        // taken. Plain motion only — no button down, no band running — because during a drag the
        // pointer is doing something else and a promise about clicking would be a lie. Returns
        // false so the event still reaches the camera; this only asks for a repaint, it does not
        // consume the gesture, which is the difference between a hint and a handler.
        if (evt.Moving() && !evt.LeftIsDown() && !m_rubber.is_dragging()) {
            if (update_solid_hover(canvas, evt)) canvas.set_as_dirty();
            return false;
        }
        if (evt.Dragging() && evt.LeftIsDown() && m_pick_pending) {
            if (!m_rubber.is_dragging()) {
                if (std::max(std::abs(evt.GetX() - m_pick_press_x),
                             std::abs(evt.GetY() - m_pick_press_y)) <= 8)
                    return false;                    // still inside the click budget
                m_rubber.start_dragging(Vec2d(m_pick_press_x, m_pick_press_y),
                                        GLSelectionRectangle::Select);
            }
            m_rubber.dragging(Vec2d(evt.GetX(), evt.GetY()));
            return true;
        }
        // Any event with the left button up ends a band — not just LeftUp. A release that lands
        // outside the canvas never sends us one, and a band left running would then paint a
        // rectangle that follows the cursor with no button held.
        if (m_rubber.is_dragging() && !evt.LeftIsDown()) {
            m_pick_pending = false;
            if (evt.LeftUp()) pick_bodies_in_rectangle();   // a release elsewhere selects nothing
            m_rubber.stop_dragging();
            return evt.LeftUp();
        }
        // Click vs drag. Consuming the LeftDown here killed camera orbit/pan the moment a
        // solid was on screen: Orca starts a rotate drag on the press, so swallowing it meant
        // the canvas never began one. (A SpaceMouse kept working — it never goes through
        // wxMouseEvent.) Remember the press, let it fall through so the canvas can orbit, and
        // resolve the pick on release only if the pointer stayed put.
        if (evt.LeftDown()) {
            m_pick_press_x = evt.GetX();
            m_pick_press_y = evt.GetY();
            m_pick_pending = true;
            dp_pick_trace("down x=%d y=%d", evt.GetX(), evt.GetY());
            return false;
        }
        if (!(evt.LeftUp() && m_pick_pending)) {
            if (evt.LeftUp()) dp_pick_trace("up with no pending press (press was eaten upstream)");
            return false;
        }
        m_pick_pending = false;
        dp_pick_trace("up x=%d y=%d drift=%d", evt.GetX(), evt.GetY(),
                      std::max(std::abs(evt.GetX() - m_pick_press_x),
                               std::abs(evt.GetY() - m_pick_press_y)));
        // Threshold per axis, at GTK's own drag threshold. A hand-held mouse drifts several
        // pixels during an ordinary click — a tight budget silently swallowed real clicks and
        // looked exactly like "selection does not work". Synthetic clicks never drift, which
        // is why the headless rig could not show this.
        if (std::max(std::abs(evt.GetX() - m_pick_press_x),
                     std::abs(evt.GetY() - m_pick_press_y)) > 8) {
            dp_pick_trace("rejected as drag");
            return false;   // it was a drag: the canvas already orbited, don't also select
        }
        // Committed-sketch loop pick is computed FIRST. A click that lands on a loop's
        // STROKE (edge) selects that loop even when it lies on a solid face — so a sketch
        // drawn ON a face can be selected and extruded/cut (Onshape engraving workflow).
        // Open-face area (no loop stroke under the cursor) falls through to the solid
        // whole/face/edge cycle; an interior hit with no solid behind it is the last resort.
        Point pos(evt.GetX(), evt.GetY());
        const Linef3 ray  = canvas.mouse_ray(pos);
        const Linef3 ray8 = canvas.mouse_ray(Point(evt.GetX() + 8, evt.GetY()));
        int    edge_feat = -1, edge_reg = -1, edge_ent = -1; double edge_d = 1e30;  // nearest stroke
        int    face_feat = -1, face_reg = -1;                        // interior (fallback)
        dp_pick_trace("display sketches available: %zu", m_display_sketches.size());
        for (const DisplaySketch& d : m_display_sketches) {
            const Vec2d p   = d.plane.project(ray.a,  ray.vector());
            const Vec2d p8  = d.plane.project(ray8.a, ray8.vector());
            const double tol = std::max(1e-3, (p8 - p).norm());
            hit_display_sketch(d, p, tol, edge_feat, edge_reg, edge_ent, edge_d, face_feat, face_reg);
        }
        // A precise hit on a loop outline wins over the solid face beneath it.
        if (edge_feat >= 0) {
            m_display_pick = edge_feat; m_display_pick_region = edge_reg;
            if (on_display_sketch_selected) on_display_sketch_selected(edge_feat, edge_reg);
            return true;
        }
        // No loop stroke under the cursor: the solid is the foreground (whole/face/edge cycle).
        if (handle_solid_click(canvas, evt)) return true;
        // Interior of a committed loop with no solid behind it.
        if (face_feat >= 0) {
            m_display_pick = face_feat; m_display_pick_region = face_reg;
            if (on_display_sketch_selected) on_display_sketch_selected(face_feat, face_reg);
            return true;
        }
        m_display_pick = -1; m_display_pick_region = -1;  // clicked bare plate -> drop highlight
        // ...and the SOLID selection goes with it (snaporca-od0). A click that hits nothing has to
        // mean what a rubber band that sweeps nothing already means — pick_bodies_in_rectangle
        // clears on an empty sweep, and the two gestures cannot disagree about the same outcome.
        // Until now the face survived a click on bare plate, so "click away, then click the face
        // again" arrived here as the SECOND click on the same face and escalated to the whole
        // body, when the click away was the user letting go of it.
        //
        // The cost is real and is the intended trade: Thicken / Shell / Draft hold their input
        // face in the panel's m_sel_solid_face, so a stray click on empty canvas with one of
        // those cards open gives that face back. Their handlers already write the "(pick a solid
        // face)" placeholder and rebuild the ghost at level 0, so the card SAYS it lost the pick
        // rather than confirming against a face the viewport has stopped highlighting.
        //
        // Guarded on there being something to clear: this runs on every click that misses, and
        // the callback re-renders the open card's preview.
        if (m_solid_sel != SolidSel::None) {
            clear_solid_selection();
            dp_pick_trace("clicked empty space -> selection cleared");
            if (on_solid_selection_changed)
                on_solid_selection_changed(int(m_solid_sel), m_sel_body, m_sel_face, m_sel_edge);
        }
        // Last resort: a click that hit no geometry but landed on a reference/base plane picks it.
        if (m_dbp_active) {
            const int h = hit_test_base_pick(canvas, evt);
            if (h >= 0 && h < int(m_dbp_base.size())) {
                if (on_datum_base_picked) on_datum_base_picked(m_dbp_base[h]);
                return true;
            }
        }
        return false;                                     // let the stock canvas orbit / deselect
    }

    // In-canvas edit-op tools (Fillet/Chamfer/Offset/Mirror): pick entities, then a
    // draggable arrow + editable value label (Mirror: a two-phase pick) drives a live
    // ghost. A click on empty space confirms; right-click/Esc cancels the gesture.
    // Standalone Trim / Extend scissors: click a segment to cut it back to (Trim) or out to
    // (Extend) its nearest intersection with the other live entities. One cut per click; the
    // tool stays active for more cuts; right-click exits. Drag falls through so the camera can
    // still orbit. Operates directly on the live sketch — no Constrain mode.
    if (m_mode == Mode::Trim || m_mode == Mode::Extend) {
        if (evt.Moving()) { screen_to_plane(canvas, evt, m_cursor); m_has_cursor = true; return true; }
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            const Linef3 r2 = canvas.mouse_ray(Point(evt.GetX() + 8, evt.GetY()));
            const double tol = std::max(1e-3, (m_plane.project(r2.a, r2.vector()) - p).norm());
            if (apply_live_trim(p, tol * 3.0, m_mode == Mode::Extend)) {
                resolve_live();
            } else if (on_readout) {
                // nde #15: don't fail silently. The pick found nothing to cut/extend — either
                // the click missed every live segment, or the picked segment has no crossing /
                // target among the OTHER live entities (committed sketches aren't trimmed).
                on_readout(m_mode == Mode::Extend
                    ? std::string("Extend: click a line/arc that can reach another live entity")
                    : std::string("Trim: click a segment where it crosses another live entity"));
            }
            return true;
        }
        if (evt.RightDown()) { request_exit(); return true; }
        return false;   // let move/drag orbit the camera
    }

    if (is_edit_op_mode()) {
        if (evt.Moving()) {
            screen_to_plane(canvas, evt, m_cursor);
            m_has_cursor = true;
            return true;
        }
        if (m_op_dragging_arrow && evt.Dragging() && evt.LeftIsDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            drag_op_arrow(p);
            return true;
        }
        if (evt.LeftUp()) {
            if (m_op_dragging_arrow) { m_op_dragging_arrow = false; return true; }
            return false;
        }
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            const Linef3 r2 = canvas.mouse_ray(Point(evt.GetX() + 8, evt.GetY()));
            const double tol = std::max(1e-3, (m_plane.project(r2.a, r2.vector()) - p).norm());
            // 1) live gizmo: click the value label to type, or grab the arrow to drag.
            if (op_ready() && m_mode != Mode::Mirror) {
                const Linef3 rl = canvas.mouse_ray(Point(evt.GetX() + 24, evt.GetY()));
                const double ltol = std::max(tol, (m_plane.project(rl.a, rl.vector()) - p).norm());
                if ((m_op_label - p).norm() <= ltol) { open_op_editor(); return true; }
                if (hit_test_op_arrow(p, tol))        { m_op_dragging_arrow = true; return true; }
            }
            // 2) entity pick (close enough to an entity edge).
            double best = 1e30; int bi = -1;
            for (size_t i = 0; i < m_entities.size(); ++i) {
                const double d = entity_pick_dist(p, m_entities[i]);
                if (d < best) { best = d; bi = int(i); }
            }
            if (bi >= 0 && best <= tol * 3.0) { op_pick(bi); return true; }
            // 3) empty click confirms a ready gesture.
            if (op_ready()) confirm_op();
            return true;
        }
        if (evt.RightDown()) {
            if (m_op_a >= 0 || !m_mirror_targets.empty()) {
                reset_op();
                m_selection.clear();
                if (on_selection_changed) on_selection_changed(0);
            } else {
                request_exit();
            }
            return true;
        }
        return false;
    }

    // In-canvas transform tools (Move/Rotate/Scale/Array/PolarArray): pick subject
    // entities, then a single draggable handle + editable value label(s) drive a live
    // ghost. A click on empty space confirms; right-click drops the gesture / exits.
    if (is_transform_mode()) {
        if (evt.Moving()) {
            screen_to_plane(canvas, evt, m_cursor);
            m_has_cursor = true;
            return true;
        }
        if (m_tf_dragging && evt.Dragging() && evt.LeftIsDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            drag_tf_handle(p);
            return true;
        }
        if (evt.LeftUp()) {
            if (m_tf_dragging) { m_tf_dragging = false; return true; }
            return false;
        }
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            const Linef3 r2 = canvas.mouse_ray(Point(evt.GetX() + 8, evt.GetY()));
            const double tol = std::max(1e-3, (m_plane.project(r2.a, r2.vector()) - p).norm());
            // 1) live gizmo: click a value label to type, or grab the handle to drag.
            if (tf_ready()) {
                const Linef3 rl = canvas.mouse_ray(Point(evt.GetX() + 24, evt.GetY()));
                const double ltol = std::max(tol, (m_plane.project(rl.a, rl.vector()) - p).norm());
                if ((m_tf_label_a - p).norm() <= ltol) { open_tf_editor_a();     return true; }
                if ((m_tf_label_b - p).norm() <= ltol) { open_tf_editor_count(); return true; }
                if (hit_test_tf_handle(p, tol))        { m_tf_dragging = true;   return true; }
            }
            // 2) entity pick (close enough to an entity edge).
            double best = 1e30; int bi = -1;
            for (size_t i = 0; i < m_entities.size(); ++i) {
                const double d = entity_pick_dist(p, m_entities[i]);
                if (d < best) { best = d; bi = int(i); }
            }
            if (bi >= 0 && best <= tol * 3.0) { tf_pick(bi); return true; }
            // 3) empty click confirms a ready gesture.
            if (tf_ready()) confirm_transform();
            return true;
        }
        if (evt.RightDown()) {
            if (!m_tf_targets.empty()) {
                reset_tf();
                m_selection.clear();
                if (on_selection_changed) on_selection_changed(0);
            } else {
                request_exit();
            }
            return true;
        }
        return false;
    }

    // Imported-art bbox transform: drag a corner to scale, the centre to move. Right-click
    // ends the session. Values stream live to the host via on_imported_transform.
    if (m_mode == Mode::TransformArt) {
        if (evt.Moving()) {
            screen_to_plane(canvas, evt, m_cursor);
            m_has_cursor = true;
            return true;
        }
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            const Linef3 r2 = canvas.mouse_ray(Point(evt.GetX() + 10, evt.GetY()));
            const double tol = std::max(1e-3, (m_plane.project(r2.a, r2.vector()) - p).norm());
            const int h = hit_test_xform_handle(p, tol);
            if (h >= 0) {
                m_xform_handle = h;
                if (h == 4) m_xform_anchor = p;             // centre move: track the delta
                else { Vec2d c[4]; xform_world_corners(c); m_xform_anchor = c[(h + 2) % 4]; }
            }
            return true;                                     // swallow (no camera orbit while placing)
        }
        if (m_xform_handle >= 0 && evt.Dragging() && evt.LeftIsDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            drag_xform_handle(p);
            return true;
        }
        if (evt.LeftUp()) { m_xform_handle = -1; return true; }
        if (evt.RightDown()) { if (on_exit) on_exit(); else cancel(); return true; }
        return false;
    }

    // Constrain mode: pick a segment on click; let move/drag fall through so the
    // camera can still orbit while inspecting the sketch.
    if (m_mode == Mode::Constrain) {
        if (m_constrain_entities) {
            // Pick any entity (line/circle/arc/point): rolling two-slot selection.
            if (evt.LeftDown()) {
                Vec2d p;
                screen_to_plane(canvas, evt, p);
                double best = 1e30;
                int bi = -1;
                for (size_t i = 0; i < m_entities.size(); ++i) {
                    const double d = entity_pick_dist(p, m_entities[i]);
                    if (d < best) { best = d; bi = int(i); }
                }
                if (bi >= 0) {
                    // Rolling three-slot selection: slots 0/1 feed all 2-entity
                    // constraints; slot 2 is the Symmetric axis (only filled once
                    // 0 and 1 are set). A click past slot 2 restarts the cycle.
                    if (m_pick0 < 0)                                         { m_pick0 = bi; m_pick0_pt = p; }
                    else if (m_pick1 < 0 && bi != m_pick0)                    m_pick1 = bi;
                    else if (m_pick1 >= 0 && m_pick2 < 0 && bi != m_pick0 && bi != m_pick1) m_pick2 = bi;
                    else                                                     { m_pick0 = bi; m_pick1 = m_pick2 = -1; m_pick0_pt = p; }
                }
                return true;
            }
            if (evt.RightDown()) { cancel(); return true; }
            return false;
        }
        if (evt.LeftDown()) {
            if (m_points.size() < 2)
                return false;
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            const size_t n = m_points.size();
            double best = 1e30;
            int bi = -1;
            for (size_t i = 0; i < n; ++i) {
                const double d = point_segment_dist(p, m_points[i], m_points[(i + 1) % n]);
                if (d < best) { best = d; bi = int(i); }
            }
            if (bi >= 0) {
                m_sel_a = bi;
                m_sel_b = int((bi + 1) % n);
            }
            return true;
        }
        if (evt.RightDown()) {
            cancel();
            return true;
        }
        return false;
    }

    // Selection mode: click to pick an entity, Shift/Ctrl to extend, double-click
    // to grab the whole connected loop. Drag falls through so the camera can orbit.
    if (m_mode == Mode::Select) {
        if (update_hover(canvas, evt)) return true;   // repaint when the hovered handle changes
        const bool extend = evt.ShiftDown() || evt.ControlDown();

        // Live point drag: once an endpoint/centre was grabbed on LeftDown, dragging
        // moves it (re-solving constraints live) until the button is released. When
        // nothing is grabbed, drag falls through so the camera can still orbit.
        if (m_dragging_point && evt.Dragging() && evt.LeftIsDown()) {
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            if (m_drag_poly_fi >= 0)
                drag_polygon_vertex(m_drag_poly_fi, m_drag_ei, m_drag_role, p);  // keep regular
            else if (m_drag_rect_fi >= 0)
                drag_rect_corner(m_drag_rect_fi, p);          // resize axis-aligned box
            else if (m_drag_slot_fi >= 0)
                drag_slot_handle(m_drag_slot_fi, p);          // move a slot end
            else if (m_drag_ei >= 0 && m_drag_ei < int(m_entities.size()) &&
                     m_entities[m_drag_ei].type == SketchEntity::Type::Arc)
                drag_arc_handle(m_drag_ei, m_drag_role, p);   // center/radius/angle grips
            else if (m_drag_ei >= 0 && m_drag_ei < int(m_entities.size()) &&
                     m_entities[m_drag_ei].type == SketchEntity::Type::EllipseArc)
                drag_ellipsearc_handle(m_drag_ei, m_drag_role, p);  // center/sweep endpoints
            else {
                set_point(m_drag_ei, m_drag_role, p);
                resolve_live_drag(m_drag_ei, m_drag_role);
            }
            return true;
        }
        // Derived-handle drag (A4): the circle RadiusHandle (and later slot/rect/etc.)
        // isn't an entity point, so it rides set_handle, which applies the role-specific
        // geometry edit + re-solve.
        if (m_dragging_handle && evt.Dragging() && evt.LeftIsDown()) {
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            set_handle(m_drag_handle, p);
            return true;
        }
        if (evt.LeftUp()) {
            if (m_dragging_point) {
                Vec2d p;
                screen_to_plane(canvas, evt, p);
                if (m_drag_poly_fi >= 0)
                    drag_polygon_vertex(m_drag_poly_fi, m_drag_ei, m_drag_role, p);
                else if (m_drag_rect_fi >= 0)
                    drag_rect_corner(m_drag_rect_fi, p);
                else if (m_drag_slot_fi >= 0)
                    drag_slot_handle(m_drag_slot_fi, p);
                else if (m_drag_ei >= 0 && m_drag_ei < int(m_entities.size()) &&
                         m_entities[m_drag_ei].type == SketchEntity::Type::Arc)
                    drag_arc_handle(m_drag_ei, m_drag_role, p);
                else if (m_drag_ei >= 0 && m_drag_ei < int(m_entities.size()) &&
                         m_entities[m_drag_ei].type == SketchEntity::Type::EllipseArc)
                    drag_ellipsearc_handle(m_drag_ei, m_drag_role, p);
                else {
                    set_point(m_drag_ei, m_drag_role, p);
                    resolve_live_drag(m_drag_ei, m_drag_role);
                }
                m_dragging_point = false;
                m_drag_ei = -1;
                m_drag_poly_fi = -1;
                m_drag_rect_fi = -1;
                m_drag_slot_fi = -1;
                return true;
            }
            if (m_dragging_handle) {
                Vec2d p;
                screen_to_plane(canvas, evt, p);
                set_handle(m_drag_handle, p);
                m_dragging_handle = false;
                return true;
            }
            return false;
        }

        if (evt.LeftDown() || evt.LeftDClick()) {
            m_dragging_point = false;           // a fresh press disarms any stale grab
            m_dragging_handle = false;
            m_drag_poly_fi = -1;
            m_drag_rect_fi = -1;
            m_drag_slot_fi = -1;
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            if (evt.LeftDClick()) {                // double-click a quote label -> edit it
                const Linef3 rd = canvas.mouse_ray(Point(evt.GetX() + 28, evt.GetY()));
                const double dtol = std::max(2.0, (m_plane.project(rd.a, rd.vector()) - p).norm());
                const int di = hit_test_dimension(p, dtol);
                if (di >= 0) { edit_dimension(di); return true; }
            }
            // Zoom-aware tolerance: project a point 8 px away and measure in plane units.
            const Linef3 r2 = canvas.mouse_ray(Point(evt.GetX() + 8, evt.GetY()));
            const Vec2d p2 = m_plane.project(r2.a, r2.vector());
            const double tol = std::max(1e-3, (p2 - p).norm());

            // Single-click a dimension quote label -> edit its value in place. A placed
            // driving quote reopens its editor; a live (non-driving) characteristic quote
            // is first promoted to a driving dimension (place_dimension), then its editor
            // opens — so typing a value sets the precise dimension. Generous label
            // tolerance (~24 px) since text labels are wider than a point grip.
            if (evt.LeftDown()) {
                const Linef3 rdl = canvas.mouse_ray(Point(evt.GetX() + 24, evt.GetY()));
                const double ltol = std::max(tol, (m_plane.project(rdl.a, rdl.vector()) - p).norm());
                const int di = hit_test_dimension(p, ltol);
                if (di >= 0) { open_value_editor(di); return true; }
                for (const DimAnnot& q : m_live_quotes) {
                    if ((q.label_pos - p).norm() <= ltol) {
                        if (q.kind == DimType::Angle)
                            open_angle_editor(q.ea);   // geometric rotate to a typed angle
                        else
                            place_dimension(q);        // promote -> driving dim + open editor
                        return true;
                    }
                }
                if (m_live_poly_fi >= 0) {
                    if ((m_live_poly_side_label - p).norm() <= ltol) {
                        open_polygon_side_editor(m_live_poly_fi);   // geometric uniform scale
                        return true;
                    }
                    if ((m_live_poly_angle_label - p).norm() <= ltol) {
                        open_polygon_angle_editor(m_live_poly_fi);  // geometric rotate
                        return true;
                    }
                }
                if (m_live_arc_ei >= 0 && (m_live_arc_angle_label - p).norm() <= ltol) {
                    open_arc_angle_editor(m_live_arc_ei);           // geometric sweep change
                    return true;
                }
                if (m_live_ellipse_ei >= 0) {
                    if ((m_live_ellipse_major_label - p).norm() <= ltol) {
                        open_ellipse_axis_editor(m_live_ellipse_ei, true);   // semi-major
                        return true;
                    }
                    if ((m_live_ellipse_minor_label - p).norm() <= ltol) {
                        open_ellipse_axis_editor(m_live_ellipse_ei, false);  // semi-minor
                        return true;
                    }
                }
                if (m_live_rrect_fi >= 0) {
                    if ((m_live_rrect_w_label - p).norm() <= ltol) { open_rounded_rect_editor(m_live_rrect_fi, 0); return true; }
                    if ((m_live_rrect_h_label - p).norm() <= ltol) { open_rounded_rect_editor(m_live_rrect_fi, 1); return true; }
                    if ((m_live_rrect_r_label - p).norm() <= ltol) { open_rounded_rect_editor(m_live_rrect_fi, 2); return true; }
                }
                if (m_live_aslot_fi >= 0) {
                    if ((m_live_aslot_r_label - p).norm() <= ltol) { open_arc_slot_editor(m_live_aslot_fi, true);  return true; }
                    if ((m_live_aslot_w_label - p).norm() <= ltol) { open_arc_slot_editor(m_live_aslot_fi, false); return true; }
                }
                if (m_live_slot_fi >= 0) {
                    if ((m_live_slot_len_label   - p).norm() <= ltol) { open_slot_editor(m_live_slot_fi, 0); return true; }
                    if ((m_live_slot_w_label     - p).norm() <= ltol) { open_slot_editor(m_live_slot_fi, 1); return true; }
                    if ((m_live_slot_angle_label - p).norm() <= ltol) { open_slot_editor(m_live_slot_fi, 2); return true; }
                }
            }

            // A derived handle (the circle RadiusHandle — not an entity point, so
            // hit_test_point can't grab it) arms a handle drag that resizes on motion.
            // Checked before the point/entity hit-tests so the radius grip wins near
            // the circle edge.
            if (evt.LeftDown()) {
                Handle hh;
                if (hit_test_handle(p, tol, hh) &&
                    (hh.role == HandleRole::RadiusHandle || hh.role == HandleRole::MajorAxis ||
                     hh.role == HandleRole::MinorAxis || hh.role == HandleRole::BSplineCtrl)) {
                    m_dragging_handle = true;
                    m_drag_handle     = hh;
                    m_selection.clear();
                    m_point_sel.clear();
                    m_selection.push_back(hh.ei);   // highlight the circle being resized
                    if (on_selection_changed)
                        on_selection_changed(int(m_selection.size()));
                    return true;
                }
            }

            // A nearby endpoint/centre selects that POINT (a line = a segment + 2
            // points); a click on the bare segment selects the whole entity.
            if (evt.LeftDown()) {
                int pe; SketchPointRole pr;
                if (hit_test_point(p, tol, pe, pr)) {
                    const auto key = std::make_pair(pe, pr);
                    auto it = std::find(m_point_sel.begin(), m_point_sel.end(), key);
                    if (extend) {
                        if (it == m_point_sel.end()) m_point_sel.push_back(key);
                        else m_point_sel.erase(it);
                    } else {
                        m_selection.clear();
                        m_point_sel.clear();
                        m_point_sel.push_back(key);
                    }
                    // Arm the drag so the grabbed point follows the cursor.
                    m_dragging_point = true;
                    m_drag_ei = pe;
                    m_drag_role = pr;
                    // If the grabbed point is a polygon vertex, the drag must keep the
                    // polygon REGULAR — it adjusts the circumradius + orientation (the
                    // vertex follows the cursor) instead of moving one point freely.
                    const int pf = feature_of(pe);
                    m_drag_poly_fi = (pf >= 0 && m_features[pf].kind == FeatureKind::Polygon) ? pf : -1;
                    m_drag_rect_fi = -1; m_drag_slot_fi = -1;
                    if (pf >= 0 && m_drag_poly_fi < 0) {
                        const Feature& ft = m_features[pf];
                        if (ft.kind == FeatureKind::CornerRect || ft.kind == FeatureKind::CenterRect) {
                            // Only axis-aligned boxes resize by corner (oblique rects fall
                            // back to free point move). Capture the fixed opposite corner.
                            const SketchEntity& e0 = m_entities[ft.begin];
                            const Vec2d d0 = e0.p1 - e0.p0;
                            const bool aa = std::abs(d0.x()) < 1e-6 || std::abs(d0.y()) < 1e-6;
                            Vec2d gp;
                            if (aa && point_at(pe, pr, gp)) {
                                Vec2d opp;
                                opp.x() = (std::abs(gp.x() - ft.c0.x()) < std::abs(gp.x() - ft.c1.x())) ? ft.c1.x() : ft.c0.x();
                                opp.y() = (std::abs(gp.y() - ft.c0.y()) < std::abs(gp.y() - ft.c1.y())) ? ft.c1.y() : ft.c0.y();
                                m_drag_rect_fi = pf; m_drag_rect_anchor = opp;
                            }
                        } else if (ft.kind == FeatureKind::Slot &&
                                   pr == SketchPointRole::Center &&
                                   (pe == ft.begin + 1 || pe == ft.begin + 3)) {
                            m_drag_slot_fi = pf;                 // cap@c1 = begin+1, cap@c0 = begin+3
                            m_drag_slot_c1 = (pe == ft.begin + 1);
                        }
                    }
                    if (on_selection_changed)
                        on_selection_changed(int(m_selection.size() + m_point_sel.size()));
                    return true;
                }
            }

            const int hit = hit_test(p, tol);

            if (evt.LeftDClick() && hit >= 0) {
                if (!extend) m_selection.clear();
                for (int idx : connected_loop(hit))
                    if (std::find(m_selection.begin(), m_selection.end(), idx) == m_selection.end())
                        m_selection.push_back(idx);
            } else if (hit >= 0) {
                auto it = std::find(m_selection.begin(), m_selection.end(), hit);
                if (extend) {
                    if (it == m_selection.end()) m_selection.push_back(hit);
                    else m_selection.erase(it);     // toggle off
                } else {
                    m_selection.clear();
                    m_point_sel.clear();
                    m_selection.push_back(hit);
                }
            } else if (!extend) {
                // Inside a closed loop (not on an edge/point) → select it as a face
                // and hand off to the panel, which commits the sketch and extrudes.
                if (evt.LeftDown() && on_face_selected) {
                    const int reg = region_at(p);
                    if (reg >= 0) {
                        m_selection.clear();
                        m_point_sel.clear();
                        on_face_selected(reg);
                        return true;
                    }
                }
                m_selection.clear();                // clicked empty space
                m_point_sel.clear();
            }
            if (on_selection_changed)
                on_selection_changed(int(m_selection.size() + m_point_sel.size()));
            return true;
        }
        if (evt.RightDown()) {
            // Hand the click back (return false) so the offer opens: the m_right_consumed flag
            // this return value feeds means "the tool USED this right-click", and a plain
            // right-click in Select mode is not a gesture terminator.
            //
            // But do NOT drop the selection on the way out. The offer menu describes WHAT IS
            // SELECTED, so clearing first guaranteed it could only ever describe nothing: select
            // a line, right-click, and the sketch verbs — Trim, Extend, Fillet, Chamfer, Offset,
            // Mirror, the arrays, Constrain — were all greyed, because by the time the menu was
            // built the line was no longer selected. Reported from the machine as "selected a
            // line, right-click exit from selection: only create and reference are usable".
            // Deselecting still has a gesture: left-click on empty space, a few lines above.
            return false;
        }
        return false;                               // let drag orbit the camera
    }

    // Dimension mode: click entities directly. 2 points -> Distance, a line -> Length,
    // a circle -> Diameter, an arc -> Radius, a point then a line -> DistanceToLine.
    // Each resolved pick places a driving quote and pops the value card.
    if (m_mode == Mode::Dimension) {
        if (update_hover(canvas, evt)) return true;   // repaint when the hovered handle changes
        if (evt.LeftDClick()) {                    // double-click a quote label -> edit it
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            const Linef3 r2 = canvas.mouse_ray(Point(evt.GetX() + 28, evt.GetY()));
            const Vec2d p2 = m_plane.project(r2.a, r2.vector());
            const double di_tol = std::max(2.0, (p2 - p).norm());
            const int di = hit_test_dimension(p, di_tol);
            if (di >= 0) edit_dimension(di);
            m_dim_has0 = false;
            return true;
        }
        if (evt.LeftDown()) {
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            const Linef3 r2 = canvas.mouse_ray(Point(evt.GetX() + 8, evt.GetY()));
            const Vec2d p2 = m_plane.project(r2.a, r2.vector());
            const double tol = std::max(1e-3, (p2 - p).norm());
            int pe; SketchPointRole pr;
            const bool got_pt = hit_test_point(p, tol, pe, pr);
            const int he = hit_test(p, tol);
            if (!m_dim_has0) {
                if (got_pt) {                          // first point picked: await a second
                    m_dim_e0 = pe; m_dim_r0 = pr; m_dim_has0 = true;
                } else if (he >= 0) {                  // whole-entity dimension
                    DimAnnot a; a.ea = he;
                    const SketchEntity::Type t = m_entities[he].type;
                    if (t == SketchEntity::Type::Line)        { a.kind = DimType::Length;   place_dimension(a); }
                    else if (t == SketchEntity::Type::Circle) { a.kind = DimType::Diameter; place_dimension(a); }
                    else if (t == SketchEntity::Type::Arc)    { a.kind = DimType::Radius;   place_dimension(a); }
                }
            } else {
                if (got_pt && !(pe == m_dim_e0 && pr == m_dim_r0)) {
                    DimAnnot a; a.kind = DimType::Distance;
                    a.ea = m_dim_e0; a.ra = m_dim_r0; a.eb = pe; a.rb = pr;
                    place_dimension(a);
                } else if (he >= 0 && m_entities[he].type == SketchEntity::Type::Line) {
                    DimAnnot a; a.kind = DimType::DistanceToLine;
                    a.ea = m_dim_e0; a.ra = m_dim_r0; a.eb = he;
                    place_dimension(a);
                }
                m_dim_has0 = false;                    // reset after the second pick
            }
            return true;
        }
        if (evt.RightDown()) { m_dim_has0 = false; return true; }
        return false;                                  // let drag orbit the camera
    }

    if (evt.Moving()) {
        m_snap_off = evt.ShiftDown();
        screen_to_plane(canvas, evt, m_cursor);
        m_has_cursor = true;
        m_cursor_locked = false;
        bool vsnap = false;
        m_cursor = snap_vertex(canvas, evt, m_cursor, vsnap);   // preview-snap to endpoints
        const bool line_like = (m_mode == Mode::Polyline || m_mode == Mode::Line);
        if (line_like && !m_points.empty() && !vsnap)
            m_cursor = snap_dir(m_points.back(), m_cursor, m_cursor_locked);
        if (on_cursor_metrics && line_like && !m_points.empty() && !m_awaiting_length) {
            const Vec2d d = m_cursor - m_points.back();
            on_cursor_metrics(d.norm(), std::atan2(d.y(), d.x()) * 180.0 / M_PI, m_cursor_locked);
        }
        return true;
    }

    switch (m_mode) {

    case Mode::Polyline: {
        if (evt.LeftDown()) {
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            m_snap_off = evt.ShiftDown();
            bool vsnap = false;
            p = snap_vertex(canvas, evt, p, vsnap);   // snap onto an existing endpoint
            if (near_first(p)) {              // closing the current chain back to its start
                const int base = int(m_entities.size());
                push_closed_lines(m_points);  // close the loop
                infer_auto_constraints(base); // loop self-closes via auto Coincident + H/V
                m_points.clear();
                return true;
            }
            if (!m_points.empty() && !vsnap) {
                bool lk = false;
                p = snap_dir(m_points.back(), p, lk);  // lock new segment to inference angle
            }
            m_points.push_back(p);
            arm_polyline_segment_edit();   // refine this segment's Length+Angle, then continue
            return true;
        }
        if (evt.LeftDClick()) {
            if (m_points.size() >= 2) {
                const int base = int(m_entities.size());
                push_open_chain(m_points);     // end as an open chain
                infer_auto_constraints(base);
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) {
            const int base = int(m_entities.size());
            if (m_points.size() >= 3)
                push_closed_lines(m_points);
            else if (m_points.size() == 2)
                push_open_chain(m_points);
            infer_auto_constraints(base);
            m_points.clear();
            return true;
        }
        break;
    }

    case Mode::Line: {
        if (evt.LeftDown()) {
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            m_snap_off = evt.ShiftDown();
            bool vsnap = false;
            p = snap_vertex(canvas, evt, p, vsnap);   // snap onto an existing endpoint
            if (m_points.empty()) {     // first click = anchor
                m_points.push_back(p);
                return true;
            }
            bool lk = false;
            if (!vsnap) p = snap_dir(m_points.back(), p, lk);  // vertex snap wins over angle
            m_points.push_back(p);      // second click completes the segment
            // Draw-then-edit: just commit the segment. The generic detect/service path
            // (is_creation_autoedit_mode now includes Line) auto-selects it and opens its
            // Length THEN Angle fields in sequence, each over its label — same UX as every
            // other 2D tool. Enter advances (Length drives a Distance constraint, Angle rotates
            // about P0); Esc keeps it as drawn.
            keep_segment_as_drawn();
            return true;
        }
        if (evt.RightDown()) {          // abandon the in-progress anchor
            m_points.clear();
            m_has_cursor = false;
            return true;
        }
        break;
    }

    case Mode::CornerRect: {
        if (evt.LeftDown()) {
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            if (m_points.empty()) {
                m_points.push_back(p);
            } else {
                const Vec2d A = m_points[0];
                const Vec2d B = p;
                const int base = int(m_entities.size());
                begin_feature(FeatureKind::CornerRect);
                push_closed_lines({ A, Vec2d(B.x(), A.y()), B, Vec2d(A.x(), B.y()) });
                infer_auto_constraints(base);   // corners Coincident + sides H/V
                end_feature(A, B);              // group: Width/Height live quotes
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::CenterRect: {
        if (evt.LeftDown()) {
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            if (m_points.empty()) {
                m_points.push_back(p);
            } else {
                const Vec2d C = m_points[0];
                const double hx = std::abs(p.x() - C.x());
                const double hy = std::abs(p.y() - C.y());
                const int base = int(m_entities.size());
                begin_feature(FeatureKind::CenterRect);
                push_closed_lines({
                    Vec2d(C.x() - hx, C.y() - hy), Vec2d(C.x() + hx, C.y() - hy),
                    Vec2d(C.x() + hx, C.y() + hy), Vec2d(C.x() - hx, C.y() + hy) });
                infer_auto_constraints(base);
                end_feature(Vec2d(C.x() - hx, C.y() - hy), Vec2d(C.x() + hx, C.y() + hy));
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::ObliqueRect: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            bool vsnap = false;
            if (m_points.size() < 2)                 // corners snap; 3rd click is the width
                p = snap_vertex(canvas, evt, p, vsnap);
            m_points.push_back(p);
            if (m_points.size() == 3) {
                const Vec2d A = m_points[0], B = m_points[1];
                Vec2d u = B - A;
                if (u.squaredNorm() > 1e-12) {
                    u.normalize();
                    const Vec2d n(-u.y(), u.x());
                    const double w = n.dot(m_points[2] - A);   // signed perpendicular width
                    const int base = int(m_entities.size());
                    begin_feature(FeatureKind::CornerRect);
                    push_closed_lines({ A, B, B + n * w, A + n * w });
                    infer_auto_constraints(base);   // corners Coincident + the AB pair parallel
                    end_feature(A, B + n * w);      // group: Width/Height live quotes
                }
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::RoundedRect: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            bool vsnap = false;
            if (m_points.size() < 2)                 // the two corners snap; 3rd sets radius
                p = snap_vertex(canvas, evt, p, vsnap);
            m_points.push_back(p);
            if (m_points.size() == 3) {
                const int base = int(m_entities.size());
                begin_feature(FeatureKind::RoundedRect);
                append_entities(make_rounded_rect(m_points[0], m_points[1], m_points[2]));
                infer_auto_constraints(base);
                // Group with the actual clamped fillet radius so the W/H/R live quotes
                // and rebuild edits can recover the box. (Skip grouping if degenerate.)
                const Vec2d a = m_points[0], b = m_points[1];
                const double xmin=std::min(a.x(),b.x()), xmax=std::max(a.x(),b.x());
                const double ymin=std::min(a.y(),b.y()), ymax=std::max(a.y(),b.y());
                const double bw=xmax-xmin, bh=ymax-ymin;
                const Vec2d cs[4]={{xmin,ymin},{xmax,ymin},{xmax,ymax},{xmin,ymax}};
                double r=1e18; for(const Vec2d&c:cs) r=std::min(r,(m_points[2]-c).norm());
                r=std::min(r, std::min(bw,bh)*0.5);
                end_feature(Vec2d(xmin,ymin), Vec2d(xmax,ymax), r);
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::CenterCircle: {
        if (evt.LeftDown()) {
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            if (m_points.empty()) {
                m_points.push_back(p);
            } else {
                const Vec2d C = m_points[0];
                push_circle(C, (p - C).norm());
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::TwoPointCircle: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            bool vsnap = false;
            p = snap_vertex(canvas, evt, p, vsnap);   // diameter ends snap onto geometry
            m_points.push_back(p);
            if (m_points.size() == 2) {
                const Vec2d C = (m_points[0] + m_points[1]) * 0.5;
                push_circle(C, (m_points[1] - m_points[0]).norm() * 0.5);
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::ThreePointCircle: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            m_points.push_back(p);
            if (m_points.size() == 3) {
                append_entities(make_three_point_circle(m_points[0], m_points[1], m_points[2]));
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::ThreePointArc: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            bool vsnap = false;
            if (m_points.size() < 2)                 // snap the start/end onto endpoints
                p = snap_vertex(canvas, evt, p, vsnap);  // (the 3rd click is the through-point)
            m_points.push_back(p);
            if (m_points.size() == 3) {
                // clicks: start, end, point-on-arc
                const int base = int(m_entities.size());
                append_entities(make_three_point_arc(m_points[0], m_points[1], m_points[2]));
                infer_auto_constraints(base);   // arc ends Coincident onto snapped vertices
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::TangentArc: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            bool vsnap = false;
            p = snap_vertex(canvas, evt, p, vsnap);   // snap both ends onto endpoints
            m_points.push_back(p);
            if (m_points.size() == 2) {
                const int base = int(m_entities.size());
                append_entities(make_tangent_arc(m_points[0], m_points[1]));
                infer_auto_constraints(base);   // tangent-arc ends Coincident onto vertices
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::CenterArc: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            bool vsnap = false;
            // The start (2nd click) snaps onto endpoints; center & end-dir are free.
            if (m_points.size() == 1)
                p = snap_vertex(canvas, evt, p, vsnap);
            m_points.push_back(p);
            if (m_points.size() == 3) {
                const int base = int(m_entities.size());
                append_entities(make_center_arc(m_points[0], m_points[1], m_points[2]));
                infer_auto_constraints(base);   // arc start Coincident onto a snapped vertex
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::Slot: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            if (m_points.size() < 2) {
                m_points.push_back(p);
            } else {
                // third click sets the half-width (distance to the centerline)
                Vec2d u = m_points[1] - m_points[0];
                if (u.squaredNorm() > 1e-12) {
                    u.normalize();
                    const Vec2d n(-u.y(), u.x());
                    const double w = std::abs(n.dot(p - m_points[0]));
                    const int base = int(m_entities.size());
                    begin_feature(FeatureKind::Slot);
                    append_entities(make_slot(m_points[0], m_points[1], w));
                    infer_auto_constraints(base);
                    end_feature(m_points[0], m_points[1], w);  // centres + half-width
                }
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::ArcSlot: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            bool vsnap = false;
            if (m_points.size() == 1)                // start snaps; center & end-dir are free
                p = snap_vertex(canvas, evt, p, vsnap);
            m_points.push_back(p);
            if (m_points.size() == 4) {
                // clicks: center, start, end-dir, width
                const double Rc = (m_points[1] - m_points[0]).norm();
                const double w  = std::abs((m_points[3] - m_points[0]).norm() - Rc);
                const int base = int(m_entities.size());
                begin_feature(FeatureKind::ArcSlot);
                append_entities(make_arc_slot(m_points[0], m_points[1], m_points[2], w));
                infer_auto_constraints(base);
                // c0 = centre, c1 = centreline start point; param = half-width. The end
                // direction is recovered from the cap@E arc centre when rebuilding.
                end_feature(m_points[0], m_points[1], w);
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::Polygon: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            if (m_points.empty()) {
                m_points.push_back(p);
            } else {
                const int base = int(m_entities.size());
                begin_feature(FeatureKind::Polygon);
                append_entities(make_polygon(m_points[0], p, m_polygon_sides));
                infer_auto_constraints(base);
                end_feature(m_points[0], p, (p - m_points[0]).norm(), m_polygon_sides);
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::Ellipse: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            bool vsnap = false;
            if (m_points.empty()) p = snap_vertex(canvas, evt, p, vsnap);   // center can snap
            m_points.push_back(p);
            if (m_points.size() == 3) {                                     // center, major-end, minor
                const int base = int(m_entities.size());
                append_entities(make_ellipse(m_points[0], m_points[1], m_points[2]));
                infer_auto_constraints(base);
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::EllipseArc: {
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            bool vsnap = false;
            if (m_points.empty()) p = snap_vertex(canvas, evt, p, vsnap);   // center can snap
            m_points.push_back(p);
            if (m_points.size() == 5) {                                     // center, major, minor, start, end
                const int base = int(m_entities.size());
                append_entities(make_ellipse_arc(m_points[0], m_points[1], m_points[2],
                                                  m_points[3], m_points[4]));
                infer_auto_constraints(base);
                m_points.clear();
            }
            return true;
        }
        if (evt.RightDown()) { m_points.clear(); return true; }
        break;
    }

    case Mode::BSpline: {
        // Variable-length: left-click adds a control pole (each can snap onto existing
        // geometry); double-click or right-click finishes as an open spline.
        if (evt.LeftDown()) {
            Vec2d p; screen_to_plane(canvas, evt, p);
            m_snap_off = evt.ShiftDown();
            bool vsnap = false;
            p = snap_vertex(canvas, evt, p, vsnap);   // poles can land on endpoints
            m_points.push_back(p);
            return true;
        }
        if (evt.LeftDClick() || evt.RightDown()) {
            if (m_points.size() >= 2) {
                const int base = int(m_entities.size());
                append_entities(make_bspline(m_points));
                infer_auto_constraints(base);         // end poles auto-Coincident -> loops close
            }
            m_points.clear();
            return true;
        }
        break;
    }

    case Mode::Point: {
        if (evt.LeftDown()) {
            Vec2d p;
            screen_to_plane(canvas, evt, p);
            push_point(p);
            return true;
        }
        if (evt.RightDown()) { return true; }
        break;
    }

    case Mode::Constrain:
        break;
    }

    if (evt.Dragging())
        return false;

    return false;
}

}} // namespace Slic3r::GUI
