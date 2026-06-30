#include "McpControl.hpp"

#ifndef _WIN32  // POSIX Unix-domain-socket transport only (slice 1)

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <future>
#include <chrono>
#include <memory>
#include <cmath>
#include <algorithm>

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <Standard_Failure.hxx>   // OCCT base error (not a std::exception)

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "DesignPanel.hpp"

#include "libslic3r/CadDocument.hpp"
#include "libslic3r/SketchEngine.hpp"
#include "libslic3r/GeometryEngine.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/BoundingBox.hpp"

#include <gp_Pln.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>

using json = nlohmann::json;

namespace Slic3r { namespace GUI {

namespace {

const char* feature_type_name(CadFeatureType t)
{
    switch (t) {
        case CadFeatureType::Sketch:  return "Sketch";
        case CadFeatureType::Extrude: return "Extrude";
        case CadFeatureType::Fillet:  return "Fillet";
        case CadFeatureType::Chamfer: return "Chamfer";
        case CadFeatureType::Hole:    return "Hole";
        case CadFeatureType::Thread:  return "Thread";
        case CadFeatureType::Shell:   return "Shell";
        case CadFeatureType::Revolve: return "Revolve";
        case CadFeatureType::Sweep:   return "Sweep";
        case CadFeatureType::Pattern: return "Pattern";
        case CadFeatureType::Plane:   return "Plane";
        case CadFeatureType::Loft:    return "Loft";
        case CadFeatureType::Draft:   return "Draft";
        case CadFeatureType::Import:  return "Import";
        case CadFeatureType::Boolean: return "Boolean";
        case CadFeatureType::Cut:     return "Cut";
    }
    return "Unknown";
}

// --- JSON-RPC envelope helpers -------------------------------------------------
std::string rpc_result(const json& id, const json& result)
{
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump();
}
std::string rpc_error(const json& id, int code, const std::string& msg)
{
    return json{{"jsonrpc", "2.0"}, {"id", id},
                {"error", {{"code", code}, {"message", msg}}}}.dump();
}

// --- the three slice-1 methods (run on the wx MAIN thread) ---------------------

json describe_tools()
{
    // Hand-written descriptor for slice 1. The bridge turns this into MCP tool
    // schemas; later slices grow this list (ideally from the kernel directly).
    return json{
        {"app", "SnapOrca CAD"},
        {"protocol", "jsonrpc-2.0"},
        {"slice", 1},
        {"tools", json::array({
            json{{"name", "describe_tools"}, {"summary", "List callable tools and their parameters."},
                 {"params", json::array()}},
            json{{"name", "describe_scene"}, {"summary", "Feature tree + per-body bounding boxes of the Design document."},
                 {"params", json::array()}},
            json{{"name", "extrude"}, {"summary", "Create a new solid: a centred rectangle sketch extruded to a depth."},
                 {"params", json::array({
                     json{{"name", "width"},    {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"min", 0.01}},
                     json{{"name", "height"},   {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"min", 0.01}},
                     json{{"name", "distance"}, {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "plane"},    {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
                 })}},
            json{{"name", "query_topology"}, {"summary", "Measured faces (centroid/normal/cylinder) and edges (length/circle) of a body."},
                 {"params", json::array({
                     json{{"name", "body"}, {"type", "integer"}, {"default", 0}},
                 })}},
            json{{"name", "measure"}, {"summary", "Distance (and angle, when both have direction) between two refs {face|edge|point} on a body."},
                 {"params", json::array({
                     json{{"name", "body"}, {"type", "integer"}, {"default", 0}},
                     json{{"name", "a"}, {"type", "object"}},
                     json{{"name", "b"}, {"type", "object"}},
                 })}},
            json{{"name", "slice_body"}, {"summary", "Cross-section of a body by a base plane at an offset (sections-as-evidence); returns world polylines."},
                 {"params", json::array({
                     json{{"name", "body"}, {"type", "integer"}, {"default", 0}},
                     json{{"name", "plane"}, {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
                     json{{"name", "offset"}, {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                 })}},
            json{{"name", "import_step"}, {"summary", "Import a STEP file as native B-rep bodies (the reference part to measure)."},
                 {"params", json::array({
                     json{{"name", "path"}, {"type", "string"}},
                 })}},
            json{{"name", "validate_against"}, {"summary", "Volume + bbox/centroid deviation of a body vs a reference {step:path|body:id} (the RE acceptance metric)."},
                 {"params", json::array({
                     json{{"name", "body"}, {"type", "integer"}, {"default", 0}},
                     json{{"name", "reference"}, {"type", "object"}},
                 })}},
        })},
    };
}

json describe_scene(DesignPanel* panel)
{
    CadDocument& doc = panel->mcp_doc();

    json features = json::array();
    for (size_t i = 0; i < doc.features.size(); ++i) {
        const CadFeature& f = doc.features[i];
        features.push_back(json{
            {"index", int(i)}, {"type", feature_type_name(f.type)},
            {"name", f.name}, {"enabled", f.enabled}});
    }

    json bodies = json::array();
    for (size_t i = 0; i < doc.bodies.size(); ++i) {
        json b{{"index", int(i)}, {"name", doc.bodies[i].name},
               {"has_color", doc.bodies[i].has_color}};
        // Per-body bbox/centre from the already-tessellated display meshes.
        if (i < doc.display_body_meshes.size() && !doc.display_body_meshes[i].empty()) {
            BoundingBoxf3 bb = doc.display_body_meshes[i].bounding_box();
            b["bbox"] = json{{"min", {bb.min.x(), bb.min.y(), bb.min.z()}},
                             {"max", {bb.max.x(), bb.max.y(), bb.max.z()}}};
            Vec3d c = bb.center();
            b["center"] = {c.x(), c.y(), c.z()};
        }
        bodies.push_back(std::move(b));
    }

    return json{
        {"modeling_origin", {doc.modeling_origin.x(), doc.modeling_origin.y(), doc.modeling_origin.z()}},
        {"features", std::move(features)},
        {"bodies", std::move(bodies)},
        {"error", doc.error},
    };
}

// --- Measure layer (read-only "evidence" half of the RE loop) ------------------
inline json vec3(const Vec3d& v) { return json::array({v.x(), v.y(), v.z()}); }

// Resolve body index -> shape, throwing a clear error if out of range / null.
const TopoDS_Shape& body_shape(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    int idx = params.value("body", 0);
    if (idx < 0 || idx >= int(doc.bodies.size()))
        throw std::runtime_error("body index out of range (have " + std::to_string(doc.bodies.size()) + ")");
    if (doc.bodies[idx].shape.IsNull())
        throw std::runtime_error("body has no shape");
    return doc.bodies[idx].shape;
}

json query_topology(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& shape = body_shape(panel, params);
    json faces = json::array();
    int nf = GeometryEngine::face_count(shape);
    for (int i = 0; i < nf; ++i) {
        TopoDS_Face f = GeometryEngine::face_by_index(shape, i);
        if (f.IsNull()) continue;
        json jf{{"id", i}, {"centroid", vec3(GeometryEngine::face_centroid_world(f))},
                {"normal", vec3(GeometryEngine::face_normal_world(f))}, {"kind", "planar"}};
        GeometryEngine::CylinderFace cyl = GeometryEngine::cylinder_of_face(f);
        if (cyl.ok) { jf["kind"] = "cylindrical"; jf["radius"] = cyl.radius;
                      jf["axis"] = vec3(cyl.axis); jf["internal"] = cyl.internal; }
        faces.push_back(std::move(jf));
    }
    json edges = json::array();
    int ne = GeometryEngine::edge_count(shape);
    for (int i = 0; i < ne; ++i) {
        TopoDS_Edge e = GeometryEngine::edge_by_index(shape, i);
        if (e.IsNull()) continue;
        std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(e);
        if (pts.size() < 2) continue;
        double len = 0; for (size_t k = 1; k < pts.size(); ++k) len += (pts[k] - pts[k-1]).norm();
        json je{{"id", i}, {"length", len}, {"p0", vec3(pts.front())}, {"p1", vec3(pts.back())},
                {"kind", "line"}};
        GeometryEngine::CylinderFace circ = GeometryEngine::circle_of_edge(e);
        if (circ.ok) { je["kind"] = "circle"; je["radius"] = circ.radius; je["center"] = vec3(circ.base); }
        edges.push_back(std::move(je));
    }
    return json{{"body", params.value("body", 0)}, {"face_count", nf}, {"edge_count", ne},
                {"faces", std::move(faces)}, {"edges", std::move(edges)}};
}

// One measurement reference -> a representative point and (optionally) a direction.
// ref = {"face": id} | {"edge": id} | {"point": [x,y,z]} on the given body.
bool resolve_ref(const TopoDS_Shape& shape, const json& ref, Vec3d& point, Vec3d& dir, bool& has_dir)
{
    has_dir = false;
    if (ref.contains("point")) { auto p = ref["point"]; point = Vec3d(p[0], p[1], p[2]); return true; }
    if (ref.contains("face")) {
        TopoDS_Face f = GeometryEngine::face_by_index(shape, ref["face"].get<int>());
        if (f.IsNull()) return false;
        point = GeometryEngine::face_centroid_world(f);
        dir = GeometryEngine::face_normal_world(f); has_dir = true; return true;
    }
    if (ref.contains("edge")) {
        TopoDS_Edge e = GeometryEngine::edge_by_index(shape, ref["edge"].get<int>());
        if (e.IsNull()) return false;
        std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(e);
        if (pts.empty()) return false;
        point = pts[pts.size() / 2];                       // midpoint sample
        if (pts.size() >= 2) { dir = (pts.back() - pts.front()).normalized(); has_dir = true; }
        return true;
    }
    return false;
}

json measure(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& shape = body_shape(panel, params);
    Vec3d pa, pb, da, db; bool hda = false, hdb = false;
    if (!params.contains("a") || !params.contains("b"))
        throw std::runtime_error("measure needs refs 'a' and 'b' ({face|edge|point})");
    if (!resolve_ref(shape, params["a"], pa, da, hda) || !resolve_ref(shape, params["b"], pb, db, hdb))
        throw std::runtime_error("could not resolve a measurement reference");
    json r{{"distance", (pa - pb).norm()}, {"point_a", vec3(pa)}, {"point_b", vec3(pb)}};
    if (hda && hdb) {
        double c = std::max(-1.0, std::min(1.0, da.normalized().dot(db.normalized())));
        r["angle_deg"] = std::acos(c) * 180.0 / M_PI;
    }
    return r;
}

// sections-as-evidence: cross-section of a body by a named base plane at an offset.
json slice_body(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& shape = body_shape(panel, params);
    const std::string plane_name = params.value("plane", std::string("XY"));
    const double offset = params.value("offset", 0.0);
    // Base plane normal; offset shifts the plane along it.
    gp_Dir n = plane_name == "XZ" ? gp_Dir(0, 1, 0)
             : plane_name == "YZ" ? gp_Dir(1, 0, 0)
                                  : gp_Dir(0, 0, 1);
    gp_Pnt o(n.X() * offset, n.Y() * offset, n.Z() * offset);
    BRepAlgoAPI_Section sect(shape, gp_Pln(o, n), Standard_False);
    sect.ComputePCurveOn1(Standard_False);
    sect.Approximation(Standard_True);
    sect.Build();
    if (!sect.IsDone()) throw std::runtime_error("section failed");
    // ponytail: return raw section segments (one polyline per edge), unordered. Chain into
    // loops later if a downstream step needs ordered contours.
    json polylines = json::array();
    for (TopExp_Explorer ex(sect.Shape(), TopAbs_EDGE); ex.More(); ex.Next()) {
        std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(TopoDS::Edge(ex.Current()));
        if (pts.size() < 2) continue;
        json pl = json::array();
        for (const Vec3d& p : pts) pl.push_back(vec3(p));
        polylines.push_back(std::move(pl));
    }
    return json{{"body", params.value("body", 0)}, {"plane", plane_name}, {"offset", offset},
                {"segment_count", int(polylines.size())}, {"polylines", std::move(polylines)}};
}

// --- Build: bring a reference part in (Import STEP as native B-rep bodies) ------
json import_step(DesignPanel* panel, const json& params)
{
    if (!params.contains("path")) throw std::runtime_error("import_step needs 'path'");
    const std::string path = params["path"].get<std::string>();
    std::string err;
    std::vector<TopoDS_Shape> solids = GeometryEngine::read_step_solids(path, err);
    if (solids.empty()) throw std::runtime_error(err.empty() ? "no solids in STEP" : err);

    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    int first = int(doc.features.size());
    for (const TopoDS_Shape& s : solids) {
        CadFeature f;
        f.type           = CadFeatureType::Import;
        f.name           = "STEP" + std::to_string(int(doc.features.size()) + 1);
        f.imported_solid = s;
        f.mode           = BooleanMode::New;   // each solid = its own coexisting body
        doc.features.push_back(f);
    }
    bool ok = doc.recompute();
    if (!ok) doc.undo();
    panel->mcp_after_change();
    return json{{"ok", ok}, {"imported", int(solids.size())}, {"first_feature", first},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// --- Validate: volume + bbox deviation of a body vs a reference (the "scarto %") --
// ponytail: volume delta + bbox/centroid offset (the RE skill's actual acceptance metric).
// Surface-deviation heat-map is the upgrade path (per-vertex BRepExtrema), add when needed.
struct ShapeMetrics { double volume; Vec3d centroid, bmin, bmax; };
ShapeMetrics shape_metrics(const TopoDS_Shape& s)
{
    GProp_GProps vp; BRepGProp::VolumeProperties(s, vp);
    gp_Pnt c = vp.CentreOfMass();
    Bnd_Box bb; BRepBndLib::Add(s, bb);
    Standard_Real x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
    return {vp.Mass(), Vec3d(c.X(), c.Y(), c.Z()), Vec3d(x0, y0, z0), Vec3d(x1, y1, z1)};
}

json validate_against(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& cand = body_shape(panel, params);   // candidate = the reconstruction
    if (!params.contains("reference")) throw std::runtime_error("validate_against needs 'reference' {step|body}");
    const json& r = params["reference"];

    TopoDS_Shape ref;
    if (r.contains("step")) {
        std::string err;
        std::vector<TopoDS_Shape> solids = GeometryEngine::read_step_solids(r["step"].get<std::string>(), err);
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
    return json{
        {"volume", a.volume}, {"volume_reference", b.volume}, {"volume_delta_pct", dv},
        {"centroid_offset", vec3(coff)}, {"centroid_offset_mm", coff.norm()},
        {"bbox", json{{"min", vec3(a.bmin)}, {"max", vec3(a.bmax)}}},
        {"bbox_reference", json{{"min", vec3(b.bmin)}, {"max", vec3(b.bmax)}}},
        {"bbox_delta", json{{"min", vec3(dmin)}, {"max", vec3(dmax)}}},
    };
}

json action_extrude(DesignPanel* panel, const json& params)
{
    const double w = params.value("width", 20.0);
    const double h = params.value("height", 20.0);
    const double d = params.value("distance", 10.0);
    const std::string plane_name = params.value("plane", std::string("XY"));
    if (w <= 0 || h <= 0 || d <= 0)
        throw std::runtime_error("width, height and distance must be > 0");

    CadDocument& doc = panel->mcp_doc();
    SketchPlane pl = plane_name == "XZ" ? SketchPlane::XZ()
                   : plane_name == "YZ" ? SketchPlane::YZ()
                                        : SketchPlane::XY();
    pl.origin = doc.modeling_origin;   // land on the bed centre, like the GUI

    doc.checkpoint();                  // one undo step for this action
    int s = doc.add_sketch(SketchShape::Rectangle, pl, w, h, 0.0, "Sketch");
    int e = doc.add_extrude(s, d, /*symmetric*/false, BooleanMode::New, "Extrude");
    bool ok = doc.recompute();
    if (!ok) doc.undo();               // never leave a broken feature tree
    panel->mcp_after_change();         // refresh tree + viewport + status

    return json{{"ok", ok}, {"sketch_index", s}, {"extrude_index", e},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// Dispatch one parsed request ON THE MAIN THREAD. Returns a JSON-RPC reply string.
std::string handle_on_main(const std::string& method, const json& params, const json& id)
{
    MainFrame* mf = wxGetApp().mainframe;
    if (!mf || !mf->m_design_panel)
        return rpc_error(id, -32001, "Design panel not ready");
    DesignPanel* panel = mf->m_design_panel;
    try {
        if (method == "describe_tools") return rpc_result(id, describe_tools());
        if (method == "describe_scene") return rpc_result(id, describe_scene(panel));
        if (method == "query_topology") return rpc_result(id, query_topology(panel, params));
        if (method == "measure")        return rpc_result(id, measure(panel, params));
        if (method == "slice_body")     return rpc_result(id, slice_body(panel, params));
        if (method == "import_step")    return rpc_result(id, import_step(panel, params));
        if (method == "validate_against") return rpc_result(id, validate_against(panel, params));
        if (method == "extrude")        return rpc_result(id, action_extrude(panel, params));
        return rpc_error(id, -32601, "Unknown method: " + method);
    } catch (const Standard_Failure& ex) {   // OCCT errors are NOT std::exception
        return rpc_error(id, -32000, std::string("OCCT: ") + (ex.GetMessageString() ? ex.GetMessageString() : "failure"));
    } catch (const std::exception& ex) {
        return rpc_error(id, -32000, ex.what());
    }
}

// Marshal a request to the main thread and block (with a timeout) for the reply.
std::string dispatch_request(const std::string& line)
{
    json req;
    try { req = json::parse(line); }
    catch (const std::exception& ex) { return rpc_error(nullptr, -32700, std::string("parse error: ") + ex.what()); }

    json id           = req.contains("id") ? req["id"] : json(nullptr);
    std::string method = req.value("method", std::string());
    json params        = req.contains("params") ? req["params"] : json::object();
    if (method.empty()) return rpc_error(id, -32600, "missing method");

    auto prom = std::make_shared<std::promise<std::string>>();
    auto fut  = prom->get_future();
    wxGetApp().CallAfter([prom, method, params, id]() {
        prom->set_value(handle_on_main(method, params, id));
    });
    if (fut.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
        return rpc_error(id, -32000, "main-thread timeout");
    return fut.get();
}

// Read newline-delimited requests off one client connection until EOF.
void serve_client(int cfd)
{
    std::string buf;
    char chunk[4096];
    for (;;) {
        ssize_t n = ::read(cfd, chunk, sizeof(chunk));
        if (n <= 0) break;
        buf.append(chunk, size_t(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            std::string reply = dispatch_request(line);
            reply.push_back('\n');
            if (::write(cfd, reply.data(), reply.size()) < 0) return;
        }
    }
}

void server_thread(std::string sock_path)
{
    ::unlink(sock_path.c_str());
    int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { BOOST_LOG_TRIVIAL(error) << "MCP: socket() failed"; return; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(sfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        BOOST_LOG_TRIVIAL(error) << "MCP: bind() failed on " << sock_path;
        ::close(sfd); return;
    }
    if (::listen(sfd, 1) < 0) { BOOST_LOG_TRIVIAL(error) << "MCP: listen() failed"; ::close(sfd); return; }
    BOOST_LOG_TRIVIAL(info) << "MCP control listening on " << sock_path;

    for (;;) {
        int cfd = ::accept(sfd, nullptr, nullptr);
        if (cfd < 0) continue;
        serve_client(cfd);
        ::close(cfd);
    }
}

} // namespace

void start_mcp_control_if_enabled()
{
    const char* env = std::getenv("SNAPORCA_MCP");
    if (!env || !*env) return;
    std::string path = (std::strcmp(env, "1") == 0) ? "/tmp/snaporca-mcp.sock" : env;
    static bool started = false;
    if (started) return;
    started = true;
    std::thread(server_thread, path).detach();
}

}} // namespace Slic3r::GUI

#else  // _WIN32

namespace Slic3r { namespace GUI {
void start_mcp_control_if_enabled() {}   // ponytail: no Windows transport yet
}}

#endif
