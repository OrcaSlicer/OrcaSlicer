// The place Design, its Sketch heading: sketch_create, sketch_edit, sketch_finish and sketch_cancel, which open and
// close a sketch; sketch_describe, sketch_validate, sketch_repair and sketch_list_constraints, which read and mend
// one; sketch_project, sketch_text, sketch_insert_svg and sketch_blend_curve, which add curves made from edges, text,
// an SVG file or two other curves; and the verbs that draw, edit and constrain the open sketch's entities
// (sketch_draw, sketch_constrain, sketch_dimension and the rest), each on the DesignSketchTool the mouse drives.
// sketch_silent_forms() binds them to their tool blocks, and run_design_tool() (DesignTools.cpp) runs them behind the
// Design commands' gate.

#include "slic3r/GUI/Mcp/Tools/DesignCommon.hpp"
#include "slic3r/GUI/Mcp/McpToolsDesign.hpp"
#include "libslic3r/Point.hpp"
#include <stdexcept>
#include <math.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <utility>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include <nlohmann/json.hpp>
#include <wx/string.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/SketchSolver.hpp"
#include "libslic3r/CAD/SketchImport.hpp"   // svg_to_regions, transform_regions
#include <boost/algorithm/string/case_conv.hpp>

#include <TopoDS_Shape.hxx>

using json = nlohmann::json;

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// ---- 2D sketch verbs ----------------------------------------------------------------------------
//
// The model is FreeCAD's Sketcher, adapted to this tab's right-click world. Three ideas are
// taken over deliberately:
//
//   * GEOMETRY IS SEPARATE FROM CONSTRAINTS. You add curves, then you constrain them; the
//     solver reports degrees of freedom and whether it is consistent. `sketch_describe` returns
//     both halves plus the DoF, which is the whole state a caller needs to reason about.
//   * A SKETCH IS JUDGED BY ITS LOOPS, not by its coordinates. FreeCAD asks whether the profile
//     is closed before it will build from it; `sketch_describe` answers that directly, listing
//     each closed loop, the loops it encloses as VOIDS, and — the actionable part — the exact
//     plane coordinates where a chain is still open.
//   * VALIDATE, THEN FIX. FreeCAD's ValidateSketch finds vertices that overlap within a
//     tolerance but carry no coincidence, and adds the missing ones. `sketch_validate` reports
//     them, `sketch_repair` welds and constrains them. That is what turns a loop that is closed by
//     floating-point luck into one that is closed by construction and stays closed through
//     every later solve.
//
// What is NOT taken over: FreeCAD's Sketcher is a modal dialog with its own toolbars. Here the
// vocabulary is the right-click offer, so these verbs are named after what the menu offers on a
// selection, and every one of them drives the SAME DesignSketchTool the mouse drives.

DesignSketchTool& mcp_sketch(DesignPanel* panel)
{
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr) throw no_design_view();
    if (!vp->is_sketching())
        throw CadPrecondition("no sketch is open — call sketch_create first");
    return vp->mcp_sketch_tool();
}

// An [x, y] point in the sketch plane, named `what` in the error.
Vec2d point_of(const json& v, const std::string& what)
{
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
        throw std::runtime_error(what + " must be [x, y]");
    return Vec2d(v[0].get<double>(), v[1].get<double>());
}

Vec2d point_arg(const json& j, const char* key) { return point_of(j.value(key, json()), key); }

// A required finite number.
double number_arg(const json& j, const char* key)
{
    const json v = j.value(key, json());
    if (!v.is_number() || !std::isfinite(v.get<double>())) throw std::runtime_error(std::string(key) + " must be a number");
    return v.get<double>();
}

// An angle in radians as `key`, or in degrees as `key`_deg; `def` (radians) when neither is given.
double angle_arg(const json& j, const std::string& key, double def)
{
    const std::string deg = key + "_deg";
    if (j.contains(key) && j.contains(deg)) throw std::runtime_error("give " + key + " or " + deg + ", not both");
    if (j.contains(deg)) return number_arg(j, deg.c_str()) * M_PI / 180.0;
    return j.contains(key) ? number_arg(j, key.c_str()) : def;
}

SketchEntity sketch_entity_from(const json& j)
{
    const std::string t = j.value("type", std::string(""));
    SketchEntity e;
    auto p = [&](const char* k, double dx, double dy) {
        if (!j.contains(k)) return Vec2d(dx, dy);
        const json& a = j.at(k);
        if (!a.is_array() || a.size() < 2) throw std::runtime_error(std::string(k) + " must be [x, y]");
        return Vec2d(a[0].get<double>(), a[1].get<double>());
    };
    e.construction = j.value("construction", false);
    // A circle and an arc are defined BY their centre, so a request that does not carry one is
    // incomplete, not a request for a circle at the origin. Defaulting it silently put geometry
    // somewhere the caller never asked for and then reported perfectly consistent loops, areas
    // and hole attribution ABOUT THAT WRONG GEOMETRY — which is far more expensive to disbelieve
    // than an error would have been. A point is its position, for the same reason.
    auto centre_of = [&](const char* what) {
        if (j.contains("center")) return p("center", 0, 0);
        throw std::runtime_error(std::string(what) + " needs a 'center'");
    };
    if (t == "line") {
        // Its two ends are the line, so one left out is a mistake (often the key spelled another
        // way, start/end), not the origin: a line ending there closes no loop the caller drew.
        if (!j.contains("p0") || !j.contains("p1"))
            throw std::runtime_error("line needs 'p0' and 'p1' as [x, y]");
        e.type = SketchEntity::Type::Line;
        e.p0 = p("p0", 0, 0); e.p1 = p("p1", 0, 0);
    } else if (t == "circle_center") {
        e.type   = SketchEntity::Type::Circle;
        e.center = centre_of("circle_center");
        e.radius = j.value("radius", 0.0);
        e.p0     = e.center;
        if (e.radius <= 0.0) throw std::runtime_error("circle_center needs a positive 'radius'");
    } else if (t == "arc_center") {
        e.type        = SketchEntity::Type::Arc;
        e.center      = centre_of("arc_center");
        e.radius      = j.value("radius", 0.0);
        e.start_angle = angle_arg(j, "start_angle", 0.0);
        e.end_angle   = angle_arg(j, "end_angle", 0.0);
        if (e.radius <= 0.0) throw std::runtime_error("arc_center needs a positive 'radius'");
        e.p0 = e.center + e.radius * Vec2d(std::cos(e.start_angle), std::sin(e.start_angle));
        e.p1 = e.center + e.radius * Vec2d(std::cos(e.end_angle),   std::sin(e.end_angle));
    } else if (t == "point") {
        e.type = SketchEntity::Type::Point;
        e.p0   = point_arg(j, "p");
    } else {
        throw std::logic_error("sketch_entity_from takes a line, circle_center, arc_center or point, not " + t);
    }
    return e;
}

const char* const kPointRoles[]     = {"p0", "p1", "center"};                      // SketchPointRole

json constraint_ref(int entity, SketchPointRole role, bool point)
{
    if (entity == kSketchRefOrigin) return json{{"entity", "origin"}};
    if (entity == kSketchRefAxisX)  return json{{"entity", "x_axis"}};
    if (entity == kSketchRefAxisY)  return json{{"entity", "y_axis"}};
    json r{{"entity", entity}};
    if (point) r["role"] = enum_name(kPointRoles, int(role));
    return r;
}

} // namespace

// ---- Shared with the other Design files (DesignCommon.hpp) --------------------------------------

json sketch_entity_to(const SketchEntity& e, int index)
{
    json j{{"index", index}, {"construction", e.construction}};
    switch (e.type) {
    case SketchEntity::Type::Line:
        j["type"] = "line";
        j["p0"] = json::array({e.p0.x(), e.p0.y()});
        j["p1"] = json::array({e.p1.x(), e.p1.y()});
        j["length"] = (e.p1 - e.p0).norm();
        break;
    case SketchEntity::Type::Circle:
        j["type"] = "circle";
        j["center"] = json::array({e.center.x(), e.center.y()});
        j["radius"] = e.radius;
        break;
    case SketchEntity::Type::Arc:
        j["type"] = "arc";
        j["center"] = json::array({e.center.x(), e.center.y()});
        j["radius"] = e.radius;
        j["start_angle"] = e.start_angle;
        j["end_angle"]   = e.end_angle;
        j["p0"] = json::array({e.p0.x(), e.p0.y()});
        j["p1"] = json::array({e.p1.x(), e.p1.y()});
        break;
    case SketchEntity::Type::Point:
        j["type"] = "point";
        j["p"] = json::array({e.p0.x(), e.p0.y()});
        break;
    // Ellipses and splines serialise every parameter they have, not just a type name: this is
    // the only read-back the project has, and from a bare type name a caller could only count
    // them and grade the faceted area of the loop they close (2e-2, the faceting error), never
    // check a single axis, angle or pole. A precise definition of every aspect cannot be
    // asserted about an entity whose aspects the read-back does not show.
    case SketchEntity::Type::Ellipse:
        j["type"]     = "ellipse";
        j["center"]   = json::array({e.center.x(), e.center.y()});
        j["radius"]   = e.radius;     // semi-major (a)
        j["rminor"]   = e.rminor;     // semi-minor (b)
        j["rotation"] = e.rotation;   // major-axis angle, radians
        break;
    case SketchEntity::Type::EllipseArc:
        j["type"]        = "ellipse_arc";
        j["center"]      = json::array({e.center.x(), e.center.y()});
        j["radius"]      = e.radius;
        j["rminor"]      = e.rminor;
        j["rotation"]    = e.rotation;
        j["start_angle"] = e.start_angle;
        j["end_angle"]   = e.end_angle;
        j["p0"] = json::array({e.p0.x(), e.p0.y()});
        j["p1"] = json::array({e.p1.x(), e.p1.y()});
        break;
    case SketchEntity::Type::BSpline: {
        j["type"] = "spline";
        json poles = json::array();
        for (const Vec2d& c : e.ctrl) poles.push_back(json::array({c.x(), c.y()}));
        j["ctrl"] = poles;
        j["p0"] = json::array({e.p0.x(), e.p0.y()});
        j["p1"] = json::array({e.p1.x(), e.p1.y()});
        break;
    }
    }
    return j;
}

json constraint_value(SketchConstraintType t, double v)
{
    using T = SketchConstraintType;
    switch (t) {
    case T::Angle: return v * 180.0 / M_PI;
    case T::Distance: case T::DistanceX: case T::DistanceY: case T::LockX: case T::LockY:
    case T::Radius: case T::Diameter: case T::PointOnLine: return v;
    default: return json();
    }
}

json entity_constraint_json(const SketchEntityConstraintDef& c, int index)
{
    using T = SketchConstraintType;
    json refs = json::array();
    auto pt  = [&](int e, SketchPointRole r) { refs.push_back(constraint_ref(e, r, true)); };
    auto ent = [&](int e) { refs.push_back(constraint_ref(e, SketchPointRole::P0, false)); };
    switch (c.type) {
    case T::Fix: case T::LockX: case T::LockY:
        pt(c.ea, c.ra); break;
    case T::Coincident: case T::Horizontal: case T::Vertical: case T::Distance: case T::DistanceX:
    case T::DistanceY: case T::SymmetricAboutY: case T::SymmetricAboutX:
        pt(c.ea, c.ra); pt(c.eb, c.rb); break;
    case T::Radius: case T::Diameter:
        ent(c.ea); break;
    case T::Concentric: case T::EqualLength: case T::Parallel: case T::Perpendicular: case T::Angle:
    case T::Tangent: case T::EqualRadius: case T::Collinear:
        ent(c.ea); ent(c.eb); break;
    case T::Midpoint: case T::PointOnLine: case T::PointOnObject:
        pt(c.ea, c.ra); ent(c.eb); break;
    case T::Symmetric:
        pt(c.ea, c.ra); pt(c.eb, c.rb); ent(c.ec); break;
    }
    json j{{"index", index}, {"type", enum_name(kConstraintTypes, int(c.type))}, {"refs", std::move(refs)}};
    if (json v = constraint_value(c.type, c.value); !v.is_null()) j["value"] = std::move(v);
    return j;
}

int sketch_arg(const json& v, const CadDocument& doc)
{
    if (!v.is_number_integer()) throw std::runtime_error("sketch must be a feature index (describe_design's features[].index)");
    const int si = v.get<int>();
    check_feature_index(si, doc);
    const CadFeature& f = doc.features[si];
    if (f.type != CadFeatureType::Sketch && f.type != CadFeatureType::Project)
        throw std::runtime_error("feature " + std::to_string(si) + " (" + feature_type_name(f.type) + ") is not a sketch");
    return si;
}

namespace {

// Which entities a verb acts on in the open sketch (sketch_target_indices()).
std::vector<int> sketch_targets(const json& params, DesignSketchTool& t, bool all_if_empty)
{
    return sketch_target_indices(params, t.selection(), int(t.entities().size()), all_if_empty);
}

// The open sketch's degrees of freedom, as a reply's dof. The sketch tool keeps -1 while it has no
// constraints, since it runs no solve then and its own readout stays hidden; the count is still
// known, and it is the one a solve would give once a constraint arrives.
int sketch_dof(const DesignSketchTool& t)
{
    return t.constraints().empty() ? sketch_unconstrained_dof(t.entities()) : t.dof();
}

// A sketch's loops. Their indices are the ones extrude's `loop` takes: a region lists the loops
// inside it under `holes`, and each of those is a loop of its own.
json sketch_report(const DesignSketchTool::LoopReport& rep)
{
    json loops = json::array();
    for (size_t i = 0; i < rep.loops.size(); ++i) {
        const auto& l = rep.loops[i];
        json holes = json::array();
        for (int h : l.holes) holes.push_back(h);
        loops.push_back(json{{"index", int(i)}, {"entities", l.ents}, {"holes", holes},
                             {"closed", l.closed}, {"area", l.area}});
    }
    json open_ends = json::array();
    for (const Vec2d& p : rep.open_ends) open_ends.push_back(json::array({p.x(), p.y()}));
    // A profile is buildable when at least one loop closed, nothing is left dangling and no loop
    // crosses or folds back on itself.
    json defects = json::array();
    for (const auto& l : rep.loops)
        if (l.defect) defects.push_back(json::array({l.defect_at.x(), l.defect_at.y()}));
    const bool buildable = !rep.loops.empty() && rep.open_ends.empty() && defects.empty();
    return json{{"closed_loops", loops}, {"open_ends", open_ends}, {"defects", defects}, {"buildable", buildable}};
}

// The session starts as a plane or face pick starts it, so the panel enters Sketch mode with the
// tool: key dispatch reads the panel's mode while the menu reads the viewport's, and a session the
// panel did not know of would leave every sketch key dead.
json action_sketch_create(DesignPanel* panel, const json& params)
{
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr) throw no_design_view();
    if (vp->is_sketching()) throw CadPrecondition("a sketch is already open");
    const CadDocument& doc   = panel->mcp_doc();
    const json         pl    = params.value("plane", json("XY"));
    const SketchPlane  plane = cad_resolve_plane(pl, doc, false);
    // Named for the status line as the GUI names the plane: XY, a datum's name, or the face.
    wxString on;
    if (pl.is_string())             on = wxString::FromUTF8(pl.get<std::string>());
    else if (pl.contains("datum"))  on = wxString::FromUTF8(doc.features[pl["datum"].get<int>()].name);
    else                            on = wxString::Format(_L("Face %d"), pl["face"].get<int>());
    panel->mcp_begin_sketch(plane, on);
    return json{{"ok", true}, {"plane", pl}, {"frame", frame_json(plane)}};
}

// The sketch's Confirm. A failed call is left out of an exported replay, so a call that fails must
// leave the session as it was: a new empty sketch is refused before anything changes (where the
// GUI's commit drops it with a status line), and the session stays open for the replay's next
// sketch call. A re-opened sketch is committed as its Confirm commits it, which replaces it in
// place; a replace the rebuild refuses, or a re-opened sketch left empty, ends the edit with the
// design as it was. That changes the session, so it is a reply, ok false with the reason, which
// the replay keeps and runs to end the edit where the live run ended it.
json action_sketch_finish(DesignPanel* panel, const json& params)
{
    (void)params;
    DesignSketchTool& t      = mcp_sketch(panel);
    const int         edited = panel->mcp_edited_sketch();
    if (edited < 0 && t.entities().empty()) throw CadPrecondition("the sketch is empty; add entities or call sketch_cancel");
    const DesignPanel::SketchFinish done = panel->mcp_finish_sketch();
    if (!done.committed && edited >= 0)
        return json{{"ok", false}, {"replaced", false}, {"sketch_index", edited},
                    {"error", "the sketch was not replaced: " + done.error}};
    if (!done.committed) throw CadPrecondition("the sketch was not committed");
    panel->mcp_after_change();
    // Read back from the committed feature: solving its dimensions can move geometry, and extrude
    // takes `loop` against these entities.
    const CadDocument& doc = panel->mcp_doc();
    const CadFeature&  f   = doc.features[done.index];
    json out{{"ok", true}, {"sketch_index", done.index}, {"name", f.name}, {"features", int(doc.features.size())}};
    if (done.replaced) out["replaced"] = true;
    out.update(sketch_report(t.loop_report(f.entities)));
    return out;
}

json action_sketch_cancel(DesignPanel* panel, const json& params)
{
    (void)params;
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr) throw no_design_view();
    if (!vp->is_sketching()) throw CadPrecondition("no sketch is open");
    panel->mcp_cancel_sketch();
    return json{{"ok", true}};
}

// Edit on a committed sketch's Feature tree row, for a sketch drawn from entities: the sketch
// commands then act on it, and sketch_finish puts it back in its place.
json action_sketch_edit(DesignPanel* panel, const json& params)
{
    const json v = params.is_object() ? params.value("sketch", json()) : json();
    if (!v.is_number_integer()) throw std::runtime_error("sketch must be a feature index (describe_design's features[].index)");
    const int si = v.get<int>();
    // The panel's own refusals of the view and an open sketch are the session's state; the others are the feature's.
    const DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr) throw no_design_view();
    if (vp->is_sketching()) throw CadPrecondition("a sketch is already open; finish or cancel it first");
    std::string why;
    if (!panel->mcp_edit_sketch(si, why)) throw std::runtime_error(why);
    const DesignSketchTool& t   = mcp_sketch(panel);
    const CadDocument&      doc = panel->mcp_doc();
    return json{{"ok", true},
                {"sketch", si},
                {"entities", int(t.entities().size())},
                {"constraints", int(t.constraints().size())},
                {"plane", plane_json(doc.features[si].plane, doc.modeling_origin)}};
}

json action_sketch_select(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    std::vector<int> idx;
    if (params.contains("entities"))
        for (const auto& v : params.at("entities")) idx.push_back(v.get<int>());
    const bool any = t.select_indices(idx);
    panel->mcp_viewport()->request_repaint();
    return json{{"ok", true}, {"selected", int(t.selection().size())}, {"any", any}};
}

json action_sketch_delete(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    const std::vector<int> tgt = sketch_targets(params, t, false);
    if (tgt.empty()) throw std::runtime_error("nothing selected and no 'entities' given");
    const int before = int(t.entities().size());
    t.select_indices(tgt);
    t.delete_selected();
    panel->mcp_viewport()->request_repaint();
    return json{{"ok", true}, {"removed", before - int(t.entities().size())},
                {"entities", int(t.entities().size())}};
}

json action_sketch_toggle_construction(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    const std::vector<int> tgt = sketch_targets(params, t, false);
    if (tgt.empty()) throw std::runtime_error("nothing selected and no 'entities' given");
    t.select_indices(tgt);
    const int n = t.toggle_selection_construction();
    panel->mcp_viewport()->request_repaint();
    return json{{"ok", n > 0}, {"changed", n}};
}

json action_sketch_describe(DesignPanel* panel, const json& params)
{
    // A committed sketch is read from the document, so no session is needed, nor disturbed.
    const json sketch = params.is_object() ? params.value("sketch", json()) : json();
    if (!sketch.is_null()) {
        const DesignCanvas* vp = panel->mcp_viewport();
        if (vp == nullptr) throw no_design_view();
        const CadDocument& doc = panel->mcp_doc();
        return cad_sketch_describe(doc, vp->mcp_sketch_tool(), sketch_arg(sketch, doc));
    }
    DesignSketchTool& t = mcp_sketch(panel);
    json ents = json::array();
    for (int i = 0; i < int(t.entities().size()); ++i)
        ents.push_back(sketch_entity_to(t.entities()[i], i));
    // The armed TOOL and its pending anchors. Without these the only way to tell which tool a
    // menu item actually armed is to draw with it and infer from what came out — which is how a
    // menu walk that lands one item off gets diagnosed as "the tool is broken".
    static const char* const kModeNames[] = {
        "select", "dimension", "polyline", "line", "rect_corner", "rect_center", "rect_oblique",
        "rect_rounded", "circle_center", "circle_2pt", "point",
        "circle_3pt", "arc_3pt", "arc_tangent", "arc_center", "slot", "slot_arc", "polygon",
        "ellipse", "ellipse_arc", "spline",
        "fillet", "chamfer", "offset", "mirror",
        "trim", "extend",
        "move", "rotate", "scale", "array", "array_polar",
        "transform_art",
        "constrain" };
    const int mi = int(t.mode());
    json out{{"ok", true},
             {"entities", ents},
             {"constraints", int(t.constraints().size())},
             {"dof", sketch_dof(t)},
             {"solve_ok", t.solve_ok()},
             {"tool", (mi >= 0 && mi < int(sizeof(kModeNames) / sizeof(kModeNames[0])))
                          ? kModeNames[mi] : "unknown"},
             {"pending", t.pending_points()},
             {"editing", t.value_field_open()},
             {"selection", t.selection()},
             {"selected_constraint", t.selected_constraint() >= 0 ? json(t.selected_constraint()) : json(nullptr)}};
    out.update(sketch_report(t.loop_report()));
    return out;
}

json action_sketch_validate(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    const double tol = params.value("tolerance", 1e-3);
    json out{{"ok", true}, {"tolerance", tol}, {"dof", sketch_dof(t)}, {"solve_ok", t.solve_ok()}};
    out.update(sketch_report(t.loop_report()));
    return out;
}

json action_sketch_repair(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    const double tol = params.value("tolerance", 1e-3);
    const bool   ic  = params.value("ignore_construction", true);
    const int welded = t.heal_coincidences(tol, ic);
    panel->mcp_viewport()->request_repaint();
    json out{{"ok", true}, {"welded", welded}, {"tolerance", tol},
             {"dof", sketch_dof(t)}, {"solve_ok", t.solve_ok()}};
    out.update(sketch_report(t.loop_report()));
    return out;
}

// sketch_set_value commits a typed dimension by calling apply_dimension, the function the in-canvas
// value field calls, so what a typed value does to the geometry, constrained geometry included, can
// be asserted without a window manager giving that field keyboard focus. apply_dimension clears the
// selection.
json action_sketch_set_value(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    if (!params.contains("value")) throw std::runtime_error("sketch_set_value needs 'value'");
    const double value = params["value"].get<double>();
    const DesignSketchTool::DimType kind = t.dimension_kind();
    if (kind == DesignSketchTool::DimType::None)
        throw CadPrecondition("the selection has no value to set — pick a line, an arc, a circle, or two entities");

    const char* kind_name = "none";
    switch (kind) {
    case DesignSketchTool::DimType::Length:         kind_name = "length"; break;
    case DesignSketchTool::DimType::Radius:         kind_name = "radius"; break;
    case DesignSketchTool::DimType::Diameter:       kind_name = "diameter"; break;
    case DesignSketchTool::DimType::Angle:          kind_name = "angle"; break;
    case DesignSketchTool::DimType::Distance:       kind_name = "distance"; break;
    case DesignSketchTool::DimType::DistanceToLine: kind_name = "distance_to_line"; break;
    default: break;
    }

    // Validate BEFORE apply_dimension. The sketch tool refuses a value outside its thresholds
    // itself, but only on its status line and without telling its caller, so this call would
    // report the value as set. Fail loudly instead, with the reason.
    if (!std::isfinite(value))
        throw std::runtime_error("dimension must be finite — NaN or infinity is not a dimension");
    switch (kind) {
    case DesignSketchTool::DimType::Length:
    case DesignSketchTool::DimType::Radius:
    case DesignSketchTool::DimType::Diameter:
        if (value <= 0.0)
            throw std::runtime_error(std::string(kind_name) + " must be positive (got " + std::to_string(value) + ")");
        break;
    case DesignSketchTool::DimType::Distance:
    case DesignSketchTool::DimType::DistanceToLine:
        if (value < 0.0)
            throw std::runtime_error(std::string(kind_name) + " must be >= 0 (zero means coincident / on the line)");
        break;
    case DesignSketchTool::DimType::Angle:
    default:
        break;   // any finite angle is valid
    }

    const double before = t.dimension_current();
    t.apply_dimension(value);
    panel->mcp_viewport()->request_repaint();

    json out{{"ok", true}, {"kind", kind_name}, {"before", before}, {"value", value},
             {"dof", sketch_dof(t)}, {"solve_ok", t.solve_ok()}};
    out.update(sketch_report(t.loop_report()));
    return out;
}

// With a sketch open, new or re-opened, the edges go into it (cad_sketch_project()). Otherwise the Project card's
// Confirm: a new sketch on `plane`, XY when it is left out or null, whose entities follow the body.
json action_sketch_project(DesignPanel* panel, const json& params)
{
    if (DesignCanvas* vp = panel->mcp_viewport(); vp != nullptr && vp->is_sketching()) {
        json r = cad_sketch_project(panel->mcp_doc(), vp->mcp_sketch_tool(), params);
        vp->request_repaint();
        return r;
    }
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no source body to project from");
    int source_body = params.value("source_body", -1);
    int face = params.value("face", -1);
    std::vector<int> edges;
    if (params.contains("edges") && params["edges"].is_array())
        for (const auto& v : params["edges"]) edges.push_back(v.get<int>());
    const json        plane = params.value("plane", json());
    const SketchPlane pl    = cad_resolve_plane(plane.is_null() ? json("XY") : plane, doc, false);
    doc.checkpoint();
    int idx = doc.add_project_edges(source_body, edges, face, pl, panel->mcp_feature_name(_L_CONTEXT("Project", "Design")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"scope", "feature"}, {"project_index", idx}, {"bodies", int(doc.bodies.size())},
                {"error", doc.error}, {"frame", frame_json(pl)}};
}

json action_sketch_blend_curve(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("sketch_blend_curve needs 'sketch' (feature index)");
    if (!params.contains("ent_a"))  throw std::runtime_error("sketch_blend_curve needs 'ent_a' (entity index)");
    if (!params.contains("ent_b"))  throw std::runtime_error("sketch_blend_curve needs 'ent_b' (entity index)");
    int sketch = params["sketch"].get<int>();
    int ent_a  = params["ent_a"].get<int>();
    int ent_b  = params["ent_b"].get<int>();
    int end_a  = params.value("end_a", 1);
    int end_b  = params.value("end_b", 0);
    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    int ei = doc.add_bridge(sketch, ent_a, end_a, ent_b, end_b, "Bridge");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"sketch_index", sketch}, {"entity_index", ei},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// ---- The open sketch: shapes, edit ops and constraints (cad_sketch_call) ------------------------
//
// Each runs the sketch tool's own step for its button or gesture (DesignSketchTool's *_scripted
// entry points), checks every argument first and throws, leaving the sketch as it was. sketch_draw,
// whose tools can still refuse an item while drawing it, puts the sketch back as it was before the call when one does.

// SketchEntity::Type's names, as sketch_describe writes them.
const char* const kEntityTypes[] = {"line", "arc", "circle", "point", "ellipse", "ellipse_arc", "spline"};
static_assert(std::size(kEntityTypes) == size_t(SketchEntity::Type::BSpline) + 1, "every entity type needs a name");

std::string entity_label(const std::vector<SketchEntity>& ents, int i)
{
    if (i == kSketchRefOrigin) return "origin";
    if (i == kSketchRefAxisX) return "x_axis";
    if (i == kSketchRefAxisY) return "y_axis";
    return std::string(kEntityTypes[int(ents[i].type)]) + " " + std::to_string(i);
}

// `v`, a list of entity indices of the open sketch.
std::vector<int> entity_indices(const json& v, const std::vector<SketchEntity>& ents)
{
    if (!v.is_array()) throw std::runtime_error("entities must be a list of entity indices");
    std::vector<int> out;
    for (const json& i : v) {
        if (!i.is_number_integer()) throw std::runtime_error("entities must be a list of entity indices");
        const int e = i.get<int>();
        if (e < 0 || e >= int(ents.size()))
            throw std::runtime_error("entity " + std::to_string(e) + " does not exist; the sketch has " +
                                     std::to_string(ents.size()) + " entities");
        out.push_back(e);
    }
    return out;
}

// `entities`, else the selection: what a tool's picks would be.
std::vector<int> entity_picks(const json& params, const DesignSketchTool& t)
{
    const json v = param(params, "entities");
    return v.is_null() || (v.is_array() && v.empty()) ? t.selection() : entity_indices(v, t.entities());
}

// The tools' own apply steps take over a gesture in progress or an open value field, as a click
// would; the person finishes or drops those first.
void check_sketch_idle(const DesignSketchTool& t)
{
    if (t.value_field_open()) throw CadPrecondition("a value field is open in the sketch; commit or cancel it (Enter or Esc) first");
    if (t.gesture_pending()) throw CadPrecondition("the armed sketch tool has a gesture in progress; finish it or press Esc first");
}

// How many of `cons` reference entity `e`.
int constraints_on(const std::vector<SketchEntityConstraintDef>& cons, int e)
{
    return int(std::count_if(cons.begin(), cons.end(), [e](const SketchEntityConstraintDef& c) {
        return c.ea == e || c.eb == e || c.ec == e;
    }));
}

json solve_state(const DesignSketchTool& t)
{
    return json{{"entities", int(t.entities().size())}, {"constraints", int(t.constraints().size())},
                {"dof", sketch_dof(t)}, {"solve_ok", t.solve_ok()}};
}

// One item of sketch_draw: entities added as given (`tool` Select), or a shape a drawing tool builds
// from its clicks.
struct SketchDrawItem
{
    std::vector<SketchEntity> ents;
    DesignSketchTool::Mode    tool{DesignSketchTool::Mode::Select};
    std::vector<Vec2d>        clicks;   // the tool's; a tangent arc's end only, its start being the end of what it leaves
    int                       sides{0};
    bool                      circumscribed{false};
    bool                      construction{false};
    int                       from{-1};   // the entity a tangent arc leaves; -1 the one added last
};

// sketch_draw's types, in the order its schema lists them, each with the fields it takes beside `type` and
// `construction`.
struct ShapeType
{
    const char*              name;
    std::vector<std::string> fields;
};
const ShapeType kShapeTypes[] = {
    {"line", {"p0", "p1"}},
    {"polyline", {"points"}},
    {"rectangle_2_point", {"p0", "p1"}},
    {"rectangle_center", {"center", "corner"}},
    {"rectangle_3_point", {"p0", "p1", "width"}},
    {"rectangle_rounded", {"p0", "p1", "radius"}},
    {"circle_center", {"center", "radius"}},
    {"circle_2_point", {"p0", "p1"}},
    {"circle_3_point", {"p0", "p1", "p2"}},
    {"arc_3_point", {"p0", "p1", "on_arc"}},
    {"arc_center", {"center", "radius", "start_angle", "start_angle_deg", "end_angle", "end_angle_deg"}},
    {"arc_tangent", {"end", "entity"}},
    {"slot", {"c0", "c1", "width"}},
    {"slot_arc", {"center", "start", "end", "width"}},
    {"ellipse", {"center", "radius", "rminor", "rotation", "rotation_deg"}},
    {"ellipse_arc",
     {"center", "radius", "rminor", "rotation", "rotation_deg", "start_angle", "start_angle_deg", "end_angle", "end_angle_deg"}},
    {"spline", {"ctrl"}},
    {"polygon", {"center", "sides", "radius", "across_flats", "angle", "angle_deg"}},
    {"point", {"p"}},
};

// Whether a, b and c lie on one line, so that no circle runs through them.
bool on_one_line(const Vec2d& a, const Vec2d& b, const Vec2d& c)
{
    const Vec2d u = b - a, v = c - a;
    return std::abs(u.x() * v.y() - u.y() * v.x()) < 1e-9;
}

// A sketch_draw item: a shape's fields turned into the clicks its tool takes, or plain entities. A field given as
// null is left out, as the schema's null defaults say, and a field its type does not take is refused. Every check
// is made here, or for a tangent arc's entity in sketch_draw() before anything is drawn. Whether an arc fits a
// tangent arc is known once the items before it are drawn, since its start is the end of what it leaves; when none
// does, sketch_draw() puts the sketch back as it was before the call.
SketchDrawItem sketch_draw_item(const json& given)
{
    using Mode = DesignSketchTool::Mode;
    using ET   = SketchEntity::Type;
    if (!given.is_object()) throw std::runtime_error("each shape is an object with a type and its fields");
    json j = json::object();
    for (const auto& [key, value] : given.items())
        if (!value.is_null()) j[key] = value;
    SketchDrawItem    it;
    const std::string t    = j.value("type", std::string());
    const auto        type = std::find_if(std::begin(kShapeTypes), std::end(kShapeTypes), [&t](const ShapeType& s) { return t == s.name; });
    if (type == std::end(kShapeTypes)) {
        std::string types;
        for (const ShapeType& s : kShapeTypes) types += (types.empty() ? "" : ", ") + std::string(s.name);
        throw std::runtime_error("unknown type '" + t + "': sketch_draw draws " + types);
    }
    for (const auto& [key, value] : j.items())
        if (key != "type" && key != "construction" && std::find(type->fields.begin(), type->fields.end(), key) == type->fields.end())
            throw std::runtime_error(t + " takes no " + key);
    it.construction = j.value("construction", false);
    // Smaller than the weld tolerance is no shape: its points would merge.
    auto size_arg = [&](const char* key) {
        const double v = number_arg(j, key);
        if (!(v >= kSketchJoinTol)) throw std::runtime_error(t + " " + key + " must be at least 0.001 mm");
        return v;
    };
    // Two corners of a box, which must differ in x and in y.
    auto box_corners = [&](const char* a_key, const char* b_key) {
        const Vec2d a = point_arg(j, a_key), b = point_arg(j, b_key);
        if (std::abs(b.x() - a.x()) < kSketchJoinTol || std::abs(b.y() - a.y()) < kSketchJoinTol)
            throw std::runtime_error(t + " " + a_key + " and " + b_key + " must differ in x and in y");
        return std::make_pair(a, b);
    };
    if (t == "polyline") {
        const json points = j.value("points", json());
        if (!points.is_array() || points.size() < 2) throw std::runtime_error("polyline needs points, 2 or more [[x, y], ...]");
        for (size_t i = 0; i < points.size(); ++i) {
            const Vec2d p = point_of(points[i], "points[" + std::to_string(i) + "]");
            if (i > 0 && (p - it.clicks.back()).norm() < kSketchJoinTol)
                throw std::runtime_error("polyline points " + std::to_string(i - 1) + " and " + std::to_string(i) + " coincide");
            it.clicks.push_back(p);
        }
        // The last point on the first closes the loop, as a click on the chain's start does.
        if (it.clicks.size() >= 3 && (it.clicks.back() - it.clicks.front()).norm() < kSketchJoinTol) {
            if (it.clicks.size() < 4) throw std::runtime_error("a closed polyline needs three points before the one that closes it");
            it.clicks.back() = it.clicks.front();
        }
        it.tool = Mode::Polyline;
    } else if (t == "rectangle_2_point") {
        // The Corner rectangle tool's two clicks, on opposite corners.
        const auto [a, b] = box_corners("p0", "p1");
        it.tool   = Mode::CornerRect;
        it.clicks = {a, b};
    } else if (t == "rectangle_center") {
        const auto [c, corner] = box_corners("center", "corner");
        it.tool   = Mode::CenterRect;
        it.clicks = {c, corner};
    } else if (t == "rectangle_3_point") {
        // One side, then the Oblique rectangle tool's third click, the width off it to the left of p0 to p1.
        const Vec2d  a = point_arg(j, "p0"), b = point_arg(j, "p1");
        const double w = number_arg(j, "width");
        if ((b - a).norm() < kSketchJoinTol) throw std::runtime_error("rectangle_3_point p0 and p1 must be apart");
        if (std::abs(w) < kSketchJoinTol) throw std::runtime_error("rectangle_3_point width must be at least 0.001 mm either way");
        const Vec2d u = (b - a).normalized();
        it.tool   = Mode::ObliqueRect;
        it.clicks = {a, b, a + w * Vec2d(-u.y(), u.x())};
    } else if (t == "rectangle_rounded") {
        const auto [a, b] = box_corners("p0", "p1");
        const double r    = size_arg("radius");
        const double half = 0.5 * std::min(std::abs(b.x() - a.x()), std::abs(b.y() - a.y()));
        if (!(r < half))
            throw std::runtime_error("rectangle_rounded radius must be less than half its shorter side, " + json(half).dump() + " mm");
        // The Rounded rectangle tool's third click sets the corner radius by its distance to the nearest corner.
        it.tool   = Mode::RoundedRect;
        it.clicks = {a, b, Vec2d(std::min(a.x(), b.x()) + r, std::min(a.y(), b.y()))};
    } else if (t == "circle_2_point") {
        const Vec2d a = point_arg(j, "p0"), b = point_arg(j, "p1");
        if ((b - a).norm() < 2.0 * kSketchJoinTol)
            throw std::runtime_error("circle_2_point p0 and p1, the ends of a diameter, must be at least 0.002 mm apart");
        it.tool   = Mode::TwoPointCircle;
        it.clicks = {a, b};
    } else if (t == "circle_3_point") {
        const Vec2d a = point_arg(j, "p0"), b = point_arg(j, "p1"), c = point_arg(j, "p2");
        if (on_one_line(a, b, c)) throw std::runtime_error("circle_3_point p0, p1 and p2 must not lie on one line");
        it.tool   = Mode::ThreePointCircle;
        it.clicks = {a, b, c};
    } else if (t == "arc_3_point") {
        const Vec2d a = point_arg(j, "p0"), b = point_arg(j, "p1"), m = point_arg(j, "on_arc");
        if (on_one_line(a, b, m)) throw std::runtime_error("arc_3_point p0, p1 and on_arc must not lie on one line");
        it.tool   = Mode::ThreePointArc;
        it.clicks = {a, b, m};
    } else if (t == "arc_tangent") {
        if (j.contains("entity")) {
            const json e = j.at("entity");
            if (!e.is_number_integer() || e.get<int>() < 0)
                throw std::runtime_error("arc_tangent entity must be the index of the line or arc it leaves");
            it.from = e.get<int>();
        }
        it.tool   = Mode::TangentArc;
        it.clicks = {point_arg(j, "end")};
    } else if (t == "slot_arc") {
        const Vec2d  c = point_arg(j, "center"), s = point_arg(j, "start"), e = point_arg(j, "end");
        const double w = size_arg("width");
        const double rc = (s - c).norm();
        if (rc < kSketchJoinTol) throw std::runtime_error("slot_arc start must be apart from center");
        if ((e - c).norm() < kSketchJoinTol) throw std::runtime_error("slot_arc end must be apart from center");
        const Vec2d ds = (s - c) / rc, de = (e - c).normalized();
        if (std::abs(ds.x() * de.y() - ds.y() * de.x()) < 1e-9 && ds.dot(de) > 0.0)
            throw std::runtime_error("slot_arc end must lie in another direction from center than start");
        if (!(0.5 * w < rc)) throw std::runtime_error("slot_arc width must be less than twice the distance from center to start");
        // The Arc slot tool's clicks: the centre, the centreline's start, its end's direction, and the width's half off
        // the centreline circle.
        it.tool   = Mode::ArcSlot;
        it.clicks = {c, s, e, c + (rc + 0.5 * w) * ds};
    } else if (t == "polygon") {
        const Vec2d c     = point_arg(j, "center");
        const json  sides = j.value("sides", json());
        if (!sides.is_number_integer() || sides.get<int>() < 3 || sides.get<int>() > 1000)
            throw std::runtime_error("polygon needs sides, 3 to 1000");
        if (j.contains("across_flats") == j.contains("radius"))
            throw std::runtime_error("polygon needs radius (centre to a vertex) or across_flats, one of them");
        // The tool's second click: on a vertex (inscribed), or on an edge's midpoint, half the
        // across-flats size out (circumscribed).
        it.circumscribed = j.contains("across_flats");
        const double d   = it.circumscribed ? size_arg("across_flats") / 2.0 : size_arg("radius");
        const double a   = angle_arg(j, "angle", 0.0);
        it.tool   = Mode::Polygon;
        it.sides  = sides.get<int>();
        it.clicks = {c, c + d * Vec2d(std::cos(a), std::sin(a))};
    } else if (t == "slot") {
        const Vec2d  c0 = point_arg(j, "c0"), c1 = point_arg(j, "c1");
        const double w  = size_arg("width");
        if ((c1 - c0).norm() < kSketchJoinTol) throw std::runtime_error("slot centres c0 and c1 must be apart");
        const Vec2d u = (c1 - c0).normalized();
        it.tool   = Mode::Slot;
        it.clicks = {c0, c1, c0 + 0.5 * w * Vec2d(-u.y(), u.x())};
    } else if (t == "ellipse" || t == "ellipse_arc") {
        const Vec2d  c = point_arg(j, "center");
        const double a = size_arg("radius"), b = size_arg("rminor");
        if (b > a) throw std::runtime_error(t + " rminor must not exceed radius, the semi-major axis");
        const double phi = angle_arg(j, "rotation", 0.0);
        const Vec2d  u(std::cos(phi), std::sin(phi)), v(-std::sin(phi), std::cos(phi));
        it.tool   = Mode::Ellipse;
        it.clicks = {c, c + a * u, c + b * v};
        if (t == "ellipse_arc") {
            for (const char* k : {"start_angle", "end_angle"})
                if (!j.contains(k) && !j.contains(std::string(k) + "_deg"))
                    throw std::runtime_error(std::string("ellipse_arc needs ") + k + " (or " + k + "_deg)");
            const double t0 = angle_arg(j, "start_angle", 0.0), t1 = angle_arg(j, "end_angle", 0.0);
            if (std::abs(std::remainder(t1 - t0, 2.0 * M_PI)) < 1e-9)
                throw std::runtime_error("ellipse_arc end_angle must differ from start_angle; a whole turn is an ellipse");
            // Points on the ellipse at those parametric angles, which the tool reads back off them.
            auto on = [&](double s) { return Vec2d(c + a * std::cos(s) * u + b * std::sin(s) * v); };
            it.tool = Mode::EllipseArc;
            it.clicks.push_back(on(t0));
            it.clicks.push_back(on(t1));
        }
    } else if (t == "spline") {
        const json ctrl = j.value("ctrl", json());
        if (!ctrl.is_array() || ctrl.size() < 2) throw std::runtime_error("spline needs ctrl, 2 or more poles [[x, y], ...]");
        for (size_t i = 0; i < ctrl.size(); ++i) {
            const Vec2d p = point_of(ctrl[i], "ctrl[" + std::to_string(i) + "]");
            if (i > 0 && (p - it.clicks.back()).norm() < kSketchJoinTol)
                throw std::runtime_error("spline poles " + std::to_string(i - 1) + " and " + std::to_string(i) + " coincide");
            it.clicks.push_back(p);
        }
        it.tool = Mode::BSpline;
    } else {
        it.ents.push_back(sketch_entity_from(j));   // line, circle_center, arc_center, point
    }
    return it;
}

json sketch_draw(DesignSketchTool& t, const json& params)
{
    using Mode = DesignSketchTool::Mode;
    std::vector<SketchDrawItem> items;
    const json list = param(params, "entities"), type = param(params, "type");
    if (!list.is_null()) {
        // An entities given is the list form, an empty one included. Each item holds its shape's fields, so one beside
        // the list would draw nothing, and is refused. construction is no shape's field: beside the list it is each
        // item's that gives none of its own.
        for (const auto& [key, value] : params.items())
            if (key != "entities" && key != "construction" && key != "generation" && !value.is_null())
                throw std::runtime_error(key + " belongs in an item of entities, not beside the list");
        if (!list.is_array()) throw std::runtime_error("entities must be a list of shapes, each an object with a type and its fields");
        if (list.empty()) throw std::runtime_error("entities is empty: give it the shapes to draw, or a single type and its fields");
        const json construction = param(params, "construction");
        for (json j : list) {
            if (j.is_object() && !construction.is_null() && j.value("construction", json()).is_null()) j["construction"] = construction;
            items.push_back(sketch_draw_item(j));
        }
    } else if (type.is_string() && !type.get<std::string>().empty()) {
        // One item, its fields beside `type`.
        json one = params;
        one.erase("entities");
        one.erase("generation");
        items.push_back(sketch_draw_item(one));
    } else {
        throw std::runtime_error("sketch_draw needs 'entities' or a single 'type'");
    }
    if (std::any_of(items.begin(), items.end(), [](const SketchDrawItem& it) { return it.tool != Mode::Select; }))
        check_sketch_idle(t);

    const int before = int(t.entities().size());
    json shapes = json::array();
    // An item refused while drawing puts the sketch back as it was before the call, the items before it taken off.
    const DesignSketchTool::DrawnState kept = t.drawn_state();
    try {
        for (size_t i = 0; i < items.size();) {
            if (items[i].tool == DesignSketchTool::Mode::Select) {
                // Entities next to each other are added in one batch, so their constraints are inferred
                // together, as a list of entities always was.
                std::vector<SketchEntity> run;
                size_t end = i;
                for (; end < items.size() && items[end].tool == DesignSketchTool::Mode::Select; ++end)
                    run.insert(run.end(), items[end].ents.begin(), items[end].ents.end());
                int at = t.add_entities_scripted(run);
                for (; i < end; ++i) {
                    shapes.push_back(json{{"first", at}, {"count", int(items[i].ents.size())}});
                    at += int(items[i].ents.size());
                }
            } else {
                const SketchDrawItem& it     = items[i++];
                std::vector<Vec2d>    clicks = it.clicks;
                if (it.tool == Mode::TangentArc) {
                    // It leaves a line or an arc: the entity it names, one the sketch has or one an item before it
                    // added, else the entity added last. Its start is that entity's end, as the tool's first click
                    // snaps onto it.
                    const auto& ents = t.entities();
                    if (it.from >= int(ents.size()))
                        throw std::runtime_error("arc_tangent entity " + std::to_string(it.from) + " does not exist; the sketch has " +
                                                 std::to_string(ents.size()) + " entities with the items before it");
                    if (ents.empty()) throw CadPrecondition("arc_tangent needs a line or an arc to leave, and the sketch has none");
                    const int from = it.from >= 0 ? it.from : int(ents.size()) - 1;
                    if (ents[from].type != SketchEntity::Type::Line && ents[from].type != SketchEntity::Type::Arc)
                        throw std::runtime_error("arc_tangent leaves a line or an arc, and " + entity_label(ents, from) + " is neither");
                    clicks.insert(clicks.begin(), ents[from].p1);
                }
                const int at = t.add_shape_scripted(it.tool, clicks, it.sides, it.circumscribed, it.construction, it.from);
                if (at < 0 && it.tool == Mode::TangentArc)
                    throw std::runtime_error("arc_tangent from item " + std::to_string(i - 1) +
                                             " makes no arc: its end must be apart from its start, " +
                                             json::array({clicks[0].x(), clicks[0].y()}).dump() + ", and off the line it leaves along");
                if (at < 0) throw std::runtime_error("the sketch tool built nothing from item " + std::to_string(i - 1));
                shapes.push_back(json{{"first", at}, {"count", int(t.entities().size()) - at}});
            }
        }
    } catch (...) {
        t.restore_drawn_state(kept);
        throw;
    }
    return json{{"ok", true}, {"first_index", before}, {"added", int(t.entities().size()) - before},
                {"shapes", std::move(shapes)}, {"entities", int(t.entities().size())}, {"dof", sketch_dof(t)}};
}

// sketch_fillet and sketch_chamfer: the Fillet and Chamfer tools on two picked lines.
json sketch_corner(DesignSketchTool& t, const json& params, bool chamfer)
{
    const std::string what  = chamfer ? "chamfer" : "fillet";
    const char*       key   = chamfer ? "distance" : "radius";
    const auto&       ents  = t.entities();
    const std::vector<int> lines = entity_picks(params, t);
    if (lines.size() != 2 || lines[0] == lines[1])
        throw std::runtime_error(what + " needs two lines, as entities or the selection");
    for (int i : lines)
        if (ents[i].type != SketchEntity::Type::Line)
            throw std::runtime_error(what + " needs two lines that meet at a corner; " + entity_label(ents, i) + " is not a line");
    const double value = number_arg(params, key);
    if (!(value > 0.0)) throw std::runtime_error(std::string(key) + " must be > 0");
    const int   a = lines[0], b = lines[1];
    const Vec2d da = ents[a].p1 - ents[a].p0, db = ents[b].p1 - ents[b].p0;
    if (std::abs(da.x() * db.y() - da.y() * db.x()) < 1e-12)   // the tool's own corner test (op_corner)
        throw std::runtime_error(what + " needs two lines that meet at a corner; lines " + std::to_string(a) + " and " +
                                 std::to_string(b) + " are parallel");
    check_sketch_idle(t);
    const int cons_before = int(t.constraints().size());
    if (!t.fillet_scripted(a, b, value, chamfer))
        throw std::runtime_error("the " + std::string(key) + " " + json(value).dump() + " overruns a leg of line " +
                                 std::to_string(a) + " or " + std::to_string(b));
    // The new arc or segment is last; every constraint the op added binds it, the rest it released.
    const int added = int(ents.size()) - 1, bound = constraints_on(t.constraints(), added);
    json r{{"ok", true}, {"entity", added}, {"lines", json::array({a, b})},
           {"released_constraints", cons_before + bound - int(t.constraints().size())}};
    r.update(solve_state(t));
    return r;
}

// What sketch_offset and sketch_mirror copy: `entities`, else the selection, else every entity. A listed index the
// sketch does not have is refused, and so is one listed twice.
std::vector<int> copy_sources(const json& params, const DesignSketchTool& t, bool& listed)
{
    const json given = param(params, "entities");
    listed           = !given.is_null() && !(given.is_array() && given.empty());
    const std::vector<int> out = listed ? entity_indices(given, t.entities())
                                        : sketch_target_indices(params, t.selection(), int(t.entities().size()), true);
    for (size_t i = 0; i < out.size(); ++i)
        if (std::find(out.begin(), out.begin() + i, out[i]) != out.begin() + i)
            throw std::runtime_error("entity " + std::to_string(out[i]) + " is listed twice");
    return out;
}

// The constraints `indices` of the open sketch, as sketch_list_constraints writes them.
json constraints_at(const DesignSketchTool& t, const std::vector<int>& indices)
{
    json out = json::array();
    for (int c : indices) out.push_back(entity_constraint_json(t.constraints()[c], c));
    return out;
}

// sketch_offset: offset copies. The copy of one entity alone is the Offset tool's Confirm, which adds it with no
// constraint inferred and binds it to its source, Parallel for a line and Concentric for an arc or a circle: inferred
// first, the copy of a horizontal or vertical line would be held so, and the solver refuses the redundant Parallel.
// Several entities' copies are added as drawn entities are, bound to nothing.
json sketch_offset(DesignSketchTool& t, const json& params)
{
    if (!params.contains("distance")) throw std::runtime_error("sketch_offset needs 'distance'");
    const double           d       = number_arg(params, "distance");
    bool                   listed  = false;
    const std::vector<int> sources = copy_sources(params, t, listed);
    std::vector<SketchEntity> src;
    for (int i : sources) src.push_back(t.entities()[i]);
    if (src.empty()) throw CadPrecondition("nothing to offset");
    const auto out = SketchEngine::offset_entities(src, d);
    if (out.empty()) throw std::runtime_error("offset produced nothing (ellipses and splines are not offset)");
    const int        base = int(t.entities().size());
    std::vector<int> bound;
    if (sources.size() == 1) {
        check_sketch_idle(t);
        if (!t.offset_scripted(sources.front(), d, bound)) throw std::runtime_error("the Offset tool refused the offset");
    } else {
        t.add_entities_scripted(out);
    }
    return json{{"ok", true}, {"first_index", base}, {"added", int(t.entities().size()) - base},
                {"constraints_added", constraints_at(t, bound)}, {"dof", sketch_dof(t)}};
}

// sketch_mirror: about `axis`, a line of the sketch or one of its axes, the Mirror tool's Confirm, which binds each
// copy Symmetric to its source; or across the line through axis_a and axis_b, two free points, copies bound to nothing.
json sketch_mirror(DesignSketchTool& t, const json& params)
{
    const auto&            ents    = t.entities();
    const json             axis_v  = param(params, "axis");
    bool                   listed  = false;
    std::vector<int>       targets = copy_sources(params, t, listed);
    if (axis_v.is_null()) {
        const Vec2d a = params.contains("axis_a") ? point_arg(params, "axis_a") : Vec2d(0, 0);
        const Vec2d b = params.contains("axis_b") ? point_arg(params, "axis_b") : Vec2d(0, 1);
        if (targets.empty()) throw CadPrecondition("nothing to mirror");
        std::vector<SketchEntity> src;
        for (int i : targets) src.push_back(ents[i]);
        const auto out  = SketchEngine::mirror_entities(src, a, b);
        const int  base = t.add_entities_scripted(out);
        return json{{"ok", true}, {"first_index", base}, {"added", int(out.size())},
                    {"constraints_added", json::array()}, {"dof", sketch_dof(t)}};
    }
    if (params.contains("axis_a") || params.contains("axis_b"))
        throw std::runtime_error("give axis, or axis_a and axis_b, not both");
    int axis = -1;
    if (axis_v == "x_axis")
        axis = kSketchRefAxisX;
    else if (axis_v == "y_axis")
        axis = kSketchRefAxisY;
    else if (axis_v.is_number_integer())
        axis = entity_indices(json::array({axis_v}), ents).front();
    else
        throw std::runtime_error("axis must be a line's entity index, \"x_axis\" or \"y_axis\"");
    if (axis >= 0 && ents[axis].type != SketchEntity::Type::Line)
        throw std::runtime_error("axis must be a line of the sketch, \"x_axis\" or \"y_axis\"; " + entity_label(ents, axis) +
                                 " is not a line");
    // The tool takes no pick of its axis as something to mirror.
    if (const auto it = std::find(targets.begin(), targets.end(), axis); it != targets.end()) {
        if (listed) throw std::runtime_error("entity " + std::to_string(axis) + " is the axis; it is not mirrored about itself");
        targets.erase(it);
    }
    if (targets.empty()) throw CadPrecondition("nothing to mirror");
    check_sketch_idle(t);
    const int        base = int(ents.size());
    std::vector<int> kept;
    if (!t.mirror_scripted(targets, axis, kept)) throw std::runtime_error("the Mirror tool refused the mirror");
    return json{{"ok", true}, {"first_index", base}, {"added", int(ents.size()) - base},
                {"constraints_added", constraints_at(t, kept)}, {"dof", sketch_dof(t)}};
}

// sketch_trim: a Trim click at `at`; sketch_extend (extend): an Extend click there.
json sketch_trim_or_extend(DesignSketchTool& t, const json& params, bool extend)
{
    const Vec2d  at  = point_arg(params, "at");
    const double tol = params.contains("tolerance") ? number_arg(params, "tolerance") : 0.05;
    if (!(tol > 0.0)) throw std::runtime_error("tolerance must be > 0");
    check_sketch_idle(t);
    const int  cons_before = int(t.constraints().size());
    const auto where       = json::array({at.x(), at.y()}).dump();
    int        picked      = -1;
    if (!t.trim_scripted(at, tol, extend, picked)) {
        const auto& ents = t.entities();
        if (picked < 0)
            throw std::runtime_error("no sketch entity within " + json(tol).dump() + " mm of " + where + "; tolerance widens the search");
        using ET = SketchEntity::Type;
        const ET ty = ents[picked].type;
        if (extend && ty != ET::Line && ty != ET::Arc)
            throw std::runtime_error("extend works on lines and arcs; " + entity_label(ents, picked) + " is neither");
        if (!extend && ty != ET::Line && ty != ET::Arc && ty != ET::Circle)
            throw std::runtime_error("trim works on lines, arcs and circles; " + entity_label(ents, picked) + " is none of them");
        throw std::runtime_error(extend ? "extend: " + entity_label(ents, picked) + " reaches no other entity from its end nearest " + where
                                        : "trim: nothing to cut on " + entity_label(ents, picked) + " at " + where +
                                              ": no other entity crosses it beyond that point (a circle needs two crossings)");
    }
    json r{{"ok", true}, {"entity", picked}, {"geometry", sketch_entity_to(t.entities()[picked], picked)},
           {"released_constraints", cons_before - int(t.constraints().size())}};
    r.update(solve_state(t));
    return r;
}

// sketch_transform: the Move, Rotate and Scale tools' Confirm, by params.type. sketch_pattern
// (pattern): the Linear and Polar array tools' Confirm, type linear or circular.
json sketch_transform(DesignSketchTool& t, const json& params, bool pattern)
{
    using Mode = DesignSketchTool::Mode;
    using Types = std::vector<std::pair<const char*, Mode>>;
    static const Types kTransforms = {{"move", Mode::Move}, {"rotate", Mode::Rotate}, {"scale", Mode::Scale}};
    static const Types kPatterns   = {{"linear", Mode::Array}, {"circular", Mode::PolarArray}};
    const json  typev = param(params, "type");
    const char* type  = nullptr;
    Mode        mode  = Mode::Move;
    for (const auto& [name, m] : pattern ? kPatterns : kTransforms)
        if (typev.is_string() && typev.get<std::string>() == name) { type = name; mode = m; }
    if (type == nullptr) throw std::runtime_error(pattern ? "type must be linear or circular" : "type must be move, rotate or scale");

    // What each type reads; anything else would be ignored, so it is refused.
    const bool shifts = mode == Mode::Move || mode == Mode::Array, turns = mode == Mode::Rotate || mode == Mode::PolarArray;
    const bool counts = mode == Mode::Array || mode == Mode::PolarArray, pivots = mode != Mode::Move && mode != Mode::Array;
    const std::pair<const char*, bool> takes[] = {{"dx", shifts},  {"dy", shifts},     {"angle", turns}, {"angle_deg", turns},
                                                  {"scale", mode == Mode::Scale}, {"count", counts}, {"pivot", pivots}};
    for (const auto& [key, read] : takes)
        if (!read && params.contains(key)) throw std::runtime_error(std::string(type) + " takes no " + key);

    const std::vector<int> targets = entity_picks(params, t);
    if (targets.empty()) throw std::runtime_error("nothing selected and no 'entities' given");
    for (size_t i = 0; i < targets.size(); ++i)
        if (std::find(targets.begin(), targets.begin() + i, targets[i]) != targets.begin() + i)
            throw std::runtime_error("entity " + std::to_string(targets[i]) + " is listed twice");
    const Vec2d  delta(params.contains("dx") ? number_arg(params, "dx") : 0.0, params.contains("dy") ? number_arg(params, "dy") : 0.0);
    const double angle = angle_arg(params, "angle", mode == Mode::PolarArray ? 2.0 * M_PI : 0.0);
    const double scale = params.contains("scale") ? number_arg(params, "scale") : 1.0;
    int count = 0;
    if (counts) {
        const json c = param(params, "count");
        if (!c.is_number_integer() || c.get<int>() < 2 || c.get<int>() > 1000)
            throw std::runtime_error(std::string(type) + " needs count, 2 to 1000 instances counting the original");
        count = c.get<int>();
    }
    std::optional<Vec2d> pivot;
    if (params.contains("pivot")) pivot = point_arg(params, "pivot");
    // A transform that moves nothing would still drop the constraints it invalidates.
    if (shifts && delta.norm() < 1e-9) throw std::runtime_error(std::string(type) + " needs a non-zero dx or dy");
    if (mode == Mode::Rotate && std::abs(angle) < 1e-12) throw std::runtime_error("rotate needs a non-zero angle (or angle_deg)");
    if (mode == Mode::Scale && (!(scale > 0.0) || std::abs(scale - 1.0) < 1e-12))
        throw std::runtime_error("scale needs a positive factor other than 1");
    if (mode == Mode::PolarArray && !pivot) throw std::runtime_error("circular needs pivot, the centre to array about");
    if (mode == Mode::PolarArray && std::abs(angle) < 1e-12) throw std::runtime_error("circular needs a non-zero angle");

    check_sketch_idle(t);
    const int  ents_before = int(t.entities().size()), cons_before = int(t.constraints().size());
    const auto used        = t.transform_scripted(mode, targets, delta, angle, scale, count, pivot);
    if (!used) throw std::runtime_error("the sketch tool refused the transform");
    json r{{"ok", true}, {"type", type}};
    if (counts) {
        r["first_index"] = ents_before;
        r["added"]       = int(t.entities().size()) - ents_before;
        r["bound"]       = int(t.constraints().size()) - cons_before;   // each copy's ties to its source
    } else {
        r["moved"]                = targets;
        r["released_constraints"] = cons_before - int(t.constraints().size());
    }
    if (pivots) r["pivot"] = json::array({used->x(), used->y()});
    r.update(solve_state(t));
    return r;
}

// The constraint type `v` names: SketchConstraintType's name in snake_case, or "equal", the
// CONSTRAIN bar's Equal (equal length, or equal radius for two curves).
SketchConstraintType constraint_type_arg(const json& v)
{
    if (!v.is_string())
        throw std::runtime_error("type must be a constraint type, such as \"parallel\" (tool_describe {tool: \"sketch_constrain\"} lists "
                                 "them, and {tool: \"sketch_dimension\"} the dimensions)");
    const std::string s = v.get<std::string>();
    if (s == "equal") return SketchConstraintType::EqualLength;
    for (size_t i = 0; i < std::size(kConstraintTypes); ++i)
        if (s == kConstraintTypes[i]) return SketchConstraintType(i);
    std::string close;
    for (const char* n : kConstraintTypes)
        if (edit_distance(boost::algorithm::to_lower_copy(s), n) <= 2 + s.size() / 4)
            close += std::string(close.empty() ? "" : ", ") + "\"" + n + "\"";
    throw std::runtime_error("unknown constraint type \"" + s + "\"" + (close.empty() ? "" : "; did you mean " + close) +
                             " (tool_describe {tool: \"sketch_constrain\"} lists them, and {tool: \"sketch_dimension\"} the dimensions)");
}

// Why the CONSTRAIN bar refuses a pick, in ConstraintReject order: a code an agent can test and the
// English of the status line's message (constraint_reject_text in DesignPanel.cpp).
const std::pair<const char*, const char*> kConstraintRejects[] = {
    {"none", ""},
    {"need_one_entity", "needs an entity: pass entities, or select one"},
    {"need_two_entities", "needs two entities: pass entities, or select them"},
    {"need_a_line", "applies to a line"},
    {"need_two_lines", "applies to two lines"},
    {"need_two_rounds", "needs two circles or arcs"},
    {"need_tangent_pair", "needs a line and a circle or arc, or two circles or arcs"},
    {"need_joinable_points", "needs two entities with a point to join"},
    {"need_measurable_points", "needs two entities with a point to measure between"},
    {"need_point_and_line", "needs a point and a line"},
    {"need_two_points_or_lines", "needs two points or two lines"},
    {"need_axis_line", "needs two entities, then an axis line"},
    {"need_round", "needs a circle or an arc"},
    {"unsupported", "has no CONSTRAIN bar button; name its points with refs instead"},
};
static_assert(std::size(kConstraintRejects) == size_t(ConstraintReject::Unsupported) + 1, "every refusal needs a text");

// A point reference {entity, role}: "origin", or a point of an entity. The role may be left out
// where the entity has only one point: a point's p0, a circle's or an ellipse's centre.
std::pair<int, SketchPointRole> point_ref(const json& r, const std::vector<SketchEntity>& ents, const std::string& what)
{
    if (!r.is_object() || !r.contains("entity"))
        throw std::runtime_error(what + " must be {\"entity\": i or \"origin\", \"role\": \"p0\", \"p1\" or \"center\"}");
    const json& e = r.at("entity");
    if (e == "origin") {
        if (r.contains("role")) throw std::runtime_error(what + ": the origin is a point and takes no role");
        return {kSketchRefOrigin, SketchPointRole::P0};
    }
    if (!e.is_number_integer())
        throw std::runtime_error(what + " names a point: an entity index or \"origin\"");
    const int i = entity_indices(json::array({e}), ents).front();
    // The points the solver gives each type (SketchSolver's slots).
    using ET = SketchEntity::Type;
    using R  = SketchPointRole;
    std::vector<R> roles;
    switch (ents[i].type) {
    case ET::Line: case ET::BSpline:     roles = {R::P0, R::P1}; break;
    case ET::Arc:  case ET::EllipseArc:  roles = {R::P0, R::P1, R::Center}; break;
    case ET::Circle: case ET::Ellipse:   roles = {R::Center}; break;
    case ET::Point:                      roles = {R::P0}; break;
    }
    std::string names;
    for (R x : roles) names += (names.empty() ? "" : ", ") + std::string(kPointRoles[int(x)]);
    if (!r.contains("role")) {
        if (roles.size() == 1) return {i, roles.front()};
        throw std::runtime_error(what + " needs a role: " + entity_label(ents, i) + " has " + names);
    }
    const R role = R(enum_index(what + " role", r.at("role"), kPointRoles));
    if (std::find(roles.begin(), roles.end(), role) == roles.end())
        throw std::runtime_error(what + ": role \"" + kPointRoles[int(role)] + "\" does not exist on " + entity_label(ents, i) +
                                 " (it has " + names + ")");
    return {i, role};
}

// A whole-entity reference {entity}: an entity, or "x_axis" / "y_axis", the sketch's axes.
int entity_ref(const json& r, const std::vector<SketchEntity>& ents, const std::string& what)
{
    if (!r.is_object() || !r.contains("entity"))
        throw std::runtime_error(what + " must be {\"entity\": i, \"x_axis\" or \"y_axis\"}");
    if (r.contains("role")) throw std::runtime_error(what + " is a whole entity and takes no role");
    const json& e = r.at("entity");
    if (e == "x_axis") return kSketchRefAxisX;
    if (e == "y_axis") return kSketchRefAxisY;
    if (!e.is_number_integer()) throw std::runtime_error(what + " names an entity: an index, \"x_axis\" or \"y_axis\"");
    return entity_indices(json::array({e}), ents).front();
}

// A constraint from `refs`, as entity_constraint_json writes it: each type's references are points
// (P) or whole entities (E) in the order the solver reads them.
SketchEntityConstraintDef refs_constraint(SketchConstraintType type, const std::string& name, const json& refs,
                                          const std::vector<SketchEntity>& ents)
{
    using T  = SketchConstraintType;
    using ET = SketchEntity::Type;
    const char* shape = "PP";
    const char* says  = "two points";
    switch (type) {
    case T::Fix: case T::LockX: case T::LockY:
        shape = "P"; says = "one point"; break;
    case T::Radius: case T::Diameter:
        shape = "E"; says = "one entity"; break;
    case T::Concentric: case T::EqualLength: case T::Parallel: case T::Perpendicular: case T::Angle:
    case T::Tangent: case T::EqualRadius: case T::Collinear:
        shape = "EE"; says = "two entities"; break;
    case T::Midpoint: case T::PointOnLine: case T::PointOnObject:
        shape = "PE"; says = "a point, then an entity"; break;
    case T::Symmetric:
        shape = "PPE"; says = "two points, then the axis line"; break;
    default: break;
    }
    if (!refs.is_array() || refs.size() != std::strlen(shape))
        throw std::runtime_error(name + " takes refs: " + says);
    SketchEntityConstraintDef d;
    d.type = type;
    int*             ent[3]  = {&d.ea, &d.eb, &d.ec};
    SketchPointRole* role[3] = {&d.ra, &d.rb, &d.rc};
    for (size_t k = 0; k < refs.size(); ++k) {
        const std::string what = name + " refs[" + std::to_string(k) + "]";
        if (shape[k] == 'P') std::tie(*ent[k], *role[k]) = point_ref(refs[k], ents, what);
        else                 *ent[k] = entity_ref(refs[k], ents, what);
    }
    // The entity each type acts on, as the planner checks a pick (plan_entity_constraint); the
    // solver would drop anything else without a word. The axes are lines, though no segment.
    auto line  = [&](int e) { return e == kSketchRefAxisX || e == kSketchRefAxisY || (e >= 0 && ents[e].type == ET::Line); };
    auto seg   = [&](int e) { return e >= 0 && ents[e].type == ET::Line; };
    auto round = [&](int e) { return e >= 0 && (ents[e].type == ET::Circle || ents[e].type == ET::Arc); };
    auto need  = [&](bool ok, int e, const char* kind) {
        if (!ok) throw std::runtime_error(name + " needs " + kind + "; " + entity_label(ents, e) + " is not one");
    };
    switch (type) {
    case T::Radius: case T::Diameter:
        need(round(d.ea), d.ea, "a circle or an arc");
        d.ra = SketchPointRole::Center;
        break;
    case T::Concentric: case T::EqualRadius:
        need(round(d.ea), d.ea, "two circles or arcs");
        need(round(d.eb), d.eb, "two circles or arcs");
        if (type == T::Concentric) d.ra = d.rb = SketchPointRole::Center;
        break;
    case T::EqualLength:
        need(seg(d.ea), d.ea, "two lines");
        need(seg(d.eb), d.eb, "two lines");
        break;
    case T::Parallel: case T::Perpendicular: case T::Angle: case T::Collinear:
        need(line(d.ea), d.ea, "two lines");
        need(line(d.eb), d.eb, "two lines");
        break;
    case T::Tangent:
        if (!((round(d.ea) && (line(d.eb) || round(d.eb))) || (line(d.ea) && round(d.eb))))
            throw std::runtime_error(name + " needs a line and a circle or arc, or two circles or arcs; got " +
                                     entity_label(ents, d.ea) + " and " + entity_label(ents, d.eb));
        break;
    case T::Midpoint:      need(seg(d.eb), d.eb, "a line to sit at the middle of"); break;
    case T::PointOnLine:   need(line(d.eb), d.eb, "a line"); break;
    case T::PointOnObject: need(line(d.eb) || round(d.eb), d.eb, "a line, circle or arc"); break;
    case T::Symmetric:     need(line(d.ec), d.ec, "an axis line"); break;
    default: break;
    }
    return d;
}

json constraint_items(const std::vector<SketchEntityConstraintDef>& cons)
{
    json items = json::array();
    for (size_t i = 0; i < cons.size(); ++i) items.push_back(entity_constraint_json(cons[i], int(i)));
    return items;
}

// Whether `type` is a dimension, which holds a measured value; the other types are geometric.
bool is_dimension(SketchConstraintType type)
{
    using T = SketchConstraintType;
    return type == T::Distance || type == T::DistanceX || type == T::DistanceY || type == T::Angle || type == T::Radius ||
           type == T::Diameter;
}

// sketch_constrain: a geometric constraint; sketch_dimension (dimension): a dimension. Each is a
// CONSTRAIN bar button on the picks, or a constraint on named points, and refuses the other's types.
json sketch_constrain(DesignSketchTool& t, const json& params, bool dimension)
{
    if (is_dimension(constraint_type_arg(param(params, "type"))) != dimension) {
        const std::string type = params.at("type").get<std::string>();
        throw std::runtime_error(dimension ? type + " is a geometric constraint, which sketch_constrain adds"
                                           : type + " is a dimension, which sketch_dimension adds");
    }
    const std::vector<SketchEntityConstraintDef> defs  = cad_constraint_defs(params, t.entities(), t.selection());
    const int                                    first = t.constrain_scripted(defs);
    if (first < 0)
        throw std::runtime_error("over-constrained: " + param(params, "type").get<std::string>() +
                                 " was rejected and rolled back (dof " + std::to_string(sketch_dof(t)) + ")");
    json added = json::array(), items = json::array();
    for (int i = first; i < int(t.constraints().size()); ++i) {
        added.push_back(i);
        items.push_back(entity_constraint_json(t.constraints()[i], i));
    }
    json r{{"ok", true}, {"added", std::move(added)}, {"items", std::move(items)}};
    r.update(solve_state(t));
    return r;
}

json sketch_list_constraints(DesignSketchTool& t, const json&)
{
    json r{{"ok", true}, {"scope", "live"}, {"items", constraint_items(t.constraints())}};
    r.update(solve_state(t));
    return r;
}

// sketch_delete_constraint: a constraint row's delete button in a sketch session.
json sketch_delete_constraint(DesignSketchTool& t, const json& params)
{
    const json v = param(params, "index");
    if (!v.is_number_integer()) throw std::runtime_error("index must be a constraint index (sketch_list_constraints' items[].index)");
    const int i = v.get<int>();
    if (i < 0 || i >= int(t.constraints().size()))
        throw std::runtime_error("constraint " + std::to_string(i) + " is out of range; the sketch has " +
                                 std::to_string(t.constraints().size()) + " constraints");
    json removed = entity_constraint_json(t.constraints()[i], i);
    if (!t.remove_constraint(i)) throw std::runtime_error("constraint " + std::to_string(i) + " could not be removed");
    json r{{"ok", true}, {"removed", std::move(removed)}};
    r.update(solve_state(t));
    return r;
}

// A Design command on the open sketch alone, then a repaint.
json on_open_sketch(DesignPanel* panel, const char* name, const json& params)
{
    json r = cad_sketch_call(mcp_sketch(panel), name, params);
    panel->mcp_viewport()->request_repaint();
    return r;
}

json action_sketch_list_constraints(DesignPanel* panel, const json& params)
{
    const json sketch = param(params, "sketch");
    if (sketch.is_null()) return cad_sketch_call(mcp_sketch(panel), "sketch_list_constraints", params);
    // A committed sketch's are read from the document, so no session is needed, nor disturbed.
    const CadDocument& doc = panel->mcp_doc();
    const int          si  = sketch_arg(sketch, doc);
    const CadFeature&  f   = doc.features[si];
    if (f.entities.empty())
        throw std::runtime_error("sketch " + std::to_string(si) + " is not drawn from entities; feature_describe reads its constraints");
    return json{{"ok", true}, {"scope", "committed"}, {"sketch", si}, {"items", constraint_items(f.entity_constraints)},
                {"constraints", int(f.entity_constraints.size())}};
}

// ---- Text and SVG art ---------------------------------------------------------------------------

// The `position` param: [x, y] in the plane, where the text's or the art's centre goes; the origin when left out.
Vec2d position_arg(const json& params)
{
    const json v = param(params, "position");
    return v.is_null() ? Vec2d::Zero() : point_of(v, "position");
}

// sketch_insert_svg's outlines: the filled shapes of the SVG file params.path, centred on the origin and scaled by
// params.scale, as Insert SVG reads them (svg_to_regions()). Throws std::runtime_error for a file that is not there
// and one that gives no outline.
ImportRegions svg_regions(const json& params)
{
    const std::string path  = input_path(param(params, "path"), "path");
    const json        s     = param(params, "scale");
    const double      scale = s.is_null() ? 1.0 : s.is_number() ? s.get<double>() : 0.0;
    if (!(scale > 0.0) || !std::isfinite(scale)) throw std::runtime_error("scale must be a number above 0");
    ImportRegions regions = svg_to_regions(path, scale);
    if (regions.empty()) throw std::runtime_error("the SVG file gives no outline: it has no filled shape, or it is not SVG");
    return regions;
}

// sketch_insert_svg with a sketch open: the outlines moved by params.position, added as Insert SVG adds them there,
// as lines (DesignSketchTool::add_imported_regions()).
json sketch_insert_svg(DesignSketchTool& t, const json& params)
{
    if (!param(params, "plane").is_null()) throw std::runtime_error("plane is the open sketch's: leave it out");
    const ImportRegions regions = transform_regions(svg_regions(params), position_arg(params), 1.0, 1.0);
    const std::size_t   before  = t.entities().size();
    if (!t.add_imported_regions(regions)) throw std::runtime_error("the SVG file's outlines give no lines");
    return json{{"ok", true}, {"scope", "sketch"}, {"added", int(t.entities().size() - before)},
                {"entities", int(t.entities().size())}};
}

// With a sketch open, the outlines go into it (sketch_insert_svg()). Otherwise Insert SVG's art feature on `plane`, XY
// when it is left out or null, in one undo step, with no file dialog and no placement step.
json action_sketch_insert_svg(DesignPanel* panel, const json& params)
{
    if (const DesignCanvas* vp = panel->mcp_viewport(); vp != nullptr && vp->is_sketching())
        return on_open_sketch(panel, "sketch_insert_svg", params);
    const ImportRegions regions   = svg_regions(params);
    const json          plane     = param(params, "plane");
    int                 face_body = -1;
    const SketchPlane   pl        = cad_resolve_plane(plane.is_null() ? json("XY") : plane, panel->mcp_doc(), false, &face_body);
    const Vec2d         at        = position_arg(params);
    json r = add_feature(panel, "art_index", [&] { return panel->mcp_push_art(regions, _L("SVG"), pl, face_body, at); });
    r["frame"] = frame_json(pl);
    return r;
}

// The Text dialog's Text feature from arguments (DesignPanel::mcp_push_text()), in one undo step, with no dialog and
// no placement step. Every refusal comes before anything changes: the font and the outline are checked first.
json action_sketch_text(DesignPanel* panel, const json& params)
{
    const json text = param(params, "text");
    if (!text.is_string()) throw std::runtime_error("text must be the words to outline");
    const json height = param(params, "height");
    if (!height.is_null() && !(height.is_number() && height.get<double>() >= 0.5 && height.get<double>() <= 500.0))
        throw std::runtime_error("height must be 0.5 to 500 mm");
    const std::string words  = text.get<std::string>();
    const std::string font   = params.value("font", std::string());
    const bool        bold   = params.value("bold", false);
    const bool        italic = params.value("italic", false);
    const double      mm     = height.is_null() ? 0.0 : height.get<double>();
    int               face_body = -1;
    const SketchPlane pl        = cad_resolve_plane(params.value("plane", json("XY")), panel->mcp_doc(), false, &face_body);
    const Vec2d       at        = position_arg(params);
    std::string       why;
    if (CadFeature outlined; !panel->mcp_outline_text(outlined, words, font, bold, italic, mm, why))
        throw std::runtime_error(why);
    json r = add_feature(panel, "text_index", [&] {
        const int index = panel->mcp_push_text(words, font, bold, italic, mm, pl, face_body, at, why);
        if (index < 0) throw std::runtime_error(why);
        return index;
    });
    r["frame"] = frame_json(pl);
    return r;
}

} // namespace

std::vector<int> sketch_target_indices(const json& params, const std::vector<int>& selection, int entity_count, bool all_if_empty)
{
    std::vector<int> out;
    // An empty list names nothing, so it reads as left out, as the schema's default [] says and as
    // entity_picks() reads it. Anything else that is given names what it holds.
    if (const auto given = params.find("entities"); given != params.end() && !(given->is_array() && given->empty())) {
        for (const auto& v : *given) out.push_back(v.get<int>());
        return out;
    }
    out = selection;
    if (out.empty() && all_if_empty)
        for (int i = 0; i < entity_count; ++i) out.push_back(i);
    return out;
}

json cad_sketch_describe(const CadDocument& doc, const DesignSketchTool& tool, int sketch)
{
    sketch_arg(sketch, doc);
    const CadFeature& f = doc.features[sketch];
    if (f.entities.empty())
        throw std::runtime_error("sketch " + std::to_string(sketch) +
                                 " is not drawn from entities (a rectangle, circle, profile or imported art); feature_describe reads it");
    json ents = json::array();
    for (size_t i = 0; i < f.entities.size(); ++i) ents.push_back(sketch_entity_to(f.entities[i], int(i)));
    json out{{"ok", true},
             {"sketch", sketch},
             {"name", f.name},
             {"plane", plane_json(f.plane, doc.modeling_origin)},
             {"entities", std::move(ents)},
             {"constraints", int(f.entity_constraints.size())}};
    out.update(sketch_report(tool.loop_report(f.entities)));
    return out;
}

json cad_sketch_call(DesignSketchTool& tool, const std::string& name, const json& params)
{
    const json p = params.is_object() ? params : json::object();
    if (name == "sketch_draw")              return sketch_draw(tool, p);
    if (name == "sketch_fillet")            return sketch_corner(tool, p, false);
    if (name == "sketch_chamfer")           return sketch_corner(tool, p, true);
    if (name == "sketch_trim")              return sketch_trim_or_extend(tool, p, false);
    if (name == "sketch_extend")            return sketch_trim_or_extend(tool, p, true);
    if (name == "sketch_offset")            return sketch_offset(tool, p);
    if (name == "sketch_mirror")            return sketch_mirror(tool, p);
    if (name == "sketch_transform")         return sketch_transform(tool, p, false);
    if (name == "sketch_pattern")           return sketch_transform(tool, p, true);
    if (name == "sketch_constrain")         return sketch_constrain(tool, p, false);
    if (name == "sketch_dimension")         return sketch_constrain(tool, p, true);
    if (name == "sketch_list_constraints")  return sketch_list_constraints(tool, p);
    if (name == "sketch_delete_constraint") return sketch_delete_constraint(tool, p);
    if (name == "sketch_insert_svg")        return sketch_insert_svg(tool, p);
    throw std::runtime_error(name + " is not a Design command of the open sketch");
}

json cad_sketch_project(const CadDocument& doc, DesignSketchTool& tool, const json& params)
{
    if (!param(params, "plane").is_null()) throw std::runtime_error("plane is the open sketch's: leave it out");
    if (doc.bodies.empty()) throw CadPrecondition("no source body to project from");
    const json body = param(params, "source_body");
    if (!body.is_null() && !body.is_number_integer()) throw std::runtime_error("source_body must be a body index, or -1 for the last body");
    const int source = body.is_null() || body.get<int>() == -1 ? int(doc.bodies.size()) - 1 : body.get<int>();
    std::vector<int> edges;
    for (const json& v : param(params, "edges")) {
        if (!v.is_number_integer()) throw std::runtime_error("edges must be edge ids (body_describe)");
        edges.push_back(v.get<int>());
    }
    const json face = param(params, "face");
    if (!face.is_null() && !face.is_number_integer()) throw std::runtime_error("face must be a face id (body_describe), or -1");
    // A body with no shape is the design's state; project_edges_on()'s other reasons are the call's edges, face and
    // body index.
    if (source >= 0 && source < int(doc.bodies.size()) && doc.bodies[size_t(source)].shape.IsNull())
        throw CadPrecondition("body " + std::to_string(source) + " has no shape");
    std::string error;
    const std::vector<SketchEntity> ents = doc.project_edges_on(tool.plane(), source, edges, face.is_null() ? -1 : face.get<int>(), error);
    if (ents.empty()) throw std::runtime_error(error);
    const int first = tool.add_entities_scripted(ents);
    return json{{"ok", true}, {"scope", "sketch"}, {"first_index", first}, {"added", int(ents.size())},
                {"entities", int(tool.entities().size())}};
}

std::vector<SketchEntityConstraintDef> cad_constraint_defs(const json& params, const std::vector<SketchEntity>& ents,
                                                           const std::vector<int>& selection)
{
    using T = SketchConstraintType;
    const T           type = constraint_type_arg(param(params, "type"));
    const std::string name = params.at("type").get<std::string>();
    const json        refs = param(params, "refs"), picks = param(params, "entities");
    const bool by_refs  = !refs.is_null() && !(refs.is_array() && refs.empty());
    const bool by_picks = !picks.is_null() && !(picks.is_array() && picks.empty());
    if (by_refs && by_picks) throw std::runtime_error("give entities or refs, not both");

    // The value: mm, or degrees for an angle. A type that carries none refuses one.
    const json value  = param(params, "value");
    const bool valued = type == T::Distance || type == T::DistanceX || type == T::DistanceY || type == T::LockX ||
                        type == T::LockY || type == T::Angle || type == T::Radius || type == T::Diameter || type == T::PointOnLine;
    if (!value.is_null()) {
        if (!value.is_number() || !std::isfinite(value.get<double>())) throw std::runtime_error(name + " value must be a number");
        if (!valued) throw std::runtime_error(name + " takes no value");
        if ((type == T::Radius || type == T::Diameter) && !(value.get<double>() > 0.0))
            throw std::runtime_error(name + " value must be > 0");
        if (type == T::Distance && value.get<double>() < 0.0) throw std::runtime_error("distance value must be >= 0");
    }

    std::vector<SketchEntityConstraintDef> defs;
    if (by_refs) {
        defs.push_back(refs_constraint(type, name, refs, ents));
        if (value.is_null() && valued && type != T::PointOnLine)
            throw std::runtime_error(name + " needs value (mm, or degrees for an angle)");
        write_plan_value(defs, value.is_null() ? 0.0 : value.get<double>());
        return defs;
    }

    // The CONSTRAIN bar's path: the planner takes the picks and chooses the points, as for a selection.
    const std::vector<int> picked = by_picks ? entity_indices(picks, ents) : selection;
    const size_t takes = type == T::Fix || type == T::Horizontal || type == T::Vertical || type == T::Radius || type == T::Diameter
                             ? 1 : type == T::Symmetric ? 3 : 2;
    if (by_picks && picked.size() > takes)
        throw std::runtime_error(name + " takes " + std::to_string(takes) + (takes == 1 ? " entity" : " entities") + "; " +
                                 std::to_string(picked.size()) + " were given");
    auto pick = [&](size_t k) { return k < picked.size() ? picked[k] : -1; };
    ConstraintPlan plan = plan_entity_constraint(ents, pick(0), pick(1), pick(2), type);
    if (plan.kind == ConstraintPlan::Kind::Reject) {
        std::string got;
        for (int e : picked) got += (got.empty() ? "" : ", ") + entity_label(ents, e);
        const auto& [code, text] = kConstraintRejects[int(plan.reason)];
        throw std::runtime_error(name + " " + text + (got.empty() ? "" : "; got " + got) + " (" + code + ")");
    }
    // A value the button would ask for: the one given, else the current one, which the field shows.
    if (plan.kind == ConstraintPlan::Kind::AskValue) write_plan_value(plan.defs, value.is_null() ? plan.prefill : value.get<double>());
    return plan.defs;
}

void sketch_silent_forms(std::vector<SilentForm>& forms)
{
    forms.insert(forms.end(), {
        {"sketch_describe",              action_sketch_describe},
        {"sketch_validate",              action_sketch_validate},
        {"sketch_list_constraints",      action_sketch_list_constraints},
        {"sketch_project",               action_sketch_project},
        {"sketch_text",                  action_sketch_text},
        {"sketch_insert_svg",            action_sketch_insert_svg},
        {"sketch_blend_curve",           action_sketch_blend_curve},
        {"sketch_create",                action_sketch_create},
        {"sketch_finish",                action_sketch_finish},
        {"sketch_cancel",                action_sketch_cancel},
        {"sketch_edit",                  action_sketch_edit},
        {"sketch_draw",                  [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_draw", a); }},
        {"sketch_select",                action_sketch_select},
        {"sketch_delete",                action_sketch_delete},
        {"sketch_toggle_construction",   action_sketch_toggle_construction},
        {"sketch_offset",                [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_offset", a); }},
        {"sketch_mirror",                [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_mirror", a); }},
        {"sketch_repair",                action_sketch_repair},
        {"sketch_set_value",             action_sketch_set_value},
        {"sketch_fillet",                [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_fillet", a); }},
        {"sketch_chamfer",               [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_chamfer", a); }},
        {"sketch_trim",                  [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_trim", a); }},
        {"sketch_extend",                [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_extend", a); }},
        {"sketch_transform",             [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_transform", a); }},
        {"sketch_pattern",               [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_pattern", a); }},
        {"sketch_constrain",             [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_constrain", a); }},
        {"sketch_dimension",             [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_dimension", a); }},
        {"sketch_delete_constraint",     [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_delete_constraint", a); }},
    });
}

}}} // namespace Slic3r::GUI::Mcp
