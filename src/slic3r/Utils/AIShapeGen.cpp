#include "AIShapeGen.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r {

using json = nlohmann::json;

namespace {

constexpr int    MAX_DEPTH = 64;    // guard against pathological nesting
constexpr double FA        = 2.0 * PI / 180.0;   // facet angle: ~1 deg -> smooth

double getnum(const json &n, const char *key, double def)
{
    return (n.contains(key) && n[key].is_number()) ? n[key].get<double>() : def;
}

// Reads [x,y,z]; a bare number is broadcast to all three (handy for scale).
Vec3d getvec3(const json &n, const char *key, const Vec3d &def)
{
    if (! n.contains(key))
        return def;
    const json &v = n[key];
    if (v.is_array() && v.size() == 3 && v[0].is_number())
        return Vec3d(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
    if (v.is_number()) {
        double d = v.get<double>();
        return Vec3d(d, d, d);
    }
    return def;
}

// radius, honoring an alternative "diameter".
double get_radius(const json &n, double def_radius)
{
    if (n.contains("radius"))
        return getnum(n, "radius", def_radius);
    if (n.contains("diameter"))
        return 0.5 * getnum(n, "diameter", 2.0 * def_radius);
    return def_radius;
}

// A = A (op) B. Tries CGAL first, falls back to mcut. Returns false on failure.
bool csg_combine(TriangleMesh &acc, const TriangleMesh &b, char op, std::string &error)
{
    try {
        switch (op) {
            case 'u': MeshBoolean::cgal::plus(acc, b);      break;
            case 'd': MeshBoolean::cgal::minus(acc, b);     break;
            default:  MeshBoolean::cgal::intersect(acc, b); break;
        }
        return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(info) << "AIShapeGen: CGAL boolean failed (" << e.what() << "), trying mcut";
    } catch (...) {
        BOOST_LOG_TRIVIAL(info) << "AIShapeGen: CGAL boolean crashed, trying mcut";
    }
    try {
        const std::string opts = op == 'u' ? "UNION" : op == 'd' ? "A_NOT_B" : "INTERSECTION";
        std::vector<TriangleMesh> out;
        MeshBoolean::mcut::make_boolean(acc, b, out, opts);
        if (out.empty()) { error = "boolean operation produced an empty result"; return false; }
        acc = out.front();   // extra disjoint volumes (rare) are dropped; logged below
        if (out.size() > 1)
            BOOST_LOG_TRIVIAL(info) << "AIShapeGen: boolean produced " << out.size() << " volumes; keeping the first";
        return true;
    } catch (const std::exception &e) {
        error = std::string("boolean operation failed: ") + e.what();
        return false;
    } catch (...) {
        error = "boolean operation failed";
        return false;
    }
}

bool build_mesh(const json &node, TriangleMesh &out, std::string &error, int depth)
{
    if (depth > MAX_DEPTH)     { error = "shape spec nested too deeply";     return false; }
    if (! node.is_object())    { error = "each shape node must be an object"; return false; }

    // Accept common key variations models emit for the node kind.
    std::string type = node.value("type", std::string());
    if (type.empty()) type = node.value("operation", std::string());
    if (type.empty()) type = node.value("op", std::string());
    if (type.empty()) type = node.value("shape", std::string());
    if (type.empty()) { error = "a shape node is missing its \"type\""; return false; }

    if (type == "box" || type == "cube") {
        Vec3d s = getvec3(node, "size", Vec3d(20, 20, 20));
        if (s.x() <= 0 || s.y() <= 0 || s.z() <= 0) { error = "box \"size\" must be positive"; return false; }
        out = TriangleMesh(its_make_cube(s.x(), s.y(), s.z()));
        out.transform(Geometry::translation_transform(Vec3d(-0.5 * s.x(), -0.5 * s.y(), -0.5 * s.z()))); // center
    } else if (type == "cylinder") {
        double r = get_radius(node, 10.0), h = getnum(node, "height", 20.0);
        if (r <= 0 || h <= 0) { error = "cylinder needs positive radius/diameter and height"; return false; }
        out = TriangleMesh(its_make_cylinder(r, h, FA));
        out.transform(Geometry::translation_transform(Vec3d(0, 0, -0.5 * h))); // center on Z
    } else if (type == "cone") {
        double r = get_radius(node, 10.0), h = getnum(node, "height", 20.0);
        if (r <= 0 || h <= 0) { error = "cone needs positive radius/diameter and height"; return false; }
        out = TriangleMesh(its_make_cone(r, h, FA));
        out.transform(Geometry::translation_transform(Vec3d(0, 0, -0.5 * h)));
    } else if (type == "sphere") {
        double r = get_radius(node, 10.0);
        if (r <= 0) { error = "sphere needs a positive radius/diameter"; return false; }
        out = TriangleMesh(its_make_sphere(r, FA));
    } else if (type == "union" || type == "difference" || type == "intersection") {
        if (! node.contains("children") || ! node["children"].is_array() || node["children"].empty()) {
            error = type + " requires a non-empty \"children\" array";
            return false;
        }
        const json &ch = node["children"];
        if (! build_mesh(ch[0], out, error, depth + 1))
            return false;
        const char op = type == "union" ? 'u' : type == "difference" ? 'd' : 'i';
        for (size_t i = 1; i < ch.size(); ++i) {
            TriangleMesh b;
            if (! build_mesh(ch[i], b, error, depth + 1))
                return false;
            if (! csg_combine(out, b, op, error))
                return false;
        }
    } else {
        error = "unknown shape type \"" + type + "\"";
        return false;
    }

    // Per-node affine: scale, then rotate (degrees), then translate.
    const Vec3d t     = getvec3(node, "translate", Vec3d::Zero());
    const Vec3d r_deg = getvec3(node, "rotate",    Vec3d::Zero());
    const Vec3d scale = getvec3(node, "scale",     Vec3d(1, 1, 1));
    if (scale.x() == 0 || scale.y() == 0 || scale.z() == 0) { error = "scale factors must be non-zero"; return false; }
    const Vec3d r_rad = r_deg * (PI / 180.0);
    out.transform(Geometry::assemble_transform(t, r_rad, scale), /*fix_left_handed=*/true);
    return true;
}

// Remove <think>...</think> reasoning blocks. Reasoning models (Nemotron,
// DeepSeek-R1, QwQ, ...) emit their chain-of-thought in these tags and put the
// real answer AFTER </think>; the block itself often contains draft JSON + prose
// that would otherwise corrupt our extraction.
std::string strip_think_blocks(const std::string &in)
{
    std::string s = in;
    for (const char *open : {"<think>", "<thinking>", "<reasoning>"}) {
        const std::string close = std::string("</") + (open + 1);   // "</think>" etc.
        std::size_t pos;
        while ((pos = s.find(open)) != std::string::npos) {
            std::size_t end = s.find(close, pos);
            if (end == std::string::npos) { s.erase(pos); break; }   // unterminated: drop the tail
            s.erase(pos, (end + close.size()) - pos);
        }
    }
    return s;
}

// Strip a leading/trailing ``` or ```json/```stl code fence, if present.
std::string strip_code_fence(const std::string &in)
{
    std::string s = in;
    auto not_space = [](unsigned char c) { return ! std::isspace(c); };
    // trim
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    if (s.rfind("```", 0) == 0) {
        auto nl = s.find('\n');
        if (nl != std::string::npos) s = s.substr(nl + 1);         // drop the ```lang line
        auto close = s.rfind("```");
        if (close != std::string::npos) s = s.substr(0, close);
    }
    return s;
}

Model *finalize_object(Model &model, TriangleMesh &&mesh, const std::string &name, std::string &error)
{
    if (mesh.facets_count() == 0) { error = "generated mesh has no facets"; return nullptr; }
    ModelObject *obj = model.add_object();
    obj->name = name;
    obj->add_instance();
    ModelVolume *vol = obj->add_volume(std::move(mesh));
    vol->name = name;
    obj->ensure_on_bed();
    return &model;
}

} // namespace

std::string ai_shape_spec_instructions()
{
    return
        "Output ONLY one JSON object for the 3D shape and nothing else. Units are millimeters.\n"
        "RULES:\n"
        "- EVERY node uses the key \"type\" (never \"operation\" or \"shape\").\n"
        "- Every shape is ALREADY centered at the origin. Add \"translate\" ONLY to move a shape "
        "off-center; a hole through the center needs NO translate.\n"
        "- Primitives:\n"
        "    {\"type\":\"box\",\"size\":[x,y,z]}\n"
        "    {\"type\":\"cylinder\",\"radius\":r,\"height\":h}   (or \"diameter\":d)\n"
        "    {\"type\":\"cone\",\"radius\":r,\"height\":h}\n"
        "    {\"type\":\"sphere\",\"radius\":r}                  (or \"diameter\":d)\n"
        "- Combine with CSG:\n"
        "    {\"type\":\"union|difference|intersection\",\"children\":[ ...nodes... ]}\n"
        "    (\"difference\" subtracts every later child from the first)\n"
        "- Optional per-node transform: \"translate\":[x,y,z], \"rotate\":[deg,deg,deg], \"scale\":[sx,sy,sz]\n"
        "- Cylinders/cones stand along Z. To bore a hole along Z through a centered box, a same-height "
        "centered cylinder is correct with NO translate.\n"
        "- Only geometric primitives + booleans are possible; you cannot model organic or freeform "
        "objects (animals, faces, characters).\n"
        "Example — a 30mm cube with a 10mm hole through its center:\n"
        "  {\"type\":\"difference\",\"children\":["
        "{\"type\":\"box\",\"size\":[30,30,30]},"
        "{\"type\":\"cylinder\",\"radius\":5,\"height\":30}]}";
}

bool ai_build_model_from_spec(const json &spec, const std::string &name, Model &out_model, std::string &error)
{
    TriangleMesh mesh;
    try {
        if (! build_mesh(spec, mesh, error, 0))
            return false;
    } catch (const std::exception &e) {
        error = std::string("shape build failed: ") + e.what();
        return false;
    }

    // Make the result watertight/manifold: CSG booleans can leave non-manifold
    // edges (coincident faces, flush cuts) that make the mesh un-printable. First
    // a cheap cleanup, then a CGAL self-union — which resolves self-intersections
    // and is a no-op on already-clean meshes. Best-effort: keep the cleaned mesh
    // if self-union fails.
    its_merge_vertices(mesh.its);
    its_remove_degenerate_faces(mesh.its);
    mesh = TriangleMesh(mesh.its);
    try {
        TriangleMesh repaired = mesh;
        MeshBoolean::self_union(repaired);
        if (repaired.facets_count() > 0)
            mesh = std::move(repaired);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(info) << "AIShapeGen: manifold repair (self-union) failed: " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(info) << "AIShapeGen: manifold repair (self-union) crashed";
    }

    return finalize_object(out_model, std::move(mesh), name.empty() ? "AI shape" : name, error) != nullptr;
}

bool ai_build_model_from_stl(const std::string &stl_bytes, const std::string &name, Model &out_model, std::string &error)
{
    if (stl_bytes.empty()) { error = "empty STL data"; return false; }
    try {
        boost::filesystem::path path =
            boost::filesystem::path(temporary_dir()) / boost::filesystem::unique_path("ai_shape_%%%%%%.stl");
        {
            std::ofstream f(path.string(), std::ios::binary);
            if (! f) { error = "could not open a temp file for the STL"; return false; }
            f.write(stl_bytes.data(), static_cast<std::streamsize>(stl_bytes.size()));
        }
        TriangleMesh mesh;
        const bool ok = mesh.ReadSTLFile(path.string().c_str(), true, nullptr);
        boost::filesystem::remove(path);
        if (! ok) { error = "could not parse the STL data"; return false; }
        return finalize_object(out_model, std::move(mesh), name.empty() ? "AI shape" : name, error) != nullptr;
    } catch (const std::exception &e) {
        error = std::string("STL load failed: ") + e.what();
        return false;
    }
}

// One-line snippet of arbitrary text for user-facing error messages / logs.
static std::string snippet_of(const std::string &s, std::size_t n = 240)
{
    std::string out = s.substr(0, n);
    for (char &c : out) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (s.size() > n) out += "...";
    return out;
}

bool ai_build_model_from_response(const std::string &response, const std::string &name, Model &out_model, std::string &error)
{
    // Always log the raw response so failures are diagnosable from the log file.
    BOOST_LOG_TRIVIAL(info) << "AIShapeGen: raw AI response (" << response.size()
                            << " bytes): " << snippet_of(response, 4000);

    const std::string s = strip_code_fence(strip_think_blocks(response));
    if (s.empty()) { error = "The AI returned an empty response."; return false; }

    // Locate a JSON object anywhere in the reply — models often wrap the spec in
    // prose or a code fence, so don't require it to be the very first character.
    std::size_t lb = s.find('{');
    std::size_t rb = s.rfind('}');
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
        const std::string candidate = s.substr(lb, rb - lb + 1);
        try {
            json spec = json::parse(candidate);
            std::string spec_err;
            if (ai_build_model_from_spec(spec, name, out_model, spec_err))
                return true;
            // Valid JSON, but not a usable shape spec — report the specific reason.
            BOOST_LOG_TRIVIAL(warning) << "AIShapeGen: JSON is not a valid shape spec: " << spec_err
                                       << " | json: " << snippet_of(candidate, 1500);
            error = "The AI returned JSON, but it isn't a valid shape spec: " + spec_err;
            return false;
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(info) << "AIShapeGen: JSON parse failed (" << e.what()
                                    << ") on: " << snippet_of(candidate, 1500);
        }
    }

    // Raw STL fallback (ASCII STL contains both "solid" and "facet").
    if (s.find("solid") != std::string::npos && s.find("facet") != std::string::npos)
        return ai_build_model_from_stl(s, name, out_model, error);

    // Nothing usable — surface what the model actually said, plus a scope hint.
    error = "The AI didn't return a usable shape. It replied: \"" + snippet_of(s) + "\"  "
            "This generator builds parametric shapes (boxes, cylinders, spheres, and boolean cuts), "
            "so mechanical parts work best — organic shapes (animals, faces, etc.) aren't supported.";
    return false;
}

bool ai_build_model_from_tool_call(const std::string &args_json, const std::string &name, Model &out_model, std::string &error)
{
    BOOST_LOG_TRIVIAL(info) << "AIShapeGen: create_model tool args (" << args_json.size()
                            << " bytes): " << snippet_of(args_json, 2000);
    try {
        json args = json::parse(args_json);
        json spec;
        if (args.is_object() && args.contains("shape") && args["shape"].is_object())
            spec = args["shape"];
        else if (args.is_object() && (args.contains("type") || args.contains("operation") ||
                                      args.contains("op") || args.contains("children")))
            spec = args;                       // the model put the node at the top level
        else { error = "The create_model call didn't contain a shape."; return false; }
        return ai_build_model_from_spec(spec, name, out_model, error);
    } catch (const std::exception &e) {
        error = std::string("Could not parse the create_model arguments: ") + e.what();
        return false;
    }
}

bool ai_model_fits_bed(const Model &model, double bed_x, double bed_y, double bed_z, std::string &warning)
{
    BoundingBoxf3 bb;
    bool any = false;
    for (const ModelObject *o : model.objects)
        for (const ModelVolume *v : o->volumes) {
            BoundingBoxf3 vb = v->mesh().bounding_box();
            if (! any) { bb = vb; any = true; } else bb.merge(vb);
        }
    if (! any)
        return true;

    const Vec3d sz = bb.size();
    if (sz.x() > bed_x || sz.y() > bed_y || sz.z() > bed_z) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "Generated object is %.1f x %.1f x %.1f mm, which exceeds the build volume of %.0f x %.0f x %.0f mm.",
                      sz.x(), sz.y(), sz.z(), bed_x, bed_y, bed_z);
        warning = buf;
        return false;
    }
    return true;
}

} // namespace Slic3r
