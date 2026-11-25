// The place Design, its Solid heading: solid_extrude and solid_revolve, which grow a body from a profile;
// solid_fillet, solid_chamfer, solid_shell, solid_draft, solid_rib and solid_hole, which shape one; solid_sweep,
// solid_loft, solid_thicken and solid_thread; solid_boolean, solid_mirror, solid_pattern, solid_split and
// solid_delete_face; and solid_align with solid_list_align_types, which fasten one body to another. The cad_*
// builders they share with the Design tab's cards are here too (McpToolsDesign.hpp). solid_silent_forms() binds them
// to their tool blocks, and run_design_tool() (DesignTools.cpp) runs them behind the Design commands' gate.

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

#include <nlohmann/json.hpp>
#include <wx/string.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/SketchSolver.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/CAD/ThreadStandards.hpp"
#include <boost/algorithm/string/case_conv.hpp>

#include <TopoDS_Edge.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

using json = nlohmann::json;

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// The operations each command takes (operation_arg()): solid_extrude's, auto among them; solid_revolve's, solid_sweep's and solid_loft's;
// solid_boolean's, which combines two bodies there and makes none; solid_mirror's, which keeps its image apart or joins it.
const std::vector<std::string> kExtrudeOperations = {"auto", "new_body", "join", "cut", "intersect"};
const std::vector<std::string> kSolidOperations   = {"new_body", "join", "cut", "intersect"};
const std::vector<std::string> kBooleanOperations = {"join", "cut", "intersect"};
const std::vector<std::string> kMirrorOperations  = {"new_body", "join"};

// The `operation` argument: one of the words `takes` lists, "auto" coming back empty; `def` when the call leaves it
// out or gives null, and with no `def` the call must give one.
std::optional<BooleanMode> operation_arg(const json& params, const char* def, const std::vector<std::string>& takes)
{
    json v = params.value("operation", json());
    if (v.is_null() && def != nullptr) v = def;
    const std::string word = v.is_string() ? v.get<std::string>() : std::string();
    if (std::find(takes.begin(), takes.end(), word) != takes.end()) {
        if (word == "auto") return std::nullopt;
        for (size_t i = 0; i < std::size(kOperations); ++i)
            if (word == kOperations[i]) return BooleanMode(i);
    }
    std::string list;
    for (size_t i = 0; i < takes.size(); ++i)
        list += (i == 0 ? "" : i + 1 == takes.size() ? " or " : ", ") + takes[i];
    throw std::runtime_error("operation must be " + list);
}

// Optional explicit closed profile: params["profile"] = [[x,y],...] in plane mm.
// This is the Measure->Build bridge — feed a measured contour straight back.
// An empty array is the row's default and means no profile.
bool profile_from(const json& params, SketchProfile& out)
{
    if (!params.contains("profile") || params["profile"].empty()) return false;
    out.points.clear();
    for (const auto& p : params["profile"]) out.points.emplace_back(p[0].get<double>(), p[1].get<double>());
    out.closed = true;
    if (out.points.size() < 3) throw std::runtime_error("profile needs at least 3 points [x, y]");
    return true;
}

} // namespace

// ---- Shared with the other Design files (DesignCommon.hpp) ----------------------------------

int target_body_arg(const json& params, const CadDocument& doc)
{
    int bi = params.value("body", -1);
    if (bi >= int(doc.bodies.size()))
        throw std::runtime_error("body index out of range (have " + std::to_string(doc.bodies.size()) + ")");
    return bi;   // <0 -> kernel uses the last body
}

std::optional<FaceGroup> face_group_named(const json& v)
{
    for (size_t i = 0; i < std::size(kFaceGroups); ++i)
        if (v.is_string() && v.get<std::string>() == kFaceGroups[i]) return FaceGroup(i);
    return std::nullopt;
}

int revolve_axis_arg(const json& params)
{
    const int axis = params.value("axis", 0);
    if (axis != 0 && axis != 1) throw std::runtime_error("axis must be 0 (plane X) or 1 (plane Y)");
    return axis;
}

bool revolve_line(const std::vector<SketchEntity>& ents, int e)
{
    return e >= 0 && e < int(ents.size()) && ents[size_t(e)].type == SketchEntity::Type::Line;
}

int revolve_axis_entity_arg(const json& params, const CadDocument& doc, int si)
{
    const json ae = params.value("axis_entity", json());
    if (ae.is_null()) return params.contains("axis") ? -1 : DesignPanel::default_revolve_axis_entity(doc.features[si].entities);
    if (!ae.is_number_integer()) throw std::runtime_error("axis_entity must be an entity index of the sketch, or -1 for the plane axis");
    if (ae.get<int>() != -1 && !revolve_line(doc.features[si].entities, ae.get<int>()))
        throw std::runtime_error("axis_entity " + std::to_string(ae.get<int>()) + " is not a line of sketch " + std::to_string(si));
    return ae.get<int>();
}

namespace {

// `edges` is one id or an array of ids; an array becomes ONE feature, every id resolved
// against the same body (a chain of single-edge features would see the ids drift).
std::vector<int> edge_ids_arg(const json& params, const char* verb)
{
    if (!params.contains("edges"))
        throw std::runtime_error(std::string(verb) + " needs 'edges' (an id or array of ids from body_describe)");
    const json& e = params["edges"];
    std::vector<int> ids;
    if (e.is_array()) for (const json& v : e) ids.push_back(v.get<int>());
    else              ids.push_back(e.get<int>());
    if (ids.empty()) throw std::runtime_error(std::string(verb) + ": 'edges' is an empty array");
    return ids;
}

// fillet and chamfer: the Fillet and Chamfer cards' Confirm on picked edges.
json action_dressup(DesignPanel* panel, const json& params, bool chamfer)
{
    CadDocument& doc = panel->mcp_doc();
    const auto   add = cad_dressup(doc, params, chamfer);
    const char*  key = chamfer ? "chamfer_index" : "fillet_index";
    json r = add_feature(panel, key, [&] { return add(panel->mcp_feature_name(chamfer ? _L("Chamfer") : _L("Fillet"))); });
    const int target = doc.features[r.at(key).get<int>()].target_body;
    r["body"] = target >= 0 ? target : int(doc.bodies.size()) - 1;
    return r;
}

json action_fillet(DesignPanel* panel, const json& params) { return action_dressup(panel, params, false); }
json action_chamfer(DesignPanel* panel, const json& params) { return action_dressup(panel, params, true); }

// The Hole card's Confirm; the reply says where the hole went (cad_hole_placement) and the frame of its plane.
json action_hole(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    json         resolved;
    const auto   add = cad_hole(doc, params, resolved);
    json r = add_feature(panel, "hole_index", [&] { return add(panel->mcp_feature_name(_L_CONTEXT("Hole", "Design"))); });
    r.update(resolved);
    return r;
}

json action_boolean(DesignPanel* panel, const json& params)
{
    const BooleanMode m = *operation_arg(params, nullptr, kBooleanOperations);
    const int target = params.value("target", 0);
    const int tool   = params.value("tool", 1);
    const bool keep  = params.value("keep_tool", false);
    const double tol = params.value("tolerance", 0.0);
    CadDocument& doc = panel->mcp_doc();
    int n = int(doc.bodies.size());
    if (target < 0 || target >= n || tool < 0 || tool >= n)
        throw std::runtime_error("target/tool body index out of range (have " + std::to_string(n) + ")");
    if (target == tool) throw std::runtime_error("target and tool must differ");
    doc.checkpoint();
    int b = doc.add_boolean(m, target, tool, keep, tol, -1, -1, panel->mcp_feature_name(_L("Boolean")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"boolean_index", b}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// The Pattern card's Confirm: a linear or circular pattern replies with the frame of its plane.
json action_pattern(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const auto   add = cad_pattern(doc, params);
    json r = add_feature(panel, "pattern_index", [&] { return add(panel->mcp_feature_name(_L_CONTEXT("Pattern", "Design"))); });
    const CadFeature& f = doc.features[r.at("pattern_index").get<int>()];
    if (f.pattern_curve_sketch < 0) r["frame"] = frame_json(f.plane);
    return r;
}

json action_shell(DesignPanel* panel, const json& params)
{
    const double thickness = params.value("thickness", 1.0);
    if (thickness <= 0) throw std::runtime_error("thickness must be > 0");
    const int face = params.value("face", -1);   // face id to leave open (-1 = closed hollow)
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no body to shell");
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int s = doc.add_shell(thickness, face, bi, panel->mcp_feature_name(_L_CONTEXT("Shell", "Design")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"shell_index", s}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_rib(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("rib needs 'sketch' (feature index)");
    if (!params.contains("entity")) throw std::runtime_error("rib needs 'entity' (entity index)");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no body to rib");
    int sketch    = params["sketch"].get<int>();
    int entity    = params["entity"].get<int>();
    double thickness = params.value("thickness", 2.0);
    double depth     = params.value("depth", 10.0);
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int idx = doc.add_rib(sketch, entity, thickness, depth, bi, panel->mcp_feature_name(_L_CONTEXT("Rib", "Design")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"rib_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_draft(DesignPanel* panel, const json& params)
{
    if (!params.contains("face")) throw std::runtime_error("draft needs 'face' (id from body_describe)");
    const double angle = params.value("angle", 5.0);
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no body to draft");
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int d = doc.add_draft(angle, params["face"].get<int>(), bi, panel->mcp_feature_name(_L("Draft")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"draft_index", d}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// ---- Solids from a profile: extrude and revolve -------------------------------------------------
//
// With `sketch` they are the Extrude and Revolve cards' Confirm on a committed sketch (or one loop
// of it); without, they draw their own sketch first, a `profile` or a rectangle on `plane`.

// A join or cut needs something to work on, as the Revolve card says.
void check_has_body(BooleanMode mode, const CadDocument& doc)
{
    if (mode != BooleanMode::New && doc.bodies.empty())
        throw CadPrecondition("operation " + std::string(kOperations[int(mode)]) + " needs a body; add or import one first");
}

double distance_arg(const json& params)
{
    const double d = params.value("distance", 10.0);
    if (!(d > 0)) throw std::runtime_error("distance must be > 0");
    return d;
}

ExtrudeEnd extrude_end_arg(const json& params)
{
    const std::string end = params.value("end", std::string("blind"));
    // up_to_vertex needs a picked vertex, which no param carries.
    for (int i = 0; i < int(ExtrudeEnd::UpToVertex); ++i)
        if (end == kExtrudeEnds[i]) return ExtrudeEnd(i);
    throw std::runtime_error("end must be blind, symmetric, two_sided, through_all or up_to_face");
}

// The end condition and the options that go with it, as the card's Confirm carries them onto the
// new feature (on_add_extrude). apply_feature reads extrude_end, not the kernel's `symmetric` flag;
// up_to_face is a face id of the target body.
void set_extrude_options(CadFeature& fe, const json& params, ExtrudeEnd end, double d)
{
    fe.flip        = params.value("flip", false);
    fe.taper_deg   = params.value("taper", 0.0);
    fe.extrude_end = end;
    if (end == ExtrudeEnd::TwoSided) fe.distance2  = params.value("distance2", d);
    if (end == ExtrudeEnd::UpToFace) fe.up_to_face = params.value("up_to_face", -1);
}

// The sketch is the profile and fixes its plane, so these would be ignored; refusing them shows the
// caller its mistake. An empty profile is the row's default, which means none.
void refuse_with_sketch(const json& params)
{
    for (const char* k : {"profile", "width", "height", "plane"})
        if (params.contains(k) && !(params.at(k).is_array() && params.at(k).empty()))
            throw std::runtime_error(std::string(k) + " cannot be given with sketch: the sketch is the profile and fixes its plane");
}

// Loop `loop` of sketch `si`: the region's own entities followed by those of the loops inside it,
// the list a loop picked in the viewport hands the Extrude card (selected_loop_entities()).
std::vector<SketchEntity> loop_entities(const DesignSketchTool& tool, const CadDocument& doc, int si, const json& loop)
{
    const std::vector<SketchEntity>& ents = doc.features[si].entities;
    if (!loop.is_number_integer()) throw std::runtime_error("loop must be a loop index (closed_loops[].index of the sketch)");
    if (ents.empty())
        throw std::runtime_error("sketch " + std::to_string(si) + " is not drawn from entities, so it has no loops to pick; extrude it whole");
    const std::vector<std::vector<int>> regions = tool.region_entity_indices_with_holes(ents);
    const int li = loop.get<int>();
    if (li < 0 || li >= int(regions.size()))
        throw std::runtime_error("loop " + std::to_string(li) + " does not exist; sketch " + std::to_string(si) + " has " +
                                 std::to_string(regions.size()) + (regions.size() == 1 ? " closed loop" : " closed loops"));
    std::vector<SketchEntity> out;
    for (int ei : regions[li])
        if (ei >= 0 && ei < int(ents.size())) out.push_back(ents[ei]);
    return out;
}

// How many separate regions a whole-sketch extrude would have to build: loops not inside another.
int separate_regions(const DesignSketchTool& tool, const std::vector<SketchEntity>& ents)
{
    const DesignSketchTool::LoopReport rep = tool.loop_report(ents);
    std::vector<bool> inside(rep.loops.size(), false);
    for (const auto& l : rep.loops)
        for (int h : l.holes)
            if (h >= 0 && h < int(inside.size())) inside[h] = true;
    return int(std::count(inside.begin(), inside.end(), false));
}

// The body an extrude or revolve went into: the one it made, else the one it joined, cut or
// intersected (its target, or by default the last body).
int result_body(const CadDocument& doc, int feature)
{
    for (size_t b = 0; b < doc.bodies.size(); ++b)
        if (doc.bodies[b].source_feature == feature) return int(b);
    const int t = doc.features[feature].target_body;
    return t >= 0 ? t : int(doc.bodies.size()) - 1;
}

// The operation a solid feature was built with and the body it went into, as the defaults resolved.
json body_reply(json r, const CadDocument& doc, const char* index_key)
{
    const int f    = r.at(index_key).get<int>();
    r["operation"] = kOperations[int(doc.features[f].mode)];
    r["body"]      = result_body(doc, f);
    return r;
}

// Both forms reply alike, so a caller need not know which it used: the sketch and the feature, and
// what the defaults resolved to.
json solid_reply(json r, const CadDocument& doc, const char* index_key, int sketch)
{
    r = body_reply(std::move(r), doc, index_key);
    const int f       = r.at(index_key).get<int>();
    r["sketch_index"] = sketch;
    r["frame"]        = frame_json(sketch >= 0 ? doc.features[sketch].plane : doc.features[f].plane);
    return r;
}

json action_extrude(DesignPanel* panel, const json& params)
{
    CadDocument&     doc    = panel->mcp_doc();
    const json       sketch = params.value("sketch", json());
    const json       loop   = params.value("loop", json());
    const ExtrudeEnd end    = extrude_end_arg(params);
    if (!params.value("face", json()).is_null()) {
        SketchPlane frame;
        const auto  add = cad_face_extrude(doc, params, frame);
        json r = add_feature(panel, "extrude_index",
                             [&] { return add(panel->mcp_feature_name(_L_CONTEXT("Extrude", "Design"))); });
        r = solid_reply(std::move(r), doc, "extrude_index", -1);
        r["frame"] = frame_json(frame);   // the face's, which the feature does not carry
        r["face"]  = params.at("face");
        r["end"]   = kExtrudeEnds[int(end)];
        return r;
    }
    if (!sketch.is_null()) {
        const DesignCanvas* vp = panel->mcp_viewport();
        if (vp == nullptr) throw no_design_view();
        const auto add = cad_sketch_extrude(doc, vp->mcp_sketch_tool(), params);
        json r;
        try {
            r = add_feature(panel, "extrude_index",
                            [&] { return add(panel->mcp_feature_name(_L_CONTEXT("Extrude", "Design"))); });
        } catch (const ToolFailure& e) {
            // A whole sketch builds one region, with the loops inside it as holes. When the rebuild
            // refused a sketch of several, name the way to take one; any other failure has another cause.
            if (!loop.is_null() || e.details.value("code", std::string()) != "rebuild_failed" ||
                separate_regions(vp->mcp_sketch_tool(), doc.features[sketch.get<int>()].entities) < 2)
                throw;
            throw ToolFailure(std::string(e.what()) +
                                  "; pass \"loop\" to extrude one of its regions (closed_loops of sketch_describe {sketch})",
                              e.details);
        }
        r = solid_reply(std::move(r), doc, "extrude_index", sketch.get<int>());
        r["end"] = kExtrudeEnds[int(end)];
        if (!loop.is_null()) r["loop"] = loop;
        return r;
    }
    if (!loop.is_null()) throw std::runtime_error("loop picks a loop of a committed sketch; give sketch too");

    const double      d    = distance_arg(params);
    const SketchPlane pl   = cad_resolve_plane(params.value("plane", json("XY")), doc, false);
    const auto        mode = operation_arg(params, "new_body", kExtrudeOperations);
    const int         body = std::max(-1, target_body_arg(params, doc));
    if (mode) check_has_body(*mode, doc);
    SketchProfile prof;
    const bool has_prof = profile_from(params, prof);
    double w = 0, h = 0;
    if (!has_prof) {
        w = params.value("width", 20.0); h = params.value("height", 20.0);
        if (w <= 0 || h <= 0) throw std::runtime_error("width and height must be > 0");
    }
    int s = -1;
    json r = add_feature(panel, "extrude_index", [&] {
        const std::string sketch_name = panel->mcp_feature_name(_L("Sketch"));
        s = has_prof ? doc.add_sketch_profile(prof, pl, sketch_name)
                     : doc.add_sketch(SketchShape::Rectangle, pl, w, h, 0.0, sketch_name);
        // "auto" decides as the card does for a sketch: join the body the profile touches.
        const int touching = mode ? -1 : doc.body_touching_sketch(s);
        const BooleanMode m = mode.value_or(touching >= 0 ? BooleanMode::Add : BooleanMode::New);
        const int e = doc.add_extrude(s, d, /*symmetric*/false, m, panel->mcp_feature_name(_L_CONTEXT("Extrude", "Design")));
        set_extrude_options(doc.features[e], params, end, d);
        doc.features[e].target_body = body >= 0 ? body : touching;
        return e;
    });
    r = solid_reply(std::move(r), doc, "extrude_index", s);
    r["end"] = kExtrudeEnds[int(end)];
    return r;
}

json action_revolve(DesignPanel* panel, const json& params)
{
    CadDocument& doc    = panel->mcp_doc();
    const json   sketch = params.value("sketch", json());
    if (!sketch.is_null()) {
        const auto add = cad_sketch_revolve(doc, params);
        json r = add_feature(panel, "revolve_index", [&] { return add(panel->mcp_feature_name(_L("Revolve"))); });
        r = solid_reply(std::move(r), doc, "revolve_index", sketch.get<int>());
        const CadFeature& f = doc.features[r.at("revolve_index").get<int>()];
        r["axis"]        = f.revolve_axis;
        r["axis_entity"] = f.revolve_axis_entity;
        return r;
    }
    if (!params.value("axis_entity", json()).is_null())
        throw std::runtime_error("axis_entity is a line of a committed sketch; give sketch too");

    const double      angle = params.value("angle", 360.0);
    const int         axis  = revolve_axis_arg(params);
    const bool        flip  = params.value("flip", false);
    const SketchPlane pl    = cad_resolve_plane(params.value("plane", json("XY")), doc, false);
    const BooleanMode mode  = *operation_arg(params, "new_body", kSolidOperations);
    const int         body  = std::max(-1, target_body_arg(params, doc));
    check_has_body(mode, doc);
    const SketchProfile prof = cad_revolve_profile(params);
    int s = -1;
    json r = add_feature(panel, "revolve_index", [&] {
        s = doc.add_sketch_profile(prof, pl, panel->mcp_feature_name(_L("Sketch")));
        const int rv = doc.add_revolve(s, angle, axis, flip, mode, panel->mcp_feature_name(_L("Revolve")));
        doc.features[rv].target_body = body;
        return rv;
    });
    r = solid_reply(std::move(r), doc, "revolve_index", s);
    r["axis"] = axis;
    return r;
}

json action_mirror(DesignPanel* panel, const json& params)
{
    const BooleanMode m = *operation_arg(params, "new_body", kMirrorOperations);
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no body to mirror");
    int bi = target_body_arg(params, doc);
    bool keep = params.value("keep_original", true);
    const SketchPlane pl = cad_resolve_plane(params.value("plane", json("XZ")), doc, false);
    doc.checkpoint();
    int idx = doc.add_mirror(pl, bi, m, panel->mcp_feature_name(_L_CONTEXT("Mirror", "Design")));
    doc.features[idx].mirror_keep_original = keep;
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"mirror_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error},
                {"frame", frame_json(pl)}};
}

// Whether a thicken call thickens a whole sheet body (type surface, or no type and no face) rather than a face.
bool thickens_surface(const json& params)
{
    const json type = params.value("type", json());
    return type.is_null() ? params.value("face", json()).is_null() : type == "surface";
}

// The Thicken and Thicken Surface cards' Confirm, each feature named as its card names it.
json action_thicken(DesignPanel* panel, const json& params)
{
    CadDocument&   doc  = panel->mcp_doc();
    const auto     add  = cad_thicken(doc, params);
    const wxString name = thickens_surface(params) ? _L("Thicken Surface") : _L("Thicken");
    return add_feature(panel, "thicken_index", [&] { return add(panel->mcp_feature_name(name)); });
}

// With a plane, the Cut card's Confirm (on_add_cut); with a face, a split along that face's plane.
json action_split(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    json         resolved;
    const auto   add  = cad_split(doc, params, resolved);
    const bool   face = !params.value("face", json()).is_null();
    json r = add_feature(panel, "split_index", [&] {
        return add(panel->mcp_feature_name(face ? _L_CONTEXT("Split", "Design") : _L_CONTEXT("Cut", "Design")));
    });
    r.update(resolved);
    return r;
}

json action_delete_face(DesignPanel* panel, const json& params)
{
    if (!params.contains("faces")) throw std::runtime_error("solid_delete_face needs 'faces' (array of face ids)");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no body to delete faces from");
    int bi = target_body_arg(params, doc);
    std::vector<int> faces;
    if (params["faces"].is_array())
        for (const auto& v : params["faces"]) faces.push_back(v.get<int>());
    doc.checkpoint();
    int idx = doc.add_delete_face(bi, faces, panel->mcp_feature_name(_L("Delete Face")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// Thread, sweep and loft: their cards' Confirm. They rebuild off the UI thread, as the cards do.
json action_thread(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    json         resolved;
    const auto   add = cad_thread(doc, params, resolved);
    json r = add_feature(panel, "thread_index", [&] { return add(panel->mcp_feature_name(_L("Thread"))); }, true);
    r.update(resolved);
    r["body"] = result_body(doc, r.at("thread_index").get<int>());   // a rod threaded from nothing is a new body
    return r;
}

json action_sweep(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const auto   add = cad_sweep(doc, params);
    json r = add_feature(panel, "sweep_index", [&] { return add(panel->mcp_feature_name(_L("Sweep"))); }, true);
    return body_reply(std::move(r), doc, "sweep_index");
}

json action_loft(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const auto   add = cad_loft(doc, params);
    json r = add_feature(panel, "loft_index", [&] { return add(panel->mcp_feature_name(_L("Loft"))); }, true);
    return body_reply(std::move(r), doc, "loft_index");
}

// The Mate card's Confirm: the feature keeps the history's name for it, Mate.
json action_align(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const auto   add = cad_align(doc, params);
    return add_feature(panel, "align_index", [&] { return add(panel->mcp_feature_name(_L("Mate"))); });
}

json action_list_align_types(DesignPanel* panel, const json& params) { return cad_align_types(panel->mcp_doc(), params); }

} // namespace

std::function<int(const std::string&)> cad_sketch_extrude(CadDocument& doc, const DesignSketchTool& tool, const json& params)
{
    refuse_with_sketch(params);
    const int        si   = sketch_arg(params.value("sketch", json()), doc);
    const double     d    = distance_arg(params);
    const ExtrudeEnd end  = extrude_end_arg(params);
    const auto       mode = operation_arg(params, "auto", kExtrudeOperations);
    int              body = std::max(-1, target_body_arg(params, doc));
    if (mode) check_has_body(*mode, doc);
    const json loop = params.value("loop", json());
    std::vector<SketchEntity> subset;
    if (!loop.is_null()) subset = loop_entities(tool, doc, si, loop);
    // The card's defaults (open_tool): it works on the body the profile lies on or touches, and
    // "auto" joins that body, or starts a new one when the profile touches none.
    const int touching = doc.body_touching_sketch(si);
    if (body < 0) body = touching;
    const BooleanMode m = mode.value_or(touching >= 0 ? BooleanMode::Add : BooleanMode::New);
    return [&doc, params, si, d, end, body, m, by_loop = !loop.is_null(), subset](const std::string& name) {
        const SketchPlane plane = doc.features[si].plane;
        const int e = by_loop ? doc.add_extrude_entities(subset, plane, d, false, m, name) : doc.add_extrude(si, d, false, m, name);
        set_extrude_options(doc.features[e], params, end, d);
        doc.features[e].target_body = body;
        return e;
    };
}

std::function<int(const std::string&)> cad_sketch_revolve(CadDocument& doc, const json& params)
{
    refuse_with_sketch(params);
    const int         si    = sketch_arg(params.value("sketch", json()), doc);
    const double      angle = params.value("angle", 360.0);
    const int         axis  = revolve_axis_arg(params);
    const bool        flip  = params.value("flip", false);
    const BooleanMode mode  = *operation_arg(params, "new_body", kSolidOperations);
    const int         body  = std::max(-1, target_body_arg(params, doc));
    check_has_body(mode, doc);
    const int axis_entity = revolve_axis_entity_arg(params, doc, si);
    return [&doc, si, angle, axis, flip, mode, body, axis_entity](const std::string& name) {
        const int r = doc.add_revolve(si, angle, axis, flip, mode, name);
        doc.features[r].revolve_axis_entity = axis_entity;
        doc.features[r].target_body         = body;
        return r;
    };
}

json cad_align_types(const CadDocument& doc, const json& params)
{
    for (const char* k : {"cs_a", "cs_b"})
        if (!params.value(k, json()).is_number_integer())
            throw std::runtime_error(std::string("solid_list_align_types needs '") + k + "' (CoordSys feature index)");
    const int cs_a  = params.at("cs_a").get<int>();
    const int cs_b  = params.at("cs_b").get<int>();
    json      types = json::array();
    for (const CadDocument::MateOption& o : doc.mate_options(cs_a, cs_b))
        types.push_back(json{{"type", enum_name(kAlignTypes, o.kind)}, {"fits", o.viable}, {"reason", o.viable ? json() : json(o.reason)}});
    return json{{"ok", true}, {"cs_a", cs_a}, {"cs_b", cs_b}, {"types", std::move(types)}};
}

SketchProfile cad_revolve_profile(const json& params)
{
    SketchProfile prof;
    if (profile_from(params, prof)) return prof;
    const double w = params.value("width", 20.0), h = params.value("height", 10.0);
    if (w <= 0 || h <= 0) throw std::runtime_error("width and height must be > 0");
    // On one side of the axis, as make_revolve requires: a rectangle across it would sweep
    // through itself. Both corner orders run counter-clockwise, as a drawn profile does.
    const double a = w / 2;
    prof.points = revolve_axis_arg(params) == 0 ? std::vector<Vec2d>{{-a, 0}, {a, 0}, {a, h}, {-a, h}}
                                                : std::vector<Vec2d>{{0, -a}, {h, -a}, {h, a}, {0, a}};
    prof.closed = true;
    return prof;
}

std::function<int(const std::string& name)> cad_face_extrude(CadDocument& doc, const json& params, SketchPlane& frame)
{
    for (const char* k : {"sketch", "loop", "profile", "width", "height", "plane"})
        if (params.contains(k) && !params.at(k).is_null() && !(params.at(k).is_array() && params.at(k).empty()))
            throw std::runtime_error(std::string(k) + " cannot be given with face: the face is the profile and fixes its plane");
    const json fv = params.at("face");
    if (!fv.is_number_integer()) throw std::runtime_error("face must be a face id (body_describe)");
    if (doc.bodies.empty()) throw CadPrecondition("a face push/pull needs a body");
    const int asked = target_body_arg(params, doc);
    const int b     = asked >= 0 ? asked : int(doc.bodies.size()) - 1;
    const int fi    = fv.get<int>();
    const TopoDS_Face face = GeometryEngine::face_by_index(doc.bodies[b].shape, fi);
    if (face.IsNull())
        throw std::runtime_error("face " + std::to_string(fi) + " of body " + std::to_string(b) + " does not exist (it has " +
                                 std::to_string(GeometryEngine::face_count(doc.bodies[b].shape)) + " faces)");
    if (!doc.plane_of_face(b, fi, frame))
        throw std::runtime_error("face " + std::to_string(fi) + " of body " + std::to_string(b) + " is " + face_kind_name(face) +
                                 "; a push/pull needs a planar face");
    // The kernel grows a face blind or symmetric along its normal (apply_feature); the card's other
    // ends and its taper act on sketch profiles only.
    const ExtrudeEnd end = extrude_end_arg(params);
    if (end != ExtrudeEnd::Blind && end != ExtrudeEnd::Symmetric) throw std::runtime_error("a face push/pull ends blind or symmetric");
    if (params.value("taper", 0.0) != 0.0) throw std::runtime_error("taper does not apply to a face push/pull");
    const double      d    = distance_arg(params);
    const bool        flip = params.value("flip", false);
    // The card joins a picked face's own body by default.
    const BooleanMode mode = operation_arg(params, "auto", kExtrudeOperations).value_or(BooleanMode::Add);
    return [&doc, fi, d, mode, end, flip, b](const std::string& name) {
        const int e = doc.add_extrude_face(fi, d, false, mode, name);
        doc.features[e].extrude_end = end;
        doc.features[e].flip        = flip;
        doc.features[e].target_body = b;   // the face id is of this body
        return e;
    };
}

json cad_hole_placement(const CadDocument& doc, const json& params, SketchPlane& plane, int& target_body)
{
    if (doc.bodies.empty()) throw CadPrecondition("no body to drill");
    int face_body = -1;
    plane = cad_resolve_plane(params.value("plane", json("XY")), doc, true, &face_body);
    const int body = target_body_arg(params, doc);
    if (face_body >= 0 && body >= 0 && body != face_body)
        throw std::runtime_error("the plane is a face of body " + std::to_string(face_body) + ", so that body is drilled, not body " +
                                 std::to_string(body));
    // A face plane drills its own body, as the Hole card does for a picked face; otherwise the
    // body asked for, or the kernel's default, the last body.
    target_body = face_body >= 0 ? face_body : body;
    json r{{"body", target_body >= 0 ? target_body : int(doc.bodies.size()) - 1}};
    const json at = params.value("at", json());
    if (at.is_null()) {
        r["x"] = params.value("x", 0.0);
        r["y"] = params.value("y", 0.0);
        return r;
    }
    if (params.contains("x") || params.contains("y")) throw std::runtime_error("give at, or x and y, not both");
    static const std::string kAtForms = "at must be [x, y, z], {\"body\": b, \"face\": f} or {\"body\": b, \"edge\": e}";
    Vec3d p;
    if (at.is_array()) {
        if (at.size() != 3 || !at[0].is_number() || !at[1].is_number() || !at[2].is_number()) throw std::runtime_error(kAtForms);
        p = Vec3d(at[0].get<double>(), at[1].get<double>(), at[2].get<double>());
    } else if (at.is_object() && at.size() == 2 && at.contains("body") && at.at("body").is_number_integer() &&
               ((at.contains("face") && at.at("face").is_number_integer()) || (at.contains("edge") && at.at("edge").is_number_integer()))) {
        // A round reference stands for its axis, so the hole shares it: the STEP-hole fix.
        const int b = at.at("body").get<int>();
        if (b < 0 || b >= int(doc.bodies.size()))
            throw std::runtime_error("at: body " + std::to_string(b) + " is out of range (have " + std::to_string(doc.bodies.size()) + ")");
        const TopoDS_Shape&          shape   = doc.bodies[b].shape;
        const bool                   is_face = at.contains("face");
        const int                    id      = at.at(is_face ? "face" : "edge").get<int>();
        const std::string            what    = std::string(is_face ? "face " : "edge ") + std::to_string(id) + " of body " + std::to_string(b);
        GeometryEngine::CylinderFace round;
        if (is_face) {
            const TopoDS_Face f = GeometryEngine::face_by_index(shape, id);
            if (f.IsNull()) throw std::runtime_error("at: " + what + " does not exist");
            round = GeometryEngine::cylinder_of_face(f);
            if (!round.ok) throw std::runtime_error(what + " is " + face_kind_name(f) + "; \"at\" takes a cylindrical face or a circular edge");
        } else {
            const TopoDS_Edge e = GeometryEngine::edge_by_index(shape, id);
            if (e.IsNull()) throw std::runtime_error("at: " + what + " does not exist");
            round = GeometryEngine::circle_of_edge(e);
            if (!round.ok) throw std::runtime_error(what + " is a " + edge_kind_name(e) + "; \"at\" takes a cylindrical face or a circular edge");
        }
        if (std::abs(round.axis.dot(plane.normal)) < std::cos(M_PI / 180.0))
            throw std::runtime_error("the axis of " + what + " is not square to the plane, so a hole drilled from it cannot share that axis");
        p = round.base;
    } else {
        throw std::runtime_error(kAtForms);
    }
    // Projected onto the plane along its normal, in the plane's frame.
    const Vec3d d = p - plane.origin;
    r["x"]         = d.dot(plane.x_axis);
    r["y"]         = d.dot(plane.y_axis);
    r["off_plane"] = d.dot(plane.normal);
    return r;
}

std::function<int(const std::string& name)> cad_thread(CadDocument& doc, const json& params, json& resolved)
{
    auto given = [&](const char* k) { return params.contains(k) && !params.at(k).is_null(); };
    auto flag  = [&](const char* k) {
        if (!params.at(k).is_boolean()) throw std::runtime_error(std::string(k) + " must be true or false");
        return params.at(k).get<bool>();
    };
    // A size in mm that replaces the standard's; absent keeps it.
    auto size = [&](const char* k) -> std::optional<double> {
        if (!given(k)) return std::nullopt;
        if (!params.at(k).is_number() || !(params.at(k).get<double>() > 0))
            throw std::runtime_error(std::string(k) + " must be a number > 0 (mm)");
        return params.at(k).get<double>();
    };
    const bool on_face = given("face"), on_edge = given("edge");
    if (on_face && on_edge) throw std::runtime_error("give face or edge, not both");

    SketchPlane                  plane;
    GeometryEngine::CylinderFace cyl;
    bool                         internal = false;
    int                          target   = -1;   // the kernel's last body
    if (on_face || on_edge) {
        // A picked bore, boss or rim: the thread runs along its axis from its base, as the card's
        // pick sets it, on that body.
        const std::string kind = on_face ? "face" : "edge";
        for (const char* k : {"plane", "x", "y", "at"})
            if (given(k)) throw std::runtime_error(std::string(k) + " cannot be given with " + kind + ": the thread runs on its axis");
        if (doc.bodies.empty()) throw CadPrecondition("no body to thread");
        if (!params.at(kind).is_number_integer()) throw std::runtime_error(kind + " must be an id from body_describe");
        target = target_body_arg(params, doc);
        if (target < 0) target = int(doc.bodies.size()) - 1;
        const int           id    = params.at(kind).get<int>();
        const TopoDS_Shape& shape = doc.bodies[target].shape;
        const std::string   what  = kind + " " + std::to_string(id) + " of body " + std::to_string(target);
        if (!doc.thread_target(target, on_face ? id : -1, on_face ? -1 : id, plane, cyl)) {
            if (on_face) {
                const TopoDS_Face f = GeometryEngine::face_by_index(shape, id);
                if (f.IsNull())
                    throw std::runtime_error(what + " does not exist (it has " + std::to_string(GeometryEngine::face_count(shape)) + " faces)");
                throw std::runtime_error(what + " is " + face_kind_name(f) + "; a thread needs a cylindrical face or a circular edge");
            }
            const TopoDS_Edge e = GeometryEngine::edge_by_index(shape, id);
            if (e.IsNull())
                throw std::runtime_error(what + " does not exist (it has " + std::to_string(GeometryEngine::edge_count(shape)) + " edges)");
            throw std::runtime_error(what + " is a " + edge_kind_name(e) + "; a thread needs a cylindrical face or a circular edge");
        }
        // A bore taps, a boss is threaded; a rim cannot tell, so it is external unless told.
        internal = given("internal") ? flag("internal") : cyl.internal;
        resolved = {{"x", 0.0}, {"y", 0.0}};
    } else {
        if (!given("internal"))
            throw std::runtime_error("internal is required for a thread on a plane: true taps a hole in a body, false threads a rod");
        internal = flag("internal");
        if (!doc.bodies.empty()) {
            resolved = cad_hole_placement(doc, params, plane, target);   // placed as a hole is
        } else {
            // A rod threaded from nothing: there is no body to place against.
            if (internal) throw CadPrecondition("an internal thread needs a body to tap; add or import one first");
            if (given("at")) throw std::runtime_error("at places a thread against a body, and there is none; give x and y");
            plane    = cad_resolve_plane(params.value("plane", json("XY")), doc, true);
            resolved = {{"x", params.value("x", 0.0)}, {"y", params.value("y", 0.0)}};
        }
    }

    // The standard named, else the one nearest the picked cylinder, as the card infers it.
    const ThreadSpec* spec = nullptr;
    if (given("designation")) {
        const json& d = params.at("designation");
        if (!d.is_string()) throw std::runtime_error("designation must be a string such as \"M5\" or \"1/4-20 UNC\"");
        spec = find_thread_standard(d.get<std::string>());
        if (spec == nullptr) {
            // The same name in another case, else the names that start the other or are one edit away.
            const std::string want = boost::algorithm::to_lower_copy(d.get<std::string>());
            std::string       close;
            for (const ThreadSpec& t : thread_standards())
                if (boost::algorithm::to_lower_copy(std::string(t.name)) == want) close = t.name;
            if (close.empty() && !want.empty())
                for (const ThreadSpec& t : thread_standards()) {
                    const std::string n = boost::algorithm::to_lower_copy(std::string(t.name));
                    if (n.rfind(want, 0) == 0 || want.rfind(n, 0) == 0 || edit_distance(n, want) <= 1)
                        close += (close.empty() ? "" : ", ") + t.name;
                }
            throw std::runtime_error("unknown designation \"" + d.get<std::string>() + "\"" +
                                     (close.empty() ? std::string("; ISO metric (M5, M8x1) and UTS (1/4-20 UNC) sizes are known") : "; close: " + close));
        }
    } else if (cyl.ok) {
        spec = nearest_thread_standard(2.0 * cyl.radius, internal);
    }
    const std::optional<double> diameter = size("diameter"), pitch = size("pitch"), depth = size("depth"), length = size("length");
    if (spec == nullptr && (!diameter || !pitch))
        throw std::runtime_error("a thread on a plane needs designation, or diameter and pitch for a custom thread");
    ThreadSpec s = spec != nullptr ? *spec : ThreadSpec{"", *diameter, *pitch, ThreadSpec::Series::MetricCoarse};
    if (diameter) s.major_diameter_mm = *diameter;
    if (pitch) s.pitch_mm = *pitch;
    // The 60 degree V of the standards: a tapped thread runs from the minor diameter out to the
    // major, a rod's in from the crests, as the card sets the depth for the role.
    const double d = depth ? *depth : internal ? s.internal_depth_mm() : s.thread_depth_mm();
    double       len = 0;
    if (length)
        len = *length;
    else if (on_face && cyl.height > 1e-6)
        len = cyl.height;   // the card's default: the whole face
    else
        throw std::runtime_error(on_face   ? "length is required: the face has no length"
                                 : on_edge ? "length is required for a thread on an edge"
                                           : "length is required for a thread on a plane");

    const bool custom      = spec == nullptr || s.major_diameter_mm != spec->major_diameter_mm || s.pitch_mm != spec->pitch_mm;
    resolved["designation"] = custom ? json() : json(spec->name);
    resolved["internal"]    = internal;
    resolved["diameter"]    = s.major_diameter_mm;
    resolved["pitch"]       = s.pitch_mm;
    resolved["depth"]       = d;
    resolved["length"]      = len;
    resolved["frame"]       = frame_json(plane);
    const double x = resolved.at("x").get<double>(), y = resolved.at("y").get<double>();
    return [&doc, s, len, d, internal, x, y, plane, target](const std::string& name) {
        const int t = doc.add_thread(0.5 * s.major_diameter_mm, s.pitch_mm, len, d, internal, x, y, plane, name);
        doc.features[t].target_body = target;
        return t;
    };
}

std::function<int(const std::string& name)> cad_sweep(CadDocument& doc, const json& params)
{
    const int  si = sketch_arg(params.value("sketch", json()), doc);
    const json pv = params.value("path", json());
    if (!pv.is_number_integer()) throw std::runtime_error("path must be the feature index of a sketch or a helix (design_describe)");
    const int pi = pv.get<int>();
    check_feature_index(pi, doc);
    const CadFeatureType pt = doc.features[pi].type;
    if (pt != CadFeatureType::Sketch && pt != CadFeatureType::Helix)
        throw std::runtime_error("path " + std::to_string(pi) + " (" + feature_type_name(pt) + ") is not a sketch or a helix");
    if (pi == si) throw std::runtime_error("the path cannot be the profile sketch itself");
    const BooleanMode mode = *operation_arg(params, "new_body", kSolidOperations);
    const int         body = std::max(-1, target_body_arg(params, doc));
    check_has_body(mode, doc);
    return [&doc, si, pi, mode, body](const std::string& name) {
        const int i = doc.add_sweep(si, pi, mode, name);
        doc.features[i].target_body = body;
        return i;
    };
}

std::function<int(const std::string& name)> cad_loft(CadDocument& doc, const json& params)
{
    const json pv = params.value("profiles", json());
    if (!pv.is_array()) throw std::runtime_error("profiles must be a list of sketch feature indices, in loft order");
    if (pv.size() < 2) throw std::runtime_error("loft needs 2 or more sketches in profiles");
    std::vector<int> refs;
    for (const json& v : pv) {
        if (!v.is_number_integer()) throw std::runtime_error("profiles must hold sketch feature indices (design_describe)");
        const int si = sketch_arg(v, doc);
        if (std::find(refs.begin(), refs.end(), si) != refs.end())
            throw std::runtime_error("sketch " + std::to_string(si) + " is listed twice in profiles");
        refs.push_back(si);
    }
    const json ruled = params.value("ruled", json(false));
    if (!ruled.is_boolean()) throw std::runtime_error("ruled must be true or false");
    const BooleanMode mode = *operation_arg(params, "new_body", kSolidOperations);
    const int         body = std::max(-1, target_body_arg(params, doc));
    check_has_body(mode, doc);
    return [&doc, refs, ruled = ruled.get<bool>(), mode, body](const std::string& name) {
        const int i = doc.add_loft(refs, ruled, mode, name);
        doc.features[i].target_body = body;
        return i;
    };
}

std::function<int(const std::string& name)> cad_hole(CadDocument& doc, const json& params, json& resolved)
{
    const int  style    = enum_index("type", params.value("type", json("simple")), kHoleTypes);
    // A seat's sizes are its type's: beside another type they would size nothing, so they are refused. A null is the
    // argument left out.
    static const std::pair<const char*, int> kSeatOf[] = {{"cbore_diameter", 1}, {"cbore_depth", 1}, {"csink_diameter", 2}, {"csink_angle", 2}};
    for (const auto& [key, seat] : kSeatOf)
        if (seat != style && params.contains(key) && !params.at(key).is_null())
            throw std::runtime_error(std::string(key) + " goes with type " + kHoleTypes[seat] + ", not " + kHoleTypes[style]);
    const json named    = params.value("designation", json());
    const bool standard = !named.is_null();
    if (standard) {
        if (params.contains("diameter")) throw std::runtime_error("give diameter or designation, not both");
        // The standard sizes the seat too (CadDocument::add_hole_standard), so these would be replaced.
        for (const char* k : {"cbore_diameter", "cbore_depth", "csink_diameter", "csink_angle", "standard"})
            if (params.contains(k))
                throw std::runtime_error(std::string(k) + " cannot be given with designation: the standard sizes the hole and its seat");
        if (!named.is_string() || named.get<std::string>().empty())
            throw std::runtime_error("designation must be a standard size such as \"M6\" or \"1/4-20\"");
    }
    const double diameter = params.value("diameter", 5.0);
    if (diameter <= 0) throw std::runtime_error("diameter must be > 0");
    const double      depth          = params.value("depth", 10.0);
    const bool        through        = params.value("through", false);
    const double      cbore_diameter = params.value("cbore_diameter", 0.0);
    const double      cbore_depth    = params.value("cbore_depth", 0.0);
    const double      csink_diameter = params.value("csink_diameter", 0.0);
    const double      csink_angle    = params.value("csink_angle", 90.0);
    const std::string provenance     = params.value("standard", std::string());
    const std::string designation    = standard ? named.get<std::string>() : std::string();
    SketchPlane plane;
    int         body = -1;
    resolved          = cad_hole_placement(doc, params, plane, body);
    resolved["frame"] = frame_json(plane);
    const double x = resolved.at("x").get<double>(), y = resolved.at("y").get<double>();
    // add_hole_standard throws on a designation it does not know, which the caller's add_feature rolls back.
    return [&doc, style, standard, designation, diameter, depth, through, cbore_diameter, cbore_depth, csink_diameter,
            csink_angle, provenance, x, y, plane, body](const std::string& name) {
        const int h = standard ? doc.add_hole_standard(designation, style, through, depth, x, y, plane, name)
                               : doc.add_hole_styled(diameter, depth, through, x, y, plane, style, cbore_diameter, cbore_depth,
                                                     csink_diameter, csink_angle, provenance, name);
        doc.features[h].target_body = body;
        return h;
    };
}

std::function<int(const std::string& name)> cad_thicken(CadDocument& doc, const json& params)
{
    const json type = params.value("type", json());
    if (!type.is_null() && type != "face" && type != "surface") throw std::runtime_error("type must be face or surface");
    const bool surface = thickens_surface(params);
    const json face    = params.value("face", json());
    if (surface && !face.is_null())
        throw std::runtime_error("type surface thickens a whole sheet body and takes no face; type face thickens a face");
    if (!surface && face.is_null()) throw std::runtime_error("type face needs face, a face id (body_describe)");
    if (!surface && !face.is_number_integer()) throw std::runtime_error("face must be a face id (body_describe)");
    if (doc.bodies.empty()) throw CadPrecondition(surface ? "no sheet body to thicken" : "no body to thicken");
    const int    body      = target_body_arg(params, doc);
    const double thickness = params.value("thickness", 2.0);
    const bool   flip      = params.value("flip", false);
    if (surface)
        return [&doc, body, thickness, flip](const std::string& name) { return doc.add_thicken_surface(body, thickness, flip, name); };
    return [&doc, body, fi = face.get<int>(), thickness, flip](const std::string& name) {
        return doc.add_thicken(body, fi, thickness, flip, name);
    };
}

std::function<int(const std::string& name)> cad_pattern(CadDocument& doc, const json& params)
{
    const int  layout = enum_index("type", params.value("type", json("linear")), kPatternTypes);
    const bool curve  = layout == 2;
    // What each layout reads beside count and body, by layout (linear, circular, curve): beside another it would place
    // nothing, so it is refused. A null is the argument left out.
    struct Takes
    {
        const char* key;
        const char* types;
        bool        by[3];
    };
    static const Takes kTakes[] = {{"spacing", "linear", {true, false, false}}, {"dir", "linear", {true, false, false}},
                                   {"angle", "circular", {false, true, false}}, {"plane", "linear or circular", {true, true, false}},
                                   {"sketch", "curve", {false, false, true}},   {"entity", "curve", {false, false, true}}};
    for (const Takes& t : kTakes)
        if (!t.by[layout] && params.contains(t.key) && !params.at(t.key).is_null())
            throw std::runtime_error(std::string(t.key) + " goes with type " + t.types + ", not " + kPatternTypes[layout]);
    // The curve the copies follow, which a curve pattern needs.
    if (curve)
        for (const char* k : {"sketch", "entity"})
            if (!params.value(k, json()).is_number_integer())
                throw std::runtime_error(std::string("a curve pattern needs ") + k +
                                         (std::string(k) == "sketch" ? ", the feature index of the sketch holding the guide curve"
                                                                     : ", the guide curve's entity index within that sketch"));
    const int count = params.value("count", 3);
    if (count < 1) throw std::runtime_error("count must be >= 1");
    if (doc.bodies.empty()) throw CadPrecondition("no body to pattern");
    const int body = target_body_arg(params, doc);
    if (curve)
        return [&doc, count, sketch = params.at("sketch").get<int>(), entity = params.at("entity").get<int>(), body](const std::string& name) {
            return doc.add_pattern_on_curve(count, sketch, entity, body, name);
        };
    const double      spacing = params.value("spacing", 10.0);   // linear step (mm)
    const int         dir     = params.value("dir", 0);          // 0 = plane X, 1 = plane Y
    const double      angle   = params.value("angle", 360.0);    // circular total sweep (deg)
    const SketchPlane plane   = cad_resolve_plane(params.value("plane", json("XY")), doc, false);
    return [&doc, circular = layout == 1, count, spacing, dir, angle, body, plane](const std::string& name) {
        const int p = doc.add_pattern(circular, count, spacing, dir, angle, body, name);
        doc.features[p].plane = plane;   // the circular pattern's axis, the linear one's step directions
        return p;
    };
}

std::function<int(const std::string& name)> cad_align(CadDocument& doc, const json& params)
{
    for (const char* k : {"cs_a", "cs_b"})
        if (!params.value(k, json()).is_number_integer())
            throw std::runtime_error(std::string("align needs '") + k + "' (CoordSys feature index)");
    const int    kind   = enum_index("type", params.value("type", json("fastened")), kAlignTypes);
    const int    cs_a   = params.at("cs_a").get<int>();
    const int    cs_b   = params.at("cs_b").get<int>();
    const double offset = params.value("offset", 0.0);
    const double angle  = params.value("angle", 0.0);
    const bool   flip   = params.value("flip", false);
    return [&doc, kind, cs_a, cs_b, offset, angle, flip](const std::string& name) {
        return doc.add_mate(kind, cs_a, cs_b, offset, angle, flip, name);
    };
}

std::function<int(const std::string& name)> cad_dressup(CadDocument& doc, const json& params, bool chamfer)
{
    const char*                    verb  = chamfer ? "chamfer" : "fillet";
    const json                     given = params.value("edges", json());
    // A word names the card's Edges choice, an edge group; anything else is ids.
    const std::optional<FaceGroup> group = face_group_named(given);
    if (given.is_string() && !group) throw std::runtime_error("edges must be an edge id, a list of them, or " + kFaceGroupWords);
    const std::vector<int>         edges = group ? std::vector<int>() : edge_ids_arg(params, verb);
    const char*                    key   = chamfer ? "distance" : "radius";
    const double                   size  = params.value(key, 1.0);
    if (size <= 0) throw std::runtime_error(std::string(key) + " must be > 0");
    if (doc.bodies.empty()) throw CadPrecondition(std::string("no body to ") + verb);
    const int body = target_body_arg(params, doc);
    return [&doc, chamfer, group, edges, size, body](const std::string& name) {
        const int i = group ? (chamfer ? doc.add_chamfer(size, *group, name) : doc.add_fillet(size, *group, name))
                            : (chamfer ? doc.add_chamfer(size, edges, name) : doc.add_fillet(size, edges, name));
        if (body >= 0) doc.features[i].target_body = body;   // the edge ids are resolved against this body's shape
        return i;
    };
}

std::function<int(const std::string& name)> cad_split(CadDocument& doc, const json& params, json& resolved)
{
    auto given = [&](const char* k) { return params.contains(k) && !params.at(k).is_null(); };
    auto flag  = [&](const char* k, bool def) {
        if (!given(k)) return def;
        if (!params.at(k).is_boolean()) throw std::runtime_error(std::string(k) + " must be true or false");
        return params.at(k).get<bool>();
    };
    const bool by_plane = given("plane"), by_face = given("face");
    if (by_plane && by_face) throw std::runtime_error("give plane or face, not both");
    if (!by_plane && !by_face)
        throw std::runtime_error("split needs plane (\"XY\", \"XZ\", \"YZ\", {\"datum\": i} or {\"body\": b, \"face\": f}) or face (a face id to split along)");
    if (doc.bodies.empty()) throw CadPrecondition("no body to split");
    const int  body       = target_body_arg(params, doc);
    const int  t          = body >= 0 ? body : int(doc.bodies.size()) - 1;
    const bool keep_upper = flag("keep_upper", true), keep_lower = flag("keep_lower", true);
    if (!keep_upper && !keep_lower) throw std::runtime_error("keep_upper and keep_lower are both false, so nothing would be kept");
    // The kernel keeps the upper piece in the body's place and puts the lower one after it.
    resolved = {{"body", t}};
    if (keep_upper) resolved["upper_body"] = t;
    if (keep_lower) resolved["lower_body"] = keep_upper ? t + 1 : t;

    if (by_face) {
        for (const char* k : {"offset", "flip"})
            if (given(k))
                throw std::runtime_error(std::string(k) + " goes with plane; to split along a face at an offset, give plane {\"body\": b, \"face\": f}");
        if (!params.at("face").is_number_integer()) throw std::runtime_error("face must be a face id (body_describe)");
        const int  face = params.at("face").get<int>();
        const json fbv  = params.value("face_body", json(-1));
        if (!fbv.is_number_integer() || fbv.get<int>() < -1 || fbv.get<int>() >= int(doc.bodies.size()))
            throw std::runtime_error("face_body must be a body index, or -1 for the body being split");
        const int face_body = fbv.get<int>();
        const int fb        = face_body >= 0 ? face_body : t;
        if (GeometryEngine::face_by_index(doc.bodies[fb].shape, face).IsNull())
            throw std::runtime_error("face " + std::to_string(face) + " of body " + std::to_string(fb) + " does not exist (it has " +
                                     std::to_string(GeometryEngine::face_count(doc.bodies[fb].shape)) + " faces)");
        return [&doc, body, face_body, face, keep_upper, keep_lower](const std::string& name) {
            return doc.add_split_by_face(body, face_body, face, keep_upper, keep_lower, name);
        };
    }
    if (given("face_body")) throw std::runtime_error("face_body goes with face");
    const SketchPlane plane = cad_resolve_plane(params.at("plane"), doc, false);
    const json        ov    = params.value("offset", json(0.0));
    if (!ov.is_number()) throw std::runtime_error("offset must be a number (mm along the plane's normal)");
    const double offset = ov.get<double>();
    const bool   flip   = flag("flip", false);
    // The plane the kernel cuts along (apply_cut): moved along its normal, then turned over.
    SketchPlane cut = plane;
    cut.origin += cut.normal * offset;
    if (flip) cut.normal = -cut.normal;
    // A plane clear of the body would leave one piece empty; the card does not check this.
    Bnd_Box box;
    BRepBndLib::AddOptimal(doc.bodies[t].shape, box, false, false);
    if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        double lo = 1e300, hi = -1e300;
        for (double x : {x0, x1})
            for (double y : {y0, y1})
                for (double z : {z0, z1}) {
                    const double side = (Vec3d(x, y, z) - cut.origin).dot(cut.normal);
                    lo = std::min(lo, side);
                    hi = std::max(hi, side);
                }
        if (lo >= -1e-6 || hi <= 1e-6)
            throw std::runtime_error("the plane misses body " + std::to_string(t) + ": all of it lies on its " +
                                     (lo >= -1e-6 ? "upper" : "lower") + " side, so there is nothing to split");
    }
    resolved["frame"] = frame_json(cut);
    return [&doc, plane, offset, flip, keep_upper, keep_lower, body](const std::string& name) {
        return doc.add_cut(plane, offset, flip, keep_upper, keep_lower, body, name);
    };
}

void solid_silent_forms(std::vector<SilentForm>& forms)
{
    forms.insert(forms.end(), {
        {"solid_extrude",                action_extrude},
        {"solid_revolve",                action_revolve},
        {"solid_fillet",                 action_fillet},
        {"solid_chamfer",                action_chamfer},
        {"solid_hole",                   action_hole},
        {"solid_boolean",                action_boolean},
        {"solid_pattern",                action_pattern},
        {"solid_shell",                  action_shell},
        {"solid_rib",                    action_rib},
        {"solid_draft",                  action_draft},
        {"solid_mirror",                 action_mirror},
        {"solid_thicken",                action_thicken},
        {"solid_split",                  action_split},
        {"solid_thread",                 action_thread},
        {"solid_sweep",                  action_sweep},
        {"solid_loft",                   action_loft},
        {"solid_delete_face",            action_delete_face},
        {"solid_align",                  action_align},
        {"solid_list_align_types",       action_list_align_types},
    });
}

}}} // namespace Slic3r::GUI::Mcp
