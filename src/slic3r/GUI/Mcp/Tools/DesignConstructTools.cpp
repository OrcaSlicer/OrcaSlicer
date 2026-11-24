// The place Design, its Construct heading: construct_plane, construct_axis, construct_coordinate_system and helix, the
// datum features that sketches and features are placed on, and cad_resolve_plane(), which finds the plane a call names.
// construct_silent_forms() binds them to their tool blocks, and run_design_tool() (DesignTools.cpp) runs them behind
// the Design commands' gate.

#include "slic3r/GUI/Mcp/Tools/DesignCommon.hpp"
#include "slic3r/GUI/Mcp/McpToolsDesign.hpp"
#include "libslic3r/Point.hpp"
#include <stdexcept>
#include <math.h>

#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <initializer_list>
#include <map>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/format.hpp"

#include <TopoDS_Edge.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <BRepAdaptor_CompCurve.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <TopoDS_Wire.hxx>

using json = nlohmann::json;

namespace Slic3r { namespace GUI { namespace Mcp {

// ---- Shared with the other Design files (DesignCommon.hpp) ----------------------------------

int datum_ordinal(const CadDocument& doc, int fi)
{
    int ordinal = 0;
    for (int i = 0; i < fi; ++i)
        if (doc.features[i].type == CadFeatureType::Plane && doc.features[i].enabled) ++ordinal;
    return ordinal;
}

namespace {

json action_construct_axis(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    std::string t = params.value("type", std::string("two_points"));
    AxisType at = AxisType::TwoPoints;
    if (t == "face_normal")        at = AxisType::FaceNormal;
    else if (t == "cylinder")      at = AxisType::CylinderCenterline;
    else if (t == "plane_intersection") at = AxisType::PlaneIntersection;
    else if (t == "along_edge")    at = AxisType::AlongEdge;
    doc.checkpoint();
    int idx = doc.add_axis(at, panel->mcp_feature_name(_L("Axis")));
    CadFeature& f = doc.features[idx];
    if (params.contains("p1") && params["p1"].is_array() && params["p1"].size() >= 3)
        f.axis_p1 = Vec3d(params["p1"][0].get<double>(), params["p1"][1].get<double>(), params["p1"][2].get<double>());
    if (params.contains("p2") && params["p2"].is_array() && params["p2"].size() >= 3)
        f.axis_p2 = Vec3d(params["p2"][0].get<double>(), params["p2"][1].get<double>(), params["p2"][2].get<double>());
    f.axis_body     = params.value("body", -1);
    f.axis_face     = params.value("face", -1);
    f.axis_edge     = params.value("edge", -1);
    f.axis_plane_a  = params.value("plane_a", -1);
    f.axis_plane_b  = params.value("plane_b", -1);
    // Datum features don't produce a body; check for an error returned by resolve.
    bool recompute_ok = doc.recompute();
    auto axes = doc.resolve_datum_axes();
    std::string err = doc.error;
    if (recompute_ok && err.empty() && !axes.empty() && !axes.back().error.empty())
        err = axes.back().error;
    panel->mcp_after_change();
    return json{{"ok", true}, {"axis_index", idx}, {"error", err}};
}

json action_construct_coordinate_system(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    std::string t = params.value("type", std::string("point_world"));
    CoordSysType ct = CoordSysType::PointWorld;
    if (t == "face_and_direction") ct = CoordSysType::FaceAndDirection;
    Vec3d pt(0, 0, 0);
    if (params.contains("point") && params["point"].is_array() && params["point"].size() >= 3)
        pt = Vec3d(params["point"][0].get<double>(), params["point"][1].get<double>(), params["point"][2].get<double>());
    doc.checkpoint();
    int idx = doc.add_coordsys(ct, pt, panel->mcp_feature_name(_L("Coordinate system")));
    CadFeature& f = doc.features[idx];
    f.coordsys_body  = params.value("body", -1);
    f.coordsys_face  = params.value("face", -1);
    f.coordsys_edge  = params.value("edge", -1);
    if (params.contains("x_hint") && params["x_hint"].is_array() && params["x_hint"].size() >= 3)
        f.coordsys_x_hint = Vec3d(params["x_hint"][0].get<double>(), params["x_hint"][1].get<double>(), params["x_hint"][2].get<double>());
    bool recompute_ok = doc.recompute();
    auto css = doc.resolve_datum_coordsys();
    std::string err = doc.error;
    if (recompute_ok && err.empty() && !css.empty() && !css.back().error.empty())
        err = css.back().error;
    panel->mcp_after_change();
    return json{{"ok", true}, {"coordsys_index", idx}, {"error", err}};
}

// The Plane card's Confirm (on_add_plane): one undo step, after the card's checks.
json action_construct_plane(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const auto   add = cad_datum_plane(doc, params);
    json r = add_feature(panel, "plane_index", [&] { return add(panel->mcp_feature_name(_L_CONTEXT("Plane", "Design"))); });
    const int fi = r.at("plane_index").get<int>();
    r["name"]  = doc.features[fi].name;
    r["frame"] = frame_json(doc.resolve_datum_planes().at(datum_ordinal(doc, fi)).second);
    return r;
}

json action_helix(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    CadFeature   h;   // what add_helix records, built first so a curve the kernel refuses adds nothing
    h.type              = CadFeatureType::Helix;
    h.plane             = cad_resolve_plane(params.value("plane", json("XY")), doc, false);
    h.helix_radius      = params.value("radius", 10.0);
    h.helix_pitch       = params.value("pitch", 5.0);
    h.helix_height      = params.value("height", 20.0);
    h.helix_left_handed = params.value("left_handed", false);
    h.helix_taper_deg   = params.value("taper_deg", 0.0);
    std::string       err;
    const TopoDS_Wire curve = doc.build_helix_wire(h, err);
    if (curve.IsNull()) throw std::runtime_error("helix: " + (err.empty() ? std::string("the curve could not be built") : err));
    // Where the curve starts and which way it leaves: a sweep's profile is drawn across it there.
    BRepAdaptor_CompCurve c(curve);
    gp_Pnt start;
    gp_Vec tangent;
    c.D1(c.FirstParameter(), start, tangent);
    doc.checkpoint();
    int idx = doc.add_helix(h.plane, h.helix_radius, h.helix_pitch, h.helix_height, h.helix_left_handed, h.helix_taper_deg,
                            panel->mcp_feature_name(_L("Helix")));
    panel->mcp_after_change();
    return json{{"ok", true}, {"helix_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error},
                {"frame", frame_json(h.plane)}, {"start", vec3(Vec3d(start.X(), start.Y(), start.Z()))},
                {"tangent", vec3(Vec3d(tangent.X(), tangent.Y(), tangent.Z()).normalized())}};
}

} // namespace

SketchPlane cad_resolve_plane(const json& plane, const CadDocument& doc, bool drill, int* face_body)
{
    static const std::string kForms =
        "plane must be \"XY\", \"XZ\", \"YZ\", {\"datum\": i} or {\"body\": b, \"face\": f}";
    if (plane.is_string()) {
        // The base planes through the modeling origin, as the GUI's plane list offers them.
        const std::string n = plane.get<std::string>();
        SketchPlane pl;
        if      (n == "XY") pl = SketchPlane::XY();
        else if (n == "XZ") pl = SketchPlane::XZ();
        else if (n == "YZ") pl = SketchPlane::YZ();
        else throw std::runtime_error(kForms);
        pl.origin += doc.modeling_origin;
        return pl;
    }
    if (plane.is_object() && plane.size() == 1 && plane.contains("datum") && plane["datum"].is_number_integer()) {
        // A datum is named by its feature index; the kernel lists the enabled Plane features in
        // order, so its position there is the count of enabled Plane features before it.
        const int fi = plane["datum"].get<int>();
        if (fi < 0 || fi >= int(doc.features.size()))
            throw std::runtime_error("datum " + std::to_string(fi) + " is not a feature (the design has " +
                                     std::to_string(doc.features.size()) + ")");
        const CadFeature& f = doc.features[fi];
        if (f.type != CadFeatureType::Plane)
            throw std::runtime_error("feature " + std::to_string(fi) + " (" + feature_type_name(f.type) +
                                     ") is not a datum plane");
        if (!f.enabled)
            throw CadPrecondition("datum plane " + std::to_string(fi) + " is hidden; show it first");
        const int  ordinal = datum_ordinal(doc, fi);
        const auto datums  = doc.resolve_datum_planes();
        if (ordinal >= int(datums.size()))
            throw CadPrecondition("datum plane " + std::to_string(fi) + " could not be resolved");
        return datums[ordinal].second;
    }
    if (plane.is_object() && plane.size() == 2 && plane.contains("body") && plane.contains("face") &&
        plane["body"].is_number_integer() && plane["face"].is_number_integer()) {
        const int b = plane["body"].get<int>(), fi = plane["face"].get<int>();
        if (b < 0 || b >= int(doc.bodies.size()))
            throw std::runtime_error("body " + std::to_string(b) + " is out of range (have " +
                                     std::to_string(doc.bodies.size()) + ")");
        const TopoDS_Face face = GeometryEngine::face_by_index(doc.bodies[b].shape, fi);
        if (face.IsNull())
            throw std::runtime_error("face " + std::to_string(fi) + " of body " + std::to_string(b) + " does not exist (it has " +
                                     std::to_string(GeometryEngine::face_count(doc.bodies[b].shape)) + " faces)");
        if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane)
            throw std::runtime_error("face " + std::to_string(fi) + " of body " + std::to_string(b) + " is " +
                                     face_kind_name(face) + "; a plane needs a planar face");
        SketchPlane pl;
        if (!(drill ? doc.inward_plane_of_face(b, fi, pl) : doc.plane_of_face(b, fi, pl)))
            throw std::runtime_error("face " + std::to_string(fi) + " of body " + std::to_string(b) + " has no plane");
        if (face_body != nullptr) *face_body = b;
        return pl;
    }
    throw std::runtime_error(kForms);
}

std::function<int(const std::string& name)> cad_datum_plane(CadDocument& doc, const json& params)
{
    using P = PlaneType;
    CadFeature f;
    f.type       = CadFeatureType::Plane;
    f.plane_type = P(enum_index("type", params.value("type", json("offset")), kPlaneTypes));
    const P    t       = f.plane_type;
    auto       given   = [&](const char* k) { return params.contains(k) && !params.at(k).is_null(); };
    const bool on_face = given("face_a");

    // What each construction reads in resolve_datum_planes(); a param it would ignore is refused.
    auto uses = [&](const std::string& k) {
        if (k == "face_a") return t != P::TwoEdges;
        if (k == "face_b") return t == P::Midplane;
        if (k == "edge_a") return t == P::Angle || t == P::TwoEdges;
        if (k == "edge_b") return t == P::TwoEdges;
        if (k == "base")   return (t == P::Offset || t == P::Coincident || t == P::Angle) && !on_face;
        if (k == "offset") return t == P::Offset;
        if (k == "angle")  return (t == P::Offset && !on_face) || t == P::Angle || t == P::Tangent;
        return t == P::Offset && !on_face;   // axis
    };
    for (const char* k : {"face_a", "face_b", "edge_a", "edge_b", "base", "offset", "angle", "axis"})
        if (given(k) && !uses(k))
            throw std::runtime_error(std::string(k) + " is not used by a " + kPlaneTypes[int(t)] + " plane" +
                                     (on_face && std::string(k) != "face_a" ? " on face_a" : ""));

    // {"body": b, "face": f} or {"body": b, "edge": e}, on the current topology.
    auto ref = [&](const char* key, const char* kind, int& body, int& id) {
        if (!given(key)) return;
        const json& v = params.at(key);
        if (!v.is_object() || v.size() != 2 || !v.contains("body") || !v.contains(kind) || !v.at("body").is_number_integer() ||
            !v.at(kind).is_number_integer())
            throw std::runtime_error(std::string(key) + " must be {\"body\": b, \"" + kind + "\": id}");
        body = v.at("body").get<int>();
        id   = v.at(kind).get<int>();
        if (body < 0 || body >= int(doc.bodies.size()))
            throw std::runtime_error(std::string(key) + ": body " + std::to_string(body) + " is out of range (have " +
                                     std::to_string(doc.bodies.size()) + ")");
        const TopoDS_Shape& s    = doc.bodies[body].shape;
        const bool          face = std::string(kind) == "face";
        if (face ? GeometryEngine::face_by_index(s, id).IsNull() : GeometryEngine::edge_by_index(s, id).IsNull())
            throw std::runtime_error(std::string(key) + ": " + kind + " " + std::to_string(id) + " of body " + std::to_string(body) +
                                     " does not exist (it has " +
                                     std::to_string(face ? GeometryEngine::face_count(s) : GeometryEngine::edge_count(s)) + " " + kind + "s)");
    };
    ref("face_a", "face", f.plane_face_body, f.plane_face);
    ref("face_b", "face", f.plane_face2_body, f.plane_face2);
    ref("edge_a", "edge", f.plane_edge_body, f.plane_edge);
    ref("edge_b", "edge", f.plane_edge2_body, f.plane_edge2);

    // The card's base list: the three base planes, then the shown datum planes in order.
    const json base = params.value("base", json("XY"));
    if (base.is_object() && !base.contains("datum"))
        throw std::runtime_error("base must be \"XY\", \"XZ\", \"YZ\" or {\"datum\": i}; a face goes in face_a");
    cad_resolve_plane(base, doc, false);   // refuses what is not a base plane or a shown datum
    f.plane_base = base.is_string() ? (base == "XZ" ? 1 : base == "YZ" ? 2 : 0) : 3 + datum_ordinal(doc, base.at("datum").get<int>());

    // The card's spin controls: their defaults and limits.
    auto number = [&](const char* key, double def, double lo, double hi) {
        if (!given(key)) return def;
        const json& v = params.at(key);
        if (!v.is_number() || !(v.get<double>() >= lo && v.get<double>() <= hi))
            throw std::runtime_error(Slic3r::format("%1% must be a number from %2% to %3%", key, lo, hi));
        return v.get<double>();
    };
    f.plane_offset     = number("offset", 20.0, -100000, 100000);
    f.plane_angle_tilt = number("angle", 0.0, -180, 180);
    if (given("axis")) {
        if (!params.at("axis").is_number_integer() || (params.at("axis") != 0 && params.at("axis") != 1))
            throw std::runtime_error("axis must be 0 (the base plane's X) or 1 (its Y)");
        f.plane_axis = params.at("axis").get<int>();
    }
    if (given("size")) {
        const json& sz = params.at("size");
        if (!sz.is_array() || sz.size() != 2 || !sz[0].is_number() || !sz[1].is_number() ||
            !(sz[0].get<double>() >= 1 && sz[0].get<double>() <= 100000 && sz[1].get<double>() >= 1 && sz[1].get<double>() <= 100000))
            throw std::runtime_error("size must be [u, v], each from 1 to 100000 mm");
        f.plane_u_size = sz[0].get<double>();
        f.plane_v_size = sz[1].get<double>();
    }
    if (const std::string why = doc.datum_plane_refusal(f); !why.empty()) throw std::runtime_error(why);
    return [&doc, f](const std::string& name) {
        const int   i = doc.add_plane(f.plane_base, f.plane_offset, f.plane_angle_tilt, f.plane_axis, name);
        CadFeature& p = doc.features[i];
        // The references and size the card's apply_plane_refs() copies.
        p.plane_type       = f.plane_type;
        p.plane_face_body  = f.plane_face_body;  p.plane_face  = f.plane_face;
        p.plane_face2_body = f.plane_face2_body; p.plane_face2 = f.plane_face2;
        p.plane_edge_body  = f.plane_edge_body;  p.plane_edge  = f.plane_edge;
        p.plane_edge2_body = f.plane_edge2_body; p.plane_edge2 = f.plane_edge2;
        p.plane_u_size     = f.plane_u_size;
        p.plane_v_size     = f.plane_v_size;
        return i;
    };
}

void construct_silent_forms(std::vector<SilentForm>& forms)
{
    forms.insert(forms.end(), {
        {"construct_axis",               action_construct_axis},
        {"construct_coordinate_system",  action_construct_coordinate_system},
        {"construct_plane",              action_construct_plane},
        {"helix",                        action_helix},
    });
}

}}} // namespace Slic3r::GUI::Mcp
