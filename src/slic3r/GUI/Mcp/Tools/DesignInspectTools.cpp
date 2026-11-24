// The place Design, its Inspect heading: body_describe, a body's faces and edges with their ids; measure;
// body_mass_properties; body_section; body_compare, a body against a reference; and interference.
// inspect_silent_forms() binds them to their tool blocks, and run_design_tool() (DesignTools.cpp) runs them behind the
// Design commands' gate.

#include "slic3r/GUI/Mcp/Tools/DesignCommon.hpp"
#include "slic3r/GUI/Mcp/McpToolsDesign.hpp"
#include "libslic3r/Point.hpp"
#include <stdexcept>
#include <math.h>
#include <TopAbs_ShapeEnum.hxx>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <utility>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"

#include <gp_Pln.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GeomAbs_CurveType.hxx>
#include <BRep_Tool.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <gp_Dir.hxx>
#include <gp_Elips.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>

using json = nlohmann::json;

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// The kinds body_describe gives faces, in GeomAbs_SurfaceType order (the edges' are kEdgeKinds).
const char* const kFaceKinds[] = {"planar",     "cylindrical", "conical", "spherical", "toroidal", "bezier",
                                  "bspline",    "revolution",  "extrusion", "offset",  "other"};
static_assert(std::size(kFaceKinds) == size_t(GeomAbs_OtherSurface) + 1, "every surface type needs a kind");

// The kinds body_describe gives edges, by edge_kind().
const char* const kEdgeKinds[] = {"line", "circle", "ellipse", "bspline", "bezier", "other"};

// The kind of an edge's curve, its index in kEdgeKinds.
int edge_kind(GeomAbs_CurveType t)
{
    switch (t) {
    case GeomAbs_Line:         return 0;
    case GeomAbs_Circle:       return 1;
    case GeomAbs_Ellipse:      return 2;
    case GeomAbs_BSplineCurve: return 3;
    case GeomAbs_BezierCurve:  return 4;
    default:                   return 5;
    }
}

} // namespace

// ---- Shared with the other Design files (DesignCommon.hpp) ----------------------------------

const char* edge_kind_name(const TopoDS_Edge& e) { return kEdgeKinds[edge_kind(BRepAdaptor_Curve(e).GetType())]; }

const char* face_kind_name(const TopoDS_Face& f) { return kFaceKinds[BRepAdaptor_Surface(f).GetType()]; }

namespace {

// Resolve body index -> shape, throwing a clear error if out of range / null.
const TopoDS_Shape& body_shape(const CadDocument& doc, const json& params)
{
    int idx = params.value("body", 0);
    if (idx < 0 || idx >= int(doc.bodies.size()))
        throw std::runtime_error("body index out of range (have " + std::to_string(doc.bodies.size()) + ")");
    if (doc.bodies[idx].shape.IsNull())
        throw CadPrecondition("body has no shape");
    return doc.bodies[idx].shape;
}

json body_describe(DesignPanel* panel, const json& params) { return cad_body_describe(panel->mcp_doc(), params); }

json measure(DesignPanel* panel, const json& params) { return cad_measure(panel->mcp_doc(), params); }

json body_mass_properties(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& shape = body_shape(panel->mcp_doc(), params);
    auto mp = GeometryEngine::mass_properties(shape);
    if (!mp.valid) throw CadKernelFailure("mass properties could not be computed");
    // A sheet body has no volume and no inertia. Report the area and say so, rather than
    // returning numbers a caller would reasonably treat as a material check.
    if (!mp.is_solid)
        return json{
            {"surface_area", mp.surface_area},
            {"volume", 0.0},
            {"is_solid", false},
            {"valid", mp.valid},
            {"note", "sheet body (open shell): it encloses no material, so volume and inertia "
                     "are not defined; surface_area is exact"},
        };
    return json{
        {"volume", mp.volume},
        {"surface_area", mp.surface_area},
        {"center_of_mass", json::array({mp.center_of_mass.x(), mp.center_of_mass.y(), mp.center_of_mass.z()})},
        {"inertia", mp.inertia},
        {"is_solid", true},
        {"valid", mp.valid},
    };
}

// Chain raw section segments (each a sampled-edge polyline) into ordered contours by joining
// endpoints within tol. Grows the tail; when the tail is stuck, reverses the contour and grows
// the other end. A contour is closed when its two ends meet. OCCT section vertices are exact,
// so a small absolute tol suffices.
std::vector<std::pair<std::vector<Vec3d>, bool>>
chain_segments(std::vector<std::vector<Vec3d>> segs, double tol)
{
    std::vector<std::pair<std::vector<Vec3d>, bool>> contours;
    std::vector<char> used(segs.size(), 0);
    auto meets = [&](const Vec3d& a, const Vec3d& b) { return (a - b).norm() <= tol; };
    for (size_t i = 0; i < segs.size(); ++i) {
        if (used[i] || segs[i].size() < 2) continue;
        used[i] = 1;
        std::vector<Vec3d> c = segs[i];
        for (int side = 0; side < 2; ) {           // grow tail; reverse once when stuck
            bool grew = false;
            for (size_t j = 0; j < segs.size(); ++j) {
                if (used[j] || segs[j].size() < 2) continue;
                if (meets(c.back(), segs[j].front())) {
                    c.insert(c.end(), segs[j].begin() + 1, segs[j].end()); used[j] = 1; grew = true;
                } else if (meets(c.back(), segs[j].back())) {
                    for (auto it = segs[j].rbegin() + 1; it != segs[j].rend(); ++it) c.push_back(*it);
                    used[j] = 1; grew = true;
                }
            }
            if (grew) { side = 0; continue; }
            std::reverse(c.begin(), c.end()); ++side;   // try the other end
        }
        bool closed = c.size() > 2 && meets(c.front(), c.back());
        contours.emplace_back(std::move(c), closed);
    }
    return contours;
}

// sections-as-evidence: cross-section of a body by a named base plane at an offset.
json body_section(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& shape = body_shape(panel->mcp_doc(), params);
    const std::string plane_name = params.value("plane", std::string("XY"));
    const double offset = params.value("offset", 0.0);
    // Base plane normal; offset shifts the plane along it.
    gp_Dir n = plane_name == "XZ" ? gp_Dir(0, 1, 0)
             : plane_name == "YZ" ? gp_Dir(1, 0, 0)
                                  : gp_Dir(0, 0, 1);
    gp_Pnt o(n.X() * offset, n.Y() * offset, n.Z() * offset);
    BRepAlgoAPI_Section sect(shape, gp_Pln(o, n), false);
    sect.ComputePCurveOn1(false);
    sect.Approximation(true);
    sect.Build();
    if (!sect.IsDone()) throw CadKernelFailure("section failed");
    std::vector<std::vector<Vec3d>> segs;
    for (TopExp_Explorer ex(sect.Shape(), TopAbs_EDGE); ex.More(); ex.Next()) {
        std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(TopoDS::Edge(ex.Current()));
        if (pts.size() >= 2) segs.push_back(std::move(pts));
    }
    const int raw = int(segs.size());
    auto contours = chain_segments(std::move(segs), 1e-3);
    json jcont = json::array();
    int closed_n = 0;
    for (auto& pc : contours) {
        if (pc.second) ++closed_n;
        json pts = json::array();
        for (const Vec3d& p : pc.first) pts.push_back(vec3(p));
        jcont.push_back(json{{"closed", pc.second}, {"points", std::move(pts)}});
    }
    return json{{"body", params.value("body", 0)}, {"plane", plane_name}, {"offset", offset},
                {"segment_count", raw}, {"contour_count", int(contours.size())},
                {"closed_count", closed_n}, {"contours", std::move(jcont)}};
}

// ---- Validate: volume + bbox deviation of a body vs a reference (the "scarto %") ----------------
// ponytail: volume delta + bbox/centroid offset (the RE skill's actual acceptance metric).
// Surface-deviation heat-map is the upgrade path (per-vertex BRepExtrema), add when needed.
struct ShapeMetrics { double volume; Vec3d centroid, bmin, bmax; };
ShapeMetrics shape_metrics(const TopoDS_Shape& s)
{
    GProp_GProps vp; BRepGProp::VolumeProperties(s, vp);
    gp_Pnt c = vp.CentreOfMass();
    Bnd_Box bb; BRepBndLib::Add(s, bb);
    double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
    return {vp.Mass(), Vec3d(c.X(), c.Y(), c.Z()), Vec3d(x0, y0, z0), Vec3d(x1, y1, z1)};
}

json body_compare(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& cand = body_shape(panel->mcp_doc(), params);   // candidate = the reconstruction
    if (!params.contains("reference")) throw std::runtime_error("body_compare needs 'reference' {step|body}");
    const json& r = params["reference"];

    TopoDS_Shape ref;
    if (r.contains("step")) {
        std::string err;
        std::vector<TopoDS_Shape> solids = GeometryEngine::read_step_solids(input_path(r["step"], "reference.step"), err);
        if (solids.empty()) throw std::runtime_error(err.empty() ? "reference STEP has no solids" : err);
        BRep_Builder b; TopoDS_Compound comp; b.MakeCompound(comp);
        for (const TopoDS_Shape& s : solids) if (!s.IsNull()) b.Add(comp, s);
        ref = comp;
    } else if (r.contains("body")) {
        CadDocument& doc = panel->mcp_doc();
        int idx = r["body"].get<int>();
        if (idx < 0 || idx >= int(doc.bodies.size()) || doc.bodies[idx].shape.IsNull())
            throw std::runtime_error("reference body index out of range / null");
        ref = doc.bodies[idx].shape;
    } else {
        throw std::runtime_error("reference must be {\"step\": path} or {\"body\": id}");
    }

    ShapeMetrics a = shape_metrics(cand), b = shape_metrics(ref);
    double dv = b.volume > 0 ? (a.volume - b.volume) / b.volume * 100.0 : 0.0;
    Vec3d coff = a.centroid - b.centroid;
    Vec3d dmin = a.bmin - b.bmin, dmax = a.bmax - b.bmax;
    // Surface-level deviation (one-sided Hausdorff, candidate vertices -> reference solid):
    // catches local shape error that matching volume + bbox can hide.
    GeometryEngine::Deviation dev = GeometryEngine::surface_deviation(cand, ref);
    return json{
        {"volume", a.volume}, {"volume_reference", b.volume}, {"volume_delta_pct", dv},
        {"centroid_offset", vec3(coff)}, {"centroid_offset_mm", coff.norm()},
        {"bbox", json{{"min", vec3(a.bmin)}, {"max", vec3(a.bmax)}}},
        {"bbox_reference", json{{"min", vec3(b.bmin)}, {"max", vec3(b.bmax)}}},
        {"bbox_delta", json{{"min", vec3(dmin)}, {"max", vec3(dmax)}}},
        {"surface_deviation", json{{"max_mm", dev.max_mm}, {"mean_mm", dev.mean_mm},
                                   {"rms_mm", dev.rms_mm}, {"samples", dev.sample_count}}},
    };
}

json action_check_interference(DesignPanel* panel, const json& params)
{
    const double min_volume = params.value("min_volume", 1e-6);
    CadDocument& doc = panel->mcp_doc();
    // Read-only: no checkpoint(), no recompute(), no mcp_after_change().
    const auto hits = doc.check_interference(min_volume);
    json arr = json::array();
    for (const auto& h : hits) {
        arr.push_back(json{{"body_a", h.body_a}, {"body_b", h.body_b}, {"volume", h.volume}});
    }
    return json{{"ok", true}, {"count", int(hits.size())}, {"interferences", arr}};
}

} // namespace

std::vector<Vec3d> cad_body_vertices(const CadDocument& doc, int body)
{
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(body_shape(doc, json{{"body", body}}), TopAbs_VERTEX, map);
    std::vector<Vec3d> out;
    out.reserve(size_t(map.Extent()));
    for (int i = 1; i <= map.Extent(); ++i) {
        const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(map(i)));
        out.emplace_back(p.X(), p.Y(), p.Z());
    }
    return out;
}

json cad_body_describe(const CadDocument& doc, const json& params)
{
    if (const json before = params.value("before", json()); !before.is_null()) {
        if (!before.is_number_integer()) throw std::runtime_error("before must be a feature index (describe_design's features[].index)");
        json as_of = params;
        as_of.erase("before");
        json out      = cad_body_describe(cad_document_before(doc, before.get<int>()), as_of);
        out["before"] = before;
        // The ids are the model's before that feature, read at the live document's generation, which a call passing
        // them is checked against.
        out["generation"] = doc.topo_generation;
        return out;
    }
    const json vertices = params.value("vertices", json(false));
    if (!vertices.is_boolean()) throw std::runtime_error("vertices must be true or false");
    const TopoDS_Shape& shape = body_shape(doc, params);

    // Filters. Each narrows the list it applies to; direction applies to both.
    auto kind_list = [&](const char* key, const auto& names) {
        std::vector<int> out;
        const json v = params.value(key, json());
        if (v.is_null()) return out;
        if (!v.is_array()) throw std::runtime_error(std::string(key) + " must be a list of kinds");
        // A face kind by its surface's noun, as agents often name it: plane for planar.
        static const std::map<std::string, std::string> nouns{
            {"plane", "planar"}, {"cylinder", "cylindrical"}, {"cone", "conical"}, {"sphere", "spherical"}, {"torus", "toroidal"}};
        for (const json& k : v) {
            const auto noun = k.is_string() && std::string(key) == "kinds" ? nouns.find(k.get<std::string>()) : nouns.end();
            out.push_back(enum_index(key, noun == nouns.end() ? k : json(noun->second), names));
        }
        return out;
    };
    const std::vector<int> kinds      = kind_list("kinds", kFaceKinds);
    const std::vector<int> edge_kinds = kind_list("edge_kinds", kEdgeKinds);
    std::optional<Vec3d>   direction;
    if (const json d = params.value("direction", json()); !d.is_null()) {
        if (!d.is_array() || d.size() != 3 || !d[0].is_number() || !d[1].is_number() || !d[2].is_number())
            throw std::runtime_error("direction must be [x, y, z]");
        const Vec3d v(d[0].get<double>(), d[1].get<double>(), d[2].get<double>());
        if (!(v.norm() > 1e-9)) throw std::runtime_error("direction must not be zero");
        direction = v.normalized();
    }
    const json convex = params.value("convex", json());
    if (!convex.is_null() && !convex.is_boolean()) throw std::runtime_error("convex must be true or false");
    int extreme = -1;   // index into kExtremes: axis = extreme / 2, the + side when it is even
    static const char* const kExtremes[] = {"+x", "-x", "+y", "-y", "+z", "-z"};
    if (const json e = params.value("at_extreme", json()); !e.is_null()) extreme = enum_index("at_extreme", e, kExtremes);
    const json adj = params.value("adjacency", json(false));
    if (!adj.is_boolean()) throw std::runtime_error("adjacency must be true or false");
    const bool adjacency = adj.get<bool>() || !convex.is_null();
    const json lim = params.value("limit", json()), off = params.value("offset", json());
    if (!lim.is_null() && !(lim.is_number_integer() && lim.get<long long>() >= 1)) throw std::runtime_error("limit must be an integer >= 1");
    if (!off.is_null() && !(off.is_number_integer() && off.get<long long>() >= 0)) throw std::runtime_error("offset must be an integer >= 0");
    const double kParallel = std::cos(M_PI / 180.0);   // within 1 degree

    // Enumerate once. The _by_index accessors rescan the shape on every call (edge_by_index
    // rebuilds the whole indexed map), so indexing a body face-by-face is quadratic: ~15 s on a
    // 4.7k-face imported solid, on the UI thread. faces_of/edges_of keep the very same ids.
    const std::vector<TopoDS_Face> faces = GeometryEngine::faces_of(shape);
    const std::vector<TopoDS_Edge> edges = GeometryEngine::edges_of(shape);
    const int nf = int(faces.size()), ne = int(edges.size());
    std::vector<std::vector<int>> edge_faces, face_edges;
    std::vector<std::optional<int>> convexity(ne);   // only for an edge between two faces
    if (adjacency) {
        edge_faces = GeometryEngine::edge_faces(shape);
        face_edges.resize(nf);
        for (int e = 0; e < ne; ++e) {
            for (int f : edge_faces[e]) face_edges[f].push_back(e);
            if (edge_faces[e].size() == 2)
                convexity[e] = GeometryEngine::edge_convexity(edges[e], faces[edge_faces[e][0]], faces[edge_faces[e][1]]);
        }
    }
    std::vector<std::optional<GeometryEngine::SurfaceInfo>> surfaces(nf);
    auto surface = [&](int f) -> const GeometryEngine::SurfaceInfo& {
        if (!surfaces[f]) surfaces[f] = GeometryEngine::surface_info(faces[f]);
        return *surfaces[f];
    };
    double ext_at = 0, ext_tol = 0;
    if (extreme >= 0) {
        Bnd_Box box;
        BRepBndLib::AddOptimal(shape, box, false, false);
        double lo[3], hi[3];
        box.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        ext_at  = extreme % 2 == 0 ? hi[extreme / 2] : lo[extreme / 2];
        ext_tol = std::max(1e-3, 1e-6 * Vec3d(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]).norm());
    }

    auto face_matches = [&](int f) {
        const GeometryEngine::SurfaceInfo& s = surface(f);
        if (!kinds.empty() && std::find(kinds.begin(), kinds.end(), int(s.type)) == kinds.end()) return false;
        // A normal is the face's own only on a plane; elsewhere it is one point's.
        if (direction && (s.type != GeomAbs_Plane || GeometryEngine::face_normal_world(faces[f]).dot(*direction) < kParallel))
            return false;
        return extreme < 0 || std::abs(s.centroid[extreme / 2] - ext_at) <= ext_tol;
    };
    auto edge_matches = [&](int e) {
        const BRepAdaptor_Curve c(edges[e]);
        if (!edge_kinds.empty() && std::find(edge_kinds.begin(), edge_kinds.end(), edge_kind(c.GetType())) == edge_kinds.end())
            return false;
        if (direction) {
            if (c.GetType() != GeomAbs_Line) return false;
            const gp_Dir t = c.Line().Direction();
            if (std::abs(Vec3d(t.X(), t.Y(), t.Z()).dot(*direction)) < kParallel) return false;
        }
        return convex.is_null() || (convexity[e] && *convexity[e] == (convex.get<bool>() ? 1 : -1));
    };

    const bool face_filter = !kinds.empty() || direction || extreme >= 0;
    const bool edge_filter = !edge_kinds.empty() || direction || !convex.is_null();
    std::vector<int> face_ids, edge_ids;
    for (int f = 0; f < nf; ++f)
        if ((!face_filter && !edge_filter) || (face_filter && face_matches(f))) face_ids.push_back(f);
    // A degenerate edge (a cone's apex) has no length and is not listed.
    for (int e = 0; e < ne; ++e)
        if (!BRep_Tool::Degenerated(edges[e]) && ((!face_filter && !edge_filter) || (edge_filter && edge_matches(e))))
            edge_ids.push_back(e);
    // With adjacency, a list none of whose filters was given holds the neighbours of the other's matches.
    auto neighbours = [](const std::vector<int>& of, const std::vector<std::vector<int>>& links, size_t n) {
        std::vector<bool> hit(n, false);
        for (int i : of)
            for (int j : links[i]) hit[j] = true;
        std::vector<int> out;
        for (size_t j = 0; j < n; ++j)
            if (hit[j]) out.push_back(int(j));
        return out;
    };
    if (adjacency && edge_filter && !face_filter) face_ids = neighbours(edge_ids, edge_faces, nf);
    if (adjacency && face_filter && !edge_filter) edge_ids = neighbours(face_ids, face_edges, ne);

    const size_t first = off.is_null() ? 0 : off.get<size_t>();
    const size_t count = lim.is_null() ? std::max(face_ids.size(), edge_ids.size()) : lim.get<size_t>();
    json jfaces = json::array();
    for (size_t k = first; k < face_ids.size() && k - first < count; ++k) {
        const int f = face_ids[k];
        const GeometryEngine::SurfaceInfo& s = surface(f);
        json jf{{"id", f}, {"kind", kFaceKinds[s.type]}, {"area", s.area}, {"centroid", vec3(s.centroid)},
                {"normal", vec3(GeometryEngine::face_normal_world(faces[f]))}};
        switch (s.type) {
        case GeomAbs_Cylinder:
            jf["radius"] = s.radius; jf["axis"] = vec3(s.axis); jf["internal"] = s.internal;
            break;
        case GeomAbs_Cone:
            jf["apex"] = vec3(s.location); jf["axis"] = vec3(s.axis); jf["half_angle_deg"] = s.half_angle * 180.0 / M_PI;
            jf["radius_min"] = s.radius; jf["radius_max"] = s.radius2; jf["internal"] = s.internal;
            break;
        case GeomAbs_Sphere:
            jf["center"] = vec3(s.location); jf["radius"] = s.radius; jf["internal"] = s.internal;
            break;
        case GeomAbs_Torus:
            jf["center"] = vec3(s.location); jf["axis"] = vec3(s.axis);
            jf["major_radius"] = s.radius; jf["minor_radius"] = s.radius2;
            break;
        default: break;
        }
        if (adjacency) jf["edges"] = face_edges[f];
        jfaces.push_back(std::move(jf));
    }
    json jedges = json::array();
    for (size_t k = first; k < edge_ids.size() && k - first < count; ++k) {
        const int e = edge_ids[k];
        const std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(edges[e]);
        if (pts.size() < 2) continue;
        double len = 0;
        for (size_t i = 1; i < pts.size(); ++i) len += (pts[i] - pts[i - 1]).norm();
        const BRepAdaptor_Curve c(edges[e]);
        json je{{"id", e}, {"kind", kEdgeKinds[edge_kind(c.GetType())]}, {"length", len}, {"p0", vec3(pts.front())}, {"p1", vec3(pts.back())}};
        if (c.GetType() == GeomAbs_Circle) {
            const GeometryEngine::CylinderFace circ = GeometryEngine::circle_of_edge(edges[e]);
            je["radius"] = circ.radius;
            je["center"] = vec3(circ.base);
        } else if (c.GetType() == GeomAbs_Ellipse) {
            const gp_Elips el = c.Ellipse();
            je["center"]       = json::array({el.Location().X(), el.Location().Y(), el.Location().Z()});
            je["major_radius"] = el.MajorRadius();
            je["minor_radius"] = el.MinorRadius();
        }
        if (adjacency) {
            je["faces"] = edge_faces[e];
            if (convexity[e] && *convexity[e] != 0) je["convex"] = *convexity[e] > 0;
            else if (convexity[e]) je["tangent"] = true;
        }
        jedges.push_back(std::move(je));
    }
    json out{{"body", params.value("body", 0)}, {"face_count", nf}, {"edge_count", ne},
             {"faces_total", face_ids.size()}, {"edges_total", edge_ids.size()},
             {"faces", std::move(jfaces)}, {"edges", std::move(jedges)},
             // The ids above are indices into this exact topology and expire with it. Echo
             // this back as "generation" on the calls that consume them.
             {"generation", doc.topo_generation}};
    if (first + count < std::max(face_ids.size(), edge_ids.size())) out["next_offset"] = first + count;
    if (vertices.get<bool>()) {
        json                     jvertices = json::array();
        const std::vector<Vec3d> points    = cad_body_vertices(doc, params.value("body", 0));
        for (size_t v = 0; v < points.size(); ++v) jvertices.push_back(json{{"id", int(v)}, {"p", vec3(points[v])}});
        out["vertices"] = std::move(jvertices);
    }
    return out;
}

namespace {

// One measure ref: its shape, the point that stands for it (a face's centroid, an edge's middle
// sample, an axis's middle, the point itself) and, but for a point, a direction (a face's normal,
// an edge's chord, an axis). `planar` is set for a planar face.
TopoDS_Shape measure_ref(const CadDocument& doc, const json& params, const char* name, Vec3d& point,
                         std::optional<Vec3d>& dir, bool& planar)
{
    const json&       ref  = params.at(name);
    const std::string what = std::string("could not resolve ref \"") + name + "\": ";
    planar = false;
    dir.reset();
    if (!ref.is_object() || int(ref.contains("face")) + int(ref.contains("edge")) + int(ref.contains("point")) != 1)
        throw std::runtime_error(what + "a ref is {\"face\": f}, {\"edge\": e} or {\"point\": [x, y, z]}, with an optional \"body\"");
    if (ref.contains("point")) {
        const json& p = ref.at("point");
        if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number())
            throw std::runtime_error(what + "point must be [x, y, z]");
        point = Vec3d(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
        return BRepBuilderAPI_MakeVertex(gp_Pnt(point.x(), point.y(), point.z())).Vertex();
    }
    const json bv = ref.contains("body") ? ref.at("body") : params.value("body", json(0));
    if (!bv.is_number_integer()) throw std::runtime_error(what + "body must be a body index");
    const int b = bv.get<int>();
    if (b < 0 || b >= int(doc.bodies.size()) || doc.bodies[b].shape.IsNull())
        throw std::runtime_error(what + "body " + std::to_string(b) + " is out of range (have " + std::to_string(doc.bodies.size()) + ")");
    const TopoDS_Shape& shape = doc.bodies[b].shape;
    const json          axis  = ref.value("axis", json(false));
    if (!axis.is_boolean()) throw std::runtime_error(what + "axis must be true or false");
    const std::string on_body = " of body " + std::to_string(b);
    if (ref.contains("face")) {
        if (!ref.at("face").is_number_integer()) throw std::runtime_error(what + "face must be a face id");
        const int         fi = ref.at("face").get<int>();
        const TopoDS_Face f  = GeometryEngine::face_by_index(shape, fi);
        if (f.IsNull())
            throw std::runtime_error(what + "face " + std::to_string(fi) + on_body + " does not exist (it has " +
                                     std::to_string(GeometryEngine::face_count(shape)) + " faces)");
        if (axis.get<bool>()) {
            // A cylinder's axis over the face's length: a hole's centre line, for distances to it.
            const GeometryEngine::CylinderFace c = GeometryEngine::cylinder_of_face(f);
            if (!c.ok)
                throw std::runtime_error(what + "face " + std::to_string(fi) + on_body + " is " + face_kind_name(f) +
                                         "; axis takes a cylindrical face");
            const Vec3d top = c.base + c.axis * c.height;
            point = 0.5 * (c.base + top);
            dir   = c.axis;
            return BRepBuilderAPI_MakeEdge(gp_Pnt(c.base.x(), c.base.y(), c.base.z()), gp_Pnt(top.x(), top.y(), top.z())).Edge();
        }
        point  = GeometryEngine::face_centroid_world(f);
        dir    = GeometryEngine::face_normal_world(f);
        planar = BRepAdaptor_Surface(f).GetType() == GeomAbs_Plane;
        return f;
    }
    if (axis.get<bool>()) throw std::runtime_error(what + "axis takes a cylindrical face");
    if (!ref.at("edge").is_number_integer()) throw std::runtime_error(what + "edge must be an edge id");
    const int         ei = ref.at("edge").get<int>();
    const TopoDS_Edge e  = GeometryEngine::edge_by_index(shape, ei);
    if (e.IsNull())
        throw std::runtime_error(what + "edge " + std::to_string(ei) + on_body + " does not exist (it has " +
                                 std::to_string(GeometryEngine::edge_count(shape)) + " edges)");
    const std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(e);
    if (pts.size() < 2) throw std::runtime_error(what + "edge " + std::to_string(ei) + on_body + " has no length");
    point = pts[pts.size() / 2];   // the middle sample
    dir   = (pts.back() - pts.front()).normalized();
    return e;
}

} // namespace

json cad_measure(const CadDocument& doc, const json& params)
{
    if (!params.is_object() || !params.contains("a") || !params.contains("b"))
        throw std::runtime_error("measure needs refs 'a' and 'b' ({face|edge|point})");
    Vec3d                pa, pb;
    std::optional<Vec3d> da, db;
    bool                 planar_a = false, planar_b = false;
    const TopoDS_Shape   sa = measure_ref(doc, params, "a", pa, da, planar_a);
    const TopoDS_Shape   sb = measure_ref(doc, params, "b", pb, db, planar_b);
    json r{{"distance", (pa - pb).norm()}, {"point_a", vec3(pa)}, {"point_b", vec3(pb)}};
    if (da && db) {
        const double c = std::max(-1.0, std::min(1.0, da->normalized().dot(db->normalized())));
        r["angle_deg"] = std::acos(c) * 180.0 / M_PI;
    }
    Vec3d        ca, cb;
    const double d = GeometryEngine::min_distance(sa, sb, ca, cb);
    if (d < 0) throw CadKernelFailure("the minimum distance between the refs could not be computed");
    r["min_distance"] = d;
    r["closest_a"]    = vec3(ca);
    r["closest_b"]    = vec3(cb);
    // The clearance between two parallel planar faces, as a lid fit wants it: across the gap, not
    // along it.
    if (planar_a && planar_b && da->cross(*db).norm() < 1e-6) r["parallel_gap"] = std::abs((pb - pa).dot(*da));
    return r;
}

void inspect_silent_forms(std::vector<SilentForm>& forms)
{
    forms.insert(forms.end(), {
        {"body_describe",                body_describe},
        {"measure",                      measure},
        {"body_mass_properties",         body_mass_properties},
        {"body_section",                 body_section},
        {"body_compare",                 body_compare},
        {"interference",                 action_check_interference},
    });
}

}}} // namespace Slic3r::GUI::Mcp
