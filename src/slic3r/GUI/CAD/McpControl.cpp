#include "slic3r/GUI/CAD/McpControl.hpp"
#include "libslic3r/Point.hpp"
#include <stdexcept>
#include <math.h>
#include <TopAbs_ShapeEnum.hxx>
#include <Standard_TypeDef.hxx>
#include <exception>
#include <cstdint>

#ifndef _WIN32  // the Unix-domain-socket transport is POSIX only
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>   // umask/chmod: the socket's file mode IS its access control
#include <unistd.h>
#endif
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
#include <utility>
#include <atomic>
#include <cctype>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <tuple>

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <wx/string.h>
#include <Standard_Failure.hxx>   // OCCT base error

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"   // itObject
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Mcp/McpPaths.hpp"
#include "slic3r/GUI/Mcp/UiInspector.hpp"   // gizmo_name: the name get_state reports
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"
#include "slic3r/GUI/CAD/DesignOffer.hpp"   // offer table — served whole by list_verbs, fired by run_verb

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/CAD/ThreadStandards.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
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
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_CompCurve.hxx>
#include <GeomAbs_CurveType.hxx>
#include <BRep_Tool.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <gp_Dir.hxx>
#include <gp_Elips.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <TopoDS_Wire.hxx>

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
        case CadFeatureType::Mirror:  return "Mirror";
        case CadFeatureType::Axis:    return "Axis";
        case CadFeatureType::CoordSys: return "CoordSys";
        case CadFeatureType::Helix:    return "Helix";
        case CadFeatureType::Transform: return "Transform";
        case CadFeatureType::Thicken:  return "Thicken";
        case CadFeatureType::Project:  return "Project";
        case CadFeatureType::DeleteFace: return "DeleteFace";
        case CadFeatureType::Rib:        return "Rib";
        case CadFeatureType::SurfaceExtrude: return "SurfaceExtrude";
        case CadFeatureType::SurfaceRevolve: return "SurfaceRevolve";
        case CadFeatureType::ThickenSurface: return "ThickenSurface";
        case CadFeatureType::SurfaceOffset:   return "SurfaceOffset";
        case CadFeatureType::SurfaceLoft:     return "SurfaceLoft";
        case CadFeatureType::SurfaceFill:     return "SurfaceFill";
        case CadFeatureType::Mate:      return "Mate";
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

// --- describe_tools: the parameter schema of record ------------------------------

// The `plane` param. A face resolves differently for drilling than for anything else (see
// cad_resolve_plane), so the row says which frame the method gets.
json plane_param(const json& def, bool drill)
{
    return json{{"name", "plane"}, {"type", "string|object"}, {"default", def},
                {"description", drill
                     ? "\"XY\", \"XZ\" or \"YZ\" through modeling_origin; {\"datum\": i}, a Plane feature; or "
                       "{\"body\": b, \"face\": f}, a planar face to drill into that body (origin at its "
                       "centroid, normal into the body, x along its longest edge). The reply's frame gives the axes."
                     : "\"XY\", \"XZ\" or \"YZ\" through modeling_origin; {\"datum\": i}, a Plane feature; or "
                       "{\"body\": b, \"face\": f}, a planar face (origin at its centroid, normal out of the "
                       "body, x from the world axes). The reply's frame gives the axes."}};
}

// One row per dispatched method (kCadMethods); each row's `access` is added from that table.
// The bridge turns these rows into MCP tool schemas, and treats a param without a default as
// required.
json tool_rows()
{
    return json{
        {"app", "Orca CAD"},
        {"protocol", "jsonrpc-2.0"},
        {"slice", 5},
        // Read this before using any face or edge id.
        {"id_lifetime",
         "Global face and edge ids are indices into the CURRENT topology and expire the moment "
         "a feature rebuilds the model. Reading the scene once and then issuing several "
         "operations addresses the wrong edge on every call after the first, and does NOT "
         "error, because a stale id still names a real edge. Either re-read query_topology "
         "before each id-taking call, or pass the 'generation' you were given back with the "
         "call and have it refused if the model has moved on."},
        {"tools", json::array({
            json{{"name", "describe_tools"}, {"summary", "List callable methods and their parameters. Filter with method (a name or a list of names) and query (text in a name or summary)."},
                 {"params", json::array({
                     json{{"name", "method"}, {"type", "string|array"}, {"default", ""}, {"description", "only these methods"}},
                     json{{"name", "query"},  {"type", "string"}, {"default", ""}, {"description", "only methods whose name or summary contains this, in any case"}},
                 })}},
            json{{"name", "describe_scene"}, {"summary", "The Design document: features (bound: fields an expression drives), variables with their values, bodies (bbox, source_feature, visible, color, moved by the Move gizmo), undo_depth / redo_depth and generation."},
                 {"params", json::array()}},
            json{{"name", "describe_feature"}, {"summary", "One feature as its card shows it: params, the editable ones with their ranges, expr (bindings), refs (feature and body indices, face and edge ids), a sketch's plane, entities and constraints (angles in degrees), used_by and bodies_made."},
                 {"params", json::array({
                     json{{"name", "feature"}, {"type", "integer"}, {"description", "feature index (describe_scene)"}},
                 })}},
            json{{"name", "set_feature"}, {"summary", "Change a feature's editable fields as its card's Confirm does when editing: one undo step and a rebuild. A field an expression drives is refused. Reply: changed {field: [old, new]}."},
                 {"params", json::array({
                     json{{"name", "feature"}, {"type", "integer"}, {"description", "feature index (describe_scene)"}},
                     json{{"name", "fields"},  {"type", "object"}, {"description", "{field: value} from describe_feature's editable, e.g. {\"distance\": 15}: numbers within its ranges; mode new|union|subtract|intersect; extrude_end blind|symmetric|two_sided|through_all; shape rectangle|circle; xf_* vectors [x, y, z]"}},
                 })}},
            json{{"name", "extrude"}, {"summary", "Extrude a committed sketch, or one loop of it, as the Extrude card does; push/pull a planar face of a body; or a profile drawn here: an explicit closed `profile` (list of [x,y]) or a centred width x height rectangle on `plane`. End conditions match Onshape. Reply: extrude_index, the index of its sketch (-1 for a face), the boolean and body used, end, frame."},
                 {"params", json::array({
                     json{{"name", "sketch"},   {"type", "integer"}, {"default", nullptr}, {"description", "a committed Sketch feature to extrude (sketch_commit's sketch_index); profile, width, height and plane are then refused"}},
                     json{{"name", "loop"},     {"type", "integer"}, {"default", nullptr}, {"description", "with sketch: one closed loop (closed_loops[].index of sketch_describe {sketch}) with the loops inside it as holes; a hole is a loop of its own, so its index extrudes the disc. Omit for the whole sketch"}},
                     json{{"name", "face"},     {"type", "integer"}, {"default", nullptr}, {"description", "push/pull: a planar face (query_topology id) of `body` grown along its outward normal (flip for inward); boolean auto joins that body. sketch, loop, profile, width, height, plane and taper are then refused, and end is blind or symmetric"}},
                     json{{"name", "width"},    {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"min", 0.01}},
                     json{{"name", "height"},   {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"min", 0.01}},
                     json{{"name", "distance"}, {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     plane_param("XY", false),
                     json{{"name", "profile"},  {"type", "array"}, {"default", json::array()}, {"description", "optional closed contour [[x,y],...] in plane mm; overrides width/height"}},
                     json{{"name", "boolean"},  {"type", "string"}, {"enum", json::array({"auto", "new", "union", "subtract", "intersect"})}, {"default", nullptr}, {"description", "auto joins the body the profile touches, else makes a new body, as the card's default; default auto with sketch, new without"}},
                     json{{"name", "body"},     {"type", "integer"}, {"default", -1}, {"description", "the body to join, subtract from or intersect, or whose face is pushed; omit for the body a sketch touches, else the last body"}},
                     json{{"name", "end"},      {"type", "string"}, {"enum", json::array({"blind", "symmetric", "two_sided", "through_all", "up_to_face"})}, {"default", "blind"}},
                     json{{"name", "distance2"},{"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "second-side depth when end=two_sided (else falls back to distance)"}},
                     json{{"name", "up_to_face"},{"type", "integer"}, {"default", -1}, {"description", "face id (query_topology) of the body the extrude works on, when end=up_to_face"}},
                     json{{"name", "taper"},    {"type", "number"}, {"unit", "deg"}, {"default", 0}, {"description", "draft/taper of the side wall"}},
                     json{{"name", "flip"},     {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "revolve"}, {"summary", "Revolve a committed sketch as the Revolve card does, about a line of it or a plane axis; or a profile drawn here: an explicit `profile` offset from the axis, or a rectangle. Reply: revolve_index, the index of its sketch, the boolean, body and axis used, frame."},
                 {"params", json::array({
                     json{{"name", "sketch"},      {"type", "integer"}, {"default", nullptr}, {"description", "a committed Sketch feature to revolve (sketch_commit's sketch_index); profile, width, height and plane are then refused"}},
                     json{{"name", "axis_entity"}, {"type", "integer"}, {"default", nullptr}, {"description", "with sketch: a line of the sketch (entity index) as the axis, or -1 for the plane axis; omit for the sketch's only centerline (construction line) when it has one and axis is not given"}},
                     json{{"name", "width"},   {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"min", 0.01}},
                     json{{"name", "height"},  {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "angle"},   {"type", "number"}, {"unit", "deg"}, {"default", 360}},
                     json{{"name", "axis"},    {"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 0}, {"description", "the plane's X (0) or Y (1) axis through its origin"}},
                     json{{"name", "flip"},    {"type", "boolean"}, {"default", false}},
                     plane_param("XY", false),
                     json{{"name", "profile"}, {"type", "array"}, {"default", json::array()}, {"description", "optional closed contour [[x,y],...] in plane mm; overrides width/height"}},
                     json{{"name", "boolean"}, {"type", "string"}, {"enum", json::array({"new", "union", "subtract", "intersect"})}, {"default", "new"}},
                     json{{"name", "body"},    {"type", "integer"}, {"default", -1}, {"description", "the body to join, subtract from or intersect; omit for the last body"}},
                 })}},
            json{{"name", "fillet"}, {"summary", "Round measured edges of a body (edge ids from query_topology on that body)."},
                 {"params", json::array({
                     json{{"name", "edge"},   {"type", "integer|array"}, {"description", "one edge id, or an array of ids rounded together in one feature"}},
                     json{{"name", "radius"}, {"type", "number"}, {"unit", "mm"}, {"default", 1}, {"min", 0.01}},
                     json{{"name", "body"},   {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body. edge id is resolved against THIS body."}},
                 })}},
            json{{"name", "chamfer"}, {"summary", "Chamfer measured edges of a body (edge ids from query_topology on that body)."},
                 {"params", json::array({
                     json{{"name", "edge"},     {"type", "integer|array"}, {"description", "one edge id, or an array of ids chamfered together in one feature"}},
                     json{{"name", "distance"}, {"type", "number"}, {"unit", "mm"}, {"default", 1}, {"min", 0.01}},
                     json{{"name", "body"},     {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body. edge id is resolved against THIS body."}},
                 })}},
            json{{"name", "hole"}, {"summary", "Drill a circular hole at (x,y) or `at` on a plane. Reply: hole_index, body, x and y in the frame, off_plane with at, frame."},
                 {"params", json::array({
                     json{{"name", "diameter"}, {"type", "number"}, {"unit", "mm"}, {"default", 5}, {"min", 0.01}},
                     json{{"name", "depth"},    {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "through"},  {"type", "boolean"}, {"default", false}},
                     json{{"name", "x"},        {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "offset in the plane's frame from its origin, NOT world"}},
                     json{{"name", "y"},        {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "offset in the plane's frame from its origin, NOT world"}},
                     plane_param("XY", true),
                     json{{"name", "at"},       {"type", "array|object"}, {"default", nullptr}, {"description", "instead of x and y: a world point [x, y, z], projected onto the plane along its normal (off_plane says how far), or {\"body\": b, \"face\": f} a cylindrical face or {\"body\": b, \"edge\": e} a circular edge whose axis the hole shares. A world point moves with modeling_origin; x and y replay on any printer"}},
                     json{{"name", "body"},     {"type", "integer"}, {"default", -1}, {"description", "the body to drill; omit for a face plane's body, else the last body"}},
                 })}},
            json{{"name", "hole_styled"}, {"summary", "Drill a hole with optional counterbore (style=1) or countersink (style=2) at (x,y) or `at` on a plane. Reply as hole."},
                 {"params", json::array({
                     json{{"name", "diameter"},        {"type", "number"}, {"unit", "mm"}, {"default", 5}, {"min", 0.01}},
                     json{{"name", "depth"},           {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "through"},         {"type", "boolean"}, {"default", true}},
                     json{{"name", "x"},               {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "offset in the plane's frame from its origin, NOT world"}},
                     json{{"name", "y"},               {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "offset in the plane's frame from its origin, NOT world"}},
                     plane_param("XY", true),
                     json{{"name", "at"},       {"type", "array|object"}, {"default", nullptr}, {"description", "instead of x and y: a world point [x, y, z], projected onto the plane along its normal (off_plane says how far), or {\"body\": b, \"face\": f} a cylindrical face or {\"body\": b, \"edge\": e} a circular edge whose axis the hole shares. A world point moves with modeling_origin; x and y replay on any printer"}},
                     json{{"name", "body"},     {"type", "integer"}, {"default", -1}, {"description", "the body to drill; omit for a face plane's body, else the last body"}},
                     json{{"name", "style"},           {"type", "integer"}, {"default", 0}, {"description", "0=simple, 1=counterbore, 2=countersink"}},
                     json{{"name", "cbore_diameter"},  {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "cbore_depth"},     {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "csink_diameter"},  {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "csink_angle"},     {"type", "number"}, {"unit", "deg"}, {"default", 90}},
                     json{{"name", "standard"},        {"type", "string"}, {"default", ""}, {"description", "provenance designation, e.g. M6"}},
                 })}},
            json{{"name", "hole_standard"}, {"summary", "Drill a standard clearance hole (ISO 273 / ANSI) at (x,y) or `at` on a plane. style: 0=simple, 1=counterbore, 2=countersink. Reply as hole."},
                 {"params", json::array({
                     json{{"name", "designation"},  {"type", "string"}, {"description", "e.g. M6, 1/4-20"}},
                     json{{"name", "style"},        {"type", "integer"}, {"default", 0}},
                     json{{"name", "through"},      {"type", "boolean"}, {"default", true}},
                     json{{"name", "depth"},        {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "x"},            {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "offset in the plane's frame from its origin, NOT world"}},
                     json{{"name", "y"},            {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "offset in the plane's frame from its origin, NOT world"}},
                     plane_param("XY", true),
                     json{{"name", "at"},       {"type", "array|object"}, {"default", nullptr}, {"description", "instead of x and y: a world point [x, y, z], projected onto the plane along its normal (off_plane says how far), or {\"body\": b, \"face\": f} a cylindrical face or {\"body\": b, \"edge\": e} a circular edge whose axis the hole shares. A world point moves with modeling_origin; x and y replay on any printer"}},
                     json{{"name", "body"},     {"type", "integer"}, {"default", -1}, {"description", "the body to drill; omit for a face plane's body, else the last body"}},
                 })}},
            json{{"name", "thread"}, {"summary", "Cut a thread as the Thread card does: along a cylindrical face (a bore is tapped, a boss threaded) or a circular edge, or on a plane at (x,y) or `at`. Without designation, a face or edge takes the nearest standard size, ISO metric and UTS alike. Long threads take seconds and the reply may be a job. Reply: thread_index, designation (null when custom), internal, diameter, pitch, depth, length, body, x, y, frame."},
                 {"params", json::array({
                     json{{"name", "face"},        {"type", "integer"}, {"default", nullptr}, {"description", "a cylindrical face id of `body` (query_topology); the thread runs its length from its lower end"}},
                     json{{"name", "edge"},        {"type", "integer"}, {"default", nullptr}, {"description", "instead of face: a circular edge id of `body`, the thread rising along its normal; needs length"}},
                     json{{"name", "body"},        {"type", "integer"}, {"default", -1}, {"description", "the body of face or edge, or the body threaded on a plane; omit for the last body (on a plane, a face plane's body)"}},
                     json{{"name", "designation"}, {"type", "string"}, {"default", nullptr}, {"description", "e.g. M5, M8x1, 1/4-20 UNC: sets diameter, pitch and depth"}},
                     json{{"name", "internal"},    {"type", "boolean"}, {"default", nullptr}, {"description", "true taps a hole, false threads a rod; required on a plane, else from the face (a bore is internal), external for an edge"}},
                     json{{"name", "diameter"},    {"type", "number"}, {"unit", "mm"}, {"default", nullptr}, {"description", "nominal (major) diameter, replacing the standard's; with pitch, a custom thread"}},
                     json{{"name", "pitch"},       {"type", "number"}, {"unit", "mm"}, {"default", nullptr}, {"description", "replaces the standard's"}},
                     json{{"name", "depth"},       {"type", "number"}, {"unit", "mm"}, {"default", nullptr}, {"description", "radial depth; default the 60 degree V's for the pitch and role"}},
                     json{{"name", "length"},      {"type", "number"}, {"unit", "mm"}, {"default", nullptr}, {"description", "default the face's length; required for an edge or a plane"}},
                     plane_param(nullptr, true),
                     json{{"name", "x"},           {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "on a plane: offset in the plane's frame from its origin, NOT world"}},
                     json{{"name", "y"},           {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "on a plane: offset in the plane's frame from its origin, NOT world"}},
                     json{{"name", "at"},          {"type", "array|object"}, {"default", nullptr}, {"description", "on a plane, instead of x and y: a world point [x, y, z], or {\"body\": b, \"face\": f} a cylindrical face or {\"body\": b, \"edge\": e} a circular edge whose axis the thread shares, as hole takes it"}},
                 })}},
            json{{"name", "boolean"}, {"summary", "Combine two bodies: union | subtract (tool from target) | intersect."},
                 {"params", json::array({
                     json{{"name", "op"},        {"type", "string"}, {"enum", json::array({"union", "subtract", "intersect"})}, {"default", "subtract"}},
                     json{{"name", "target"},    {"type", "integer"}, {"default", 0}},
                     json{{"name", "tool"},      {"type", "integer"}, {"default", 1}},
                     json{{"name", "keep_tool"}, {"type", "boolean"}, {"default", false}},
                     json{{"name", "tolerance"}, {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                 })}},
            json{{"name", "pattern"}, {"summary", "Replicate a body: linear (count along a plane axis at spacing) or circular (count over an angle about the plane normal)."},
                 {"params", json::array({
                     json{{"name", "circular"}, {"type", "boolean"}, {"default", false}},
                     json{{"name", "count"},    {"type", "integer"}, {"default", 3}, {"min", 1}},
                     json{{"name", "spacing"},  {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"description", "linear step"}},
                     json{{"name", "dir"},      {"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 0}, {"description", "linear axis: 0=plane X, 1=plane Y"}},
                     json{{"name", "angle"},    {"type", "number"}, {"unit", "deg"}, {"default", 360}, {"description", "circular total sweep"}},
                     plane_param("XY", false),
                     json{{"name", "body"},     {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                 })}},
            json{{"name", "pattern_on_curve"}, {"summary", "Replicate a body along a sketch curve: `count` copies placed at equal-parameter points on the entity, each translated by (P_i - P_0)."},
                 {"params", json::array({
                     json{{"name", "count"},  {"type", "integer"}, {"default", 3}, {"min", 1}},
                     json{{"name", "sketch"}, {"type", "integer"}, {"description", "feature index of the sketch holding the guide curve"}},
                     json{{"name", "entity"}, {"type", "integer"}, {"description", "entity index of the guide curve within that sketch"}},
                     json{{"name", "body"},   {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                 })}},
            json{{"name", "shell"}, {"summary", "Hollow a body to a wall thickness (inward); optionally leave one face open."},
                 {"params", json::array({
                     json{{"name", "thickness"}, {"type", "number"}, {"unit", "mm"}, {"default", 1}, {"min", 0.01}},
                     json{{"name", "face"},      {"type", "integer"}, {"default", -1}, {"description", "face id to leave open (query_topology); omit for a closed hollow"}},
                     json{{"name", "body"},      {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                 })}},
            json{{"name", "draft"}, {"summary", "Taper a body face by an angle about its base (pull direction +Z)."},
                 {"params", json::array({
                     json{{"name", "face"},  {"type", "integer"}, {"description", "face id to draft (query_topology)"}},
                     json{{"name", "angle"}, {"type", "number"}, {"unit", "deg"}, {"default", 5}},
                     json{{"name", "body"},  {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                 })}},
            json{{"name", "mirror"}, {"summary", "Mirror a body about a base plane. mode=new creates a mirrored copy; mode=add fuses the mirror back into the source."},
                 {"params", json::array({
                     plane_param("XZ", false),
                     json{{"name", "mode"},         {"type", "string"}, {"enum", json::array({"new", "add"})},     {"default", "new"}},
                     json{{"name", "keep_original"},{"type", "boolean"}, {"default", true}, {"description", "when mode=new, keep the source body"}},
                     json{{"name", "body"},         {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                 })}},
            json{{"name", "transform"}, {"summary", "Move and/or rotate a body (B-rep transform). copy=true keeps the source and appends the transformed copy as a new body."},
                 {"params", json::array({
                     json{{"name", "body"},    {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                     json{{"name", "dx"},      {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "dy"},      {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "dz"},      {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "axis_x"},  {"type", "number"}, {"default", 0}},
                     json{{"name", "axis_y"},  {"type", "number"}, {"default", 0}},
                     json{{"name", "axis_z"},  {"type", "number"}, {"default", 1}},
                     json{{"name", "pivot_x"}, {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "pivot_y"}, {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "pivot_z"}, {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "angle"},   {"type", "number"}, {"unit", "deg"}, {"default", 0}},
                     json{{"name", "copy"},    {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "axis"}, {"summary", "Create a datum axis (reference line): two points, face normal, cylinder centreline, plane intersection, or along edge."},
                 {"params", json::array({
                     json{{"name", "type"},     {"type", "string"}, {"enum", json::array({"two_points", "face_normal", "cylinder", "plane_intersection", "along_edge"})}, {"default", "two_points"}},
                     json{{"name", "p1"},       {"type", "array"}, {"default", json::array({0,0,0})}, {"description", "first point [x,y,z] for two_points"}},
                     json{{"name", "p2"},       {"type", "array"}, {"default", json::array({0,0,10})}, {"description", "second point [x,y,z] for two_points"}},
                     json{{"name", "body"},     {"type", "integer"}, {"default", -1}, {"description", "body for face/edge refs"}},
                     json{{"name", "face"},     {"type", "integer"}, {"default", -1}, {"description", "face id (query_topology) for face_normal/cylinder"}},
                     json{{"name", "edge"},     {"type", "integer"}, {"default", -1}, {"description", "edge id (query_topology) for along_edge"}},
                     json{{"name", "plane_a"},  {"type", "integer"}, {"default", -1}, {"description", "first datum plane feature index for plane_intersection"}},
                     json{{"name", "plane_b"},  {"type", "integer"}, {"default", -1}, {"description", "second datum plane feature index for plane_intersection"}},
                 })}},
            json{{"name", "coordsys"}, {"summary", "Create a datum coordinate system (origin + orthonormal axes). PointWorld aligns to world; FaceAndDirection uses a face for Z and an edge/hint for X."},
                 {"params", json::array({
                     json{{"name", "type"},      {"type", "string"}, {"enum", json::array({"point_world", "face_and_direction"})}, {"default", "point_world"}},
                     json{{"name", "point"},     {"type", "array"}, {"default", json::array({0,0,0})}, {"description", "origin [x,y,z] for point_world"}},
                     json{{"name", "body"},      {"type", "integer"}, {"default", -1}, {"description", "body for face/edge refs"}},
                     json{{"name", "face"},      {"type", "integer"}, {"default", -1}, {"description", "face id (query_topology) for Z axis"}},
                     json{{"name", "edge"},      {"type", "integer"}, {"default", -1}, {"description", "edge id (query_topology) for X axis hint"}},
                     json{{"name", "x_hint"},    {"type", "array"}, {"default", json::array({1,0,0})}, {"description", "fallback X direction hint if no edge given"}},
                 })}},
            json{{"name", "plane"}, {"summary", "Add a datum plane as the Plane card does, to sketch or build on as {\"datum\": plane_index}: offset from a base plane or a face, at an angle about an edge, midway between two faces, tangent to a cylindrical face, through two edges, or coincident with a base plane or a face. Not the `plane` param of other methods. Reply: plane_index, name, frame."},
                 {"params", json::array({
                     json{{"name", "type"},   {"type", "string"}, {"enum", json::array({"offset", "angle", "midplane", "tangent", "two_edges", "coincident"})}, {"default", "offset"}},
                     json{{"name", "base"},   {"type", "string|object"}, {"default", "XY"}, {"description", "\"XY\", \"XZ\" or \"YZ\" through modeling_origin, or {\"datum\": i}: what offset, coincident and angle build on without face_a"}},
                     json{{"name", "offset"}, {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"description", "offset: distance along the normal of base or face_a"}},
                     json{{"name", "angle"},  {"type", "number"}, {"unit", "deg"}, {"default", 0}, {"description", "offset without face_a: tilt about the base axis `axis`; angle: turn about edge_a from the reference plane; tangent: position around the cylinder"}},
                     json{{"name", "axis"},   {"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 0}, {"description", "offset without face_a: tilt about the base plane's X (0) or Y (1) axis"}},
                     json{{"name", "face_a"}, {"type", "object"}, {"default", nullptr}, {"description", "Face A, {\"body\": b, \"face\": f}: offset or coincident on a face; angle's reference plane; midplane's first face; tangent's cylindrical face"}},
                     json{{"name", "face_b"}, {"type", "object"}, {"default", nullptr}, {"description", "Face B, {\"body\": b, \"face\": f}: midplane's second face"}},
                     json{{"name", "edge_a"}, {"type", "object"}, {"default", nullptr}, {"description", "Edge A, {\"body\": b, \"edge\": e}: angle's hinge; two_edges' first edge"}},
                     json{{"name", "edge_b"}, {"type", "object"}, {"default", nullptr}, {"description", "Edge B, {\"body\": b, \"edge\": e}: two_edges' second edge"}},
                     json{{"name", "size"},   {"type", "array"}, {"unit", "mm"}, {"default", json::array({60, 60})}, {"description", "[u, v]: the size the plane is drawn at"}},
                 })}},
            json{{"name", "helix"}, {"summary", "Create a helical curve, a sweep path for springs, coils and augers, starting on the plane's x axis at `radius` from its origin. pitch = axial rise per turn. left_handed flips the winding. taper_deg != 0 gives a conical helix. Reply: helix_index, frame, start and tangent (world), to draw a sweep's profile across the curve there."},
                 {"params", json::array({
                     json{{"name", "radius"},      {"type", "number"}, {"unit", "mm"},  {"default", 10}, {"min", 0.01}},
                     json{{"name", "pitch"},       {"type", "number"}, {"unit", "mm"},  {"default", 5},  {"min", 0.01}},
                     json{{"name", "height"},      {"type", "number"}, {"unit", "mm"},  {"default", 20}, {"min", 0.01}},
                     json{{"name", "left_handed"}, {"type", "boolean"}, {"default", false}},
                     json{{"name", "taper_deg"},   {"type", "number"}, {"unit", "deg"}, {"default", 0}},
                     plane_param("XY", false),
                 })}},
            json{{"name", "sweep"}, {"summary", "Sweep a committed sketch's closed profile along a path, a sketch or a helix, as the Sweep card does. Draw the profile across the path's start (a helix's start and tangent are in its reply); along a helix the profile keeps its angle to the helix axis. Long sweeps take seconds and the reply may be a job. Reply: sweep_index, bodies, boolean, body."},
                 {"params", json::array({
                     json{{"name", "sketch"},  {"type", "integer"}, {"description", "the profile: a committed sketch's feature index"}},
                     json{{"name", "path"},    {"type", "integer"}, {"description", "feature index of a sketch or a helix, other than the profile"}},
                     json{{"name", "boolean"}, {"type", "string"}, {"enum", json::array({"new", "union", "subtract", "intersect"})}, {"default", "new"}},
                     json{{"name", "body"},    {"type", "integer"}, {"default", -1}, {"description", "the body a union, subtract or intersect works on; omit for the last body"}},
                 })}},
            json{{"name", "loft"}, {"summary", "Loft a solid through two or more committed sketches, in order, each on its own plane (sketch on datum planes), as the Loft card does. Lofts take seconds and the reply may be a job. Reply: loft_index, bodies, boolean, body."},
                 {"params", json::array({
                     json{{"name", "profiles"}, {"type", "array"}, {"description", "sketch feature indices, in loft order"}},
                     json{{"name", "ruled"},    {"type", "boolean"}, {"default", false}, {"description", "straight sections between the profiles instead of a smooth skin"}},
                     json{{"name", "boolean"},  {"type", "string"}, {"enum", json::array({"new", "union", "subtract", "intersect"})}, {"default", "new"}},
                     json{{"name", "body"},     {"type", "integer"}, {"default", -1}, {"description", "the body a union, subtract or intersect works on; omit for the last body"}},
                 })}},
            json{{"name", "thicken"}, {"summary", "Offset a face of a body by a wall thickness, producing a new thin solid body."},
                 {"params", json::array({
                     json{{"name", "body"},      {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                     json{{"name", "face"},      {"type", "integer"}, {"description", "face id to thicken (query_topology)"}},
                     json{{"name", "thickness"}, {"type", "number"}, {"unit", "mm"}, {"default", 2}, {"min", 0.01}},
                     json{{"name", "flip"},      {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "split"}, {"summary", "Split a body with a plane, as the Cut card does, moved by offset and turned over by flip; or along the plane of a face. The upper piece is on the plane's normal side; keeping both makes two bodies, the lower one right after the upper. Reply: split_index, bodies, body, upper_body and lower_body (the pieces kept), frame (a plane's, after offset and flip)."},
                 {"params", json::array({
                     json{{"name", "body"},      {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                     plane_param(nullptr, false),
                     json{{"name", "offset"},    {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "with plane: along its normal"}},
                     json{{"name", "flip"},      {"type", "boolean"}, {"default", false}, {"description", "with plane: turn it over, swapping upper and lower"}},
                     json{{"name", "face"},      {"type", "integer"}, {"default", nullptr}, {"description", "instead of plane: a face id (query_topology) to split along"}},
                     json{{"name", "face_body"}, {"type", "integer"}, {"default", -1}, {"description", "with face: the body that owns it; -1 = target"}},
                     json{{"name", "keep_upper"},{"type", "boolean"}, {"default", true}},
                     json{{"name", "keep_lower"},{"type", "boolean"}, {"default", true}},
                 })}},
             json{{"name", "project"}, {"summary", "Project edges of a solid onto a sketch plane, producing a new sketch feature."},
                  {"params", json::array({
                      json{{"name", "source_body"}, {"type", "integer"}, {"default", -1}, {"description", "body owning the edges; -1 = last body"}},
                      json{{"name", "face"},        {"type", "integer"}, {"default", -1}, {"description", "global face id on the source body; when set, all its edges are projected"}},
                      json{{"name", "edges"},       {"type", "array"}, {"default", json::array()}, {"description", "global edge ids to project; empty => project the face"}},
                      plane_param("XY", false),
                  })}},
             json{{"name", "delete_face"}, {"summary", "Remove faces from a solid, healing the gap via OCCT defeaturing."},
                  {"params", json::array({
                      json{{"name", "body"},  {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                      json{{"name", "faces"}, {"type", "array"}, {"description", "global face ids to delete"}},
                  })}},
            json{{"name", "bridge"}, {"summary", "Build a cubic-Bezier G1 bridge (BSpline) between two sketch-entity endpoints within a sketch feature."},
                  {"params", json::array({
                      json{{"name", "sketch"}, {"type", "integer"}, {"description", "sketch feature index"}},
                      json{{"name", "ent_a"},  {"type", "integer"}, {"description", "first entity index within the sketch"}},
                      json{{"name", "end_a"},  {"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 1}, {"description", "0 = start/p0 side, 1 = end/p1 side"}},
                      json{{"name", "ent_b"},  {"type", "integer"}, {"description", "second entity index within the sketch"}},
                      json{{"name", "end_b"},  {"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 0}, {"description", "0 = start/p0 side, 1 = end/p1 side"}},
                  })}},
            json{{"name", "rib"}, {"summary", "Grow a thin rib wall (stiffener) from an open Line sketch entity, fused to a body. The wall is centred on the line in the sketch plane, thickness wide, and grows depth along the sketch plane's normal."},
                  {"params", json::array({
                      json{{"name", "sketch"},    {"type", "integer"}, {"description", "sketch feature index holding the open line"}},
                      json{{"name", "entity"},    {"type", "integer"}, {"description", "entity index of the open Line within the sketch"}},
                      json{{"name", "thickness"}, {"type", "number"}, {"unit", "mm"}, {"default", 2}, {"min", 0.01}},
                      json{{"name", "depth"},     {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                      json{{"name", "body"},      {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                  })}},
            json{{"name", "surface_extrude"}, {"summary", "Extrude a sketch wire with no end caps -> an open sheet body."},
                  {"params", json::array({
                      json{{"name", "sketch"},  {"type", "integer"}, {"description", "sketch feature index"}},
                      json{{"name", "distance"},{"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                  })}},
            json{{"name", "surface_revolve"}, {"summary", "Revolve a sketch wire with no caps -> an open sheet body."},
                  {"params", json::array({
                      json{{"name", "sketch"},  {"type", "integer"}, {"description", "sketch feature index"}},
                      json{{"name", "angle"},   {"type", "number"}, {"unit", "deg"}, {"default", 360}},
                      json{{"name", "axis"},    {"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 0}},
                  })}},
            json{{"name", "mate"}, {"summary", "Mate two bodies: transform the moving body (cs_b) so its connector lands on the fixed one (cs_a). kind: 0=Fastened, 1=Planar, 2=Revolute, 3=Slider, 4=Cylindrical."},
                 {"params", json::array({
                     json{{"name", "kind"},    {"type", "integer"}, {"default", 0}, {"description", "0=Fastened (rigid), 1=Planar (normal only), 2=Revolute (free rotation about axis), 3=Slider (free translation along axis), 4=Cylindrical (free rotation+translation)"}},
                     json{{"name", "cs_a"},    {"type", "integer"}, {"description", "feature index of the fixed CoordSys (mate connector A)"}},
                     json{{"name", "cs_b"},    {"type", "integer"}, {"description", "feature index of the CoordSys on the body that moves"}},
                     json{{"name", "offset"},  {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "angle"},   {"type", "number"}, {"unit", "deg"}, {"default", 0}},
                     json{{"name", "flip"},    {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "check_interference"}, {"summary", "Solid bodies that overlap, as {body_a, body_b, volume}. Read-only; bodies that merely touch enclose no volume and are not reported."},
                 {"params", json::array({
                     json{{"name", "min_volume"}, {"type", "number"}, {"unit", "mm^3"}, {"default", 1e-6}, {"description", "overlap volume above which a pair counts as interfering"}},
                 })}},
            json{{"name", "query_topology"}, {"summary", "A body's faces (kind, area, centroid, normal, and a cylinder's, cone's, sphere's or torus's numbers) and edges (kind, length, ends, a circle's centre and radius). Face filters (kinds, direction, at_extreme) and edge filters (edge_kinds, direction, convex) narrow their own list; when filters are given, a list none of its filters names is empty, or with adjacency holds the neighbours of the other list. Reply: face_count and edge_count of the body, faces_total and edges_total after filtering, next_offset while a page has more, generation."},
                 {"params", json::array({
                     json{{"name", "body"},       {"type", "integer"}, {"default", 0}},
                     json{{"name", "kinds"},      {"type", "array"}, {"default", nullptr}, {"description", "face kinds to list: planar, cylindrical, conical, spherical, toroidal, bezier, bspline, revolution, extrusion, offset, other"}},
                     json{{"name", "edge_kinds"}, {"type", "array"}, {"default", nullptr}, {"description", "edge kinds to list: line, circle, ellipse, bspline, bezier, other"}},
                     json{{"name", "direction"},  {"type", "array"}, {"default", nullptr}, {"description", "[x, y, z]: planar faces whose outward normal points this way, and line edges along it either way, within 1 degree"}},
                     json{{"name", "at_extreme"}, {"type", "string"}, {"enum", json::array({"+x", "-x", "+y", "-y", "+z", "-z"})}, {"default", nullptr}, {"description", "faces whose centroid lies on that side of the body's bounding box, e.g. +z for the top face"}},
                     json{{"name", "convex"},     {"type", "boolean"}, {"default", nullptr}, {"description", "edges where two faces meet in an outside (true) or inside (false) corner; implies adjacency"}},
                     json{{"name", "adjacency"},  {"type", "boolean"}, {"default", false}, {"description", "faces list their edges; edges list their faces (one for a seam) and say convex true or false, or tangent where the faces meet smoothly"}},
                     json{{"name", "limit"},      {"type", "integer"}, {"default", nullptr}, {"min", 1}, {"description", "page size of both lists; omit for all"}},
                     json{{"name", "offset"},     {"type", "integer"}, {"default", 0}},
                 })}},
            json{{"name", "measure"}, {"summary", "Two refs, on one body or two: distance between their representative points (a face's centroid, an edge's middle, an axis's middle), min_distance with closest_a and closest_b, angle_deg when both have a direction, parallel_gap across two parallel planar faces."},
                 {"params", json::array({
                     json{{"name", "body"}, {"type", "integer"}, {"default", 0}, {"description", "the body of a ref that names none"}},
                     json{{"name", "a"}, {"type", "object"}, {"description", "{\"face\": f}, {\"edge\": e} or {\"point\": [x, y, z]}, with an optional \"body\"; {\"face\": f, \"axis\": true} is a cylindrical face's axis over its length"}},
                     json{{"name", "b"}, {"type", "object"}, {"description", "as a"}},
                 })}},
            json{{"name", "mass_properties"}, {"summary", "Volume / surface area / centre of mass / inertia tensor of a body."},
                 {"params", json::array({
                     json{{"name", "body"}, {"type", "integer"}, {"default", 0}},
                 })}},
            json{{"name", "slice_body"}, {"summary", "Cross-section of a body by a base plane at an offset (sections-as-evidence); returns ordered world contours, each flagged closed/open."},
                 {"params", json::array({
                     json{{"name", "body"}, {"type", "integer"}, {"default", 0}},
                     json{{"name", "plane"}, {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
                     json{{"name", "offset"}, {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                 })}},
            json{{"name", "import_step"}, {"summary", "Import a STEP file as native B-rep bodies (the reference part to measure)."},
                 {"params", json::array({
                     json{{"name", "path"}, {"type", "string"}, {"description", "absolute path"}},
                 })}},
            json{{"name", "import_mesh"}, {"summary", "Convert a triangle mesh (STL/OBJ) into an editable B-rep body. Reports whether the result is a real solid or an open shell, and why."},
                 {"params", json::array({
                     json{{"name", "path"}, {"type", "string"}, {"description", "absolute path"}},
                     json{{"name", "tolerance"}, {"type", "number"}, {"default", 0.01}},
                     json{{"name", "merge_angle_deg"}, {"type", "number"}, {"default", 5.0}},
                 })}},
            json{{"name", "commit"}, {"summary", "Commit to Plate: send the visible bodies to Prepare at their Move-gizmo positions, as one Prepare undo step, and show Prepare. A lone body is sent even when hidden. A body named in the Bodies list or with body_set names its object or part. Reply: objects (list_objects index after replace, name, volumes), bodies_sent, bodies_hidden, replaced."},
                 {"params", json::array({
                     json{{"name", "mode"},    {"type", "string"}, {"enum", json::array({"assembly", "bodies"})}, {"default", ""}, {"description", "assembly: one object with a part per body; bodies: an object per body. Default: the Commit button's mode, which this leaves as it is."}},
                     json{{"name", "replace"}, {"type", "array"}, {"default", json::array()}, {"description", "list_objects indices to delete in the same undo step, such as an earlier commit. Their settings, modifiers and paint go with them."}},
                 })}},
            json{{"name", "new_design"}, {"summary", "New Design: erase every feature, body and variable and the Design undo history. Committed objects stay on the plate. Reply: cleared {features, bodies, variables}."},
                 {"params", json::array({
                     json{{"name", "discard_changes"}, {"type", "boolean"}, {"default", false}, {"description", "needed when the design is not empty"}},
                 })}},
            json{{"name", "export_step"}, {"summary", "Export STEP: write every body to a file as native B-rep, at its Move-gizmo position. Reply: path, bytes, bodies, moved_bodies."},
                 {"params", json::array({
                     json{{"name", "path"},      {"type", "string"}, {"description", "absolute path; its directory must exist"}},
                     json{{"name", "overwrite"}, {"type", "boolean"}, {"default", false}, {"description", "replace an existing file"}},
                 })}},
            json{{"name", "undo"}, {"summary", "Edit > Undo in the Design tab: take back the last change to the Design history (a feature added, edited, deleted, moved, shown or hidden, or renamed; a variable), or in a sketch session the last drawn shape. run_action undo undoes Prepare's history instead. Reply: scope (design or sketch), undo_depth, redo_depth, features, bodies; entities in a sketch."},
                 {"params", json::array()}},
            json{{"name", "redo"}, {"summary", "Edit > Redo in the Design tab: put back what undo took back, until the next change. Reply as undo's."},
                 {"params", json::array()}},
            json{{"name", "feature_delete"}, {"summary", "Delete a feature as its tree row's Delete does; deleting a sketch also deletes the features built on it. One undo step. Later indices shift. Reply: removed (how many features went), features, bodies."},
                 {"params", json::array({
                     json{{"name", "feature"}, {"type", "integer"}, {"description", "feature index (describe_scene)"}},
                 })}},
            json{{"name", "feature_move"}, {"summary", "Move a feature one place up or down the history, as Move up / Move down do. A feature cannot come before one it uses. One undo step. Reply: feature (its new index), from."},
                 {"params", json::array({
                     json{{"name", "feature"}, {"type", "integer"}, {"description", "feature index (describe_scene)"}},
                     json{{"name", "delta"},   {"type", "integer"}, {"enum", json::array({-1, 1})}, {"description", "-1 up, 1 down"}},
                 })}},
            json{{"name", "feature_set_enabled"}, {"summary", "Show or hide (suppress) a feature as its row's eye does; a hidden feature is left out of the rebuild and bodies may renumber. One undo step, none when it already was. Reply: enabled, changed, bodies."},
                 {"params", json::array({
                     json{{"name", "feature"}, {"type", "integer"}, {"description", "feature index (describe_scene)"}},
                     json{{"name", "enabled"}, {"type", "boolean"}},
                 })}},
            json{{"name", "feature_rename"}, {"summary", "Rename a feature as F2 on its row does. Its own undo step. Reply: name, changed."},
                 {"params", json::array({
                     json{{"name", "feature"}, {"type", "integer"}, {"description", "feature index (describe_scene)"}},
                     json{{"name", "name"},    {"type", "string"}, {"description", "trimmed; not empty"}},
                 })}},
            json{{"name", "body_set"}, {"summary", "Rename, colour or show/hide a body, as its Bodies row and the Color button do; give one or more. Not in the undo history. The name and colour are saved with the project and the name names its committed object; visibility is view state, and commit leaves hidden bodies out. Reply: name, color, visible."},
                 {"params", json::array({
                     json{{"name", "body"},    {"type", "integer"}, {"description", "body index (describe_scene)"}},
                     json{{"name", "name"},    {"type", "string"},  {"default", nullptr}, {"description", "trimmed; not empty"}},
                     json{{"name", "color"},   {"type", "string"},  {"default", nullptr}, {"description", "\"#RRGGBB\""}},
                     json{{"name", "visible"}, {"type", "boolean"}, {"default", nullptr}},
                 })}},
            json{{"name", "body_delete"}, {"summary", "Delete a body as its row's Delete does: the feature that made it goes, which can take other features and bodies with it. One undo step. Reply: deleted_feature {index, type, name}, removed, features, bodies."},
                 {"params", json::array({
                     json{{"name", "body"}, {"type", "integer"}, {"description", "body index (describe_scene)"}},
                 })}},
            json{{"name", "validate_against"}, {"summary", "Volume + bbox/centroid + surface deviation (max/mean/rms mm) of a body vs a reference {step:path|body:id} (the RE acceptance metric)."},
                 {"params", json::array({
                     json{{"name", "body"}, {"type", "integer"}, {"default", 0}},
                     json{{"name", "reference"}, {"type", "object"}},
                 })}},
            json{{"name", "sketch_begin"}, {"summary", "Open a sketch session on a plane, as picking a plane or face for Sketch does. Sketch points are [x, y] in that plane's frame (mm), from its origin: modeling_origin for a base plane. Reply: frame."},
                 {"params", json::array({
                     plane_param("XY", false),
                 })}},
            json{{"name", "sketch_commit"}, {"summary", "Commit the open sketch as a Sketch feature, as the sketch's Confirm does, and leave sketch mode. An empty sketch is refused and stays open. Reply: sketch_index, name, features (the new count) and its loops (closed_loops with the index extrude's loop takes, open_ends, defects, buildable)."},
                 {"params", json::array()}},
            json{{"name", "sketch_cancel"}, {"summary", "Discard the open sketch and leave sketch mode."},
                 {"params", json::array()}},
            json{{"name", "sketch_add"}, {"summary", "Add entities or shapes to the open sketch: `entities` (a list), or one item's fields beside `type`, or `rect`. Entities: line {p0, p1}, circle {center, radius}, arc {center, radius, start_angle, end_angle}, point {p}. Shapes, built as their drawing tools build them: polygon {center, sides, radius (to a vertex) or across_flats, angle (of that vertex, or of the first edge's midpoint)}, slot {c0, c1, width}, ellipse {center, radius (semi-major), rminor, rotation}, ellipse_arc {the ellipse's fields, start_angle, end_angle: parametric, counter-clockwise}, spline {ctrl: 2 or more poles}. Points are [x, y] in the sketch plane (mm); angles are radians, or degrees as start_angle_deg and the like. A polygon is lines; its side count is not kept once committed. Reply: first_index, added, shapes [{first, count}] per item, entities, dof."},
                 {"params", json::array({
                     json{{"name", "entities"},     {"type", "array"}, {"default", json::array()}, {"description", "entity or shape objects, each with a type"}},
                     json{{"name", "type"},         {"type", "string"}, {"enum", json::array({"line", "arc", "circle", "point", "polygon", "slot", "ellipse", "ellipse_arc", "spline"})}, {"default", ""}, {"description", "one item, its fields beside it"}},
                     json{{"name", "rect"},         {"type", "array"}, {"default", json::array()}, {"description", "[x0, y0, x1, y1]: a closed rectangle of four lines"}},
                     json{{"name", "start_angle"},  {"type", "number"}, {"unit", "rad"}, {"default", 0}, {"description", "arc start, counter-clockwise from the plane's x axis"}},
                     json{{"name", "end_angle"},    {"type", "number"}, {"unit", "rad"}, {"default", 0}},
                     json{{"name", "construction"}, {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "sketch_select"}, {"summary", "Select sketch entities by index; an empty list clears the selection. Reply: selected."},
                 {"params", json::array({
                     json{{"name", "entities"}, {"type", "array"}, {"default", json::array()}, {"description", "entity indices"}},
                 })}},
            json{{"name", "sketch_delete"}, {"summary", "Delete sketch entities. Reply: removed, entities."},
                 {"params", json::array({
                     json{{"name", "entities"}, {"type", "array"}, {"default", json::array()}, {"description", "entity indices; omit for the selection"}},
                 })}},
            json{{"name", "sketch_construction"}, {"summary", "Toggle construction on sketch entities. Reply: changed."},
                 {"params", json::array({
                     json{{"name", "entities"}, {"type", "array"}, {"default", json::array()}, {"description", "entity indices; omit for the selection"}},
                 })}},
            json{{"name", "sketch_offset"}, {"summary", "Add an offset copy of sketch lines, arcs and circles. Reply: first_index, added, dof."},
                 {"params", json::array({
                     json{{"name", "distance"}, {"type", "number"}, {"unit", "mm"}, {"description", "signed offset"}},
                     json{{"name", "entities"}, {"type", "array"}, {"default", json::array()}, {"description", "entity indices; omit for the selection, else every entity"}},
                 })}},
            json{{"name", "sketch_mirror"}, {"summary", "Add a mirrored copy of sketch entities about the line through axis_a and axis_b. Reply: first_index, added, dof."},
                 {"params", json::array({
                     json{{"name", "axis_a"},   {"type", "array"}, {"default", json::array({0, 0})}, {"description", "[x, y] in the sketch plane, mm"}},
                     json{{"name", "axis_b"},   {"type", "array"}, {"default", json::array({0, 1})}, {"description", "[x, y] in the sketch plane, mm"}},
                     json{{"name", "entities"}, {"type", "array"}, {"default", json::array()}, {"description", "entity indices; omit for the selection, else every entity"}},
                 })}},
            json{{"name", "sketch_describe"}, {"summary", "The open sketch, or a committed one: entities, constraint count and its loops (closed_loops {index, entities, holes, area}, open_ends, defects, buildable). The open sketch adds dof, the armed tool and the selection; a committed one adds its name and plane."},
                 {"params", json::array({
                     json{{"name", "sketch"}, {"type", "integer"}, {"default", nullptr}, {"description", "a committed Sketch feature's index; omit for the open sketch"}},
                 })}},
            json{{"name", "sketch_validate"}, {"summary", "The open sketch's loops (closed_loops, open_ends, defects, buildable) and dof."},
                 {"params", json::array({
                     json{{"name", "tolerance"}, {"type", "number"}, {"unit", "mm"}, {"default", 0.001}, {"description", "reported back"}},
                 })}},
            json{{"name", "sketch_heal"}, {"summary", "Weld sketch endpoints that nearly meet and constrain them coincident, so loops are closed by construction. Reply: welded and the loops."},
                 {"params", json::array({
                     json{{"name", "tolerance"},           {"type", "number"}, {"unit", "mm"}, {"default", 0.001}},
                     json{{"name", "ignore_construction"}, {"type", "boolean"}, {"default", true}},
                 })}},
            json{{"name", "sketch_set_value"}, {"summary", "Set the dimension of the selected sketch entities, as the value field does: a line's length, a circle's diameter, an arc's radius, or the angle or distance between two. Select them with sketch_select first. Reply: kind, before, value, dof and the loops."},
                 {"params", json::array({
                     json{{"name", "value"}, {"type", "number"}, {"description", "mm, or degrees for an angle"}},
                 })}},
            json{{"name", "sketch_fillet"}, {"summary", "Round the corner of two lines of the open sketch, as the Fillet tool does: both lines are cut back and joined by an arc of `radius`, bound coincident and tangent to them; constraints the sharp corner held are released. Reply: entity (the arc), lines, released_constraints, entities, constraints, dof, solve_ok."},
                 {"params", json::array({
                     json{{"name", "entities"}, {"type", "array"}, {"default", json::array()}, {"description", "the two line indices; omit for the selection"}},
                     json{{"name", "radius"},   {"type", "number"}, {"unit", "mm"}},
                 })}},
            json{{"name", "sketch_chamfer"}, {"summary", "Bevel the corner of two lines of the open sketch, as the Chamfer tool does: both lines are cut back by `distance` from the corner and joined by a line bound coincident to them. Reply: entity (the new line), lines, released_constraints, entities, constraints, dof, solve_ok."},
                 {"params", json::array({
                     json{{"name", "entities"}, {"type", "array"}, {"default", json::array()}, {"description", "the two line indices; omit for the selection"}},
                     json{{"name", "distance"}, {"type", "number"}, {"unit", "mm"}, {"description", "setback along each line"}},
                 })}},
            json{{"name", "sketch_trim"}, {"summary", "Trim the open sketch's entity nearest `at`, as a Trim click there does: a line or an arc is cut back from its end nearer `at` to the first crossing with another entity beyond `at`; a circle loses the part between the crossings either side of `at` and becomes an arc. With extend, as an Extend click: the line's or arc's end nearer `at` runs out to the next entity. The entity's constraints are released. Reply: entity, geometry (its new fields), released_constraints, entities, dof."},
                 {"params", json::array({
                     json{{"name", "at"},        {"type", "array"}, {"description", "[x, y] in the sketch plane, mm: a point on the part to cut, or near the end to extend"}},
                     json{{"name", "extend"},    {"type", "boolean"}, {"default", false}},
                     json{{"name", "tolerance"}, {"type", "number"}, {"unit", "mm"}, {"default", 0.05}, {"description", "how far from `at` the entity may be"}},
                 })}},
            json{{"name", "sketch_transform"}, {"summary", "Move, rotate, scale or array entities of the open sketch, as the transform tools' Confirm does. move {dx, dy}, rotate {angle, pivot} and scale {scale, pivot} change the entities and release the constraints that no longer hold. array {dx, dy, count} adds copies stepped by dx, dy; polar_array {pivot, angle, count} adds copies every angle/count about pivot (angle is the whole sweep, default 360 degrees). count includes the original, and each copy is bound to its source (equal length and parallel, or its radius). pivot defaults to the entities' centroid. Angles are radians, or degrees as angle_deg. Reply: moved and released_constraints, or first_index, added and bound; pivot; entities, dof."},
                 {"params", json::array({
                     json{{"name", "op"},        {"type", "string"}, {"enum", json::array({"move", "rotate", "scale", "array", "polar_array"})}},
                     json{{"name", "entities"},  {"type", "array"}, {"default", json::array()}, {"description", "entity indices; omit for the selection"}},
                     json{{"name", "dx"},        {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "dy"},        {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "angle"},     {"type", "number"}, {"unit", "rad"}, {"default", nullptr}, {"description", "or angle_deg"}},
                     json{{"name", "scale"},     {"type", "number"}, {"default", nullptr}},
                     json{{"name", "count"},     {"type", "integer"}, {"default", nullptr}, {"min", 2}, {"max", 1000}},
                     json{{"name", "pivot"},     {"type", "array"}, {"default", nullptr}, {"description", "[x, y] in the sketch plane, mm"}},
                 })}},
            json{{"name", "sketch_constrain"}, {"summary", "Add a constraint to the open sketch, as a CONSTRAIN bar button does (sketch_set_value is the value field, V). With `entities`, else the selection, the button's own rules choose the points, and a value it would ask for is `value` or the current one. With `refs` the references are taken as written, in sketch_constraints' form: {entity, role} for a point (role p0, p1 or center; entity also \"origin\") and {entity} for a whole entity (also \"x_axis\", \"y_axis\"). distance, lock_x, lock_y, point_on_line and point_on_object have no button and take refs. A constraint the sketch cannot solve with is rolled back and the call fails. Reply: added (indices), items, constraints, dof, solve_ok."},
                 {"params", json::array({
                     json{{"name", "type"},     {"type", "string"}, {"enum", json::array({"fix", "coincident", "horizontal", "vertical", "distance", "lock_x", "lock_y", "equal_length", "parallel", "perpendicular", "concentric", "tangent", "midpoint", "symmetric", "angle", "radius", "diameter", "point_on_line", "point_on_object", "equal_radius", "collinear", "distance_x", "distance_y", "symmetric_about_y", "symmetric_about_x", "equal"})}, {"description", "\"equal\" is the Equal button: equal length, or equal radius for two curves"}},
                     json{{"name", "entities"}, {"type", "array"}, {"default", json::array()}, {"description", "1 to 3 entity indices; omit for the selection"}},
                     json{{"name", "refs"},     {"type", "array"}, {"default", json::array()}, {"description", "[{entity, role?}], instead of entities"}},
                     json{{"name", "value"},    {"type", "number"}, {"default", nullptr}, {"description", "mm, or degrees for an angle"}},
                 })}},
            json{{"name", "sketch_constraints"}, {"summary", "The constraints of the open sketch, or of a committed one: index, type, refs ({entity, role} for a point, {entity} for a whole entity) and value (mm, or degrees for an angle). Reply: scope (live or committed), items, and dof and solve_ok for the open sketch."},
                 {"params", json::array({
                     json{{"name", "sketch"}, {"type", "integer"}, {"default", nullptr}, {"description", "a committed Sketch feature's index; omit for the open sketch"}},
                 })}},
            json{{"name", "sketch_unconstrain"}, {"summary", "Delete a constraint of the open sketch, as its row's delete button does; the constraints after it move down one index. Reply: removed, constraints, dof, solve_ok."},
                 {"params", json::array({
                     json{{"name", "index"}, {"type", "integer"}, {"description", "sketch_constraints' items[].index"}},
                 })}},
            json{{"name", "list_verbs"}, {"summary", "The Design tab's right-click verbs: id, name, key, whether each applies to the current selection, and whether it opens a dialog."},
                 {"params", json::array({
                     json{{"name", "applicable_only"}, {"type", "boolean"}, {"default", false}},
                     json{{"name", "sketch_mode"},     {"type", "boolean"}, {"default", nullptr}, {"description", "when given, only sketch-mode (true) or feature-mode (false) verbs"}},
                 })}},
            json{{"name", "run_verb"}, {"summary", "Fire a verb from list_verbs as its menu row or key does. It runs on the next event-loop turn, so the reply says only that it was dispatched; a verb that opens a card or dialog leaves it open."},
                 {"params", json::array({
                     json{{"name", "verb"}, {"type", "string"}, {"description", "verb id"}},
                 })}},
            json{{"name", "set_variable"}, {"summary", "Set a design variable to an expression, or remove it with an empty expr; feature fields bound with set_feature_expr follow it."},
                 {"params", json::array({
                     json{{"name", "name"}, {"type", "string"}},
                     json{{"name", "expr"}, {"type", "string"}, {"description", "e.g. \"20\" or \"width / 2 + 3\"; \"\" removes the variable"}},
                 })}},
            json{{"name", "set_feature_expr"}, {"summary", "Bind a feature's numeric field to an expression over the design variables, or remove the binding with an empty expr (the field keeps its value)."},
                 {"params", json::array({
                     json{{"name", "feature"}, {"type", "integer"}, {"description", "feature index (describe_scene)"}},
                     json{{"name", "field"},   {"type", "string"}, {"description", "a numeric field of describe_feature's params, e.g. width, distance, hole_diameter, thread_diameter"}},
                     json{{"name", "expr"},    {"type", "string"}, {"description", "\"\" removes the binding"}},
                 })}},
            json{{"name", "surface_loft"}, {"summary", "Skin two or more profile sketches with no end caps -> an open sheet body."},
                 {"params", json::array({
                     json{{"name", "profiles"}, {"type", "array"}, {"description", "sketch feature indices, in order"}},
                     json{{"name", "ruled"},    {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "surface_fill"}, {"summary", "Fill a sketch's closed boundary with one smooth face -> a sheet body."},
                 {"params", json::array({
                     json{{"name", "sketch"}, {"type", "integer"}, {"description", "sketch feature index"}},
                 })}},
            json{{"name", "surface_offset"}, {"summary", "Offset a sheet body by a signed distance -> another sheet body."},
                 {"params", json::array({
                     json{{"name", "body"},   {"type", "integer"}, {"default", -1}, {"description", "sheet body; omit for the last body"}},
                     json{{"name", "offset"}, {"type", "number"}, {"unit", "mm"}, {"default", 1}},
                 })}},
            json{{"name", "thicken_surface"}, {"summary", "Thicken a whole sheet body into a solid."},
                 {"params", json::array({
                     json{{"name", "body"},      {"type", "integer"}, {"default", -1}, {"description", "sheet body; omit for the last body"}},
                     json{{"name", "thickness"}, {"type", "number"}, {"unit", "mm"}, {"default", 2}, {"min", 0.01}},
                     json{{"name", "flip"},      {"type", "boolean"}, {"default", false}},
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
        json jf{{"index", int(i)}, {"type", feature_type_name(f.type)}, {"name", f.name}, {"enabled", f.enabled}};
        // The fields an expression drives: set_feature refuses them, describe_feature has the expressions.
        if (!f.expr.empty()) {
            json bound = json::array();
            for (const auto& kv : f.expr) bound.push_back(kv.first);
            jf["bound"] = std::move(bound);
        }
        features.push_back(std::move(jf));
    }

    // A variable that does not evaluate leaves every value null; the error says why.
    std::string var_error;
    const std::map<std::string, double> values = doc.variable_values(var_error);
    json variables = json::array();
    for (const auto& [name, expr] : doc.variables) {
        const auto v = values.find(name);
        variables.push_back(json{{"name", name}, {"expr", expr}, {"value", v != values.end() ? json(v->second) : json()}});
    }

    const DesignCanvas* vp = panel->mcp_viewport();
    json bodies = json::array();
    for (size_t i = 0; i < doc.bodies.size(); ++i) {
        // The user's name when the body has one, the derived maker name otherwise — the same
        // rule the Bodies row shows, so a driver and a person read the same thing.
        json b{{"index", int(i)},
               {"name", doc.bodies[i].has_user_name ? doc.bodies[i].user_name : doc.bodies[i].name},
               {"user_name", doc.bodies[i].has_user_name},
               {"has_color", doc.bodies[i].has_color},
               {"source_feature", doc.bodies[i].source_feature},
               {"visible", panel->mcp_body_shown(int(i))},
               // The Move gizmo's placement, which bbox (the recipe's body) does not include.
               {"moved", panel->mcp_body_moved(int(i))}};
        // The colour the canvas draws: the override when has_color, else the automatic one.
        if (vp != nullptr) b["color"] = encode_color(vp->body_color(int(i)));
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

    json out{
        {"modeling_origin", {doc.modeling_origin.x(), doc.modeling_origin.y(), doc.modeling_origin.z()}},
        {"features", std::move(features)},
        {"variables", std::move(variables)},
        {"bodies", std::move(bodies)},
        {"error", doc.error},
        // Pass this back as "generation" on any call that takes a face or edge id and the call
        // is refused if the topology has moved on. See the note on the dispatcher.
        {"generation", doc.topo_generation},
        {"undo_depth", doc.undo_depth()},
        {"redo_depth", doc.redo_depth()},
        // Whether sketch endpoints that nearly meet are welded: the document's rule, which changes
        // the loops sketch_add's entities make.
        {"auto_close_loops", doc.auto_close_loops},
    };
    if (!var_error.empty()) out["variables_error"] = var_error;
    if (!doc.mate_conflicts.empty()) {
        json conflicts = json::array();
        for (const auto& [feature, reason] : doc.mate_conflicts)
            conflicts.push_back(json{{"feature", feature}, {"reason", reason}});
        out["mate_conflicts"] = std::move(conflicts);
    }
    return out;
}

// --- Measure layer (read-only "evidence" half of the RE loop) ------------------
inline json vec3(const Vec3d& v) { return json::array({v.x(), v.y(), v.z()}); }

// Shared Build helpers.

// The kinds query_topology gives faces, in GeomAbs_SurfaceType order, and edges.
const char* const kFaceKinds[] = {"planar",     "cylindrical", "conical", "spherical", "toroidal", "bezier",
                                  "bspline",    "revolution",  "extrusion", "offset",  "other"};
static_assert(std::size(kFaceKinds) == size_t(GeomAbs_OtherSurface) + 1, "every surface type needs a kind");
const char* const kEdgeKinds[] = {"line", "circle", "ellipse", "bspline", "bezier", "other"};

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

// What kind of surface a face is, for the messages that refuse one.
const char* face_kind_name(const TopoDS_Face& f) { return kFaceKinds[BRepAdaptor_Surface(f).GetType()]; }
const char* edge_kind_name(const TopoDS_Edge& e) { return kEdgeKinds[edge_kind(BRepAdaptor_Curve(e).GetType())]; }

// A shown Plane feature's place among the datum planes: the kernel lists the enabled Plane features
// in order, and a datum reference holds 3 + that position.
int datum_ordinal(const CadDocument& doc, int fi)
{
    int ordinal = 0;
    for (int i = 0; i < fi; ++i)
        if (doc.features[i].type == CadFeatureType::Plane && doc.features[i].enabled) ++ordinal;
    return ordinal;
}

// Every reply that used a plane says where it was: a face plane's x axis is not something a caller
// can guess, and the XZ base plane is left-handed (x cross y is -normal), as the GUI stores it.
json frame_json(const SketchPlane& p)
{
    return json{{"origin", vec3(p.origin)}, {"normal", vec3(p.normal)},
                {"x_axis", vec3(p.x_axis)}, {"y_axis", vec3(p.y_axis)}};
}

// A plane's frame, with `base` when it is the XY, XZ or YZ plane through the modeling origin
// (whatever its origin within that plane): the name the plane list gives it.
json plane_json(const SketchPlane& p, const Vec3d& modeling_origin)
{
    json j = frame_json(p);
    const std::pair<const char*, SketchPlane> bases[] = {{"XY", SketchPlane::XY()}, {"XZ", SketchPlane::XZ()}, {"YZ", SketchPlane::YZ()}};
    for (const auto& [name, b] : bases)
        if ((p.normal - b.normal).norm() < 1e-9 && (p.x_axis - b.x_axis).norm() < 1e-9 &&
            (p.y_axis - b.y_axis).norm() < 1e-9 && std::abs((p.origin - modeling_origin).dot(b.normal)) < 1e-6)
            j["base"] = name;
    return j;
}

// Adds features as a card's Confirm does: one undo step, a rebuild, the tree and status refreshed.
// `add` creates them and returns the index reported under `index_key`. A rebuild that fails, or an
// `add` that throws, leaves the document as it was with nothing to redo, puts "Edit rejected" on
// the status line as a refused GUI edit does, and fails the call with the kernel's reason, so the
// failed call can be left out of an exported script.
// `slow` rebuilds as the cards do, off the UI thread behind the progress dialog (recompute_guarded),
// for features whose helical pipes and skins can take seconds: the window keeps painting and
// inspection calls keep answering meanwhile, while the rebuild refuses every Design call.
json add_feature(DesignPanel* panel, const char* index_key, const std::function<int()>& add, bool slow = false)
{
    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    int         idx = -1;
    std::string why;
    try {
        idx = add();
        if (!(slow ? panel->recompute_guarded(_L("Rebuilding model…")) : doc.recompute()))
            why = doc.error.empty() ? "the model could not be rebuilt" : doc.error;
    } catch (const Standard_Failure& ex) {   // derives from std::exception, so it comes first
        why = std::string("OCCT: ") + (*ex.what() ? ex.what() : "failure");
    } catch (const std::exception& ex) {
        why = *ex.what() ? ex.what() : "the feature could not be added";
    }
    if (!why.empty()) {
        doc.rollback();
        doc.error = why;   // the rollback's rebuild cleared it; the status line reads it
        panel->mcp_after_change(false);
        throw CadCallError(-32000, why);
    }
    panel->mcp_after_change();
    return json{{"ok", true}, {index_key, idx}, {"bodies", int(doc.bodies.size())}};
}
BooleanMode bool_from(const std::string& s)
{
    if (s == "union" || s == "add")       return BooleanMode::Add;
    if (s == "subtract" || s == "cut")    return BooleanMode::Cut;
    if (s == "intersect" || s == "common")return BooleanMode::Intersect;
    return BooleanMode::New;
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

// Resolve body index -> shape, throwing a clear error if out of range / null.
const TopoDS_Shape& body_shape(const CadDocument& doc, const json& params)
{
    int idx = params.value("body", 0);
    if (idx < 0 || idx >= int(doc.bodies.size()))
        throw std::runtime_error("body index out of range (have " + std::to_string(doc.bodies.size()) + ")");
    if (doc.bodies[idx].shape.IsNull())
        throw std::runtime_error("body has no shape");
    return doc.bodies[idx].shape;
}

json query_topology(DesignPanel* panel, const json& params) { return cad_query_topology(panel->mcp_doc(), params); }

json measure(DesignPanel* panel, const json& params) { return cad_measure(panel->mcp_doc(), params); }

json mass_properties(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& shape = body_shape(panel->mcp_doc(), params);
    auto mp = GeometryEngine::mass_properties(shape);
    if (!mp.valid) throw std::runtime_error("mass properties could not be computed (null/empty shape)");
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
json slice_body(DesignPanel* panel, const json& params)
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
    if (!sect.IsDone()) throw std::runtime_error("section failed");
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

// A file the call reads, checked as every path the MCP server takes: absolute UTF-8 (a relative
// one would resolve against the GUI's working directory) and existing. Returns its UTF-8 form.
std::string input_path(const json& value, const std::string& what)
{
    if (!value.is_string()) throw std::runtime_error(what + " must be an absolute path");
    std::string err;
    const auto path = Mcp::resolve_input_path(value.get<std::string>(), err);
    if (!path) throw std::runtime_error(err);
    return Mcp::path_to_utf8(*path);
}

// --- Build: bring a reference part in (Import STEP as native B-rep bodies) ------
json import_step(DesignPanel* panel, const json& params)
{
    if (!params.contains("path")) throw std::runtime_error("import_step needs 'path'");
    const std::string path = input_path(params["path"], "path");
    std::string err;
    std::vector<TopoDS_Shape> solids = GeometryEngine::read_step_solids(path, err);
    if (solids.empty()) throw std::runtime_error(err.empty() ? "no solids in STEP" : err);

    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    int first = int(doc.features.size());
    for (const TopoDS_Shape& s : solids) {
        CadFeature f;
        f.type           = CadFeatureType::Import;
        f.name           = panel->mcp_feature_name("STEP");
        f.imported_solid = s;
        f.mode           = BooleanMode::New;   // each solid = its own coexisting body
        doc.features.push_back(f);
    }
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"imported", int(solids.size())}, {"first_feature", first},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// --- Import a triangle mesh as a B-rep body (GeometryEngine::mesh_to_brep) ---
// Same destination as import_step: a CadFeatureType::Import body every feature tool can edit.
// The full conversion stats come back so a caller can tell an honest solid from an open shell
// instead of discovering it later when a boolean silently fails.
json import_mesh(DesignPanel* panel, const json& params)
{
    if (!params.contains("path")) throw std::runtime_error("import_mesh needs 'path'");
    const std::string path       = input_path(params["path"], "path");
    const double tolerance       = params.value("tolerance", 0.01);
    const double merge_angle_deg = params.value("merge_angle_deg", 5.0);

    TriangleMesh mesh;
    const std::string ext = boost::algorithm::to_lower_copy(
        boost::filesystem::path(path).extension().string());
    if (ext == ".stl") {
        if (!mesh.ReadSTLFile(path.c_str())) throw std::runtime_error("could not read STL: " + path);
    } else if (ext == ".obj") {
        ObjInfo obj_info; std::string obj_err;
        if (!load_obj(path.c_str(), &mesh, obj_info, obj_err))
            throw std::runtime_error("could not read OBJ: " + obj_err);
    } else {
        throw std::runtime_error("unsupported mesh format (want .stl or .obj): " + ext);
    }

    GeometryEngine::MeshBrepStats st;
    const TopoDS_Shape shape = GeometryEngine::mesh_to_brep(mesh.its, tolerance, merge_angle_deg, st);
    if (shape.IsNull()) throw std::runtime_error("mesh conversion produced no geometry");

    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    const int first = int(doc.features.size());
    CadFeature f;
    f.type           = CadFeatureType::Import;
    f.name           = panel->mcp_feature_name(_L("Mesh"));
    f.imported_solid = shape;
    f.mode           = BooleanMode::New;
    doc.features.push_back(f);

    const bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"first_feature", first}, {"bodies", int(doc.bodies.size())},
                {"input_triangles", st.input_tris}, {"kept_triangles", st.kept_tris},
                {"degenerate_collapsed", st.degenerate_collapsed},
                {"degenerate_sliver", st.degenerate_sliver},
                {"faces_built", st.faces_built}, {"faces_failed", st.faces_failed},
                {"faces_final", st.faces_final}, {"unique_edges", st.unique_edges},
                {"boundary_edges", st.boundary_edges},
                {"nonmanifold_edges", st.nonmanifold_edges},
                {"watertight", st.watertight}, {"is_solid", st.is_solid},
                {"volume", st.volume}, {"error", doc.error}};
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
    double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
    return {vp.Mass(), Vec3d(c.X(), c.Y(), c.Z()), Vec3d(x0, y0, z0), Vec3d(x1, y1, z1)};
}

json validate_against(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& cand = body_shape(panel->mcp_doc(), params);   // candidate = the reconstruction
    if (!params.contains("reference")) throw std::runtime_error("validate_against needs 'reference' {step|body}");
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

// Resolve an optional explicit body target. Default (no `body`, or <0) = last body, which is
// what the kernel picks anyway. Validated BEFORE any checkpoint so a bad index throws clean.
int target_body_arg(const json& params, const CadDocument& doc)
{
    int bi = params.value("body", -1);
    if (bi >= int(doc.bodies.size()))
        throw std::runtime_error("body index out of range (have " + std::to_string(doc.bodies.size()) + ")");
    return bi;   // <0 -> kernel uses the last body
}

// `edge` is one id or an array of ids; an array becomes ONE feature, every id resolved
// against the same body (a chain of single-edge features would see the ids drift).
std::vector<int> edge_ids_arg(const json& params, const char* verb)
{
    if (!params.contains("edge"))
        throw std::runtime_error(std::string(verb) + " needs 'edge' (id or array of ids from query_topology)");
    const json& e = params["edge"];
    std::vector<int> ids;
    if (e.is_array()) for (const json& v : e) ids.push_back(v.get<int>());
    else              ids.push_back(e.get<int>());
    if (ids.empty()) throw std::runtime_error(std::string(verb) + ": 'edge' is an empty array");
    return ids;
}

json action_fillet(DesignPanel* panel, const json& params)
{
    const std::vector<int> edges = edge_ids_arg(params, "fillet");
    const double radius = params.value("radius", 1.0);
    if (radius <= 0) throw std::runtime_error("radius must be > 0");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to fillet");
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int f = doc.add_fillet(radius, edges, panel->mcp_feature_name(_L("Fillet")));
    if (bi >= 0) doc.features[f].target_body = bi;   // edge id resolved against THIS body's shape
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"fillet_index", f}, {"body", bi < 0 ? int(doc.bodies.size()) - 1 : bi},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_chamfer(DesignPanel* panel, const json& params)
{
    const std::vector<int> edges = edge_ids_arg(params, "chamfer");
    const double dist = params.value("distance", 1.0);
    if (dist <= 0) throw std::runtime_error("distance must be > 0");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to chamfer");
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int c = doc.add_chamfer(dist, edges, panel->mcp_feature_name(_L("Chamfer")));
    if (bi >= 0) doc.features[c].target_body = bi;   // edge id resolved against THIS body's shape
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"chamfer_index", c}, {"body", bi < 0 ? int(doc.bodies.size()) - 1 : bi},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// A hole method's reply: the feature, where it went (cad_hole_placement) and the frame of its plane.
json hole_reply(json r, const json& placement, const SketchPlane& pl)
{
    r.update(placement);
    r["frame"] = frame_json(pl);
    return r;
}

json action_hole(DesignPanel* panel, const json& params)
{
    const double dia   = params.value("diameter", 5.0);
    const double depth = params.value("depth", 10.0);
    const bool   thru  = params.value("through", false);
    if (dia <= 0) throw std::runtime_error("diameter must be > 0");
    CadDocument& doc = panel->mcp_doc();
    SketchPlane  pl;
    int          body = -1;
    const json   at   = cad_hole_placement(doc, params, pl, body);
    const double x = at.at("x").get<double>(), y = at.at("y").get<double>();
    return hole_reply(add_feature(panel, "hole_index", [&] {
        const int h = doc.add_hole(dia, depth, thru, x, y, pl, panel->mcp_feature_name(_L_CONTEXT("Hole", "Design")));
        doc.features[h].target_body = body;
        return h;
    }), at, pl);
}

json action_hole_styled(DesignPanel* panel, const json& params)
{
    const double dia   = params.value("diameter", 5.0);
    const double depth = params.value("depth", 10.0);
    const bool   thru  = params.value("through", true);
    const int    style          = params.value("style", 0);
    const double cbore_diameter = params.value("cbore_diameter", 0.0);
    const double cbore_depth    = params.value("cbore_depth", 0.0);
    const double csink_diameter = params.value("csink_diameter", 0.0);
    const double csink_angle    = params.value("csink_angle", 90.0);
    const std::string standard  = params.value("standard", std::string(""));
    if (dia <= 0) throw std::runtime_error("diameter must be > 0");
    CadDocument& doc = panel->mcp_doc();
    SketchPlane  pl;
    int          body = -1;
    const json   at   = cad_hole_placement(doc, params, pl, body);
    const double x = at.at("x").get<double>(), y = at.at("y").get<double>();
    return hole_reply(add_feature(panel, "hole_index", [&] {
        const int h = doc.add_hole_styled(dia, depth, thru, x, y, pl, style,
                                          cbore_diameter, cbore_depth,
                                          csink_diameter, csink_angle, standard,
                                          panel->mcp_feature_name(_L_CONTEXT("Hole", "Design")));
        doc.features[h].target_body = body;
        return h;
    }), at, pl);
}

json action_hole_standard(DesignPanel* panel, const json& params)
{
    const std::string desig = params.value("designation", std::string(""));
    if (desig.empty()) throw std::runtime_error("designation is required");
    const int    style  = params.value("style", 0);
    const bool   thru   = params.value("through", true);
    const double depth  = params.value("depth", 10.0);
    CadDocument& doc = panel->mcp_doc();
    SketchPlane  pl;
    int          body = -1;
    const json   at   = cad_hole_placement(doc, params, pl, body);
    const double x = at.at("x").get<double>(), y = at.at("y").get<double>();
    // add_hole_standard throws on an unknown designation; add_feature rolls that back too.
    return hole_reply(add_feature(panel, "hole_index", [&] {
        const int h = doc.add_hole_standard(desig, style, thru, depth, x, y, pl,
                                            panel->mcp_feature_name(_L_CONTEXT("Hole", "Design")));
        doc.features[h].target_body = body;
        return h;
    }), at, pl);
}

json action_boolean(DesignPanel* panel, const json& params)
{
    BooleanMode m = bool_from(params.value("op", std::string("subtract")));
    if (m == BooleanMode::New) throw std::runtime_error("op must be union | subtract | intersect");
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

json action_pattern(DesignPanel* panel, const json& params)
{
    const bool   circular = params.value("circular", false);
    const int    count    = params.value("count", 3);
    const double spacing  = params.value("spacing", 10.0);   // linear step (mm)
    const int    dir      = params.value("dir", 0);          // 0 = plane X, 1 = plane Y
    const double angle    = params.value("angle", 360.0);    // circular total sweep (deg)
    if (count < 1) throw std::runtime_error("count must be >= 1");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to pattern");
    int bi = target_body_arg(params, doc);
    const SketchPlane pl = cad_resolve_plane(params.value("plane", json("XY")), doc, false);
    doc.checkpoint();
    int p = doc.add_pattern(circular, count, spacing, dir, angle, bi, panel->mcp_feature_name(_L_CONTEXT("Pattern", "Design")));
    doc.features[p].plane = pl;   // axis (circular) / step dirs (linear)
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"pattern_index", p}, {"bodies", int(doc.bodies.size())}, {"error", doc.error},
                {"frame", frame_json(pl)}};
}

json action_pattern_on_curve(DesignPanel* panel, const json& params)
{
    const int count  = params.value("count", 3);
    if (count < 1) throw std::runtime_error("count must be >= 1");
    if (!params.contains("sketch")) throw std::runtime_error("pattern_on_curve needs 'sketch' (feature index)");
    if (!params.contains("entity")) throw std::runtime_error("pattern_on_curve needs 'entity' (entity index)");
    const int sketch = params["sketch"].get<int>();
    const int entity = params["entity"].get<int>();
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to pattern");
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int p = doc.add_pattern_on_curve(count, sketch, entity, bi, panel->mcp_feature_name(_L_CONTEXT("Pattern", "Design")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"pattern_index", p}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_shell(DesignPanel* panel, const json& params)
{
    const double thickness = params.value("thickness", 1.0);
    if (thickness <= 0) throw std::runtime_error("thickness must be > 0");
    const int face = params.value("face", -1);   // face id to leave open (-1 = closed hollow)
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to shell");
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
    if (doc.bodies.empty()) throw std::runtime_error("no body to rib");
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

json action_surface_extrude(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("surface_extrude needs 'sketch' (feature index)");
    CadDocument& doc = panel->mcp_doc();
    int sketch = params["sketch"].get<int>();
    double distance = params.value("distance", 10.0);
    doc.checkpoint();
    int idx = doc.add_surface_extrude(sketch, distance, panel->mcp_feature_name(_L("Surface Extrude")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_revolve(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("surface_revolve needs 'sketch' (feature index)");
    CadDocument& doc = panel->mcp_doc();
    int sketch = params["sketch"].get<int>();
    double angle = params.value("angle", 360.0);
    int axis = params.value("axis", 0);
    doc.checkpoint();
    int idx = doc.add_surface_revolve(sketch, angle, axis, panel->mcp_feature_name(_L("Surface Revolve")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_thicken_surface(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no sheet body to thicken");
    int bi = target_body_arg(params, doc);
    double thickness = params.value("thickness", 2.0);
    bool flip = params.value("flip", false);
    doc.checkpoint();
    int idx = doc.add_thicken_surface(bi, thickness, flip, panel->mcp_feature_name(_L("Thicken Surface")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_offset(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no sheet body to offset");
    int bi = target_body_arg(params, doc);
    double offset = params.value("offset", 1.0);
    doc.checkpoint();
    int idx = doc.add_surface_offset(bi, offset, panel->mcp_feature_name(_L("Surface Offset")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_loft(DesignPanel* panel, const json& params)
{
    if (!params.contains("profiles") || !params["profiles"].is_array())
        throw std::runtime_error("surface_loft needs 'profiles' (array of int feature indices)");
    CadDocument& doc = panel->mcp_doc();
    std::vector<int> profiles;
    for (const json& j : params["profiles"]) profiles.push_back(j.get<int>());
    bool ruled = params.value("ruled", false);
    doc.checkpoint();
    int idx = doc.add_surface_loft(profiles, ruled, panel->mcp_feature_name(_L("Surface Loft")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_fill(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("surface_fill needs 'sketch' (feature index)");
    CadDocument& doc = panel->mcp_doc();
    int sketch = params["sketch"].get<int>();
    doc.checkpoint();
    int idx = doc.add_surface_fill(sketch, panel->mcp_feature_name(_L("Surface Fill")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_draft(DesignPanel* panel, const json& params)
{
    if (!params.contains("face")) throw std::runtime_error("draft needs 'face' (id from query_topology)");
    const double angle = params.value("angle", 5.0);
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to draft");
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int d = doc.add_draft(angle, params["face"].get<int>(), bi, panel->mcp_feature_name(_L("Draft")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"draft_index", d}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// ---- 2D sketch verbs --------------------------------------------------------------------
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
//     them, `sketch_heal` welds and constrains them. That is what turns a loop that is closed by
//     floating-point luck into one that is closed by construction and stays closed through
//     every later solve.
//
// What is NOT taken over: FreeCAD's Sketcher is a modal dialog with its own toolbars. Here the
// vocabulary is the right-click offer, so these verbs are named after what the menu offers on a
// selection, and every one of them drives the SAME DesignSketchTool the mouse drives.

DesignSketchTool& mcp_sketch(DesignPanel* panel)
{
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr) throw std::runtime_error("no viewport");
    if (!vp->is_sketching())
        throw std::runtime_error("no sketch is open — call sketch_begin first");
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
    // than an error would have been. `p0` is accepted as an alias because that is exactly what a
    // circle stores internally (e.p0 = e.center below), so a caller who writes p0 means centre.
    auto centre_of = [&](const char* what) {
        if (j.contains("center")) return p("center", 0, 0);
        if (j.contains("centre")) return p("centre", 0, 0);
        if (j.contains("p0"))     return p("p0", 0, 0);
        throw std::runtime_error(std::string(what) + " needs a 'center' (or 'p0')");
    };
    if (t == "line") {
        // Its two ends are the line, so one left out is a mistake (often the key spelled another
        // way, start/end), not the origin: a line ending there closes no loop the caller drew.
        if (!j.contains("p0") || !j.contains("p1"))
            throw std::runtime_error("line needs 'p0' and 'p1' as [x, y]");
        e.type = SketchEntity::Type::Line;
        e.p0 = p("p0", 0, 0); e.p1 = p("p1", 0, 0);
    } else if (t == "circle") {
        e.type   = SketchEntity::Type::Circle;
        e.center = centre_of("circle");
        e.radius = j.value("radius", 0.0);
        e.p0     = e.center;
        if (e.radius <= 0.0) throw std::runtime_error("circle needs a positive 'radius'");
    } else if (t == "arc") {
        e.type        = SketchEntity::Type::Arc;
        e.center      = centre_of("arc");
        e.radius      = j.value("radius", 0.0);
        e.start_angle = angle_arg(j, "start_angle", 0.0);
        e.end_angle   = angle_arg(j, "end_angle", 0.0);
        if (e.radius <= 0.0) throw std::runtime_error("arc needs a positive 'radius'");
        e.p0 = e.center + e.radius * Vec2d(std::cos(e.start_angle), std::sin(e.start_angle));
        e.p1 = e.center + e.radius * Vec2d(std::cos(e.end_angle),   std::sin(e.end_angle));
    } else if (t == "point") {
        e.type = SketchEntity::Type::Point;
        e.p0   = p("p", 0, 0);
    } else {
        throw std::runtime_error("unknown entity type '" + t + "' (line, arc, circle, point, polygon, slot, ellipse, ellipse_arc, spline)");
    }
    return e;
}

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
    // Ellipses and splines used to serialise as a TYPE NAME and nothing else, so every
    // parameter they have was invisible to the only read-back this project has. A ladder could
    // count them and grade the faceted area of the loop they close (2e-2, the faceting error) —
    // it could not check a single axis, angle or pole. "Precise definition of every aspect"
    // cannot be asserted about an entity whose aspects the instrument cannot see.
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

// --- A feature as its card shows it: describe_feature and set_feature ------------------------

// Names in the cad_call vocabulary, in enum order.
const char* const kModes[]          = {"new", "union", "subtract", "intersect"};   // BooleanMode
const char* const kShapes[]         = {"rectangle", "circle"};                     // SketchShape
const char* const kExtrudeEnds[]    = {"blind", "symmetric", "two_sided", "through_all", "up_to_face", "up_to_vertex"};
const char* const kFaceGroups[]     = {"top", "bottom", "lateral", "all"};
const char* const kPlaneTypes[]     = {"offset", "angle", "midplane", "tangent", "two_edges", "coincident"};
const char* const kAxisTypes[]      = {"two_points", "face_normal", "cylinder", "plane_intersection", "along_edge"};
const char* const kCoordSysTypes[]  = {"point_world", "face_and_direction"};
const char* const kPointRoles[]     = {"p0", "p1", "center"};                      // SketchPointRole
// SketchConstraintType is append-only (recipes store it by position), and so is this list.
const char* const kConstraintTypes[] = {
    "fix", "coincident", "horizontal", "vertical", "distance", "lock_x", "lock_y", "equal_length", "parallel",
    "perpendicular", "concentric", "tangent", "midpoint", "symmetric", "angle", "radius", "diameter", "point_on_line",
    "point_on_object", "equal_radius", "collinear", "distance_x", "distance_y", "symmetric_about_y", "symmetric_about_x"};
static_assert(std::size(kConstraintTypes) == size_t(SketchConstraintType::SymmetricAboutX) + 1,
              "every constraint type needs a name");

// A recipe is read from a file, so an enum value may be one this build has no name for.
template<size_t N> const char* enum_name(const char* const (&names)[N], int i)
{
    return i >= 0 && i < int(N) ? names[i] : "unknown";
}

// The position of `v` in `names`, or an error naming the choices.
template<size_t N> int enum_index(const std::string& key, const json& v, const char* const (&names)[N])
{
    for (size_t i = 0; i < N; ++i)
        if (v.is_string() && v.get<std::string>() == names[i]) return int(i);
    std::string list;
    for (const char* n : names) list += (list.empty() ? "" : ", ") + std::string(n);
    throw std::runtime_error(key + " must be one of " + list);
}

// A constraint's value as cad_call states it: an angle in degrees (the recipe holds radians), a
// length in mm. Null for the types that carry none.
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

json constraint_ref(int entity, SketchPointRole role, bool point)
{
    if (entity == kSketchRefOrigin) return json{{"entity", "origin"}};
    if (entity == kSketchRefAxisX)  return json{{"entity", "x_axis"}};
    if (entity == kSketchRefAxisY)  return json{{"entity", "y_axis"}};
    json r{{"entity", entity}};
    if (point) r["role"] = enum_name(kPointRoles, int(role));
    return r;
}

// An entity constraint's references, as the solver reads them: a point names its entity and role,
// a whole entity (a line's direction, a circle's radius) only the entity.
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

// A profile sketch's constraint, on the profile's point indices.
json profile_constraint_json(const SketchConstraintDef& c, int index)
{
    json points = json::array();
    for (int p : {c.a, c.b, c.c, c.d})
        if (p >= 0) points.push_back(p);
    json j{{"index", index}, {"type", enum_name(kConstraintTypes, int(c.type))}, {"points", std::move(points)}};
    if (json v = constraint_value(c.type, c.value); !v.is_null()) j["value"] = std::move(v);
    return j;
}

// A feature's card as data. `params` is what the card shows; `editable` the fields its Confirm
// writes, named as the CadFeature members (the names set_feature_expr binds); `ranges` the limits
// its spin controls set on the numeric ones; `refs` the structure the card keeps as it is (feature
// and body indices, face and edge ids, which expire with every rebuild); `expr` the bindings.
// describe_feature shows it and set_feature checks against it, so the two cannot disagree.
json feature_card(const CadFeature& f)
{
    json params = json::object(), ranges = json::object(), refs = json::object(), editable = json::array();
    auto show = [&](const char* k, json v) { params[k] = std::move(v); };
    auto edit = [&](const char* k, json v) { params[k] = std::move(v); editable.push_back(k); };
    auto num  = [&](const char* k, double v, double lo, double hi) { edit(k, v); ranges[k] = json::array({lo, hi}); };
    auto ref  = [&](const char* k, json v) { refs[k] = std::move(v); };
    using T = CadFeatureType;
    switch (f.type) {
    case T::Sketch:
        if (!f.imported_regions.empty()) {
            // Text or SVG art, vectorised once: only its placement edits without the Text dialog.
            if (f.is_text()) {
                show("text_string", f.text_string);
                show("text_font", f.text_font);
                show("text_height", f.text_height);
            }
            show("regions", int(f.imported_regions.size()));
            show("import_offset", json::array({f.import_offset.x(), f.import_offset.y()}));
            num("import_scale_x", f.import_scale_x, 0.001, 1000);
            num("import_scale_y", f.import_scale_y, 0.001, 1000);
            if (f.import_on_face) ref("import_face_body", f.import_face_body);
        } else if (f.entities.empty() && f.profile.points.empty()) {   // the Sketch card's shape
            edit("shape", enum_name(kShapes, int(f.shape)));
            num("width", f.width, 0.1, 1000);
            num("height", f.height, 0.1, 1000);
            num("radius", f.radius, 0.1, 1000);
        }
        break;
    case T::Extrude:
        num("distance", f.distance, -1000, 1000);
        num("distance2", f.distance2, 0, 100000);
        num("taper_deg", f.taper_deg, -89, 89);
        edit("extrude_end", enum_name(kExtrudeEnds, int(f.extrude_end)));
        edit("flip", f.flip);
        edit("mode", enum_name(kModes, int(f.mode)));
        ref("sketch_ref", f.sketch_ref);
        ref("target_body", f.target_body);
        ref("up_to_face", f.up_to_face);
        ref("extrude_src_face", f.extrude_src_face);
        break;
    case T::Revolve:
        num("revolve_angle", f.revolve_angle, 1, 360);
        edit("revolve_axis", f.revolve_axis);
        edit("flip", f.flip);
        edit("mode", enum_name(kModes, int(f.mode)));
        ref("sketch_ref", f.sketch_ref);
        ref("revolve_axis_entity", f.revolve_axis_entity);
        ref("target_body", f.target_body);
        break;
    case T::Sweep:
        edit("mode", enum_name(kModes, int(f.mode)));
        ref("sketch_ref", f.sketch_ref);
        ref("sweep_path_ref", f.sweep_path_ref);
        ref("target_body", f.target_body);
        break;
    case T::Loft:
        edit("mode", enum_name(kModes, int(f.mode)));
        edit("loft_ruled", f.loft_ruled);
        ref("loft_profile_refs", f.loft_profile_refs);
        ref("target_body", f.target_body);
        break;
    case T::Fillet:
    case T::Chamfer:
        num("dressup_size", f.dressup_size, 0.1, 1000);
        ref("edges", f.dressup_edge_ids());
        ref("face_group", enum_name(kFaceGroups, int(f.face_group)));
        ref("target_body", f.target_body);
        break;
    case T::Hole:
        num("hole_diameter", f.hole_diameter, 0.1, 1000);
        num("hole_depth", f.hole_depth, 0.1, 1000);
        edit("hole_through", f.hole_through);
        num("hole_x", f.hole_x, -1000, 1000);
        num("hole_y", f.hole_y, -1000, 1000);
        // The seat is not on the card; hole_styled and hole_standard set it. 0 = simple.
        edit("hole_style", f.hole_style);
        num("hole_cbore_diameter", f.hole_cbore_diameter, 0, 1000);
        num("hole_cbore_depth", f.hole_cbore_depth, 0, 1000);
        num("hole_csink_diameter", f.hole_csink_diameter, 0, 1000);
        num("hole_csink_angle", f.hole_csink_angle, 1, 179);
        show("hole_standard", f.hole_standard);
        ref("target_body", f.target_body);
        break;
    case T::Thread:
        // The card shows the diameter, the size a thread is named by; either one may be set.
        num("thread_diameter", 2.0 * f.thread_radius, 0.1, 1000);
        num("thread_radius", f.thread_radius, 0.05, 500);
        num("thread_pitch", f.thread_pitch, 0.1, 1000);
        num("thread_height", f.thread_height, 0.1, 1000);
        num("thread_depth", f.thread_depth, 0.1, 1000);
        edit("thread_internal", f.thread_internal);
        num("thread_x", f.thread_x, -1000, 1000);
        num("thread_y", f.thread_y, -1000, 1000);
        ref("target_body", f.target_body);
        break;
    case T::Shell:
        num("shell_thickness", f.shell_thickness, 0.01, 100000);
        ref("shell_face", f.shell_face);
        ref("target_body", f.target_body);
        break;
    case T::Draft:
        num("draft_angle", f.draft_angle, -89, 89);
        ref("draft_face", f.draft_face);
        ref("target_body", f.target_body);
        break;
    case T::Thicken:
        num("thicken_thickness", f.thicken_thickness, 0.01, 100000);
        edit("thicken_flip", f.thicken_flip);
        ref("thicken_face", f.thicken_face);
        ref("target_body", f.target_body);
        break;
    case T::ThickenSurface:
        num("thicken_thickness", f.thicken_thickness, 0.01, 100000);
        edit("thicken_flip", f.thicken_flip);
        ref("target_body", f.target_body);
        break;
    case T::Pattern:
        edit("pattern_circular", f.pattern_circular);
        num("pattern_count", f.pattern_count, 1, 999);
        num("pattern_spacing", f.pattern_spacing, 0.01, 100000);
        edit("pattern_dir", f.pattern_dir);
        num("pattern_angle", f.pattern_angle, 1, 360);
        ref("pattern_curve_sketch", f.pattern_curve_sketch);
        ref("pattern_curve_entity", f.pattern_curve_entity);
        ref("target_body", f.target_body);
        break;
    case T::Boolean:
        edit("mode", enum_name(kModes, int(f.mode)));
        edit("bool_keep_tool", f.bool_keep_tool);
        num("bool_tolerance", f.bool_tolerance, 0, 100);
        ref("target_body", f.target_body);
        ref("bool_tool_body", f.bool_tool_body);
        ref("bool_target_face", f.bool_target_face);
        ref("bool_tool_face", f.bool_tool_face);
        break;
    case T::Cut:
        num("cut_offset", f.cut_offset, -10000, 10000);
        edit("cut_flip", f.cut_flip);
        edit("cut_keep_upper", f.cut_keep_upper);
        edit("cut_keep_lower", f.cut_keep_lower);
        ref("cut_face_body", f.cut_face_body);
        ref("cut_face", f.cut_face);
        ref("target_body", f.target_body);
        break;
    case T::Mirror:
        edit("mirror_keep_original", f.mirror_keep_original);
        edit("mode", enum_name(kModes, int(f.mode)));
        ref("target_body", f.target_body);
        break;
    case T::Transform:
        edit("xf_translate", vec3(f.xf_translate));
        edit("xf_axis", vec3(f.xf_axis));
        edit("xf_pivot", vec3(f.xf_pivot));
        num("xf_angle_deg", f.xf_angle_deg, -360, 360);
        edit("xf_copy", f.xf_copy);
        ref("target_body", f.target_body);
        break;
    case T::Plane:
        num("plane_offset", f.plane_offset, -100000, 100000);
        num("plane_angle_tilt", f.plane_angle_tilt, -180, 180);
        num("plane_u_size", f.plane_u_size, 1, 100000);
        num("plane_v_size", f.plane_v_size, 1, 100000);
        ref("plane_type", enum_name(kPlaneTypes, int(f.plane_type)));
        ref("plane_base", f.plane_base);
        ref("plane_axis", f.plane_axis);
        ref("plane_face_body", f.plane_face_body);
        ref("plane_face", f.plane_face);
        ref("plane_face2_body", f.plane_face2_body);
        ref("plane_face2", f.plane_face2);
        ref("plane_edge_body", f.plane_edge_body);
        ref("plane_edge", f.plane_edge);
        ref("plane_edge2_body", f.plane_edge2_body);
        ref("plane_edge2", f.plane_edge2);
        break;
    case T::Helix:
        num("helix_radius", f.helix_radius, 0.01, 10000);
        num("helix_pitch", f.helix_pitch, 0.01, 10000);
        num("helix_height", f.helix_height, 0.01, 10000);
        num("helix_taper_deg", f.helix_taper_deg, -89, 89);
        edit("helix_left_handed", f.helix_left_handed);
        break;
    case T::Rib:
        num("rib_thickness", f.rib_thickness, 0.01, 100000);
        num("rib_depth", f.rib_depth, 0.01, 100000);
        ref("rib_sketch_ref", f.rib_sketch_ref);
        ref("rib_entity", f.rib_entity);
        ref("target_body", f.target_body);
        break;
    case T::SurfaceExtrude:
        num("distance", f.distance, 0.1, 1000);
        ref("sketch_ref", f.sketch_ref);
        break;
    case T::SurfaceRevolve:
        num("revolve_angle", f.revolve_angle, 1, 360);
        show("revolve_axis", f.revolve_axis);
        show("flip", f.flip);
        ref("sketch_ref", f.sketch_ref);
        ref("revolve_axis_entity", f.revolve_axis_entity);
        break;
    case T::SurfaceOffset:
        num("plane_offset", f.plane_offset, -100000, 100000);
        ref("target_body", f.target_body);
        break;
    case T::Mate:
        edit("mate_kind", f.mate_kind);
        num("mate_offset", f.mate_offset, -100000, 100000);
        num("mate_angle", f.mate_angle, -360, 360);
        edit("mate_flip", f.mate_flip);
        ref("mate_cs_a", f.mate_cs_a);
        ref("mate_cs_b", f.mate_cs_b);
        break;
    case T::Axis:
        ref("axis_type", enum_name(kAxisTypes, int(f.axis_type)));
        ref("axis_p1", vec3(f.axis_p1));
        ref("axis_p2", vec3(f.axis_p2));
        ref("axis_body", f.axis_body);
        ref("axis_face", f.axis_face);
        ref("axis_edge", f.axis_edge);
        ref("axis_plane_a", f.axis_plane_a);
        ref("axis_plane_b", f.axis_plane_b);
        break;
    case T::CoordSys:
        ref("coordsys_type", enum_name(kCoordSysTypes, int(f.coordsys_type)));
        ref("coordsys_point", vec3(f.coordsys_point));
        ref("coordsys_body", f.coordsys_body);
        ref("coordsys_face", f.coordsys_face);
        ref("coordsys_edge", f.coordsys_edge);
        ref("coordsys_x_hint", vec3(f.coordsys_x_hint));
        break;
    case T::Project:
        ref("project_source_body", f.project_source_body);
        ref("project_edges", f.project_edges);
        ref("project_face", f.project_face);
        break;
    case T::DeleteFace:
        ref("delete_faces", f.delete_faces);
        ref("target_body", f.target_body);
        break;
    case T::SurfaceLoft:
        show("loft_ruled", f.loft_ruled);
        ref("loft_profile_refs", f.loft_profile_refs);
        break;
    case T::SurfaceFill:
        ref("sketch_ref", f.sketch_ref);
        break;
    case T::Import:
        break;
    }
    json expr = json::object();
    for (const auto& [field, e] : f.expr) expr[field] = e;
    return json{{"params", std::move(params)}, {"editable", std::move(editable)}, {"ranges", std::move(ranges)},
                {"expr", std::move(expr)}, {"refs", std::move(refs)}};
}

// Writes one editable field that is not a bindable number: a card's check box, choice or vector.
// Throws for a value of the wrong type or one the card does not offer.
void set_card_choice(CadFeature& f, const std::string& k, const json& v)
{
    auto flag = [&](bool& field) {
        if (!v.is_boolean()) throw std::runtime_error(k + " must be true or false");
        field = v.get<bool>();
    };
    auto choice = [&](int& field, int hi) {
        if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() > hi)
            throw std::runtime_error(k + " must be an integer from 0 to " + std::to_string(hi));
        field = v.get<int>();
    };
    auto point3 = [&](Vec3d& field, bool direction) {
        if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number())
            throw std::runtime_error(k + " must be [x, y, z]");
        const Vec3d p(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
        if (direction && !(p.norm() > 1e-9)) throw std::runtime_error(k + " must not be zero");
        if (!direction && !(p.cwiseAbs().maxCoeff() <= 100000))
            throw std::runtime_error(k + " must be between -100000 and 100000 on each axis");
        field = p;
    };
    if (k == "flip")                 return flag(f.flip);
    if (k == "hole_through")         return flag(f.hole_through);
    if (k == "thread_internal")      return flag(f.thread_internal);
    if (k == "pattern_circular")     return flag(f.pattern_circular);
    if (k == "loft_ruled")           return flag(f.loft_ruled);
    if (k == "bool_keep_tool")       return flag(f.bool_keep_tool);
    if (k == "cut_flip")             return flag(f.cut_flip);
    if (k == "cut_keep_upper")       return flag(f.cut_keep_upper);
    if (k == "cut_keep_lower")       return flag(f.cut_keep_lower);
    if (k == "mirror_keep_original") return flag(f.mirror_keep_original);
    if (k == "thicken_flip")         return flag(f.thicken_flip);
    if (k == "helix_left_handed")    return flag(f.helix_left_handed);
    if (k == "xf_copy")              return flag(f.xf_copy);
    if (k == "mate_flip")            return flag(f.mate_flip);
    if (k == "hole_style")           return choice(f.hole_style, 2);
    if (k == "pattern_dir")          return choice(f.pattern_dir, 1);
    if (k == "mate_kind")            return choice(f.mate_kind, 4);
    if (k == "revolve_axis") {
        // Choosing the plane's X or Y axis on the card drops a sketch line chosen as the axis.
        choice(f.revolve_axis, 1);
        f.revolve_axis_entity = -1;
        return;
    }
    if (k == "xf_translate") return point3(f.xf_translate, false);
    if (k == "xf_pivot")     return point3(f.xf_pivot, false);
    if (k == "xf_axis")      return point3(f.xf_axis, true);
    if (k == "shape") {
        f.shape = SketchShape(enum_index(k, v, kShapes));
        return;
    }
    if (k == "mode") {
        // A Boolean combines two bodies, so it makes no new one; a Mirror adds its mirrored body
        // or fuses it back.
        const BooleanMode m = BooleanMode(enum_index(k, v, kModes));
        if (f.type == CadFeatureType::Boolean && m == BooleanMode::New)
            throw std::runtime_error("a Boolean's mode is union, subtract or intersect");
        if (f.type == CadFeatureType::Mirror && m != BooleanMode::New && m != BooleanMode::Add)
            throw std::runtime_error("a Mirror's mode is new or union");
        f.mode = m;
        return;
    }
    if (k == "extrude_end") {
        // The face or vertex an end goes up to is picked on the canvas; set_feature keeps the
        // feature's own pick and cannot make one.
        const ExtrudeEnd e = ExtrudeEnd(enum_index(k, v, kExtrudeEnds));
        if (e == ExtrudeEnd::UpToFace && f.up_to_face < 0)
            throw std::runtime_error("extrude_end up_to_face needs a target face, which only the Extrude card picks");
        if (e == ExtrudeEnd::UpToVertex && f.extrude_end != ExtrudeEnd::UpToVertex)
            throw std::runtime_error("extrude_end up_to_vertex needs a target vertex, which only the Extrude card picks");
        f.extrude_end = e;
        return;
    }
    throw std::runtime_error(k + " cannot be set");
}

void check_feature_index(int fi, const CadDocument& doc)
{
    if (fi < 0 || fi >= int(doc.features.size()))
        throw std::runtime_error("feature " + std::to_string(fi) + " is out of range; the design has " +
                                 std::to_string(doc.features.size()) + " features");
}

// The `feature` param (or `index`, as agents often call it): an index into describe_scene's features.
int feature_arg(const json& params, const CadDocument& doc)
{
    const json v = !params.is_object() ? json() : params.contains("feature") ? params["feature"] : params.value("index", json());
    if (!v.is_number_integer()) throw std::runtime_error("feature must be a feature index (describe_scene's features[].index)");
    check_feature_index(v.get<int>(), doc);
    return v.get<int>();
}

json action_describe_feature(DesignPanel* panel, const json& params)
{
    const CadDocument& doc = panel->mcp_doc();
    return cad_feature_json(doc, feature_arg(params, doc));
}

// A card's Confirm in edit mode (DesignPanel::confirm_tool): one undo step, then replace_feature(),
// which rebuilds and puts the old features back if the rebuild fails.
json action_set_feature(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const int    fi  = feature_arg(params, doc);
    // The card's values go in fields; params and values are what agents often call them.
    const char* key = params.contains("fields") ? "fields" : params.contains("params") ? "params" : params.contains("values") ? "values" : nullptr;
    if (key == nullptr) throw std::runtime_error("set_feature needs 'fields', as {\"distance\": 15}");
    CadFeature edited = doc.features[fi];
    cad_apply_feature_fields(edited, params[key]);
    const json before = feature_card(doc.features[fi])["params"];
    doc.checkpoint();
    if (!doc.replace_feature(fi, edited)) {
        // The features are back already; rollback() also takes back the undo step and restores
        // the redo branch, so the refused call leaves the history as it found it.
        const std::string why = doc.error.empty() ? "the model could not be rebuilt" : doc.error;
        doc.rollback();
        doc.error = why;
        panel->mcp_after_change(false);
        throw CadCallError(-32000, why);
    }
    panel->mcp_after_change();
    const json after   = feature_card(doc.features[fi])["params"];
    json       changed = json::object();
    for (const auto& [k, v] : after.items()) {
        const json old = before.value(k, json());
        if (old != v) changed[k] = json::array({old, v});
    }
    return json{{"ok", true}, {"feature", fi}, {"changed", std::move(changed)}, {"bodies", int(doc.bodies.size())},
                {"generation", doc.topo_generation}, {"error", doc.error}};
}

// Which entities a verb acts on: an explicit "entities" array, else the current selection,
// else — only where the verb says so — everything. Same precedence the menu uses: what you
// pointed at wins, and the menu never silently acts on the whole sketch.
std::vector<int> sketch_targets(const json& params, DesignSketchTool& t, bool all_if_empty)
{
    std::vector<int> out;
    if (params.contains("entities")) {
        for (const auto& v : params.at("entities")) out.push_back(v.get<int>());
        return out;
    }
    out = t.selection();
    if (out.empty() && all_if_empty)
        for (int i = 0; i < int(t.entities().size()); ++i) out.push_back(i);
    return out;
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
json action_sketch_begin(DesignPanel* panel, const json& params)
{
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr) throw std::runtime_error("no viewport");
    if (vp->is_sketching()) throw std::runtime_error("a sketch is already open");
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

// The sketch's Confirm. An empty sketch is refused before anything changes, where the GUI's commit
// drops it with a status line: a failed call is left out of an exported script, so the session
// must still be open when the script's next sketch call replays.
json action_sketch_commit(DesignPanel* panel, const json& params)
{
    (void)params;
    DesignSketchTool& t = mcp_sketch(panel);
    if (t.entities().empty()) throw std::runtime_error("the sketch is empty; add entities or call sketch_cancel");
    CadDocument&  doc    = panel->mcp_doc();
    const size_t  before = doc.features.size();
    DesignCanvas* vp     = panel->mcp_viewport();
    vp->sketch_confirm_pending();   // a ready edit-op or chain is kept, as the ✓ keeps it
    vp->finish_sketch();            // the commit handler checkpoints, names, solves and rebuilds
    panel->mcp_set_sketch_mode(false);
    panel->mcp_after_change();
    if (doc.features.size() <= before) throw std::runtime_error("the sketch was not committed");
    // Read back from the committed feature: solving its dimensions can move geometry, and extrude
    // takes `loop` against these entities.
    const CadFeature& f = doc.features[before];
    json out{{"ok", true}, {"sketch_index", int(before)}, {"name", f.name}, {"features", int(doc.features.size())}};
    out.update(sketch_report(t.loop_report(f.entities)));
    return out;
}

json action_sketch_cancel(DesignPanel* panel, const json& params)
{
    (void)params;
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr || !vp->is_sketching()) throw std::runtime_error("no sketch is open");
    vp->cancel_sketch();
    panel->mcp_set_sketch_mode(false);
    return json{{"ok", true}};
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

json action_sketch_construction(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    const std::vector<int> tgt = sketch_targets(params, t, false);
    if (tgt.empty()) throw std::runtime_error("nothing selected and no 'entities' given");
    t.select_indices(tgt);
    const int n = t.toggle_selection_construction();
    panel->mcp_viewport()->request_repaint();
    return json{{"ok", n > 0}, {"changed", n}};
}

json action_sketch_offset(DesignPanel* panel, const json& params)
{
    if (!params.contains("distance")) throw std::runtime_error("sketch_offset needs 'distance'");
    DesignSketchTool& t = mcp_sketch(panel);
    const double d = params.at("distance").get<double>();
    const std::vector<int> tgt = sketch_targets(params, t, true);
    std::vector<SketchEntity> src;
    for (int i : tgt)
        if (i >= 0 && i < int(t.entities().size())) src.push_back(t.entities()[i]);
    if (src.empty()) throw std::runtime_error("nothing to offset");
    const auto out = SketchEngine::offset_entities(src, d);
    if (out.empty()) throw std::runtime_error("offset produced nothing (ellipses and splines are not offset)");
    const int base = t.add_entities_scripted(out);
    panel->mcp_viewport()->request_repaint();
    return json{{"ok", true}, {"first_index", base}, {"added", int(out.size())}, {"dof", t.dof()}};
}

json action_sketch_mirror(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    auto pt = [&](const char* k, double dx, double dy) {
        if (!params.contains(k)) return Vec2d(dx, dy);
        const json& a = params.at(k);
        if (!a.is_array() || a.size() < 2) throw std::runtime_error(std::string(k) + " must be [x, y]");
        return Vec2d(a[0].get<double>(), a[1].get<double>());
    };
    const Vec2d a = pt("axis_a", 0, 0), b = pt("axis_b", 0, 1);
    const std::vector<int> tgt = sketch_targets(params, t, true);
    std::vector<SketchEntity> src;
    for (int i : tgt)
        if (i >= 0 && i < int(t.entities().size())) src.push_back(t.entities()[i]);
    if (src.empty()) throw std::runtime_error("nothing to mirror");
    const auto out = SketchEngine::mirror_entities(src, a, b);
    const int base = t.add_entities_scripted(out);
    panel->mcp_viewport()->request_repaint();
    return json{{"ok", true}, {"first_index", base}, {"added", int(out.size())}, {"dof", t.dof()}};
}

// The `sketch` a method reads or builds from: a Sketch feature, or a Project feature, whose
// entities the kernel builds as a sketch's.
int sketch_arg(const json& v, const CadDocument& doc)
{
    if (!v.is_number_integer()) throw std::runtime_error("sketch must be a feature index (describe_scene's features[].index)");
    const int si = v.get<int>();
    check_feature_index(si, doc);
    const CadFeature& f = doc.features[si];
    if (f.type != CadFeatureType::Sketch && f.type != CadFeatureType::Project)
        throw std::runtime_error("feature " + std::to_string(si) + " (" + feature_type_name(f.type) + ") is not a sketch");
    return si;
}

json action_sketch_describe(DesignPanel* panel, const json& params)
{
    // A committed sketch is read from the document, so no session is needed, nor disturbed.
    const json sketch = params.is_object() ? params.value("sketch", json()) : json();
    if (!sketch.is_null()) {
        const DesignCanvas* vp = panel->mcp_viewport();
        if (vp == nullptr) throw std::runtime_error("no viewport");
        const CadDocument& doc = panel->mcp_doc();
        return cad_sketch_describe(doc, vp->mcp_sketch_tool(), sketch_arg(sketch, doc));
    }
    DesignSketchTool& t = mcp_sketch(panel);
    json ents = json::array();
    for (int i = 0; i < int(t.entities().size()); ++i)
        ents.push_back(sketch_entity_to(t.entities()[i], i));
    // The armed TOOL and its pending anchors. Without these the only way to tell which tool a
    // menu row actually armed is to draw with it and infer from what came out — which is how a
    // menu walk that lands one row off gets diagnosed as "the tool is broken".
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
             {"dof", t.dof()},
             {"solve_ok", t.solve_ok()},
             {"tool", (mi >= 0 && mi < int(sizeof(kModeNames) / sizeof(kModeNames[0])))
                          ? kModeNames[mi] : "unknown"},
             {"pending", t.pending_points()},
             {"editing", t.value_field_open()},
             {"selection", t.selection()}};
    out.update(sketch_report(t.loop_report()));
    return out;
}

json action_sketch_validate(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    const double tol = params.value("tolerance", 1e-3);
    json out{{"ok", true}, {"tolerance", tol}, {"dof", t.dof()}, {"solve_ok", t.solve_ok()}};
    out.update(sketch_report(t.loop_report()));
    return out;
}

json action_sketch_heal(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    const double tol = params.value("tolerance", 1e-3);
    const bool   ic  = params.value("ignore_construction", true);
    const int welded = t.heal_coincidences(tol, ic);
    panel->mcp_viewport()->request_repaint();
    json out{{"ok", true}, {"welded", welded}, {"tolerance", tol},
             {"dof", t.dof()}, {"solve_ok", t.solve_ok()}};
    out.update(sketch_report(t.loop_report()));
    return out;
}

// Why sketch_set_value exists: committing a typed dimension could previously only be exercised
// by driving the in-canvas value field, and the rig's window manager never gives that frame
// keyboard focus, so the behaviour of apply_dimension on CONSTRAINED geometry was untestable.
// This calls the same function the widget calls, so the geometry can be asserted with no window
// manager involved. Note apply_dimension clears the selection.
json action_sketch_set_value(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    if (!params.contains("value")) throw std::runtime_error("sketch_set_value needs 'value'");
    const double value = params["value"].get<double>();
    const DesignSketchTool::DimType kind = t.dimension_kind();
    if (kind == DesignSketchTool::DimType::None)
        throw std::runtime_error("the selection has no value to set — pick a line, an arc, a circle, or two entities");

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

    // Validate BEFORE apply_dimension, at the socket boundary. The tool only MOVES geometry when
    // the value passes its own per-case thresholds, but it records the driving constraint
    // UNCONDITIONALLY afterwards — a negative/zero/NaN value that moved nothing would still be
    // pushed to the solver as a constraint it must satisfy and cannot, silently corrupting the
    // sketch rather than failing. (apply_dimension has the same flaw for any other caller; this
    // guard protects the socket, not the tool.) Fail loudly instead of recording a poison value.
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
             {"dof", t.dof()}, {"solve_ok", t.solve_ok()}};
    out.update(sketch_report(t.loop_report()));
    return out;
}

// --- Solids from a profile: extrude and revolve ------------------------------------------------
//
// With `sketch` they are the Extrude and Revolve cards' Confirm on a committed sketch (or one loop
// of it); without, they draw their own sketch first, a `profile` or a rectangle on `plane`.

// The card's mode choice. "auto" (where allowed) comes back empty. bool_from, which the older
// methods keep, makes a new body of a word it does not know; here that is an error.
std::optional<BooleanMode> boolean_arg(const json& params, const char* def, bool allow_auto)
{
    json v = params.value("boolean", json());
    if (v.is_null()) v = def;
    if (v.is_string()) {
        const std::string b = v.get<std::string>();
        if (allow_auto && b == "auto") return std::nullopt;
        for (size_t i = 0; i < std::size(kModes); ++i)
            if (b == kModes[i]) return BooleanMode(i);
    }
    throw std::runtime_error(allow_auto ? "boolean must be auto, new, union, subtract or intersect"
                                        : "boolean must be new, union, subtract or intersect");
}

// A join or cut needs something to work on, as the Revolve card says.
void check_has_body(BooleanMode mode, const CadDocument& doc)
{
    if (mode != BooleanMode::New && doc.bodies.empty())
        throw std::runtime_error("boolean " + std::string(kModes[int(mode)]) + " needs a body; add or import one first");
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

int revolve_axis_arg(const json& params)
{
    const int axis = params.value("axis", 0);
    if (axis != 0 && axis != 1) throw std::runtime_error("axis must be 0 (plane X) or 1 (plane Y)");
    return axis;
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

// The boolean a solid feature was built with and the body it went into, as the defaults resolved.
json body_reply(json r, const CadDocument& doc, const char* index_key)
{
    const int f  = r.at(index_key).get<int>();
    r["boolean"] = kModes[int(doc.features[f].mode)];
    r["body"]    = result_body(doc, f);
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
        if (vp == nullptr) throw std::runtime_error("no viewport");
        const auto add = cad_sketch_extrude(doc, vp->mcp_sketch_tool(), params);
        json r;
        try {
            r = add_feature(panel, "extrude_index",
                            [&] { return add(panel->mcp_feature_name(_L_CONTEXT("Extrude", "Design"))); });
        } catch (const CadCallError& e) {
            // A whole sketch builds one region, with the loops inside it as holes. When the rebuild
            // refused a sketch of several, name the way to take one.
            if (!loop.is_null() || separate_regions(vp->mcp_sketch_tool(), doc.features[sketch.get<int>()].entities) < 2)
                throw;
            throw CadCallError(e.code, std::string(e.what()) +
                                           "; pass \"loop\" to extrude one of its regions (closed_loops of sketch_describe {sketch})");
        }
        r = solid_reply(std::move(r), doc, "extrude_index", sketch.get<int>());
        r["end"] = kExtrudeEnds[int(end)];
        if (!loop.is_null()) r["loop"] = loop;
        return r;
    }
    if (!loop.is_null()) throw std::runtime_error("loop picks a loop of a committed sketch; give sketch too");

    const double      d    = distance_arg(params);
    const SketchPlane pl   = cad_resolve_plane(params.value("plane", json("XY")), doc, false);
    const auto        mode = boolean_arg(params, "new", true);
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
    const BooleanMode mode  = *boolean_arg(params, "new", false);
    const int         body  = std::max(-1, target_body_arg(params, doc));
    check_has_body(mode, doc);
    SketchProfile prof;
    const bool has_prof = profile_from(params, prof);
    double w = 0, h = 0;
    if (!has_prof) {   // a rectangle centred on the axis may self-overlap; offset it with `profile`
        w = params.value("width", 20.0); h = params.value("height", 10.0);
        if (w <= 0 || h <= 0) throw std::runtime_error("width and height must be > 0");
    }
    int s = -1;
    json r = add_feature(panel, "revolve_index", [&] {
        const std::string sketch_name = panel->mcp_feature_name(_L("Sketch"));
        s = has_prof ? doc.add_sketch_profile(prof, pl, sketch_name)
                     : doc.add_sketch(SketchShape::Rectangle, pl, w, h, 0.0, sketch_name);
        const int rv = doc.add_revolve(s, angle, axis, flip, mode, panel->mcp_feature_name(_L("Revolve")));
        doc.features[rv].target_body = body;
        return rv;
    });
    r = solid_reply(std::move(r), doc, "revolve_index", s);
    r["axis"] = axis;
    return r;
}

// Verbs whose handler opens a MODAL dialog. This matters more than it looks: the socket thread
// posts the call to the wx main thread and waits 15 s on a future, so a verb that blocks that
// thread inside ShowModal() times the RPC out AND leaves the main thread blocked until a human
// dismisses the dialog — every later call then times out too, 15 s at a time. Which is exactly
// the trap for the deck user this surface exists for: one button press and the app is modal,
// waiting for a mouse they may not be reaching for.
//
// So run_verb NEVER dispatches inline. The list here is only so list_verbs can label the keys
// that will want a mouse; re-derive it with
//     grep -n ShowModal src/slic3r/GUI/CAD/DesignPanel.cpp
// and map each handler back to its action string in DesignOffer.hpp.
bool verb_is_modal(const char* id)
{
    static const char* kModal[] = { "sk_text", "sk_svg", "colour" };
    for (const char* m : kModal)
        if (std::strcmp(m, id) == 0) return true;
    return false;
}

// Why list_verbs exists: the deck profile in VSD_n1_streamcontroller is built by parsing
// DesignPanel's key tables out of the SOURCE, so a verb without a keyboard shortcut is invisible
// to it. Serving the whole offer table over the socket lets the profile be generated from the
// running app instead, and lets a deck key name a verb rather than spend a letter.
json action_list_verbs(DesignPanel* panel, const json& params)
{
    const int      kind      = panel->mcp_offer_selection_kind();
    const uint32_t bit       = offer_bit(OfferSel(kind));
    const bool     applicable_only = params.value("applicable_only", false);
    const bool     has_sketch_mode = params.contains("sketch_mode");
    const bool     want_sketch_mode = has_sketch_mode && params["sketch_mode"].get<bool>();

    json verbs = json::array();
    for (int i = 0; i < kOfferVerbCount; ++i) {
        const OfferVerb& v = kOfferVerbs[i];
        const bool applies = (v.accepts & bit) != 0;
        if (applicable_only && !applies) continue;
        if (has_sketch_mode && v.sketch_mode != want_sketch_mode) continue;
        // Verbs whose action is nullptr exist in the vocabulary but have no GUI path yet — report
        // them with a null action rather than dropping them.
        verbs.push_back(json{
            {"id", v.id},
            {"name", v.name},
            {"row", v.row},
            {"row_name", kOfferRowNames[v.row]},
            {"key", v.key ? json(v.key) : json(nullptr)},
            {"action", v.action ? json(v.action) : json(nullptr)},
            {"sketch_mode", v.sketch_mode},
            {"applies", applies},
            {"modal", verb_is_modal(v.id)},   // opening a dialog: this key will want a mouse
            {"hint", v.hint ? json(v.hint) : json(nullptr)},
        });
    }
    return json{{"ok", true}, {"selection_kind", kind}, {"count", verbs.size()},
                {"verbs", std::move(verbs)}};
}

json action_run_verb(DesignPanel* panel, const json& params)
{
    if (!params.contains("verb")) throw std::runtime_error("run_verb needs 'verb' (an offer verb id)");
    const std::string verb = params["verb"].get<std::string>();

    // Scan the offer table BEFORE dispatching so the failure message can tell an unknown id from
    // one that exists in the vocabulary but has no GUI path (action == nullptr).
    bool known = false;
    bool has_action = false;
    const char* action = nullptr;
    for (int i = 0; i < kOfferVerbCount; ++i) {
        if (std::strcmp(kOfferVerbs[i].id, verb.c_str()) == 0) {
            known = true;
            has_action = (kOfferVerbs[i].action != nullptr);
            action = kOfferVerbs[i].action;
            break;
        }
    }

    if (!known)      throw std::runtime_error("run_verb: unknown verb '" + verb + "'");
    if (!has_action) throw std::runtime_error("run_verb: '" + verb + "' has no GUI path yet");

    // Whether the verb would be OFFERED for the current selection. Not a refusal: the GUI lets
    // you press a tool's shortcut whatever is selected, and refusing here would make the socket
    // stricter than the keyboard for no reason. Reported so a caller can tell "did nothing
    // because it did not apply" from "did nothing because it is broken".
    const int      kind    = panel->mcp_offer_selection_kind();
    const uint32_t bit     = offer_bit(OfferSel(kind));
    bool           applies = false;
    for (int i = 0; i < kOfferVerbCount; ++i)
        if (std::strcmp(kOfferVerbs[i].id, verb.c_str()) == 0) { applies = (kOfferVerbs[i].accepts & bit) != 0; break; }

    // The rule is not "validate more", it is "the socket should offer exactly what the GUI
    // offers, no more and no less". A "btn:"/"fly:" verb is reachable in the GUI ONLY through
    // the offer menu, which GREYS its row when it does not apply to the current selection — so
    // a socket caller must not be able to fire it either. But a "key:" verb is reachable from
    // the KEYBOARD whatever is selected, and the app permits that, so refusing it would make
    // the socket stricter than the keyboard for no reason. Refuse only the menu-only verbs.
    if (!applies && action &&
        (std::strncmp(action, "btn:", 4) == 0 || std::strncmp(action, "fly:", 4) == 0)) {
        throw std::runtime_error("run_verb: '" + verb +
                                 "' does not apply to the current selection (selection_kind " +
                                 std::to_string(kind) + ")");
    }

    // Dispatch on the NEXT turn of the event loop, never inline. See verb_is_modal above: a verb
    // that opens a dialog would otherwise block the thread this call is running on. Deferring
    // costs the ability to report the verb's outcome — which run_offer_action never returned
    // anyway — and buys a socket that cannot be wedged by any verb in the table.
    wxGetApp().CallAfter([panel, verb]() { panel->mcp_run_verb(verb.c_str()); });

    return json{{"ok", true}, {"verb", verb}, {"dispatched", true},
                {"applies", applies}, {"modal", verb_is_modal(verb.c_str())},
                {"selection_kind", kind}};
}

json action_mirror(DesignPanel* panel, const json& params)
{
    std::string m_str = params.value("mode", std::string("new"));
    BooleanMode m = (m_str == "add") ? BooleanMode::Add : BooleanMode::New;
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to mirror");
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

json action_transform(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to transform");
    int bi = target_body_arg(params, doc);
    Vec3d translate(params.value("dx", 0.0), params.value("dy", 0.0), params.value("dz", 0.0));
    Vec3d axis(params.value("axis_x", 0.0), params.value("axis_y", 0.0), params.value("axis_z", 1.0));
    Vec3d pivot(params.value("pivot_x", 0.0), params.value("pivot_y", 0.0), params.value("pivot_z", 0.0));
    double angle = params.value("angle", 0.0);
    bool copy = params.value("copy", false);
    doc.checkpoint();
    int idx = doc.add_transform(bi, translate, axis, pivot, angle, copy, panel->mcp_feature_name(_L("Transform")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"transform_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_thicken(DesignPanel* panel, const json& params)
{
    if (!params.contains("face")) throw std::runtime_error("thicken needs 'face' (id from query_topology)");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to thicken");
    int bi = target_body_arg(params, doc);
    int face = params["face"].get<int>();
    double thickness = params.value("thickness", 2.0);
    bool flip = params.value("flip", false);
    doc.checkpoint();
    int idx = doc.add_thicken(bi, face, thickness, flip, panel->mcp_feature_name(_L("Thicken")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"thicken_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
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

json action_project(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no source body to project from");
    int source_body = params.value("source_body", -1);
    int face = params.value("face", -1);
    std::vector<int> edges;
    if (params.contains("edges") && params["edges"].is_array())
        for (const auto& v : params["edges"]) edges.push_back(v.get<int>());
    const SketchPlane pl = cad_resolve_plane(params.value("plane", json("XY")), doc, false);
    doc.checkpoint();
    int idx = doc.add_project_edges(source_body, edges, face, pl, panel->mcp_feature_name(_L_CONTEXT("Project", "Design")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"project_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error},
                {"frame", frame_json(pl)}};
}

json action_delete_face(DesignPanel* panel, const json& params)
{
    if (!params.contains("faces")) throw std::runtime_error("delete_face needs 'faces' (array of face ids)");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to delete faces from");
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

json action_bridge(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("bridge needs 'sketch' (feature index)");
    if (!params.contains("ent_a"))  throw std::runtime_error("bridge needs 'ent_a' (entity index)");
    if (!params.contains("ent_b"))  throw std::runtime_error("bridge needs 'ent_b' (entity index)");
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

json action_axis(DesignPanel* panel, const json& params)
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

json action_coordsys(DesignPanel* panel, const json& params)
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
json action_plane(DesignPanel* panel, const json& params)
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

json action_mate(DesignPanel* panel, const json& params)
{
    if (!params.contains("cs_a")) throw std::runtime_error("mate needs 'cs_a' (CoordSys feature index)");
    if (!params.contains("cs_b")) throw std::runtime_error("mate needs 'cs_b' (CoordSys feature index)");
    const int kind      = params.value("kind", 0);
    const int cs_a      = params["cs_a"].get<int>();
    const int cs_b      = params["cs_b"].get<int>();
    const double offset = params.value("offset", 0.0);
    const double angle  = params.value("angle", 0.0);
    const bool flip     = params.value("flip", false);
    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    int idx = doc.add_mate(kind, cs_a, cs_b, offset, angle, flip, panel->mcp_feature_name(_L("Mate")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"mate_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
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

json action_set_variable(DesignPanel* panel, const json& params)
{
    if (!params.contains("name")) throw std::runtime_error("set_variable needs 'name'");
    if (!params.contains("expr")) throw std::runtime_error("set_variable needs 'expr'");
    CadDocument& doc = panel->mcp_doc();
    const std::string name = params["name"].get<std::string>();
    const std::string expr = params["expr"].get<std::string>();
    if (name.empty()) throw std::runtime_error("set_variable needs a non-empty 'name'");
    // An empty expression removes the variable, as Remove in the Variables list does.
    const bool remove = expr.empty();
    if (remove && doc.variables.count(name) == 0)
        throw std::runtime_error("there is no variable '" + name + "'");
    doc.checkpoint();
    if (remove)
        doc.variables.erase(name);
    else
        doc.variables[name] = expr;
    const bool ok = doc.recompute();
    if (!ok) {
        // rollback() rebuilds, which succeeds and clears doc.error; the reply and the status line
        // need the reason. A removal fails because a binding still names the variable, as the
        // GUI's Remove says.
        const std::string why = doc.error;
        doc.rollback();
        doc.error = remove ? std::string(wxString::Format(_L("Variable '%s' could not be removed (likely still referenced "
                                                             "by a feature expression): %s"),
                                                          wxString::FromUTF8(name), wxString::FromUTF8(why)).ToUTF8().data())
                           : why;
    }
    panel->mcp_after_change(ok);
    json r{{"ok", ok}, {"name", name}, {"error", doc.error}};
    if (remove) r["removed"] = ok;
    return r;
}

json action_set_feature_expr(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const int    fi  = feature_arg(params, doc);
    if (!params.contains("field")) throw std::runtime_error("set_feature_expr needs 'field' name");
    if (!params.contains("expr"))  throw std::runtime_error("set_feature_expr needs 'expr' string");
    const std::string field = params["field"].get<std::string>();
    const std::string expr  = params["expr"].get<std::string>();
    // A name the evaluator does not know would fail every later rebuild; the binding row refuses it too.
    if (!CadDocument::is_bindable_field(field))
        throw std::runtime_error("\"" + field + "\" is not a value an expression can drive");
    // An empty expression removes the binding, as the binding row's Clear does; the field keeps
    // the value the binding last gave it.
    const bool clear = expr.empty();
    if (clear && doc.features[fi].expr.count(field) == 0)
        throw std::runtime_error(field + " has no binding on feature " + std::to_string(fi));
    doc.checkpoint();
    if (clear)
        doc.features[fi].expr.erase(field);
    else
        doc.features[fi].expr[field] = expr;
    const bool ok = doc.recompute();
    // rollback() rebuilds, which succeeds and clears doc.error; carry the reason across so the
    // reply says why the edit was rejected.
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature", fi}, {"field", field}, {"error", doc.error}};
}

// --- The Design history, the Feature tree and the Bodies list ----------------------------

// Edit > Undo / Redo with the Design tab shown: the Design history, or inside a sketch session the
// sketch's own, which takes off or puts back the last drawn shape. Prepare's history is
// run_action undo's.
json undo_redo(DesignPanel* panel, bool redo)
{
    DesignCanvas* vp     = panel->mcp_viewport();
    const bool    sketch = vp != nullptr && vp->is_sketching();
    if (!panel->menu_can_undo_redo(redo)) {
        if (sketch) throw std::runtime_error(redo ? "Nothing to redo in the open sketch" : "Nothing to undo in the open sketch");
        throw std::runtime_error(redo ? "Nothing to redo in the Design history"
                                      : "Nothing to undo in the Design history (run_action undo undoes Prepare's)");
    }
    panel->menu_undo_redo(redo);
    if (sketch) return json{{"ok", true}, {"scope", "sketch"}, {"entities", vp->mcp_sketch_tool().entities().size()}};
    const CadDocument& doc = panel->mcp_doc();
    return json{{"ok", true},
                {"scope", "design"},
                {"undo_depth", doc.undo_depth()},
                {"redo_depth", doc.redo_depth()},
                {"features", doc.features.size()},
                {"bodies", doc.bodies.size()},
                {"generation", doc.topo_generation}};
}

// The panel's tree edits put the features back, leave no undo step and say "Edit rejected" when the
// kernel refuses one; the call then fails with the kernel's reason.
[[noreturn]] void tree_edit_refused(const CadDocument& doc)
{
    throw CadCallError(-32000, doc.error.empty() ? "the Design tab refused the edit" : doc.error);
}

json tree_edit_reply(const CadDocument& doc, json reply)
{
    reply["ok"]         = true;
    reply["features"]   = doc.features.size();
    reply["bodies"]     = doc.bodies.size();
    reply["generation"] = doc.topo_generation;
    return reply;
}

// params[key], or null when it is absent or params is not an object.
json param(const json& params, const char* key)
{
    return params.is_object() ? params.value(key, json()) : json();
}

// A `name` as the rows' rename editor takes it: trimmed, and never empty.
std::string name_arg(const json& params)
{
    const json v = param(params, "name");
    if (!v.is_string()) throw std::runtime_error("name must be a string");
    std::string name = v.get<std::string>();
    name.erase(name.find_last_not_of(" \t\r\n") + 1);
    name.erase(0, name.find_first_not_of(" \t\r\n"));
    if (name.empty()) throw std::runtime_error("name must not be empty");
    return name;
}

// The `body` param: an index into describe_scene's bodies.
int body_arg(const json& params, const CadDocument& doc)
{
    const json v = param(params, "body");
    if (!v.is_number_integer()) throw std::runtime_error("body must be a body index (describe_scene's bodies[].index)");
    const int b = v.get<int>();
    if (b < 0 || b >= int(doc.bodies.size()))
        throw std::runtime_error("body " + std::to_string(b) + " is out of range; the design has " +
                                 std::to_string(doc.bodies.size()) + " bodies");
    return b;
}

json action_feature_delete(DesignPanel* panel, const json& params)
{
    const CadDocument& doc    = panel->mcp_doc();
    const int          fi     = feature_arg(params, doc);
    const size_t       before = doc.features.size();
    if (!panel->delete_feature(fi)) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"removed", before - doc.features.size()}});
}

json action_feature_move(DesignPanel* panel, const json& params)
{
    const CadDocument& doc   = panel->mcp_doc();
    const int          fi    = feature_arg(params, doc);
    const json         delta = param(params, "delta");
    if (!delta.is_number_integer() || std::abs(delta.get<int>()) != 1)
        throw std::runtime_error("delta must be -1 (up) or 1 (down)");
    const int to = fi + delta.get<int>();
    if (to < 0 || to >= int(doc.features.size()))
        throw std::runtime_error("feature " + std::to_string(fi) + " is already " + (to < 0 ? "first" : "last"));
    if (!panel->move_feature(fi, delta.get<int>())) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"feature", to}, {"from", fi}});
}

json action_feature_set_enabled(DesignPanel* panel, const json& params)
{
    const CadDocument& doc     = panel->mcp_doc();
    const int          fi      = feature_arg(params, doc);
    const json         enabled = param(params, "enabled");
    if (!enabled.is_boolean()) throw std::runtime_error("enabled must be true or false");
    const bool changed = doc.features[fi].enabled != enabled.get<bool>();
    if (!panel->show_feature(fi, enabled.get<bool>())) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"feature", fi}, {"enabled", enabled}, {"changed", changed}});
}

json action_feature_rename(DesignPanel* panel, const json& params)
{
    const CadDocument& doc     = panel->mcp_doc();
    const int          fi      = feature_arg(params, doc);
    const std::string  name    = name_arg(params);
    const bool         changed = doc.features[fi].name != name;
    panel->rename_feature(fi, name);
    return json{{"ok", true}, {"feature", fi}, {"name", name}, {"changed", changed}, {"undo_depth", doc.undo_depth()}};
}

// The Bodies row's rename and eye and the Color button, in one call. Every argument is checked
// before anything changes.
json action_body_set(DesignPanel* panel, const json& params)
{
    const CadDocument& doc = panel->mcp_doc();
    const int          b   = body_arg(params, doc);
    std::optional<std::string> name;
    std::optional<ColorRGBA>   color;
    std::optional<bool>        visible;
    if (!param(params, "name").is_null()) name = name_arg(params);
    if (const json c = param(params, "color"); !c.is_null()) {
        const std::string s = c.is_string() ? c.get<std::string>() : std::string();
        ColorRGBA         rgba;
        if (s.size() != 7 || s[0] != '#' ||
            !std::all_of(s.begin() + 1, s.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }) ||
            !decode_color(s, rgba))
            throw std::runtime_error("color must be \"#RRGGBB\"");
        color = rgba;
    }
    if (const json v = param(params, "visible"); !v.is_null()) {
        if (!v.is_boolean()) throw std::runtime_error("visible must be true or false");
        visible = v.get<bool>();
    }
    if (!name && !color && !visible) throw std::runtime_error("body_set needs name, color or visible");
    if (name) panel->rename_body(b, *name);
    if (color) panel->set_body_color(b, *color);
    if (visible) panel->show_body(b, *visible);

    const CadBody& body = doc.bodies[b];
    json r{{"ok", true}, {"body", b}, {"name", body.has_user_name ? body.user_name : body.name},
           {"visible", panel->mcp_body_shown(b)}};
    if (const DesignCanvas* vp = panel->mcp_viewport(); vp != nullptr) r["color"] = encode_color(vp->body_color(b));
    return r;
}

json action_body_delete(DesignPanel* panel, const json& params)
{
    const CadDocument& doc = panel->mcp_doc();
    const int          b   = body_arg(params, doc);
    const int          src = doc.bodies[b].source_feature;
    if (src < 0 || src >= int(doc.features.size()))
        throw std::runtime_error("body " + std::to_string(b) + " has no feature to delete; use new_design to start over");
    const json   deleted{{"index", src}, {"type", feature_type_name(doc.features[src].type)}, {"name", doc.features[src].name}};
    const size_t before = doc.features.size();
    if (!panel->delete_body(b)) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"deleted_feature", deleted}, {"removed", before - doc.features.size()}});
}

// --- Hand-off: Commit to Plate, New Design, Export STEP ----------------------------------

// Commit to Plate as the split button does, in `mode` (by default the button's, which this leaves
// alone), then delete the `replace` objects. With `replace` the deletions and the commit are one
// Prepare undo step: the commit's own snapshot is suppressed under the outer one.
json action_commit(DesignPanel* panel, const json& params)
{
    using Mode = DesignPanel::CommitMode;
    Mode mode = panel->commit_mode();
    const json m = params.value("mode", json(""));
    if (m == "assembly") mode = Mode::Assembly;
    else if (m == "bodies") mode = Mode::Bodies;
    else if (m != "") throw std::runtime_error("mode must be \"assembly\" or \"bodies\"");

    Plater*     plater   = wxGetApp().plater();
    ObjectList* obj_list = wxGetApp().obj_list();
    if (plater == nullptr || obj_list == nullptr) throw std::runtime_error("Prepare is not ready");
    Model& model = plater->model();
    const std::vector<int> replace = cad_commit_replace(params.value("replace", json::array()), model);
    // The Delete key does not pull an object from under an open gizmo either.
    if (!replace.empty())
        for (GLCanvas3D* canvas : {plater->get_view3D_canvas3D(), plater->get_assmeble_canvas3D()})
            if (canvas != nullptr && canvas->get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined)
                throw std::runtime_error("close the " + Mcp::gizmo_name(canvas->get_gizmos_manager().get_current_type()) +
                                         " gizmo first: replace deletes objects");

    // commit_to_plate()'s own refusals, checked first so a refused call leaves no undo step.
    const CadDocument& doc = panel->mcp_doc();
    if (doc.display_mesh.its.indices.empty())
        throw std::runtime_error("Nothing to commit: the design has no body; add a feature first");
    bool any_shown = doc.display_body_meshes.size() <= 1;   // a lone body is sent even when hidden
    for (size_t b = 0; !any_shown && b < doc.display_body_meshes.size(); ++b)
        any_shown = panel->mcp_body_shown(int(b)) && !doc.display_body_meshes[b].its.indices.empty();
    if (!any_shown) throw std::runtime_error("Every body is hidden; show one first with body_set {body, visible: true}");

    json replaced = json::array();
    for (int i : replace) replaced.push_back(json{{"index", i}, {"name", model.objects[i]->name}});

    std::vector<int>                sent;
    std::vector<const ModelObject*> made;
    {
        std::optional<Plater::TakeSnapshot> snapshot;
        if (!replace.empty()) snapshot.emplace(plater, _u8L("Commit to Plate"));
        for (size_t i : panel->commit_to_plate(mode, &sent)) made.push_back(model.objects[i]);
        if (made.empty()) throw std::runtime_error("Commit to Plate sent nothing");
        std::vector<ItemForDelete> items;
        for (int i : replace) items.emplace_back(itObject, i, -1);
        obj_list->delete_from_model_and_list(items);
    }

    // The new objects' indices once the replaced ones are gone.
    json objects = json::array();
    for (const ModelObject* o : made) {
        const auto it = std::find(model.objects.begin(), model.objects.end(), o);
        objects.push_back(json{{"index", int(it - model.objects.begin())}, {"name", o->name}, {"volumes", o->volumes.size()}});
    }
    json hidden = json::array();
    for (int b = 0; b < int(doc.bodies.size()); ++b)
        if (std::find(sent.begin(), sent.end(), b) == sent.end() && !panel->mcp_body_shown(b)) hidden.push_back(b);
    return json{{"ok", true}, {"mode", mode == Mode::Bodies ? "bodies" : "assembly"}, {"objects", std::move(objects)},
                {"bodies_sent", sent}, {"bodies_hidden", std::move(hidden)}, {"replaced", std::move(replaced)},
                {"tab", "prepare"}};
}

// New Design without its confirmation dialog: discard_changes confirms, as new_project's does.
json action_new_design(DesignPanel* panel, const json& params)
{
    const CadDocument& doc = panel->mcp_doc();
    const json discard = params.value("discard_changes", json(false));
    if (!discard.is_boolean()) throw std::runtime_error("discard_changes must be true or false");
    const json cleared = {{"features", doc.features.size()}, {"bodies", doc.bodies.size()}, {"variables", doc.variables.size()}};
    if (!discard.get<bool>() && !(doc.features.empty() && doc.bodies.empty() && doc.variables.empty()))
        throw std::runtime_error("The design is not empty (" + cleared.dump() +
                                 "); pass discard_changes: true to erase it. This also clears the Design undo history; "
                                 "committed objects stay on the plate.");
    panel->clear_document();
    return json{{"ok", true}, {"cleared", cleared}};
}

// Export STEP… to a path instead of its file dialog, with the server's output-path rules.
json action_export_step(DesignPanel* panel, const json& params)
{
    const CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("Nothing to export: the design has no body; add a feature first");
    const json path_arg = params.value("path", json());
    const json overwrite = params.value("overwrite", json(false));
    if (!path_arg.is_string()) throw std::runtime_error("export_step needs 'path', an absolute file path");
    if (!overwrite.is_boolean()) throw std::runtime_error("overwrite must be true or false");
    std::string err;
    const auto path = Mcp::resolve_output_path(path_arg.get<std::string>(), overwrite.get<bool>(), err);
    if (!path) throw std::runtime_error(err);
    const std::string path_utf8 = Mcp::path_to_utf8(*path);
    if (!panel->export_step_to(path_utf8, err)) throw std::runtime_error("STEP export failed: " + err);

    boost::system::error_code ec;
    const auto bytes = boost::filesystem::file_size(*path, ec);
    json moved = json::array();
    for (int b = 0; b < int(doc.bodies.size()); ++b)
        if (panel->mcp_body_moved(b)) moved.push_back(b);
    return json{{"ok", true}, {"path", path_utf8}, {"bytes", ec ? 0 : bytes}, {"bodies", doc.bodies.size()},
                {"moved_bodies", std::move(moved)}};
}

// --- The open sketch: shapes, edit ops and constraints (cad_sketch_call) -----------------------
//
// Each runs the sketch tool's own step for its button or gesture (DesignSketchTool's *_scripted
// entry points), checks every argument first and throws, leaving the sketch as it was.

size_t      edit_distance(const std::string& a, const std::string& b);   // with the method lookup below
std::string lower(std::string s);

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
    if (t.value_field_open()) throw std::runtime_error("a value field is open in the sketch; commit or cancel it (Enter or Esc) first");
    if (t.gesture_pending()) throw std::runtime_error("the armed sketch tool has a gesture in progress; finish it or press Esc first");
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
                {"dof", t.dof()}, {"solve_ok", t.solve_ok()}};
}

// One item of sketch_add: entities added as given (`tool` Select), or a shape a drawing tool builds
// from its clicks.
struct SketchAddItem
{
    std::vector<SketchEntity> ents;
    DesignSketchTool::Mode    tool{DesignSketchTool::Mode::Select};
    std::vector<Vec2d>        clicks;
    int                       sides{0};
    bool                      circumscribed{false};
    bool                      construction{false};
};

// A sketch_add item: a shape's fields turned into the clicks its tool takes, or plain entities.
// Every check is made here, so adding a list of items cannot stop half way.
SketchAddItem sketch_add_item(const json& j)
{
    using Mode = DesignSketchTool::Mode;
    SketchAddItem it;
    const std::string t = j.value("type", std::string());
    it.construction = j.value("construction", false);
    // Smaller than the weld tolerance is no shape: its points would merge.
    auto size_arg = [&](const char* key) {
        const double v = number_arg(j, key);
        if (!(v >= kSketchJoinTol)) throw std::runtime_error(t + " " + key + " must be at least 0.001 mm");
        return v;
    };
    if (t == "polygon") {
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
        it.ents.push_back(sketch_entity_from(j));
    }
    return it;
}

json sketch_add(DesignSketchTool& t, const json& params)
{
    std::vector<SketchAddItem> items;
    const json list = param(params, "entities"), type = param(params, "type"), rect = param(params, "rect");
    if (list.is_array() && !list.empty()) {
        for (const json& j : list) items.push_back(sketch_add_item(j));
    } else if (type.is_string() && !type.get<std::string>().empty()) {
        items.push_back(sketch_add_item(params));   // one item, its fields beside `type`
    } else if (rect.is_array() && !rect.empty()) {
        // Corner rectangle as four shared-endpoint lines, so it arrives as ONE closed loop
        // rather than four segments that happen to touch.
        if (rect.size() < 4) throw std::runtime_error("rect must be [x0, y0, x1, y1]");
        const double x0 = rect[0].get<double>(), y0 = rect[1].get<double>();
        const double x1 = rect[2].get<double>(), y1 = rect[3].get<double>();
        const bool c = params.value("construction", false);
        auto seg = [&](Vec2d a, Vec2d b) { SketchEntity e; e.type = SketchEntity::Type::Line;
                                           e.p0 = a; e.p1 = b; e.construction = c; return e; };
        items.push_back(SketchAddItem{{seg({x0, y0}, {x1, y0}), seg({x1, y0}, {x1, y1}), seg({x1, y1}, {x0, y1}), seg({x0, y1}, {x0, y0})}});
    } else {
        throw std::runtime_error("sketch_add needs 'entities', a single 'type', or 'rect'");
    }
    if (std::any_of(items.begin(), items.end(), [](const SketchAddItem& it) { return it.tool != DesignSketchTool::Mode::Select; }))
        check_sketch_idle(t);

    const int before = int(t.entities().size());
    json shapes = json::array();
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
            const SketchAddItem& it = items[i++];
            const int at = t.add_shape_scripted(it.tool, it.clicks, it.sides, it.circumscribed, it.construction);
            if (at < 0) throw std::runtime_error("the sketch tool built nothing from item " + std::to_string(i - 1));
            shapes.push_back(json{{"first", at}, {"count", int(t.entities().size()) - at}});
        }
    }
    return json{{"ok", true}, {"first_index", before}, {"added", int(t.entities().size()) - before},
                {"shapes", std::move(shapes)}, {"entities", int(t.entities().size())}, {"dof", t.dof()}};
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

// sketch_trim: a Trim (or Extend) click at `at`.
json sketch_trim(DesignSketchTool& t, const json& params)
{
    const Vec2d  at     = point_arg(params, "at");
    const bool   extend = params.value("extend", false);
    const double tol    = params.contains("tolerance") ? number_arg(params, "tolerance") : 0.05;
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
    json r{{"ok", true}, {"entity", picked}, {"extend", extend}, {"geometry", sketch_entity_to(t.entities()[picked], picked)},
           {"released_constraints", cons_before - int(t.constraints().size())}};
    r.update(solve_state(t));
    return r;
}

// sketch_transform: the Move, Rotate, Scale, Array and Polar array tools' Confirm.
json sketch_transform(DesignSketchTool& t, const json& params)
{
    using Mode = DesignSketchTool::Mode;
    static const std::pair<const char*, Mode> kOps[] = {
        {"move", Mode::Move}, {"rotate", Mode::Rotate}, {"scale", Mode::Scale}, {"array", Mode::Array}, {"polar_array", Mode::PolarArray}};
    const json  opv = param(params, "op");
    const char* op  = nullptr;
    Mode        mode = Mode::Move;
    for (const auto& [name, m] : kOps)
        if (opv.is_string() && opv.get<std::string>() == name) { op = name; mode = m; }
    if (op == nullptr) throw std::runtime_error("op must be move, rotate, scale, array or polar_array");

    // What each op reads; anything else would be ignored, so it is refused.
    const bool shifts = mode == Mode::Move || mode == Mode::Array, turns = mode == Mode::Rotate || mode == Mode::PolarArray;
    const bool counts = mode == Mode::Array || mode == Mode::PolarArray, pivots = mode != Mode::Move && mode != Mode::Array;
    const std::pair<const char*, bool> takes[] = {{"dx", shifts},  {"dy", shifts},     {"angle", turns}, {"angle_deg", turns},
                                                  {"scale", mode == Mode::Scale}, {"count", counts}, {"pivot", pivots}};
    for (const auto& [key, read] : takes)
        if (!read && params.contains(key)) throw std::runtime_error(std::string(op) + " takes no " + key);

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
            throw std::runtime_error(std::string(op) + " needs count, 2 to 1000 instances counting the original");
        count = c.get<int>();
    }
    std::optional<Vec2d> pivot;
    if (params.contains("pivot")) pivot = point_arg(params, "pivot");
    // A transform that moves nothing would still drop the constraints it invalidates.
    if (shifts && delta.norm() < 1e-9) throw std::runtime_error(std::string(op) + " needs a non-zero dx or dy");
    if (mode == Mode::Rotate && std::abs(angle) < 1e-12) throw std::runtime_error("rotate needs a non-zero angle (or angle_deg)");
    if (mode == Mode::Scale && (!(scale > 0.0) || std::abs(scale - 1.0) < 1e-12))
        throw std::runtime_error("scale needs a positive factor other than 1");
    if (mode == Mode::PolarArray && !pivot) throw std::runtime_error("polar_array needs pivot, the centre to array about");
    if (mode == Mode::PolarArray && std::abs(angle) < 1e-12) throw std::runtime_error("polar_array needs a non-zero angle");

    check_sketch_idle(t);
    const int  ents_before = int(t.entities().size()), cons_before = int(t.constraints().size());
    const auto used        = t.transform_scripted(mode, targets, delta, angle, scale, count, pivot);
    if (!used) throw std::runtime_error("the sketch tool refused the transform");
    json r{{"ok", true}, {"op", op}};
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
    if (!v.is_string()) throw std::runtime_error("type must be a constraint type, such as \"parallel\" (describe_tools lists them)");
    const std::string s = v.get<std::string>();
    if (s == "equal") return SketchConstraintType::EqualLength;
    for (size_t i = 0; i < std::size(kConstraintTypes); ++i)
        if (s == kConstraintTypes[i]) return SketchConstraintType(i);
    std::string close;
    for (const char* n : kConstraintTypes)
        if (edit_distance(lower(s), n) <= 2 + s.size() / 4) close += std::string(close.empty() ? "" : ", ") + "\"" + n + "\"";
    throw std::runtime_error("unknown constraint type \"" + s + "\"" + (close.empty() ? "" : "; did you mean " + close) +
                             " (describe_tools lists them)");
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

// sketch_constrain: a CONSTRAIN bar button on the picks, or a constraint on named points.
json sketch_constrain(DesignSketchTool& t, const json& params)
{
    const std::vector<SketchEntityConstraintDef> defs  = cad_constraint_defs(params, t.entities(), t.selection());
    const int                                    first = t.constrain_scripted(defs);
    if (first < 0)
        throw std::runtime_error("over-constrained: " + param(params, "type").get<std::string>() +
                                 " was rejected and rolled back (dof " + std::to_string(t.dof()) + ")");
    json added = json::array(), items = json::array();
    for (int i = first; i < int(t.constraints().size()); ++i) {
        added.push_back(i);
        items.push_back(entity_constraint_json(t.constraints()[i], i));
    }
    json r{{"ok", true}, {"added", std::move(added)}, {"items", std::move(items)}};
    r.update(solve_state(t));
    return r;
}

json sketch_constraints(DesignSketchTool& t, const json&)
{
    json r{{"ok", true}, {"scope", "live"}, {"items", constraint_items(t.constraints())}};
    r.update(solve_state(t));
    return r;
}

// sketch_unconstrain: a constraint row's delete button in a sketch session.
json sketch_unconstrain(DesignSketchTool& t, const json& params)
{
    const json v = param(params, "index");
    if (!v.is_number_integer()) throw std::runtime_error("index must be a constraint index (sketch_constraints' items[].index)");
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

// A method on the open sketch alone, then a repaint.
json on_open_sketch(DesignPanel* panel, const char* method, const json& params)
{
    json r = cad_sketch_call(mcp_sketch(panel), method, params);
    panel->mcp_viewport()->request_repaint();
    return r;
}

json action_sketch_constraints(DesignPanel* panel, const json& params)
{
    const json sketch = param(params, "sketch");
    if (sketch.is_null()) return cad_sketch_call(mcp_sketch(panel), "sketch_constraints", params);
    // A committed sketch's are read from the document, so no session is needed, nor disturbed.
    const CadDocument& doc = panel->mcp_doc();
    const int          si  = sketch_arg(sketch, doc);
    const CadFeature&  f   = doc.features[si];
    if (f.entities.empty())
        throw std::runtime_error("sketch " + std::to_string(si) + " is not drawn from entities; describe_feature reads its constraints");
    return json{{"ok", true}, {"scope", "committed"}, {"sketch", si}, {"items", constraint_items(f.entity_constraints)},
                {"constraints", int(f.entity_constraints.size())}};
}

// --- dispatcher ---------------------------------------------------------------------

// What mcp_busy() lets through. Read only looks, so it runs under an open card or a sketch
// session; Sketch drives the live sketch session; Change edits the feature list or variables,
// which neither a card nor a sketch session may have changed under it.
enum class CadAccess { Read, Change, Sketch };

struct CadMethod
{
    const char* name;
    CadAccess   access;
    bool        ok_false;   // legacy: a rejected rebuild is reported in the reply, not as an error
    json (*run)(DesignPanel* panel, const json& params);
    // It changes Prepare's model, so only the MCP server's cad_call, under its mutate guard, may
    // run it; the socket would run it with no modal check, inside any nested event loop.
    bool        prepare = false;
};

// Every method, declared once: the dispatcher, the busy gate and describe_tools all read this.
const CadMethod kCadMethods[] = {
    {"describe_tools",      CadAccess::Read,   false, [](DesignPanel*, const json& p) { return cad_describe_tools(p); }},
    {"describe_scene",      CadAccess::Read,   false, [](DesignPanel* panel, const json&) { return describe_scene(panel); }},
    {"describe_feature",    CadAccess::Read,   false, action_describe_feature},
    {"query_topology",      CadAccess::Read,   false, query_topology},
    {"measure",             CadAccess::Read,   false, measure},
    {"mass_properties",     CadAccess::Read,   false, mass_properties},
    {"slice_body",          CadAccess::Read,   false, slice_body},
    {"validate_against",    CadAccess::Read,   false, validate_against},
    {"check_interference",  CadAccess::Read,   false, action_check_interference},
    {"list_verbs",          CadAccess::Read,   false, action_list_verbs},
    {"sketch_describe",     CadAccess::Read,   false, action_sketch_describe},
    {"sketch_validate",     CadAccess::Read,   false, action_sketch_validate},
    {"sketch_constraints",  CadAccess::Read,   false, action_sketch_constraints},
    {"import_step",         CadAccess::Change, true,  import_step},
    {"import_mesh",         CadAccess::Change, true,  import_mesh},
    {"extrude",             CadAccess::Change, false, action_extrude},
    {"revolve",             CadAccess::Change, false, action_revolve},
    {"fillet",              CadAccess::Change, true,  action_fillet},
    {"chamfer",             CadAccess::Change, true,  action_chamfer},
    {"hole",                CadAccess::Change, false, action_hole},
    {"hole_styled",         CadAccess::Change, false, action_hole_styled},
    {"hole_standard",       CadAccess::Change, false, action_hole_standard},
    {"boolean",             CadAccess::Change, true,  action_boolean},
    {"pattern",             CadAccess::Change, true,  action_pattern},
    {"pattern_on_curve",    CadAccess::Change, true,  action_pattern_on_curve},
    {"shell",               CadAccess::Change, true,  action_shell},
    {"rib",                 CadAccess::Change, true,  action_rib},
    {"draft",               CadAccess::Change, true,  action_draft},
    {"mirror",              CadAccess::Change, true,  action_mirror},
    {"transform",           CadAccess::Change, true,  action_transform},
    {"thicken",             CadAccess::Change, true,  action_thicken},
    {"split",               CadAccess::Change, false, action_split},
    {"thread",              CadAccess::Change, false, action_thread},
    {"sweep",               CadAccess::Change, false, action_sweep},
    {"loft",                CadAccess::Change, false, action_loft},
    {"project",             CadAccess::Change, true,  action_project},
    {"delete_face",         CadAccess::Change, true,  action_delete_face},
    {"bridge",              CadAccess::Change, true,  action_bridge},
    {"axis",                CadAccess::Change, true,  action_axis},
    {"coordsys",            CadAccess::Change, true,  action_coordsys},
    {"plane",               CadAccess::Change, false, action_plane},
    {"helix",               CadAccess::Change, false, action_helix},
    {"mate",                CadAccess::Change, true,  action_mate},
    {"set_variable",        CadAccess::Change, true,  action_set_variable},
    {"set_feature_expr",    CadAccess::Change, true,  action_set_feature_expr},
    {"set_feature",         CadAccess::Change, false, action_set_feature},
    {"feature_delete",      CadAccess::Change, false, action_feature_delete},
    {"feature_move",        CadAccess::Change, false, action_feature_move},
    {"feature_set_enabled", CadAccess::Change, false, action_feature_set_enabled},
    {"feature_rename",      CadAccess::Change, false, action_feature_rename},
    {"body_set",            CadAccess::Change, false, action_body_set},
    {"body_delete",         CadAccess::Change, false, action_body_delete},
    {"commit",              CadAccess::Change, false, action_commit, true},
    {"new_design",          CadAccess::Change, false, action_new_design},
    {"export_step",         CadAccess::Change, false, action_export_step},
    {"surface_extrude",     CadAccess::Change, true,  action_surface_extrude},
    {"surface_revolve",     CadAccess::Change, true,  action_surface_revolve},
    {"surface_loft",        CadAccess::Change, true,  action_surface_loft},
    {"surface_fill",        CadAccess::Change, true,  action_surface_fill},
    {"surface_offset",      CadAccess::Change, true,  action_surface_offset},
    {"thicken_surface",     CadAccess::Change, true,  action_thicken_surface},
    {"run_verb",            CadAccess::Change, false, action_run_verb},
    // In a sketch session these take back or put back the last drawn shape, as Ctrl+Z does there.
    {"undo",                CadAccess::Sketch, false, [](DesignPanel* panel, const json&) { return undo_redo(panel, false); }},
    {"redo",                CadAccess::Sketch, false, [](DesignPanel* panel, const json&) { return undo_redo(panel, true); }},
    {"sketch_begin",        CadAccess::Sketch, false, action_sketch_begin},
    {"sketch_commit",       CadAccess::Sketch, false, action_sketch_commit},
    {"sketch_cancel",       CadAccess::Sketch, false, action_sketch_cancel},
    {"sketch_add",          CadAccess::Sketch, false, [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_add", a); }},
    {"sketch_select",       CadAccess::Sketch, false, action_sketch_select},
    {"sketch_delete",       CadAccess::Sketch, false, action_sketch_delete},
    {"sketch_construction", CadAccess::Sketch, true,  action_sketch_construction},
    {"sketch_offset",       CadAccess::Sketch, false, action_sketch_offset},
    {"sketch_mirror",       CadAccess::Sketch, false, action_sketch_mirror},
    {"sketch_heal",         CadAccess::Sketch, false, action_sketch_heal},
    {"sketch_set_value",    CadAccess::Sketch, false, action_sketch_set_value},
    {"sketch_fillet",       CadAccess::Sketch, false, [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_fillet", a); }},
    {"sketch_chamfer",      CadAccess::Sketch, false, [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_chamfer", a); }},
    {"sketch_trim",         CadAccess::Sketch, false, [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_trim", a); }},
    {"sketch_transform",    CadAccess::Sketch, false, [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_transform", a); }},
    {"sketch_constrain",    CadAccess::Sketch, false, [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_constrain", a); }},
    {"sketch_unconstrain",  CadAccess::Sketch, false, [](DesignPanel* p, const json& a) { return on_open_sketch(p, "sketch_unconstrain", a); }},
};

const CadMethod* find_method(const std::string& name)
{
    for (const CadMethod& m : kCadMethods)
        if (name == m.name) return &m;
    return nullptr;
}

size_t edit_distance(const std::string& a, const std::string& b)
{
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[b.size()];
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// "; did you mean a, b?" for a mistyped method name, or nothing when no name is close.
std::string close_methods(const std::string& name)
{
    const std::string want = lower(name);
    std::string close;
    for (const CadMethod& m : kCadMethods) {
        const std::string n = m.name;
        if ((!want.empty() && (n.find(want) != std::string::npos || want.find(n) != std::string::npos))
            || edit_distance(n, want) <= 2 + want.size() / 4)
            close += (close.empty() ? "" : ", ") + n;
    }
    return close.empty() ? std::string() : "; did you mean " + close + "?";
}

// Dispatch one parsed request ON THE MAIN THREAD. Returns a JSON-RPC reply string.
std::string handle_on_main(const std::string& method, const json& params, const json& id)
{
    // The panel is built on first use, and in a headless session nobody clicks the tab that
    // would build it -- so build it here rather than refusing. Safe: this runs on the main
    // thread (see the CallAfter that dispatches us).
    DesignPanel* panel = DesignPanel::ensure();
    if (!panel)
        return rpc_error(id, -32001, "Design panel not ready");

    const CadMethod* m = find_method(method);
    if (m == nullptr)
        return rpc_error(id, -32601, "Unknown method: " + method + close_methods(method));

    // Only Read methods may run under a GUI editor or a sketch session they do not own. No method
    // may run during a rebuild, not even these: the rebuild's busy loop runs this request while
    // the GUI's worker thread holds the document. Checked before hydrating, which writes it.
    std::string why;
    if (panel->mcp_busy(m->access == CadAccess::Read, m->access == CadAccess::Sketch, why))
        return rpc_error(id, -32002, "Design tab busy: " + why);

    // A project opened without ever showing the Design tab has its recipe only in the Model;
    // load it, as showing the tab would, before anything reads or writes the document.
    panel->hydrate_from_model();

    // Stale-id guard, checked here rather than in each handler.
    //
    // Global face and edge ids are indices into TopExp::MapShapes, valid only against the
    // topology that produced them. Every dress-up rewrites those maps, so the natural way to
    // drive this socket — read the scene once, then issue several operations — silently
    // addresses the WRONG edge on every call after the first. Measured on a box: four chamfers
    // with ids re-read each time remove 0.400/0.397/0.397/0.395 mm3; the same four with ids
    // captured up front remove 0.400/0.008/0.397/0.280. Neither run errors, because a stale id
    // still resolves to a real edge — just not the one that was asked for.
    //
    // So a caller may pass back the "generation" it got from describe_scene or query_topology,
    // and a mismatch is refused instead of silently obeyed. Optional by design: omitting it
    // keeps every existing script working exactly as before, and supplying it is what buys the
    // guarantee. One check at the dispatcher rather than one per handler, so a method added
    // later cannot forget it.
    //
    // The type check is not decoration: this runs OUTSIDE the try below, and a bare
    // get<uint64_t>() on `"generation": "x"` throws nlohmann::type_error straight out of
    // the CallAfter lambda that invoked us — through a wx event loop, which does not catch,
    // so the whole GUI went down on one malformed line. Refuse it as a parameter error.
    if (params.is_object() && params.contains("generation")) {
        if (!params["generation"].is_number_unsigned())
            return rpc_error(id, -32602, "generation must be an unsigned integer");
        const uint64_t want = params["generation"].get<uint64_t>();
        const uint64_t have = panel->mcp_doc().topo_generation;
        if (want != have)
            return rpc_error(id, -32010,
                "stale face/edge ids: they were read at generation " + std::to_string(want)
                + " but the model is now at " + std::to_string(have)
                + ". Re-read query_topology and use the new ids — the old ones still name real "
                  "edges, just not the ones you measured.");
    }

    try {
        return rpc_result(id, m->run(panel, params));
    } catch (const CadCallError& ex) {
        return rpc_error(id, ex.code, ex.what());
    } catch (const Standard_Failure& ex) {   // derives from std::exception, so it comes first
        return rpc_error(id, -32000, std::string("OCCT: ") + (*ex.what() ? ex.what() : "failure"));
    } catch (const std::exception& ex) {
        return rpc_error(id, -32000, ex.what());
    }
}

#ifndef _WIN32
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
    if (const CadMethod* m = find_method(method); m != nullptr && m->prepare)
        return rpc_error(id, -32601, method + " changes the plate; call it through the app's MCP server (cad_call)");

    auto prom = std::make_shared<std::promise<std::string>>();
    auto fut  = prom->get_future();
    // 0 = queued, 1 = running, 2 = abandoned. A request that timed out while still QUEUED must
    // never run: the client has been told it failed, and a retry would otherwise apply it twice.
    auto state = std::make_shared<std::atomic<int>>(0);
    // Nothing may escape this lambda. It is invoked by the wx event loop, which has no
    // handler of its own, so an escaping exception is std::terminate — the socket would
    // become a way for any client to kill the application. handle_on_main() catches what
    // it knows about; this catches what it does not, and still answers the caller.
    wxGetApp().CallAfter([prom, state, method, params, id]() {
        int queued = 0;
        if (!state->compare_exchange_strong(queued, 1)) return;   // abandoned by a timeout
        try {
            prom->set_value(handle_on_main(method, params, id));
        } catch (const std::exception& ex) {
            prom->set_value(rpc_error(id, -32000, std::string("internal error: ") + ex.what()));
        } catch (...) {
            prom->set_value(rpc_error(id, -32000, "internal error: unknown exception"));
        }
    });
    if (fut.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
        int queued = 0;
        if (state->compare_exchange_strong(queued, 2))
            return rpc_error(id, -32000, "main-thread timeout: the command was NOT run");
        // Already running: it will finish, so wait for its real answer rather than report a
        // failure for a command that is in fact being applied.
        fut.wait();
    }
    return fut.get();
}

// Read newline-delimited requests off one client connection until EOF. A line may not grow
// past kMaxLine: a client that never sends a newline would otherwise grow this buffer until the
// process runs out of memory.
void serve_client(int cfd)
{
    constexpr size_t kMaxLine = 16u << 20;   // 16 MB — far above any real request
    std::string buf;
    char chunk[4096];
    for (;;) {
        ssize_t n = ::read(cfd, chunk, sizeof(chunk));
        if (n <= 0) break;
        buf.append(chunk, size_t(n));
        if (buf.size() > kMaxLine && buf.find('\n') == std::string::npos) {
            BOOST_LOG_TRIVIAL(error) << "MCP: request line over " << kMaxLine << " bytes; closing the connection";
            return;
        }
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            std::string reply = dispatch_request(line);
            reply.push_back('\n');
            // Never a bare write(): a client that hangs up between its request and our
            // reply raises SIGPIPE, whose default action kills the process — so closing
            // a socket mid-call would take the GUI with it.
#ifdef MSG_NOSIGNAL
            if (::send(cfd, reply.data(), reply.size(), MSG_NOSIGNAL) < 0) return;
#else
            if (::write(cfd, reply.data(), reply.size()) < 0) return;   // SO_NOSIGPIPE set at accept
#endif
        }
    }
}

// Where the socket lives, for the exit handler.
std::string g_sock_path;

void remove_socket_at_exit()
{
    struct stat st{};
    if (!g_sock_path.empty() && ::lstat(g_sock_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode))
        ::unlink(g_sock_path.c_str());
}

void server_thread(std::string sock_path)
{
    // Clear a stale socket from an earlier run — and ONLY a socket. ORCA_CAD_MCP names a path,
    // and unlinking it unconditionally deleted whatever file that path happened to be.
    {
        struct stat st{};
        if (::lstat(sock_path.c_str(), &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) {
                BOOST_LOG_TRIVIAL(error) << "MCP: " << sock_path << " exists and is not a socket; refusing to replace it";
                return;
            }
            ::unlink(sock_path.c_str());
        }
    }
    int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { BOOST_LOG_TRIVIAL(error) << "MCP: socket() failed"; return; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    // The socket is the full CAD command surface, including import_step on absolute paths.
    // It lands in a world-writable directory by default (/tmp), so its access control is
    // its file mode and nothing else — leaving that to the ambient umask means any local
    // process may drive the modeller. umask around bind() makes it 0600 with no window in
    // which a wider mode exists; the chmod afterwards covers platforms that do not apply
    // umask to sockets.
    const mode_t old_umask = ::umask(0177);
    const int bind_rc = ::bind(sfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::umask(old_umask);
    if (bind_rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "MCP: bind() failed on " << sock_path;
        ::close(sfd); return;
    }
    if (::chmod(sock_path.c_str(), S_IRUSR | S_IWUSR) < 0) {
        BOOST_LOG_TRIVIAL(error) << "MCP: cannot restrict " << sock_path << " to the owner; refusing to listen";
        ::close(sfd); ::unlink(sock_path.c_str()); return;
    }
    if (::listen(sfd, 1) < 0) { BOOST_LOG_TRIVIAL(error) << "MCP: listen() failed"; ::close(sfd); return; }
    g_sock_path = sock_path;
    std::atexit(remove_socket_at_exit);   // do not leave the socket file behind
    BOOST_LOG_TRIVIAL(info) << "MCP control listening on " << sock_path;

    for (;;) {
        int cfd = ::accept(sfd, nullptr, nullptr);
        if (cfd < 0) continue;
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
        const int on = 1;                                   // macOS/BSD equivalent of MSG_NOSIGNAL
        ::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        serve_client(cfd);
        ::close(cfd);
    }
}

#endif // !_WIN32

} // namespace

void start_mcp_control_if_enabled()
{
#ifndef _WIN32
    const char* env = std::getenv("ORCA_CAD_MCP");
    if (!env || !*env) return;
    std::string path = (std::strcmp(env, "1") == 0) ? "/tmp/orca-cad-mcp.sock" : env;
    static bool started = false;
    if (started) return;
    started = true;
    std::thread(server_thread, path).detach();
#endif
}

std::vector<std::string> cad_control_methods()
{
    std::vector<std::string> out;
    for (const CadMethod& m : kCadMethods) out.emplace_back(m.name);
    return out;
}

json cad_describe_tools(const json& params)
{
    json out = tool_rows();
    json legacy = json::array();
    for (const CadMethod& m : kCadMethods)
        if (m.ok_false) legacy.push_back(m.name);
    out["errors"] = "A call that fails is an error and leaves the design as it was. The methods in "
                    "legacy_ok_false report a rejected rebuild in their reply instead (ok false, or "
                    "a non-empty error).";
    out["legacy_ok_false"] = std::move(legacy);

    std::vector<std::string> names;
    const json want = params.is_object() ? params.value("method", json()) : json();
    if (want.is_string()) {
        if (!want.get<std::string>().empty()) names.push_back(want.get<std::string>());
    } else if (want.is_array()) {
        for (const json& n : want) {
            if (!n.is_string()) throw std::runtime_error("describe_tools: method takes a name or a list of names");
            names.push_back(n.get<std::string>());
        }
    } else if (!want.is_null()) {
        throw std::runtime_error("describe_tools: method takes a name or a list of names");
    }
    for (const std::string& n : names)
        if (find_method(n) == nullptr)
            throw std::runtime_error("describe_tools: unknown method '" + n + "'" + close_methods(n));
    const json q = params.is_object() ? params.value("query", json("")) : json("");
    if (!q.is_string()) throw std::runtime_error("describe_tools: query must be a string");
    const std::string query = lower(q.get<std::string>());

    json rows = json::array();
    for (json& row : out["tools"]) {
        const std::string name = row.value("name", std::string());
        if (!names.empty() && std::find(names.begin(), names.end(), name) == names.end()) continue;
        if (!query.empty() && lower(name + " " + row.value("summary", std::string())).find(query) == std::string::npos) continue;
        if (const CadMethod* m = find_method(name))
            row["access"] = m->access == CadAccess::Read ? "read" : m->access == CadAccess::Sketch ? "sketch" : "change";
        rows.push_back(std::move(row));
    }
    out["tools"] = std::move(rows);
    return out;
}

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
            throw std::runtime_error("datum plane " + std::to_string(fi) + " is hidden; show it first");
        const int  ordinal = datum_ordinal(doc, fi);
        const auto datums  = doc.resolve_datum_planes();
        if (ordinal >= int(datums.size()))
            throw std::runtime_error("datum plane " + std::to_string(fi) + " could not be resolved");
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

json cad_feature_json(const CadDocument& doc, int index)
{
    check_feature_index(index, doc);
    const CadFeature& f = doc.features[index];
    json j = feature_card(f);
    j["index"]   = index;
    j["type"]    = feature_type_name(f.type);
    j["name"]    = f.name;
    j["enabled"] = f.enabled;

    // The plane the recipe builds on: a datum's resolved frame, or the plane the feature holds.
    using T = CadFeatureType;
    const bool own_profile = f.type != T::Sketch && f.sketch_ref < 0 && (!f.entities.empty() || !f.profile.points.empty());
    if (f.type == T::Plane) {
        const auto datums  = doc.resolve_datum_planes();
        const int  ordinal = datum_ordinal(doc, index);
        if (f.enabled && ordinal < int(datums.size())) j["plane"] = plane_json(datums[ordinal].second, doc.modeling_origin);
    } else if (f.type == T::Sketch || f.type == T::Hole || f.type == T::Thread || f.type == T::Helix || f.type == T::Pattern ||
               f.type == T::Mirror || f.type == T::Project || (f.type == T::Cut && f.cut_face < 0) || own_profile) {
        j["plane"] = plane_json(f.plane, doc.modeling_origin);
    }

    // A sketch's geometry, or the loop an extrude or revolve carries itself; points are in the
    // plane's frame.
    if (!f.entities.empty()) {
        json entities = json::array(), constraints = json::array();
        for (size_t i = 0; i < f.entities.size(); ++i) entities.push_back(sketch_entity_to(f.entities[i], int(i)));
        for (size_t i = 0; i < f.entity_constraints.size(); ++i)
            constraints.push_back(entity_constraint_json(f.entity_constraints[i], int(i)));
        j["entities"]    = std::move(entities);
        j["constraints"] = std::move(constraints);
    } else if (!f.profile.points.empty()) {
        json points = json::array(), constraints = json::array();
        for (const Vec2d& p : f.profile.points) points.push_back(json::array({p.x(), p.y()}));
        for (size_t i = 0; i < f.constraints.size(); ++i) constraints.push_back(profile_constraint_json(f.constraints[i], int(i)));
        j["profile"]     = json{{"points", std::move(points)}, {"closed", f.profile.closed}};
        j["constraints"] = std::move(constraints);
    }

    // What consumes this feature: the references remove_feature() remaps, and for a datum plane
    // the features built on it.
    const int datum = f.type == T::Plane && f.enabled ? 3 + datum_ordinal(doc, index) : -1;
    json used_by = json::array();
    for (int i = 0; i < int(doc.features.size()); ++i) {
        bool uses = false;
        CadDocument::for_each_feature_ref(doc.features[i], [&](int r) { uses = uses || r == index; });
        if (datum >= 0) CadDocument::for_each_datum_ref(doc.features[i], [&](int r) { uses = uses || r == datum; });
        if (uses) used_by.push_back(i);
    }
    json made = json::array();
    for (size_t b = 0; b < doc.bodies.size(); ++b)
        if (doc.bodies[b].source_feature == index) made.push_back(int(b));
    j["used_by"]     = std::move(used_by);
    j["bodies_made"] = std::move(made);
    for (const auto& [mate, reason] : doc.mate_conflicts)
        if (mate == index) j["conflict"] = reason;
    return j;
}

void cad_apply_feature_fields(CadFeature& f, const json& fields)
{
    if (!fields.is_object() || fields.empty())
        throw std::runtime_error("fields must name the fields to change, as {\"distance\": 15}");
    const json  card     = feature_card(f);
    const json& editable = card["editable"];
    const json& ranges   = card["ranges"];
    const std::string type = feature_type_name(f.type);
    if (editable.empty())
        throw std::runtime_error(type + ": no field set_feature can change; its inputs are geometry or picks");
    if (fields.contains("thread_diameter") && fields.contains("thread_radius"))
        throw std::runtime_error("give thread_diameter or thread_radius, not both");
    // The two are one value, so a binding on either drives both.
    auto same_value = [](const std::string& a, const std::string& b) {
        auto thread_size = [](const std::string& k) { return k == "thread_diameter" || k == "thread_radius"; };
        return a == b || (thread_size(a) && thread_size(b));
    };

    CadFeature edited = f;
    for (const auto& [key, v] : fields.items()) {
        if (std::find(editable.begin(), editable.end(), json(key)) == editable.end()) {
            std::string list;
            for (const json& e : editable) list += (list.empty() ? "" : ", ") + e.get<std::string>();
            throw std::runtime_error(type + ": '" + key + "' is not an editable field; editable: " + list);
        }
        // Every rebuild writes a bound field from its expression, so a literal would not last.
        for (const auto& [bound, e] : f.expr)
            if (same_value(bound, key))
                throw std::runtime_error(key + " is driven by the expression '" + e + "'; change it with set_variable, or "
                                         "remove the binding with set_feature_expr {field: '" + bound + "', expr: ''}");
        const auto range = ranges.find(key);
        if (range == ranges.end()) {
            set_card_choice(edited, key, v);
            continue;
        }
        if (!v.is_number()) throw std::runtime_error(key + " must be a number");
        const double x = v.get<double>(), lo = (*range)[0].get<double>(), hi = (*range)[1].get<double>();
        if (!(x >= lo && x <= hi)) throw std::runtime_error(Slic3r::format("%1% must be between %2% and %3%", key, lo, hi));
        if (key == "pattern_count" && x != std::floor(x)) throw std::runtime_error("pattern_count must be a whole number");
        CadDocument::set_numeric_field(edited, key, x);
    }
    f = std::move(edited);
}

std::vector<int> cad_commit_replace(const json& replace, const Model& model)
{
    if (!replace.is_array()) throw std::runtime_error("replace takes a list of object indices (list_objects)");
    const int count = int(model.objects.size());
    std::vector<int> out;
    for (const json& v : replace) {
        if (!v.is_number_integer()) throw std::runtime_error("replace takes object indices (list_objects), not " + v.dump());
        const auto i = v.get<std::int64_t>();
        if (i < 0 || i >= count)
            throw std::runtime_error("object " + std::to_string(i) + " is out of range; the project has " + std::to_string(count) +
                                     " objects");
        if (std::find(out.begin(), out.end(), int(i)) != out.end())
            throw std::runtime_error("object " + std::to_string(i) + " is listed twice in replace");
        // Deleting one opens the "part of a cut object" warning, which the person answers.
        if (model.objects[size_t(i)]->is_cut())
            throw std::runtime_error("object " + std::to_string(i) +
                                     " is part of a cut object; delete it with select + run_action obj_delete, which asks first");
        out.push_back(int(i));
    }
    std::sort(out.begin(), out.end());
    return out;
}

json cad_sketch_describe(const CadDocument& doc, const DesignSketchTool& tool, int sketch)
{
    sketch_arg(sketch, doc);
    const CadFeature& f = doc.features[sketch];
    if (f.entities.empty())
        throw std::runtime_error("sketch " + std::to_string(sketch) +
                                 " is not drawn from entities (a rectangle, circle, profile or imported art); describe_feature reads it");
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

json cad_sketch_call(DesignSketchTool& tool, const std::string& method, const json& params)
{
    const json p = params.is_object() ? params : json::object();
    if (method == "sketch_add")         return sketch_add(tool, p);
    if (method == "sketch_fillet")      return sketch_corner(tool, p, false);
    if (method == "sketch_chamfer")     return sketch_corner(tool, p, true);
    if (method == "sketch_trim")        return sketch_trim(tool, p);
    if (method == "sketch_transform")   return sketch_transform(tool, p);
    if (method == "sketch_constrain")   return sketch_constrain(tool, p);
    if (method == "sketch_constraints") return sketch_constraints(tool, p);
    if (method == "sketch_unconstrain") return sketch_unconstrain(tool, p);
    throw std::runtime_error(method + " is not a method of the open sketch");
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

std::function<int(const std::string&)> cad_sketch_extrude(CadDocument& doc, const DesignSketchTool& tool, const json& params)
{
    refuse_with_sketch(params);
    const int        si   = sketch_arg(params.value("sketch", json()), doc);
    const double     d    = distance_arg(params);
    const ExtrudeEnd end  = extrude_end_arg(params);
    const auto       mode = boolean_arg(params, "auto", true);
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
    const BooleanMode mode  = *boolean_arg(params, "new", false);
    const int         body  = std::max(-1, target_body_arg(params, doc));
    check_has_body(mode, doc);
    // An entity axis is a line of this sketch; without one, a fresh card offers the sketch's only
    // centerline, unless the caller chose a plane axis.
    const std::vector<SketchEntity>& ents = doc.features[si].entities;
    int axis_entity = -1;
    const json ae = params.value("axis_entity", json());
    if (!ae.is_null()) {
        if (!ae.is_number_integer()) throw std::runtime_error("axis_entity must be an entity index of the sketch, or -1 for the plane axis");
        axis_entity = ae.get<int>();
        if (axis_entity != -1 && (axis_entity < 0 || axis_entity >= int(ents.size()) || ents[axis_entity].type != SketchEntity::Type::Line))
            throw std::runtime_error("axis_entity " + std::to_string(axis_entity) + " is not a line of sketch " + std::to_string(si));
    } else if (!params.contains("axis")) {
        axis_entity = DesignPanel::default_revolve_axis_entity(ents);
    }
    return [&doc, si, angle, axis, flip, mode, body, axis_entity](const std::string& name) {
        const int r = doc.add_revolve(si, angle, axis, flip, mode, name);
        doc.features[r].revolve_axis_entity = axis_entity;
        doc.features[r].target_body         = body;
        return r;
    };
}

std::function<int(const std::string& name)> cad_face_extrude(CadDocument& doc, const json& params, SketchPlane& frame)
{
    for (const char* k : {"sketch", "loop", "profile", "width", "height", "plane"})
        if (params.contains(k) && !params.at(k).is_null() && !(params.at(k).is_array() && params.at(k).empty()))
            throw std::runtime_error(std::string(k) + " cannot be given with face: the face is the profile and fixes its plane");
    const json fv = params.at("face");
    if (!fv.is_number_integer()) throw std::runtime_error("face must be a face id (query_topology)");
    if (doc.bodies.empty()) throw std::runtime_error("a face push/pull needs a body");
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
    const BooleanMode mode = boolean_arg(params, "auto", true).value_or(BooleanMode::Add);
    return [&doc, fi, d, mode, end, flip, b](const std::string& name) {
        const int e = doc.add_extrude_face(fi, d, false, mode, name);
        doc.features[e].extrude_end = end;
        doc.features[e].flip        = flip;
        doc.features[e].target_body = b;   // the face id is of this body
        return e;
    };
}

json cad_query_topology(const CadDocument& doc, const json& params)
{
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
    if (d < 0) throw std::runtime_error("the minimum distance between the refs could not be computed");
    r["min_distance"] = d;
    r["closest_a"]    = vec3(ca);
    r["closest_b"]    = vec3(cb);
    // The clearance between two parallel planar faces, as a lid fit wants it: across the gap, not
    // along it.
    if (planar_a && planar_b && da->cross(*db).norm() < 1e-6) r["parallel_gap"] = std::abs((pb - pa).dot(*da));
    return r;
}

json cad_hole_placement(const CadDocument& doc, const json& params, SketchPlane& plane, int& target_body)
{
    if (doc.bodies.empty()) throw std::runtime_error("no body to drill");
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
        if (doc.bodies.empty()) throw std::runtime_error("no body to thread");
        if (!params.at(kind).is_number_integer()) throw std::runtime_error(kind + " must be an id from query_topology");
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
            if (internal) throw std::runtime_error("an internal thread needs a body to tap; add or import one first");
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
            const std::string want = lower(d.get<std::string>());
            std::string       close;
            for (const ThreadSpec& t : thread_standards())
                if (lower(t.name) == want) close = t.name;
            if (close.empty() && !want.empty())
                for (const ThreadSpec& t : thread_standards()) {
                    const std::string n = lower(t.name);
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
    if (!pv.is_number_integer()) throw std::runtime_error("path must be the feature index of a sketch or a helix (describe_scene)");
    const int pi = pv.get<int>();
    check_feature_index(pi, doc);
    const CadFeatureType pt = doc.features[pi].type;
    if (pt != CadFeatureType::Sketch && pt != CadFeatureType::Helix)
        throw std::runtime_error("path " + std::to_string(pi) + " (" + feature_type_name(pt) + ") is not a sketch or a helix");
    if (pi == si) throw std::runtime_error("the path cannot be the profile sketch itself");
    const BooleanMode mode = *boolean_arg(params, "new", false);
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
        if (!v.is_number_integer()) throw std::runtime_error("profiles must hold sketch feature indices (describe_scene)");
        const int si = sketch_arg(v, doc);
        if (std::find(refs.begin(), refs.end(), si) != refs.end())
            throw std::runtime_error("sketch " + std::to_string(si) + " is listed twice in profiles");
        refs.push_back(si);
    }
    const json ruled = params.value("ruled", json(false));
    if (!ruled.is_boolean()) throw std::runtime_error("ruled must be true or false");
    const BooleanMode mode = *boolean_arg(params, "new", false);
    const int         body = std::max(-1, target_body_arg(params, doc));
    check_has_body(mode, doc);
    return [&doc, refs, ruled = ruled.get<bool>(), mode, body](const std::string& name) {
        const int i = doc.add_loft(refs, ruled, mode, name);
        doc.features[i].target_body = body;
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
    if (doc.bodies.empty()) throw std::runtime_error("no body to split");
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
        if (!params.at("face").is_number_integer()) throw std::runtime_error("face must be a face id (query_topology)");
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

json cad_control_call(const std::string& method, const json& params)
{
    const json reply = json::parse(handle_on_main(method, params, json(1)));
    if (reply.contains("error"))
        throw CadCallError(reply["error"].value("code", -32000), reply["error"].value("message", std::string("CAD call failed")));
    return reply.value("result", json());
}

}} // namespace Slic3r::GUI
