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

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <Standard_Failure.hxx>   // OCCT base error (not a std::exception)

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "DesignPanel.hpp"

#include "libslic3r/CadDocument.hpp"
#include "libslic3r/SketchEngine.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/BoundingBox.hpp"

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
