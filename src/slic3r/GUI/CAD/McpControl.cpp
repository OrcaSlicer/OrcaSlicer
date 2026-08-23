#include "slic3r/GUI/CAD/McpControl.hpp"

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
#include <utility>

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <Standard_Failure.hxx>   // OCCT base error (not a std::exception)

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"
#include "slic3r/GUI/CAD/DesignOffer.hpp"   // offer table — served whole by list_verbs, fired by run_verb

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/path.hpp>
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

// --- the three slice-1 methods (run on the wx MAIN thread) ---------------------

json describe_tools()
{
    // Hand-written descriptor. The bridge turns this into MCP tool schemas; later
    // slices grow this list (ideally from the kernel directly).
    return json{
        {"app", "SnapOrca CAD"},
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
            json{{"name", "describe_tools"}, {"summary", "List callable tools and their parameters."},
                 {"params", json::array()}},
            json{{"name", "describe_scene"}, {"summary", "Feature tree + per-body bounding boxes of the Design document."},
                 {"params", json::array()}},
            json{{"name", "extrude"}, {"summary", "Extrude a profile to a depth. Give an explicit closed `profile` (list of [x,y]) or default to a centred width x height rectangle. End conditions match Onshape."},
                 {"params", json::array({
                     json{{"name", "width"},    {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"min", 0.01}},
                     json{{"name", "height"},   {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"min", 0.01}},
                     json{{"name", "distance"}, {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "plane"},    {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
                     json{{"name", "profile"},  {"type", "array"}, {"default", json::array()}, {"description", "optional closed contour [[x,y],...] in plane mm; overrides width/height"}},
                     json{{"name", "boolean"},  {"type", "string"}, {"enum", json::array({"new", "union", "subtract", "intersect"})}, {"default", "new"}},
                     json{{"name", "end"},      {"type", "string"}, {"enum", json::array({"blind", "symmetric", "two_sided", "through_all", "up_to_face"})}, {"default", "blind"}},
                     json{{"name", "distance2"},{"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "second-side depth when end=two_sided (else falls back to distance)"}},
                     json{{"name", "up_to_face"},{"type", "integer"}, {"default", -1}, {"description", "target face id (query_topology on the last body) when end=up_to_face"}},
                     json{{"name", "taper"},    {"type", "number"}, {"unit", "deg"}, {"default", 0}, {"description", "draft/taper of the side wall"}},
                     json{{"name", "flip"},     {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "revolve"}, {"summary", "Revolve a profile about a plane axis. Give an explicit `profile` offset from the axis (or a rectangle) — angle degrees about axis 0=plane X / 1=plane Y."},
                 {"params", json::array({
                     json{{"name", "width"},   {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"min", 0.01}},
                     json{{"name", "height"},  {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "angle"},   {"type", "number"}, {"unit", "deg"}, {"default", 360}},
                     json{{"name", "axis"},    {"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 0}},
                     json{{"name", "flip"},    {"type", "boolean"}, {"default", false}},
                     json{{"name", "plane"},   {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
                     json{{"name", "profile"}, {"type", "array"}, {"default", json::array()}, {"description", "optional closed contour [[x,y],...] in plane mm; overrides width/height"}},
                     json{{"name", "boolean"}, {"type", "string"}, {"enum", json::array({"new", "union", "subtract", "intersect"})}, {"default", "new"}},
                 })}},
            json{{"name", "fillet"}, {"summary", "Round a measured edge of a body (edge id from query_topology on that body)."},
                 {"params", json::array({
                     json{{"name", "edge"},   {"type", "integer"}},
                     json{{"name", "radius"}, {"type", "number"}, {"unit", "mm"}, {"default", 1}, {"min", 0.01}},
                     json{{"name", "body"},   {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body. edge id is resolved against THIS body."}},
                 })}},
            json{{"name", "chamfer"}, {"summary", "Chamfer a measured edge of a body (edge id from query_topology on that body)."},
                 {"params", json::array({
                     json{{"name", "edge"},     {"type", "integer"}},
                     json{{"name", "distance"}, {"type", "number"}, {"unit", "mm"}, {"default", 1}, {"min", 0.01}},
                     json{{"name", "body"},     {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body. edge id is resolved against THIS body."}},
                 })}},
            json{{"name", "hole"}, {"summary", "Drill a circular hole into the current body at (x,y) on a plane."},
                 {"params", json::array({
                     json{{"name", "diameter"}, {"type", "number"}, {"unit", "mm"}, {"default", 5}, {"min", 0.01}},
                     json{{"name", "depth"},    {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "through"},  {"type", "boolean"}, {"default", false}},
                     json{{"name", "x"},        {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "in the sketch plane's frame (origin = describe_scene.modeling_origin), NOT world"}},
                     json{{"name", "y"},        {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "in the sketch plane's frame (origin = describe_scene.modeling_origin), NOT world"}},
                     json{{"name", "plane"},    {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
                 })}},
            json{{"name", "hole_styled"}, {"summary", "Drill a hole with optional counterbore (style=1) or countersink (style=2) at (x,y) on a plane."},
                 {"params", json::array({
                     json{{"name", "diameter"},        {"type", "number"}, {"unit", "mm"}, {"default", 5}, {"min", 0.01}},
                     json{{"name", "depth"},           {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "through"},         {"type", "boolean"}, {"default", true}},
                     json{{"name", "x"},               {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "in the sketch plane's frame (origin = describe_scene.modeling_origin), NOT world"}},
                     json{{"name", "y"},               {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "in the sketch plane's frame (origin = describe_scene.modeling_origin), NOT world"}},
                     json{{"name", "plane"},           {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
                     json{{"name", "style"},           {"type", "integer"}, {"default", 0}, {"description", "0=simple, 1=counterbore, 2=countersink"}},
                     json{{"name", "cbore_diameter"},  {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "cbore_depth"},     {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "csink_diameter"},  {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "csink_angle"},     {"type", "number"}, {"unit", "deg"}, {"default", 90}},
                     json{{"name", "standard"},        {"type", "string"}, {"default", ""}, {"description", "provenance designation, e.g. M6"}},
                 })}},
            json{{"name", "hole_standard"}, {"summary", "Drill a standard clearance hole (ISO 273 / ANSI) at (x,y) on a plane. style: 0=simple, 1=counterbore, 2=countersink."},
                 {"params", json::array({
                     json{{"name", "designation"},  {"type", "string"}, {"description", "e.g. M6, 1/4-20"}},
                     json{{"name", "style"},        {"type", "integer"}, {"default", 0}},
                     json{{"name", "through"},      {"type", "boolean"}, {"default", true}},
                     json{{"name", "depth"},        {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "x"},            {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "in the sketch plane's frame (origin = describe_scene.modeling_origin), NOT world"}},
                     json{{"name", "y"},            {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "in the sketch plane's frame (origin = describe_scene.modeling_origin), NOT world"}},
                     json{{"name", "plane"},        {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
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
                     json{{"name", "plane"},    {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
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
                     json{{"name", "plane"},        {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XZ"}},
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
            json{{"name", "helix"}, {"summary", "Create a helical curve (consumed by sweep as a path to build springs/coils/augers). pitch = axial rise per turn. left_handed flips the winding. taper_deg != 0 gives a conical helix."},
                 {"params", json::array({
                     json{{"name", "radius"},      {"type", "number"}, {"unit", "mm"},  {"default", 10}, {"min", 0.01}},
                     json{{"name", "pitch"},       {"type", "number"}, {"unit", "mm"},  {"default", 5},  {"min", 0.01}},
                     json{{"name", "height"},      {"type", "number"}, {"unit", "mm"},  {"default", 20}, {"min", 0.01}},
                     json{{"name", "left_handed"}, {"type", "boolean"}, {"default", false}},
                     json{{"name", "taper_deg"},   {"type", "number"}, {"unit", "deg"}, {"default", 0}},
                     json{{"name", "plane"},       {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
                 })}},
            json{{"name", "thicken"}, {"summary", "Offset a face of a body by a wall thickness, producing a new thin solid body."},
                 {"params", json::array({
                     json{{"name", "body"},      {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                     json{{"name", "face"},      {"type", "integer"}, {"description", "face id to thicken (query_topology)"}},
                     json{{"name", "thickness"}, {"type", "number"}, {"unit", "mm"}, {"default", 2}, {"min", 0.01}},
                     json{{"name", "flip"},      {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "split"}, {"summary", "Split a body along the plane of a picked face."},
                 {"params", json::array({
                     json{{"name", "body"},      {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                     json{{"name", "face_body"}, {"type", "integer"}, {"default", -1}, {"description", "body that owns the face; -1 = target"}},
                     json{{"name", "face"},      {"type", "integer"}, {"description", "face id to split along (query_topology)"}},
                     json{{"name", "keep_upper"},{"type", "boolean"}, {"default", true}},
                     json{{"name", "keep_lower"},{"type", "boolean"}, {"default", true}},
                 })}},
             json{{"name", "project"}, {"summary", "Project edges of a solid onto a sketch plane, producing a new sketch feature."},
                  {"params", json::array({
                      json{{"name", "source_body"}, {"type", "integer"}, {"default", -1}, {"description", "body owning the edges; -1 = last body"}},
                      json{{"name", "face"},        {"type", "integer"}, {"default", -1}, {"description", "global face id on the source body; when set, all its edges are projected"}},
                      json{{"name", "edges"},       {"type", "array"}, {"default", json::array()}, {"description", "global edge ids to project; empty => project the face"}},
                      json{{"name", "plane"},       {"type", "string"}, {"default", "XY"}, {"description", "target sketch plane (XY/XZ/YZ)"}},
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
            json{{"name", "rib"}, {"summary", "Grow a thin rib wall (stiffener) from an open Line sketch entity, fused to a body."},
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
                     json{{"name", "path"}, {"type", "string"}},
                 })}},
            json{{"name", "import_mesh"}, {"summary", "Convert a triangle mesh (STL/OBJ) into an editable B-rep body. Reports whether the result is a real solid or an open shell, and why."},
                 {"params", json::array({
                     json{{"name", "path"}, {"type", "string"}},
                     json{{"name", "tolerance"}, {"type", "number"}, {"default", 0.01}},
                     json{{"name", "merge_angle_deg"}, {"type", "number"}, {"default", 5.0}},
                 })}},
            json{{"name", "validate_against"}, {"summary", "Volume + bbox/centroid + surface deviation (max/mean/rms mm) of a body vs a reference {step:path|body:id} (the RE acceptance metric)."},
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
        // Pass this back as "generation" on any call that takes a face or edge id and the call
        // is refused if the topology has moved on. See the note on the dispatcher.
        {"generation", doc.topo_generation},
    };
}

// --- Measure layer (read-only "evidence" half of the RE loop) ------------------
inline json vec3(const Vec3d& v) { return json::array({v.x(), v.y(), v.z()}); }

// Shared Build helpers.
SketchPlane plane_from(const json& params, const CadDocument& doc)
{
    std::string n = params.value("plane", std::string("XY"));
    SketchPlane pl = n == "XZ" ? SketchPlane::XZ() : n == "YZ" ? SketchPlane::YZ() : SketchPlane::XY();
    pl.origin = doc.modeling_origin;   // land on the bed centre, like the GUI
    return pl;
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
bool profile_from(const json& params, SketchProfile& out)
{
    if (!params.contains("profile")) return false;
    out.points.clear();
    for (const auto& p : params["profile"]) out.points.emplace_back(p[0].get<double>(), p[1].get<double>());
    out.closed = true;
    return out.points.size() >= 3;
}

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
    // Enumerate once. The _by_index accessors rescan the shape on every call (edge_by_index
    // rebuilds the whole indexed map), so indexing a body face-by-face is quadratic: ~15 s on a
    // 4.7k-face imported solid, on the UI thread. faces_of/edges_of keep the very same ids.
    const std::vector<TopoDS_Face> all_faces = GeometryEngine::faces_of(shape);
    const std::vector<TopoDS_Edge> all_edges = GeometryEngine::edges_of(shape);

    json faces = json::array();
    const int nf = int(all_faces.size());
    for (int i = 0; i < nf; ++i) {
        const TopoDS_Face& f = all_faces[i];
        if (f.IsNull()) continue;
        json jf{{"id", i}, {"centroid", vec3(GeometryEngine::face_centroid_world(f))},
                {"normal", vec3(GeometryEngine::face_normal_world(f))}, {"kind", "planar"}};
        GeometryEngine::CylinderFace cyl = GeometryEngine::cylinder_of_face(f);
        if (cyl.ok) { jf["kind"] = "cylindrical"; jf["radius"] = cyl.radius;
                      jf["axis"] = vec3(cyl.axis); jf["internal"] = cyl.internal; }
        faces.push_back(std::move(jf));
    }
    json edges = json::array();
    const int ne = int(all_edges.size());
    for (int i = 0; i < ne; ++i) {
        const TopoDS_Edge& e = all_edges[i];
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
                {"faces", std::move(faces)}, {"edges", std::move(edges)},
                // The ids above are indices into this exact topology and expire with it. Echo
                // this back as "generation" on the calls that consume them.
                {"generation", panel->mcp_doc().topo_generation}};
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

json mass_properties(DesignPanel* panel, const json& params)
{
    const TopoDS_Shape& shape = body_shape(panel, params);
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
    auto near = [&](const Vec3d& a, const Vec3d& b) { return (a - b).norm() <= tol; };
    for (size_t i = 0; i < segs.size(); ++i) {
        if (used[i] || segs[i].size() < 2) continue;
        used[i] = 1;
        std::vector<Vec3d> c = segs[i];
        for (int side = 0; side < 2; ) {           // grow tail; reverse once when stuck
            bool grew = false;
            for (size_t j = 0; j < segs.size(); ++j) {
                if (used[j] || segs[j].size() < 2) continue;
                if (near(c.back(), segs[j].front())) {
                    c.insert(c.end(), segs[j].begin() + 1, segs[j].end()); used[j] = 1; grew = true;
                } else if (near(c.back(), segs[j].back())) {
                    for (auto it = segs[j].rbegin() + 1; it != segs[j].rend(); ++it) c.push_back(*it);
                    used[j] = 1; grew = true;
                }
            }
            if (grew) { side = 0; continue; }
            std::reverse(c.begin(), c.end()); ++side;   // try the other end
        }
        bool closed = c.size() > 2 && near(c.front(), c.back());
        contours.emplace_back(std::move(c), closed);
    }
    return contours;
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
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    const std::string path       = params["path"].get<std::string>();
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
    f.name           = "Mesh" + std::to_string(first + 1);
    f.imported_solid = shape;
    f.mode           = BooleanMode::New;
    doc.features.push_back(f);

    const bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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

json action_extrude(DesignPanel* panel, const json& params)
{
    const double d = params.value("distance", 10.0);
    if (d <= 0) throw std::runtime_error("distance must be > 0");
    CadDocument& doc = panel->mcp_doc();
    SketchPlane pl = plane_from(params, doc);
    BooleanMode mode = bool_from(params.value("boolean", std::string("new")));

    SketchProfile prof;
    bool has_prof = profile_from(params, prof);
    double w = 0, h = 0;
    if (!has_prof) {
        w = params.value("width", 20.0); h = params.value("height", 20.0);
        if (w <= 0 || h <= 0) throw std::runtime_error("width and height must be > 0");
    }
    doc.checkpoint();
    int s = has_prof ? doc.add_sketch_profile(prof, pl, "Sketch")
                     : doc.add_sketch(SketchShape::Rectangle, pl, w, h, 0.0, "Sketch");
    int e = doc.add_extrude(s, d, /*symmetric*/false, mode, "Extrude");
    // End condition (Onshape parity). apply reads extrude_end directly; the kernel `symmetric`
    // bool is unused, so set the field here. up_to_face id comes from query_topology on the
    // target (last) body. taper_deg lofts the side wall; flip negates the direction.
    const std::string end = params.value("end", std::string("blind"));
    CadFeature& fe = doc.features[e];
    fe.flip      = params.value("flip", false);
    fe.taper_deg = params.value("taper", 0.0);
    if      (end == "symmetric")   fe.extrude_end = ExtrudeEnd::Symmetric;
    else if (end == "two_sided")  { fe.extrude_end = ExtrudeEnd::TwoSided; fe.distance2 = params.value("distance2", d); }
    else if (end == "through_all")  fe.extrude_end = ExtrudeEnd::ThroughAll;
    else if (end == "up_to_face")  { fe.extrude_end = ExtrudeEnd::UpToFace; fe.up_to_face = params.value("up_to_face", -1); }
    else                            fe.extrude_end = ExtrudeEnd::Blind;
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"sketch_index", s}, {"extrude_index", e}, {"end", end},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_revolve(DesignPanel* panel, const json& params)
{
    const double angle = params.value("angle", 360.0);
    const int    axis  = params.value("axis", 0);          // 0 = plane X, 1 = plane Y
    const bool   flip  = params.value("flip", false);
    CadDocument& doc = panel->mcp_doc();
    SketchPlane pl = plane_from(params, doc);
    BooleanMode mode = bool_from(params.value("boolean", std::string("new")));

    SketchProfile prof;
    bool has_prof = profile_from(params, prof);
    double w = 0, h = 0;
    if (!has_prof) {   // ponytail: rectangle centred on the axis may self-overlap; offset via `profile`
        w = params.value("width", 20.0); h = params.value("height", 10.0);
        if (w <= 0 || h <= 0) throw std::runtime_error("width and height must be > 0");
    }
    doc.checkpoint();
    int s = has_prof ? doc.add_sketch_profile(prof, pl, "Sketch")
                     : doc.add_sketch(SketchShape::Rectangle, pl, w, h, 0.0, "Sketch");
    int r = doc.add_revolve(s, angle, axis, flip, mode, "Revolve");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"sketch_index", s}, {"revolve_index", r},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
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

json action_fillet(DesignPanel* panel, const json& params)
{
    if (!params.contains("edge")) throw std::runtime_error("fillet needs 'edge' (id from query_topology)");
    const double radius = params.value("radius", 1.0);
    if (radius <= 0) throw std::runtime_error("radius must be > 0");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to fillet");
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int f = doc.add_fillet(radius, params["edge"].get<int>(), "Fillet");
    if (bi >= 0) doc.features[f].target_body = bi;   // edge id resolved against THIS body's shape
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"fillet_index", f}, {"body", bi < 0 ? int(doc.bodies.size()) - 1 : bi},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_chamfer(DesignPanel* panel, const json& params)
{
    if (!params.contains("edge")) throw std::runtime_error("chamfer needs 'edge' (id from query_topology)");
    const double dist = params.value("distance", 1.0);
    if (dist <= 0) throw std::runtime_error("distance must be > 0");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to chamfer");
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int c = doc.add_chamfer(dist, params["edge"].get<int>(), "Chamfer");
    if (bi >= 0) doc.features[c].target_body = bi;   // edge id resolved against THIS body's shape
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"chamfer_index", c}, {"body", bi < 0 ? int(doc.bodies.size()) - 1 : bi},
                {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_hole(DesignPanel* panel, const json& params)
{
    const double dia   = params.value("diameter", 5.0);
    const double depth = params.value("depth", 10.0);
    const bool   thru  = params.value("through", false);
    const double x = params.value("x", 0.0), y = params.value("y", 0.0);
    if (dia <= 0) throw std::runtime_error("diameter must be > 0");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to drill");
    SketchPlane pl = plane_from(params, doc);
    doc.checkpoint();
    int h = doc.add_hole(dia, depth, thru, x, y, pl, "Hole");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"hole_index", h}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_hole_styled(DesignPanel* panel, const json& params)
{
    const double dia   = params.value("diameter", 5.0);
    const double depth = params.value("depth", 10.0);
    const bool   thru  = params.value("through", true);
    const double x = params.value("x", 0.0), y = params.value("y", 0.0);
    const int    style          = params.value("style", 0);
    const double cbore_diameter = params.value("cbore_diameter", 0.0);
    const double cbore_depth    = params.value("cbore_depth", 0.0);
    const double csink_diameter = params.value("csink_diameter", 0.0);
    const double csink_angle    = params.value("csink_angle", 90.0);
    const std::string standard  = params.value("standard", std::string(""));
    if (dia <= 0) throw std::runtime_error("diameter must be > 0");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to drill");
    SketchPlane pl = plane_from(params, doc);
    doc.checkpoint();
    int h = doc.add_hole_styled(dia, depth, thru, x, y, pl, style,
                                cbore_diameter, cbore_depth,
                                csink_diameter, csink_angle, standard, "Hole");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"hole_index", h}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_hole_standard(DesignPanel* panel, const json& params)
{
    const std::string desig = params.value("designation", std::string(""));
    if (desig.empty()) throw std::runtime_error("designation is required");
    const int    style  = params.value("style", 0);
    const bool   thru   = params.value("through", true);
    const double depth  = params.value("depth", 10.0);
    const double x = params.value("x", 0.0), y = params.value("y", 0.0);
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to drill");
    SketchPlane pl = plane_from(params, doc);
    doc.checkpoint();
    try {
        int h = doc.add_hole_standard(desig, style, thru, depth, x, y, pl, "Hole");
        bool ok = doc.recompute();
        if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
        panel->mcp_after_change();
        return json{{"ok", ok}, {"hole_index", h}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
    } catch (const std::exception& ex) {
        doc.undo();
        panel->mcp_after_change();
        return json{{"ok", false}, {"error", ex.what()}};
    }
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
    int b = doc.add_boolean(m, target, tool, keep, tol, -1, -1, "Boolean");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    doc.checkpoint();
    int p = doc.add_pattern(circular, count, spacing, dir, angle, bi, "Pattern");
    doc.features[p].plane = plane_from(params, doc);   // axis (circular) / step dirs (linear)
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"pattern_index", p}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
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
    int p = doc.add_pattern_on_curve(count, sketch, entity, bi, "PatternOnCurve");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    int s = doc.add_shell(thickness, face, bi, "Shell");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    int idx = doc.add_rib(sketch, entity, thickness, depth, bi, "Rib");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"rib_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_extrude(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("surface_extrude needs 'sketch' (feature index)");
    CadDocument& doc = panel->mcp_doc();
    int sketch = params["sketch"].get<int>();
    double distance = params.value("distance", 10.0);
    doc.checkpoint();
    int idx = doc.add_surface_extrude(sketch, distance, "SurfaceExtrude");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    int idx = doc.add_surface_revolve(sketch, angle, axis, "SurfaceRevolve");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    int idx = doc.add_thicken_surface(bi, thickness, flip, "ThickenSurface");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_offset(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no sheet body to offset");
    int bi = target_body_arg(params, doc);
    double offset = params.value("offset", 1.0);
    doc.checkpoint();
    int idx = doc.add_surface_offset(bi, offset, "SurfaceOffset");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    int idx = doc.add_surface_loft(profiles, ruled, "SurfaceLoft");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_fill(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("surface_fill needs 'sketch' (feature index)");
    CadDocument& doc = panel->mcp_doc();
    int sketch = params["sketch"].get<int>();
    doc.checkpoint();
    int idx = doc.add_surface_fill(sketch, "SurfaceFill");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    int d = doc.add_draft(angle, params["face"].get<int>(), bi, "Draft");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
        e.start_angle = j.value("start_angle", 0.0);
        e.end_angle   = j.value("end_angle", 0.0);
        if (e.radius <= 0.0) throw std::runtime_error("arc needs a positive 'radius'");
        e.p0 = e.center + e.radius * Vec2d(std::cos(e.start_angle), std::sin(e.start_angle));
        e.p1 = e.center + e.radius * Vec2d(std::cos(e.end_angle),   std::sin(e.end_angle));
    } else if (t == "point") {
        e.type = SketchEntity::Type::Point;
        e.p0   = p("p", 0, 0);
    } else {
        throw std::runtime_error("unknown entity type '" + t + "' (line, arc, circle, point)");
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

json action_sketch_begin(DesignPanel* panel, const json& params)
{
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr) throw std::runtime_error("no viewport");
    if (vp->is_sketching()) throw std::runtime_error("a sketch is already open");
    const std::string pl = params.value("plane", std::string("XY"));
    SketchPlane plane = SketchPlane::XY();
    if      (pl == "XZ") plane = SketchPlane::XZ();
    else if (pl == "YZ") plane = SketchPlane::YZ();
    else if (pl != "XY") throw std::runtime_error("plane must be XY, XZ or YZ");
    vp->begin_sketch(plane, DesignSketchTool::Mode::Select);
    // The panel has to enter sketch mode too, or the app is half in it: the tool sketches, the
    // offer menu offers sketch verbs, and every sketch KEY is dead because key dispatch tests
    // m_ui_mode while the menu tests the viewport. Driving the socket must leave the GUI in the
    // state a user would be in, not a state only the socket can produce.
    panel->mcp_set_sketch_mode(true);
    return json{{"ok", true}, {"plane", pl}};
}

json action_sketch_commit(DesignPanel* panel, const json& params)
{
    (void)params;
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr || !vp->is_sketching()) throw std::runtime_error("no sketch is open");
    vp->finish_sketch();
    panel->mcp_set_sketch_mode(false);
    panel->mcp_after_change();
    return json{{"ok", true}, {"features", int(panel->mcp_doc().features.size())}};
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

json action_sketch_add(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    std::vector<SketchEntity> ents;
    if (params.contains("entities")) {
        for (const auto& j : params.at("entities")) ents.push_back(sketch_entity_from(j));
    } else if (params.contains("type")) {
        ents.push_back(sketch_entity_from(params));      // single-entity shorthand
    } else if (params.contains("rect")) {
        // Corner rectangle as four shared-endpoint lines, so it arrives as ONE closed loop
        // rather than four segments that happen to touch.
        const json& r = params.at("rect");
        if (!r.is_array() || r.size() < 4) throw std::runtime_error("rect must be [x0, y0, x1, y1]");
        const double x0 = r[0].get<double>(), y0 = r[1].get<double>();
        const double x1 = r[2].get<double>(), y1 = r[3].get<double>();
        const bool c = params.value("construction", false);
        auto seg = [&](Vec2d a, Vec2d b) { SketchEntity e; e.type = SketchEntity::Type::Line;
                                           e.p0 = a; e.p1 = b; e.construction = c; return e; };
        ents.push_back(seg({x0, y0}, {x1, y0}));
        ents.push_back(seg({x1, y0}, {x1, y1}));
        ents.push_back(seg({x1, y1}, {x0, y1}));
        ents.push_back(seg({x0, y1}, {x0, y0}));
    } else {
        throw std::runtime_error("sketch_add needs 'entities', a single 'type', or 'rect'");
    }
    const int base = t.add_entities_scripted(ents);
    panel->mcp_viewport()->request_repaint();
    return json{{"ok", base >= 0}, {"first_index", base}, {"added", int(ents.size())},
                {"entities", int(t.entities().size())}, {"dof", t.dof()}};
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

json sketch_report(DesignSketchTool& t)
{
    const auto rep = t.loop_report();
    json loops = json::array();
    for (const auto& l : rep.loops) {
        json holes = json::array();
        for (int h : l.holes) holes.push_back(h);
        loops.push_back(json{{"entities", l.ents}, {"holes", holes},
                             {"closed", l.closed}, {"area", l.area}});
    }
    json open_ends = json::array();
    for (const Vec2d& p : rep.open_ends) open_ends.push_back(json::array({p.x(), p.y()}));
    // A profile is buildable when at least one loop closed and nothing is left dangling.
    const bool buildable = !rep.loops.empty() && rep.open_ends.empty();
    return json{{"closed_loops", loops}, {"open_ends", open_ends}, {"buildable", buildable}};
}

json action_sketch_describe(DesignPanel* panel, const json& params)
{
    (void)params;
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
    out.update(sketch_report(t));
    return out;
}

json action_sketch_validate(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    const double tol = params.value("tolerance", 1e-3);
    json out{{"ok", true}, {"tolerance", tol}, {"dof", t.dof()}, {"solve_ok", t.solve_ok()}};
    out.update(sketch_report(t));
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
    out.update(sketch_report(t));
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
    out.update(sketch_report(t));
    return out;
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
    doc.checkpoint();
    int idx = doc.add_mirror(plane_from(params, doc), bi, m, "Mirror");
    doc.features[idx].mirror_keep_original = keep;
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"mirror_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
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
    int idx = doc.add_transform(bi, translate, axis, pivot, angle, copy, "Transform");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    int idx = doc.add_thicken(bi, face, thickness, flip, "Thicken");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"thicken_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_split(DesignPanel* panel, const json& params)
{
    if (!params.contains("face")) throw std::runtime_error("split needs 'face' (id from query_topology)");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to split");
    int bi = target_body_arg(params, doc);
    int face_body = params.value("face_body", -1);
    int face = params["face"].get<int>();
    bool keep_upper = params.value("keep_upper", true);
    bool keep_lower = params.value("keep_lower", true);
    doc.checkpoint();
    int idx = doc.add_split_by_face(bi, face_body, face, keep_upper, keep_lower, "Split");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"split_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
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
    SketchPlane pl = plane_from(params, doc);
    doc.checkpoint();
    int idx = doc.add_project_edges(source_body, edges, face, pl, "Project");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"project_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
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
    int idx = doc.add_delete_face(bi, faces, "DeleteFace");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    int idx = doc.add_axis(at, "Axis");
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
    int idx = doc.add_coordsys(ct, pt, "CoordSys");
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

json action_helix(DesignPanel* panel, const json& params)
{
    const double radius      = params.value("radius", 10.0);
    const double pitch       = params.value("pitch", 5.0);
    const double height      = params.value("height", 20.0);
    const bool   left_handed = params.value("left_handed", false);
    const double taper       = params.value("taper_deg", 0.0);
    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    int idx = doc.add_helix(plane_from(params, doc), radius, pitch, height, left_handed, taper, "Helix");
    panel->mcp_after_change();
    return json{{"ok", true}, {"helix_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
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
    int idx = doc.add_mate(kind, cs_a, cs_b, offset, angle, flip, "Mate");
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    std::string name = params["name"].get<std::string>();
    std::string expr = params["expr"].get<std::string>();
    std::string old = doc.variables.count(name) ? doc.variables[name] : "";
    doc.checkpoint();
    doc.variables[name] = expr;
    bool ok = doc.recompute();
    // undo() recomputes, which succeeds and clears doc.error; carry the reason across so
    // the JSON reply below reports why the edit was rejected instead of an empty string.
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"name", name}, {"error", doc.error}};
}

json action_set_feature_expr(DesignPanel* panel, const json& params)
{
    if (!params.contains("feature")) throw std::runtime_error("set_feature_expr needs 'feature' index");
    if (!params.contains("field"))   throw std::runtime_error("set_feature_expr needs 'field' name");
    if (!params.contains("expr"))    throw std::runtime_error("set_feature_expr needs 'expr' string");
    CadDocument& doc = panel->mcp_doc();
    int fi = params["feature"].get<int>();
    if (fi < 0 || fi >= (int)doc.features.size())
        return json{{"ok", false}, {"error", "feature index out of range"}};
    std::string field = params["field"].get<std::string>();
    std::string expr  = params["expr"].get<std::string>();
    std::string old = doc.features[fi].expr.count(field) ? doc.features[fi].expr[field] : "";
    doc.checkpoint();
    doc.features[fi].expr[field] = expr;
    bool ok = doc.recompute();
    // undo() recomputes, which succeeds and clears doc.error; carry the reason across so
    // the JSON reply below reports why the edit was rejected instead of an empty string.
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"feature", fi}, {"field", field}, {"error", doc.error}};
}

// Dispatch one parsed request ON THE MAIN THREAD. Returns a JSON-RPC reply string.
std::string handle_on_main(const std::string& method, const json& params, const json& id)
{
    MainFrame* mf = wxGetApp().mainframe;
    if (!mf || !mf->m_design_panel)
        return rpc_error(id, -32001, "Design panel not ready");
    DesignPanel* panel = mf->m_design_panel;

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
    if (params.is_object() && params.contains("generation")) {
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
        if (method == "describe_tools") return rpc_result(id, describe_tools());
        if (method == "describe_scene") return rpc_result(id, describe_scene(panel));
        if (method == "query_topology") return rpc_result(id, query_topology(panel, params));
        if (method == "measure")        return rpc_result(id, measure(panel, params));
        if (method == "mass_properties") return rpc_result(id, mass_properties(panel, params));
        if (method == "slice_body")     return rpc_result(id, slice_body(panel, params));
        if (method == "import_step")    return rpc_result(id, import_step(panel, params));
        if (method == "import_mesh")    return rpc_result(id, import_mesh(panel, params));
        if (method == "validate_against") return rpc_result(id, validate_against(panel, params));
        if (method == "extrude")        return rpc_result(id, action_extrude(panel, params));
        if (method == "revolve")        return rpc_result(id, action_revolve(panel, params));
        if (method == "fillet")         return rpc_result(id, action_fillet(panel, params));
        if (method == "chamfer")        return rpc_result(id, action_chamfer(panel, params));
        if (method == "hole")           return rpc_result(id, action_hole(panel, params));
        if (method == "hole_styled")   return rpc_result(id, action_hole_styled(panel, params));
        if (method == "hole_standard") return rpc_result(id, action_hole_standard(panel, params));
        if (method == "boolean")        return rpc_result(id, action_boolean(panel, params));
        if (method == "pattern")        return rpc_result(id, action_pattern(panel, params));
        if (method == "pattern_on_curve") return rpc_result(id, action_pattern_on_curve(panel, params));
        if (method == "shell")          return rpc_result(id, action_shell(panel, params));
        if (method == "rib")            return rpc_result(id, action_rib(panel, params));
        if (method == "draft")          return rpc_result(id, action_draft(panel, params));
        if (method == "mirror")         return rpc_result(id, action_mirror(panel, params));
        if (method == "sketch_begin")   return rpc_result(id, action_sketch_begin(panel, params));
        if (method == "sketch_commit")  return rpc_result(id, action_sketch_commit(panel, params));
        if (method == "sketch_cancel")  return rpc_result(id, action_sketch_cancel(panel, params));
        if (method == "sketch_add")     return rpc_result(id, action_sketch_add(panel, params));
        if (method == "sketch_select")  return rpc_result(id, action_sketch_select(panel, params));
        if (method == "sketch_delete")  return rpc_result(id, action_sketch_delete(panel, params));
        if (method == "sketch_construction") return rpc_result(id, action_sketch_construction(panel, params));
        if (method == "sketch_offset")  return rpc_result(id, action_sketch_offset(panel, params));
        if (method == "sketch_mirror")  return rpc_result(id, action_sketch_mirror(panel, params));
        if (method == "sketch_describe") return rpc_result(id, action_sketch_describe(panel, params));
        if (method == "sketch_validate") return rpc_result(id, action_sketch_validate(panel, params));
        if (method == "sketch_heal")    return rpc_result(id, action_sketch_heal(panel, params));
        if (method == "sketch_set_value") return rpc_result(id, action_sketch_set_value(panel, params));
        if (method == "list_verbs")     return rpc_result(id, action_list_verbs(panel, params));
        if (method == "run_verb")       return rpc_result(id, action_run_verb(panel, params));
        if (method == "transform")      return rpc_result(id, action_transform(panel, params));
        if (method == "thicken")        return rpc_result(id, action_thicken(panel, params));
        if (method == "split")          return rpc_result(id, action_split(panel, params));
        if (method == "project")        return rpc_result(id, action_project(panel, params));
        if (method == "delete_face")    return rpc_result(id, action_delete_face(panel, params));
        if (method == "bridge")         return rpc_result(id, action_bridge(panel, params));
        if (method == "axis")           return rpc_result(id, action_axis(panel, params));
        if (method == "coordsys")       return rpc_result(id, action_coordsys(panel, params));
        if (method == "helix")          return rpc_result(id, action_helix(panel, params));
        if (method == "set_variable")     return rpc_result(id, action_set_variable(panel, params));
        if (method == "set_feature_expr") return rpc_result(id, action_set_feature_expr(panel, params));
        if (method == "surface_extrude")  return rpc_result(id, action_surface_extrude(panel, params));
        if (method == "surface_revolve")  return rpc_result(id, action_surface_revolve(panel, params));
        if (method == "thicken_surface")  return rpc_result(id, action_thicken_surface(panel, params));
        if (method == "surface_offset")   return rpc_result(id, action_surface_offset(panel, params));
        if (method == "surface_loft")    return rpc_result(id, action_surface_loft(panel, params));
        if (method == "surface_fill")    return rpc_result(id, action_surface_fill(panel, params));
        if (method == "mate")          return rpc_result(id, action_mate(panel, params));
        if (method == "check_interference") return rpc_result(id, action_check_interference(panel, params));
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
