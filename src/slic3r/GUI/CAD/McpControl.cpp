#include "slic3r/GUI/CAD/McpControl.hpp"
#include "libslic3r/Point.hpp"
#include <functional>
#include <stdexcept>
#include <math.h>
#include <TopAbs_ShapeEnum.hxx>
#include <Standard_TypeDef.hxx>
#include <exception>
#include <cstdint>

#ifndef _WIN32  // POSIX Unix-domain-socket transport only (slice 1)

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>   // umask/chmod: the socket's file mode IS its access control
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <future>
#include <chrono>
#include <memory>
#include <cmath>
#include <algorithm>
#include <utility>
#include <atomic>
#include <mutex>
#include <deque>
#include <cstdio>   // SEEK_END
#include <set>
#ifdef __linux__
#include <execinfo.h>   // backtrace: the UI thread's stack when it stops answering
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#endif

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <Standard_Failure.hxx>   // OCCT base error

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include <wx/dcscreen.h>
#include <wx/evtloop.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <map>
#include <type_traits>
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include <wx/dcmemory.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"
#include "slic3r/GUI/CAD/DesignOffer.hpp"   // offer table — served whole by list_verbs, fired by run_verb

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/CAD/SketchSolver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/path.hpp>
#include "libslic3r/BoundingBox.hpp"

#include <gp_Pln.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <initializer_list>
#include <GeomAbs_SurfaceType.hxx>
#include <boost/filesystem/operations.hpp>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/time.h>
#include <wx/string.h>
#include <wx/defs.h>
#include <wx/unichar.h>
#include <cctype>
#include <iterator>
#include <ios>
#include "slic3r/GUI/CAD/DesignImGuiUi.hpp"

using json = nlohmann::json;

// Element i of a JSON array as a finite number. An agent controls this input: indexing a short, missing or non-numeric
// array with operator[] is undefined behaviour in nlohmann (it killed the app), so every [x,y,z] goes through here.
static double jn(const json& a, size_t i)
{
    if (!a.is_array() || i >= a.size() || !a[i].is_number()) throw std::runtime_error("expected an array of numbers, e.g. [x, y, z]");
    const double v = a[i].get<double>();
    if (!std::isfinite(v)) throw std::runtime_error("numbers must be finite");
    return v;
}

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
        case CadFeatureType::Sketch3D: return "Sketch3D";
        case CadFeatureType::SmRule:          return "SheetRule";
        case CadFeatureType::SmBase:          return "SheetBase";
        case CadFeatureType::SmEdgeFlange:    return "SheetFlange";
        case CadFeatureType::SmContourFlange: return "SheetContour";
        case CadFeatureType::SmHem:           return "SheetHem";
        case CadFeatureType::SmUnfold:        return "SheetUnfold";
        case CadFeatureType::SmRefold:        return "SheetRefold";
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
std::string rpc_error(const json& id, int code, const std::string& msg, const json& data = nullptr)
{
    json e{{"code", code}, {"message", msg}};
    if (!data.is_null()) e["data"] = data;
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(e)}}.dump();
}

// Which state refused a command, as data: {owner, transient, hint}. `transient` ones are waited out by the
// dispatcher; the rest need the caller (or the user) to hand the state back.
json busy_data(const std::string& why)
{
    auto has = [&](const char* k) { return why.find(k) != std::string::npos; };
    if (has("rebuilt"))          return {{"owner", "rebuild"},     {"transient", true},  {"hint", "a rebuild is running; the command was retried for 15 s"}};
    if (has("feature card"))     return {{"owner", "feature_card"}, {"transient", false}, {"hint", "a feature card is open in the GUI: confirm or cancel it there (run_verb 'cancel', or Esc)"}};
    if (has("Text dialog"))      return {{"owner", "text_dialog"},  {"transient", false}, {"hint", "close the Text dialog in the GUI"}};
    if (has("constraining"))     return {{"owner", "constrain"},    {"transient", false}, {"hint", "leave the constrain mode in the GUI (Esc)"}};
    if (has("sketch is open"))   return {{"owner", "sketch"},       {"transient", false}, {"hint", "sketch_commit or sketch_cancel first; sketch_* methods are allowed while it is open"}};
    return {{"owner", "unknown"}, {"transient", false}};
}

// --- the three slice-1 methods (run on the wx MAIN thread) ---------------------

// Methods that never change the design (they may still write a file). The busy check lets these through while a
// card or sketch owns the tab, and describe_tools reports them as readOnly so a client need not guess.
bool read_only_method(const std::string& m)
{
    static const std::set<std::string> k = {
        "describe_tools", "describe_scene", "query_topology", "measure", "mass_properties",
        "slice_body", "validate_against", "list_verbs", "sketch_describe", "sketch_validate",
        "check_interference", "inspect_design", "list_commands", "export", "save", "screenshot", "ui.inspect", "ui.candidates", "ui.worldToScreen", "ui.screenshot", "variables_export", "sketch_dimensions" };
    return k.count(m) > 0;
}

// "a, b, c": the fields set_feature_expr can bind, from the allow-list itself.
static std::string bindable_fields_text()
{
    std::string s;
    for (const std::string& f : CadDocument::bindable_fields()) s += (s.empty() ? "" : ", ") + f;
    return s;
}

json describe_tools_core()
{
    // Hand-written descriptor. The bridge turns this into MCP tool schemas; later
    // slices grow this list (ideally from the kernel directly).
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
                     json{{"name", "thin_in"},  {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "thin extrude: wall thickness inside the (closed) outline; with thin_out, a wall around it instead of a filled solid"}},
                     json{{"name", "thin_out"}, {"type", "number"}, {"unit", "mm"}, {"default", 0}, {"description", "thin extrude: wall thickness outside the outline"}},
                     json{{"name", "entities"}, {"type", "array"}, {"description", "extrude only these loops of an existing `sketch` (entity indices from sketch_describe): disjoint ones become separate islands, a loop inside another is its hole"}},
                     json{{"name", "flip"},     {"type", "boolean"}, {"default", false}},
                     json{{"name", "sketch"},   {"type", "integer"}, {"description", "extrude this existing Sketch feature (sketch_commit returns sketch_index) instead of a polygon/rectangle; circles, arcs and holes included"}},
                 })}},
            json{{"name", "revolve"}, {"summary", "Revolve a profile about a plane axis. Give an explicit `profile` offset from the axis (or a rectangle) — angle degrees about axis 0=plane X / 1=plane Y."},
                 {"params", json::array({
                     json{{"name", "width"},   {"type", "number"}, {"unit", "mm"}, {"default", 20}, {"min", 0.01}},
                     json{{"name", "height"},  {"type", "number"}, {"unit", "mm"}, {"default", 10}, {"min", 0.01}},
                     json{{"name", "angle"},   {"type", "number"}, {"unit", "deg"}, {"default", 360}},
                     json{{"name", "axis"},    {"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 0}},
                     json{{"name", "flip"},    {"type", "boolean"}, {"default", false}},
                     json{{"name", "sketch"},  {"type", "integer"}, {"description", "revolve this existing Sketch feature (sketch_commit returns sketch_index) instead of a polygon/rectangle"}},
                     json{{"name", "plane"},   {"type", "string"}, {"enum", json::array({"XY", "XZ", "YZ"})}, {"default", "XY"}},
                     json{{"name", "profile"}, {"type", "array"}, {"default", json::array()}, {"description", "optional closed contour [[x,y],...] in plane mm; overrides width/height"}},
                     json{{"name", "boolean"}, {"type", "string"}, {"enum", json::array({"new", "union", "subtract", "intersect"})}, {"default", "new"}},
                 })}},
            json{{"name", "fillet"}, {"summary", "Round measured edges of a body (edge ids from query_topology on that body)."},
                 {"params", json::array({
                     json{{"name", "edge"},   {"type", "integer|array"}, {"description", "one edge id, or an array of ids rounded together in one feature"}},
                     json{{"name", "radius"}, {"type", "number"}, {"unit", "mm"}, {"default", 1}, {"min", 0.01}},
                     json{{"name", "body"},   {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body. edge id is resolved against THIS body."}},
                     json{{"name", "radius_end"}, {"type", "number"}, {"unit", "mm"}, {"min", 0}, {"description", "variable fillet: the radius runs linearly from `radius` to this along each edge (0 closes the round to a point)"}},
                     json{{"name", "start"},  {"type", "array"}, {"description", "[x,y,z]: with radius_end, `radius` applies at the end of the edge nearest this point"}},
                     json{{"name", "radii"},  {"type", "array"}, {"description", "radius spline: the control radii after `radius`, with `knots` (clamped, sorted, 0..1, degree = knots - radii - 2); `start` picks the end the spline begins at"}},
                     json{{"name", "knots"},  {"type", "array"}, {"description", "the full knot vector of the radius spline"}},
                     json{{"name", "fit"},    {"type", "string"}, {"enum", json::array({"taper"})}, {"description", "taper: where `radius` does not fit, the radius follows the room (a smooth law, min(radius, 0.9 x room))"}},
                     json{{"name", "chord"},  {"type", "boolean"}, {"default", false}, {"description", "`radius` is the width of the round measured across it (chord length), not a radius"}},
                 })}},
            json{{"name", "sm_rule"}, {"summary", "Sheet-metal rule used by every later sheet-metal feature: thickness, inner bend radius, K-factor, hem gap."},
                 {"params", json::array({
                     json{{"name", "thickness"},   {"type", "number"}, {"unit", "mm"}, {"default", 2.5}},
                     json{{"name", "bend_radius"}, {"type", "number"}, {"unit", "mm"}, {"default", -1}, {"description", "inner bend radius; <= 0 means equal to the thickness"}},
                     json{{"name", "k_factor"},    {"type", "number"}, {"default", 0.44}, {"description", "neutral-layer position, used to unfold"}},
                     json{{"name", "gap"},         {"type", "number"}, {"unit", "mm"}, {"default", 0.01}, {"description", "gap inside a hem"}},
                 })}},
            json{{"name", "sm_base"}, {"summary", "Base flange: a plate of the rule thickness from a closed sketch (grows along the sketch normal; flip reverses)."},
                 {"params", json::array({
                     json{{"name", "sketch"}, {"type", "integer"}, {"description", "closed profile sketch index"}},
                     json{{"name", "flip"},   {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "sm_flange"}, {"summary", "Edge flanges on the thickness edges of a sheet (edge ids from query_topology). The plate is trimmed back by the bend setback; height is measured along the flange's outer face from the virtual sharp."},
                 {"params", json::array({
                     json{{"name", "edge"},   {"type", "integer|array"}, {"description", "edge id(s) at the end of the sheet; several edges in one call close their corners"}},
                     json{{"name", "height"}, {"type", "number"}, {"unit", "mm"}, {"default", 20}},
                     json{{"name", "angle"},  {"type", "number"}, {"unit", "deg"}, {"default", 90}, {"max", 170}},
                     json{{"name", "flip"},   {"type", "boolean"}, {"default", false}, {"description", "bend toward the other face"}},
                     json{{"name", "body"},   {"type", "integer"}, {"default", -1}},
                 })}},
            json{{"name", "sm_contour"}, {"summary", "Contour flange: an open chain of lines becomes a bent sheet of the given width along the sketch normal; material grows to the right of travel (travel x normal)."},
                 {"params", json::array({
                     json{{"name", "sketch"}, {"type", "integer"}},
                     json{{"name", "width"},  {"type", "number"}, {"unit", "mm"}, {"default", 20}},
                     json{{"name", "flip"},   {"type", "boolean"}, {"default", false}},
                 })}},
            json{{"name", "sm_hem"}, {"summary", "Flat hem: folds the end of a flange back over its sheet. length is measured to the outside of the fold."},
                 {"params", json::array({
                     json{{"name", "edge"},   {"type", "integer"}, {"description", "the flange's end edge on the face to fold away from"}},
                     json{{"name", "length"}, {"type", "number"}, {"unit", "mm"}, {"default", 8}},
                     json{{"name", "body"},   {"type", "integer"}, {"default", -1}},
                 })}},
            json{{"name", "sm_unfold"}, {"summary", "Unfold every bend into a flat pattern (the first sheet face stays put)."}, {"params", json::array()}},
            json{{"name", "sm_refold"}, {"summary", "Fold an unfolded sheet back; cuts made while flat are carried into the folded part."},
                 {"params", json::array({ json{{"name", "unfold"}, {"type", "integer"}, {"description", "feature index returned by sm_unfold"}} })}},
            json{{"name", "chamfer"}, {"summary", "Chamfer measured edges of a body (edge ids from query_topology on that body)."},
                 {"params", json::array({
                     json{{"name", "edge"},     {"type", "integer|array"}, {"description", "one edge id, or an array of ids chamfered together in one feature"}},
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
                     json{{"name", "features"}, {"type", "array"}, {"description", "FEATURE pattern: repeat what these earlier features (indices) did to the body, instead of copying the body. Then `axis`/`origin` replace plane/dir: linear = count x spacing along `axis` (+ `count2` x `spacing2` along `axis2` for a grid); circular = count over `angle` about the line through `origin` along `axis`"}},
                     json{{"name", "path_sketch"}, {"type", "integer"}, {"description", "feature pattern along a curve: sketch feature index; `spacing` is then an arc length along its entity `path_entity`, instance k = the features moved by path(k*spacing) - path(0)"}},
                     json{{"name", "path_entity"}, {"type", "integer"}, {"default", 0}},
                     json{{"name", "mirror"},   {"type", "boolean"}, {"default", false}, {"description", "feature pattern: reflect what the features did in the plane through `origin` with normal `axis`, instead of repeating it"}},
                     json{{"name", "axis"},     {"type", "array"}, {"description", "[x,y,z] direction (feature pattern); the plane normal when mirroring"}},
                     json{{"name", "origin"},   {"type", "array"}, {"description", "[x,y,z] a point on the rotation axis in model coordinates, like sketch coordinates (circular feature pattern)"}},
                     json{{"name", "axis2"},    {"type", "array"}, {"description", "[x,y,z] second grid direction (linear feature pattern)"}},
                     json{{"name", "count2"},   {"type", "integer"}, {"default", 1}, {"min", 1}},
                     json{{"name", "spacing2"}, {"type", "number"}, {"unit", "mm"}, {"default", 0}},
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
                     json{{"name", "faces"},     {"type", "array"}, {"description", "several face ids to leave open"}},
                     json{{"name", "body"},      {"type", "integer"}, {"default", -1}, {"description", "target body; omit for the last body"}},
                 })}},
            json{{"name", "draft"}, {"summary", "Taper a body face by an angle about its base (pull direction +Z)."},
                 {"params", json::array({
                     json{{"name", "face"},  {"type", "integer"}, {"description", "face id to draft (query_topology)"}},
                     json{{"name", "faces"}, {"type", "array"}, {"description", "several face ids drafted together (instead of face)"}},
                     json{{"name", "pull"},  {"type", "array"}, {"description", "[x,y,z] pull direction (default +Z); with `neutral`"}},
                     json{{"name", "neutral"}, {"type", "array"}, {"description", "[x,y,z] a point on the neutral plane, which is perpendicular to the pull and stays put (default: the body's lowest Z)"}},
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
            json{{"name", "use"}, {"summary", "Use / Project a sketch onto a plane, parent-linked: editing the source moves the projection; deleting the source is refused. Lines/points/splines project onto any plane, circles and arcs onto parallel planes only."},
                 {"params", json::array({
                     json{{"name", "source_sketch"}, {"type", "integer"}, {"description", "feature index of the sketch to project"}},
                     json{{"name", "plane"},  {"type", "string"}, {"default", "XY"}, {"description", "XY, XZ, YZ or datum:N"}},
                     json{{"name", "entities"}, {"type", "array"}, {"default", json::array()}, {"description", "entity indices of the source; empty = all"}}})}},
            json{{"name", "sketch3d"}, {"summary", "Create a free 3D curve through world-mm points (polyline, or a smooth spline). It makes no solid; use it as a Sweep path."},
                 {"params", json::array({
                     json{{"name", "points"}, {"type", "array"}, {"description", "[[x,y,z], ...], at least two"}},
                     json{{"name", "smooth"}, {"type", "boolean"}, {"default", false}},
                     json{{"name", "closed"}, {"type", "boolean"}, {"default", false}}})}},
            json{{"name", "new_document"}, {"summary", "Clear every feature (undoable). For test rigs that need a known starting document."},
                 {"params", json::array()}},
            json{{"name", "ui_option"}, {"summary", "Switch a Design UI option: imgui_tree = draw the feature tree on the canvas (ImGui) instead of the side panel; imgui_ribbon = draw the tool ribbon on the canvas instead of the wx toolbar groups; imgui_menu = the right-click offer as an ImGui popup; imgui_topbar = the row above the viewport (document actions, toolbox, Commit, Confirm/Cancel) drawn on the canvas instead of the wx row."},
                 {"params", json::array({
                     json{{"name", "name"},  {"type", "string"}, {"enum", json::array({"imgui_tree", "imgui_ribbon", "imgui_menu"})}},
                     json{{"name", "value"}, {"type", "boolean"}, {"default", false}}})}},
            json{{"name", "plane"}, {"summary", "Create a datum plane: offset (mm) from base 0=XY, 1=XZ, 2=YZ or 3+N = Nth earlier datum, optional tilt (deg) about axis. Sketch on it with sketch_begin plane=datum:N."},
                 {"params", json::array({
                     json{{"name", "base"},   {"type", "integer"}, {"default", 0}},
                     json{{"name", "offset"}, {"type", "number"}, {"unit", "mm"}, {"default", 0}},
                     json{{"name", "tilt"},   {"type", "number"}, {"unit", "deg"}, {"default", 0}},
                     json{{"name", "axis"},   {"type", "integer"}, {"default", 0}}})}},
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
            json{{"name", "inspect_design"}, {"summary", "One-call state of the design: features, bodies (bbox, faces/edges, volume, area, centre of mass), variables, error feature, undo/redo depth, whether a sketch is open."},
                 {"params", json::array({
                     json{{"name", "measure"}, {"type", "boolean"}, {"default", true}, {"description", "false skips the per-body mass properties"}},
                 })}},
            json{{"name", "sketch_constrain"}, {"summary", "Add geometric constraints and dimensions to the open sketch; each is solved on its own and an inconsistent one is rejected alone (named in `rejected`). Entity refs are indices from sketch_add; points are \"3.start\", \"3.end\", \"3.center\", \"origin\"."},
                 {"params", json::array({
                     json{{"name", "atomic"}, {"type", "boolean"}, {"default", true}, {"description", "true: all constraints are solved as one system, all or nothing; false: each on its own, conflicting ones rejected alone"}},
                     json{{"name", "constraints"}, {"type", "array"}, {"description", "[{type, a, b, c, value, orientation}]. types: coincident, horizontal|vertical (one line), horizontal_points|vertical_points, distance (value mm, orientation aligned|horizontal|vertical), fix, lock_x, lock_y, equal, parallel, perpendicular, collinear, tangent, concentric, midpoint (point a, line b), symmetric (points a,b about line c), symmetric_x (a,b mirror images across the sketch X axis) | symmetric_y (across the Y axis), point_on_line, point_on_object, radius, diameter (value mm), angle (two lines, value deg)"}},
                 })}},
            json{{"name", "export"}, {"summary", "Write the design's bodies to a STEP, IGES, STL (binary) or OBJ file (absolute path under $HOME or /tmp; ORCA_CAD_MCP_ALLOW overrides the roots). The project itself is `save`."},
                 {"params", json::array({
                     json{{"name", "path"},   {"type", "string"}},
                     json{{"name", "format"}, {"type", "string"}, {"enum", json::array({"step", "iges", "stl", "obj"})}, {"default", "step"}},
                 })}},
            json{{"name", "undo"}, {"summary", "Undo the last feature-level change (one step per user action). ok is true even when there was nothing to undo; check `did`."},
                 {"params", json::array()}},
            json{{"name", "redo"}, {"summary", "Redo the last undone change."},
                 {"params", json::array()}},
            json{{"name", "batch"}, {"summary", "Run several tool calls in one turn. Stops at the first error, or when time_budget_s (default 30) is used up, and (rollback=true) restores the features and variables it started with. Sketch-session calls are not rolled back, so commit sketches inside the batch."},
                 {"params", json::array({
                     json{{"name", "commands"}, {"type", "array"}, {"description", "[{command: <tool name>, params: {...}}, ...], at most 10000"}},
                     json{{"name", "rollback"}, {"type", "boolean"}, {"default", true}},
                     json{{"name", "verbose"},  {"type", "boolean"}, {"default", true}, {"description", "false returns only ok per step"}},
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
        json row{
            {"index", int(i)}, {"type", feature_type_name(f.type)},
            {"name", f.name}, {"enabled", f.enabled}};
        if (f.attach_face >= 0) row["attached_to"] = json{{"body", f.attach_body}, {"face", f.attach_face}};   // a sketch riding a face
        features.push_back(std::move(row));
    }

    json bodies = json::array();
    for (size_t i = 0; i < doc.bodies.size(); ++i) {
        // The user's name when the body has one, the derived maker name otherwise — the same
        // rule the Bodies row shows, so a driver and a person read the same thing.
        json b{{"index", int(i)},
               {"name", doc.bodies[i].has_user_name ? doc.bodies[i].user_name : doc.bodies[i].name},
               {"user_name", doc.bodies[i].has_user_name},
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
// Compact descriptors for the methods the hand-written core above does not list. {name, type, description}.
struct P { const char* n; const char* t; const char* d; };
json compact_tool(const char* name, const std::string& summary, std::initializer_list<P> ps)
{
    json a = json::array();
    for (const P& p : ps) a.push_back(json{{"name", p.n}, {"type", p.t}, {"description", p.d}});
    return json{{"name", name}, {"summary", summary}, {"params", a}};
}

json describe_tools()
{
    json out = describe_tools_core();
    json& tools = out["tools"];
    const P kEnt{"entities", "array", "[{type:line|circle|arc|ellipse|spline|point, ...}] or indices of existing entities"};
    for (json& t : std::vector<json>{
        compact_tool("sketch_begin", "Open a sketch on a plane (XY, XZ, YZ or datum:N). Every plane passes through modeling_origin (see describe_scene), so sketch coordinates are relative to the bed centre. Only one sketch is open at a time.", {{"plane", "string", "XY|XZ|YZ|datum:N"}, {"face", "integer", "instead of plane: sketch on this planar face id (query_topology); the sketch follows the face through upstream edits"}, {"body", "integer", "body of the face, default last"}}),
        compact_tool("sketch_add", "Add entities to the open sketch. Auto-inferred exact relations (coincident joints, H/V, origin) unless infer:false.", {
            {"entities", "array", "[{type:line,p0,p1}|{type:circle,center,radius}|{type:arc,center,radius,start_angle,end_angle}|{type:point,p}...]"},
            {"type", "string", "single-entity shorthand"}, {"rect", "array", "[x0,y0,x1,y1] rectangle shorthand"},
            {"construction", "boolean", "add as construction geometry"}, {"infer", "boolean", "default true; false adds geometry only, so your own constraints are not rejected as redundant"}}),
        compact_tool("sketch_commit", "Close the open sketch into a Sketch feature; returns sketch_index, to hand to extrude/revolve/sweep/loft.", {}),
        compact_tool("sketch_cancel", "Discard the open sketch.", {}),
        compact_tool("sketch_select", "Select sketch entities by index.", {kEnt}),
        compact_tool("sketch_delete", "Delete the selected (or given) sketch entities.", {kEnt}),
        compact_tool("sketch_construction", "Toggle construction on the selected (or given) entities.", {kEnt}),
        compact_tool("sketch_offset", "Offset the selected entities by a distance.", {{"distance", "number", "mm, signed"}}),
        compact_tool("sketch_mirror", "Mirror the selected entities about a line entity.", {{"axis", "integer", "line entity index to mirror about"}}),
        compact_tool("sketch_describe", "Entities, constraints, degrees of freedom and solve state of the open sketch.", {}),
        compact_tool("sketch_validate", "Check the open sketch for open loops / overlaps before it is used as a profile.", {}),
        compact_tool("sketch_heal", "Repair small gaps and duplicate entities in the open sketch.", {}),
        compact_tool("sketch_set_value", "Set the dimension of the current selection (line length, radius, distance between two entities).", {{"value", "number", "mm or degrees"}}),
        compact_tool("list_verbs", "The GUI's tool vocabulary: every verb with its enabled state for the current selection.", {{"applicable_only", "boolean", "hide verbs that do not apply"}}),
        compact_tool("run_verb", "Fire a GUI tool verb as the toolbar would. Opens its card, so it is not batchable.", {{"verb", "string", "verb id from list_verbs"}}),
        compact_tool("set_variable", "Create or change a named variable (expression, units-free mm).", {{"name", "string", ""}, {"expr", "string", "number or expression"}}),
        compact_tool("set_parameter", "Same as set_variable, and says how many features the change rebuilt (recomputed) and how many came from the cache (restored).", {{"name", "string", ""}, {"expr", "string", "number or expression"}}),
        compact_tool("list_commands", "Every tool and every ribbon verb with whether it can run now and, if not, why. filter = a substring of the name.", {{"filter", "string", "optional substring"}}),
        compact_tool("execute", "Run a tool by name or a ribbon verb by id (see list_commands). verbose:false returns only ok and the error.", {{"command", "string", "tool name or verb id"}, {"params", "object", "that command's parameters"}, {"verbose", "boolean", "default true"}}),
        compact_tool("set_feature_expr", "Bind a feature field to an expression and recompute. An unknown field is refused (\"unknown parameter\"). Fields: " + bindable_fields_text() + ", and dim:<N> for sketch dimension N.", {{"feature", "integer", "feature index"}, {"field", "string", "field name"}, {"expr", "string", "expression over the variables"}}),
        compact_tool("sweep", "Sweep a profile sketch along a path sketch.", {{"profile", "integer", "Sketch feature index"}, {"path", "integer", "Sketch feature index"}, {"boolean", "string", "new|union|subtract|intersect"}}),
        compact_tool("loft", "Loft through two or more profile sketches, in order.", {{"profiles", "array", "[Sketch feature index,...]"}, {"ruled", "boolean", "straight (ruled) instead of smooth"}, {"boolean", "string", "new|union|subtract|intersect"}}),
        compact_tool("thread", "Helical thread about the plane normal at (x,y): external adds material to the last body, internal cuts it.", {
            {"radius", "number", "major radius, mm"}, {"pitch", "number", "mm per turn"}, {"height", "number", "axial length"}, {"depth", "number", "radial depth"},
            {"internal", "boolean", ""}, {"x", "number", ""}, {"y", "number", ""}, {"plane", "string", "XY|XZ|YZ"}}),
        compact_tool("cut", "Trim a body by a plane (split by plane).", {{"plane", "string", "XY|XZ|YZ"}, {"offset", "number", "mm along the plane normal"}, {"flip", "boolean", ""},
            {"origin", "array", "[x,y,z] model coords of a point on the plane (with normal)"}, {"normal", "array", "[x,y,z]: an explicit plane of any tilt instead of `plane`"}, {"keep", "string", "upper|lower|both (both splits into two bodies)"}, {"body", "integer", "default last"}}),
        compact_tool("surface_fill", "Fill a closed sketch boundary with a smooth sheet face.", {{"sketch", "integer", "Sketch feature index"}}),
        compact_tool("surface_loft", "Skin two or more profile sketches into an open sheet.", {{"profiles", "array", "[Sketch feature index,...]"}, {"ruled", "boolean", ""}}),
        compact_tool("surface_offset", "Offset the last sheet body.", {{"offset", "number", "signed mm"}}),
        compact_tool("thicken_surface", "Thicken the last sheet body into a solid.", {{"thickness", "number", "mm"}, {"flip", "boolean", ""}}),
        compact_tool("ui.candidates", "Select Other: what a click at at=[x,y] could take, nearest first (kind/body/face/edge). Also sets filter={face,edge,vertex} (the SELECT filter) and cycle=true|false (click cycling vs escalate-to-body).", {}),
    compact_tool("ui.inspect", "GUI state: which window has keyboard focus, whether ImGui wants text/keyboard/mouse, canvas size, sketch open. For focus and click regression tests.", {}),
        compact_tool("ui.worldToScreen", "Pixel (canvas coordinates) of a world point under the current camera.", {{"point", "array", "[x,y,z] mm"}}),
        compact_tool("ui.move", "Move the pointer over the design canvas (canvas pixels).", {{"at", "array", "[x,y] px"}, {"mods", "array", "shift|ctrl|alt"}}),
        compact_tool("ui.click", "Click on the design canvas through the real event path: move, press, release (count 2 = double click).", {{"at", "array", "[x,y] px"}, {"button", "integer", "1 left, 2 middle, 3 right"}, {"count", "integer", "1|2"}, {"mods", "array", "shift|ctrl|alt"}}),
        compact_tool("ui.drag", "Press at `from`, move to `to` in `steps`, release.", {{"from", "array", "[x,y] px"}, {"to", "array", "[x,y] px"}, {"steps", "integer", "default 8"}, {"step_ms", "integer", "pause per move, default 25"}, {"button", "integer", "1|2|3"}, {"mods", "array", "shift|ctrl|alt"}}),
        compact_tool("ui.tree", "Rig driver for the on-canvas tree row commands: copy | paste | delete | toggle (suppress) on a row; the selection is ui.inspect tree.selected.", {{"event", "string", "copy|paste|delete|toggle"}, {"row", "integer", "feature row"}}),
        compact_tool("ui.card", "Rig driver for the wx feature cards: open (dressup|extrude|shell|draft|pattern|sheet[:mode]), set {field: number}, check [feature indices] (pattern), confirm.", {{"open", "string", "card name"}, {"set", "object", "field values: size size2 law distance thin_in thin_out shell_thickness draft_angle add_shell_face add_draft_face pattern_what pattern_type pattern_axis pattern_axis2 count spacing angle count2 spacing2 mirror_offset sheet_thickness sheet_radius sheet_k sheet_gap sheet_height sheet_angle sheet_length sheet_flip sheet_mode"}, {"check", "array", "feature indices to repeat"}, {"confirm", "boolean", "press the check mark"}}),
        compact_tool("ui.key", "Press a key as the keyboard delivers it (shortcut gate first, then the focused window).", {{"key", "string", "a character, or Escape|Enter|Tab|Backspace|Delete|Space|Left|Right|Up|Down|Home|End|PageUp|PageDown|F1-F12"}, {"mods", "array", "shift|ctrl|alt"}}),
        compact_tool("ui.text", "Type text into the focused window, one character event each.", {{"text", "string", "up to 4096 characters"}}),
        compact_tool("ui.screenshot", "Same as screenshot.", {{"path", "string", "absolute .png"}, {"view", "string", ""}, {"fit", "boolean", ""}}),
        compact_tool("rename_variable", "Rename a variable; every expression that uses it follows (token-wise).", {{"from", "string", ""}, {"to", "string", ""}}),
        compact_tool("variables_export", "The variables as CSV (name,expression); also written to `path` when given.", {{"path", "string", "optional absolute .csv"}}),
        compact_tool("variables_import", "Add or replace variables from CSV text or a file; all rows are checked first.", {{"csv", "string", ""}, {"path", "string", ""}}),
        compact_tool("sketch_dimensions", "The dimensions of a committed sketch with their index, value and bound expression. Bind one with set_feature_expr {feature: sketch, field: \"dim:<index>\", expr}.", {{"sketch", "integer", "Sketch feature index"}}),
        compact_tool("rename_feature", "Rename a feature in the history.", {{"feature", "integer", "feature index"}, {"name", "string", "new name"}}),
        compact_tool("move_feature", "Move a feature to another history position; stops where a dependency (a sketch below its consumer) forbids the next step.", {{"feature", "integer", "feature index"}, {"to", "integer", "destination index"}}),
        compact_tool("suppress", "Suppress (or restore) a feature; downstream features rebuild without it.", {{"feature", "integer", "feature index"}, {"suppressed", "boolean", "default true"}}),
        compact_tool("rollback", "Roll the history back to just after a feature (later ones are kept but not built); -1 restores the full history.", {{"index", "integer", "feature index or -1"}}),
        compact_tool("save", "Save the project (the design recipe travels inside the 3MF) to an absolute .3mf path under $HOME or /tmp.", {{"path", "string", ""}}),
        compact_tool("open", "Open a .3mf project, replacing the current one. Refused while it has unsaved changes unless discard:true. Applied on the next UI turn.", {{"path", "string", ""}, {"discard", "boolean", ""}}),
        compact_tool("screenshot", "PNG of the design viewport (X11 screen read-back; the window must be visible).", {{"path", "string", "absolute .png under $HOME or /tmp"}, {"view", "string", "iso|top|front|... applied first"}, {"fit", "boolean", "fit the view first"}}),
    }) {
        bool dup = false;
        for (const json& e : tools) dup = dup || e["name"] == t["name"];
        if (!dup) tools.push_back(std::move(t));
    }
    for (json& t : tools) {   // MCP annotations
        const bool ro = read_only_method(t["name"].get<std::string>());
        t["readOnly"] = ro;
        t["destructive"] = !ro && (t["name"] == "open" || t["name"] == "new_document");
    }
    return out;
}

SketchPlane plane_from(const json& params, const CadDocument& doc)
{
    if (params.contains("normal")) {
        auto v3 = [&](const char* k) {
            const json& a = params[k];
            if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number()) throw std::runtime_error(std::string(k) + " must be [x,y,z]");
            return Vec3d(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
        };
        SketchPlane pl;
        const Vec3d nrm = v3("normal");
        if (nrm.norm() < 1e-9) throw std::runtime_error("normal must not be zero");
        pl.normal = nrm.normalized();
        pl.origin = (params.contains("origin") ? v3("origin") : Vec3d(0, 0, 0)) + doc.modeling_origin;   // model coordinates in, world stored
        const Vec3d h = std::abs(pl.normal.x()) < 0.9 ? Vec3d(1, 0, 0) : Vec3d(0, 1, 0);
        pl.x_axis = (h - pl.normal * h.dot(pl.normal)).normalized();
        pl.y_axis = pl.normal.cross(pl.x_axis);
        return pl;
    }
    std::string n = params.value("plane", std::string("XY"));
    if (n.rfind("datum:", 0) == 0) {   // "datum:N" = the Nth datum plane, as sketch_begin takes it
        const auto datums = doc.resolve_datum_planes();
        const int  k      = std::atoi(n.c_str() + 6);
        if (k < 0 || k >= int(datums.size())) throw std::runtime_error("no such datum plane: " + n);
        return datums[k].second;
    }
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
    for (const auto& p : params["profile"]) out.points.emplace_back(jn(p, 0), jn(p, 1));
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
        if (pts.size() > 2) {   // curved: a line is fixed by its ends, a curve is not - give the midpoint by arc length and a thinned polyline
            double acc = 0; Vec3d mid = pts.front();
            for (size_t k = 1; k < pts.size() && acc < len * 0.5; ++k) {
                const double seg = (pts[k] - pts[k-1]).norm();
                mid = seg > 0 && acc + seg >= len * 0.5 ? Vec3d(pts[k-1] + (pts[k] - pts[k-1]) * ((len * 0.5 - acc) / seg)) : pts[k];
                acc += seg;
            }
            je["mid"] = vec3(mid);
            json smp = json::array();
            const size_t step = std::max<size_t>(1, pts.size() / 12);
            for (size_t k = 0; k < pts.size(); k += step) smp.push_back(vec3(pts[k]));
            smp.push_back(vec3(pts.back()));
            je["samples"] = std::move(smp);
        }
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
    if (ref.contains("point")) { const json& p = ref["point"]; point = Vec3d(jn(p, 0), jn(p, 1), jn(p, 2)); return true; }
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
    const TopoDS_Shape& shape = body_shape(panel, params);
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
        doc.append_feature(f);
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
    doc.append_feature(f);

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
    double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
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

// An existing Sketch feature to extrude/revolve instead of a fresh polygon: -1 when not given.
// sketch_commit returns the index as `sketch_index`. Checked BEFORE any checkpoint so a bad index leaves no
// stray undo step.
int sketch_ref_arg(const json& params, const CadDocument& doc)
{
    if (!params.contains("sketch")) return -1;
    if (!params["sketch"].is_number_integer()) throw std::runtime_error("'sketch' must be a feature index");
    const int s = params["sketch"].get<int>();
    if (s < 0 || s >= int(doc.features.size()) || doc.features[s].type != CadFeatureType::Sketch)
        throw std::runtime_error("'sketch' must be the index of a Sketch feature (sketch_commit returns it as sketch_index)");
    return s;
}

json action_extrude(DesignPanel* panel, const json& params)
{
    const double d = params.value("distance", 10.0);
    if (d <= 0) throw std::runtime_error("distance must be > 0");
    CadDocument& doc = panel->mcp_doc();
    SketchPlane pl = plane_from(params, doc);
    BooleanMode mode = bool_from(params.value("boolean", std::string("new")));

    const int sk = sketch_ref_arg(params, doc);
    SketchProfile prof;
    bool has_prof = sk < 0 && profile_from(params, prof);
    double w = 0, h = 0;
    if (!has_prof && sk < 0) {
        w = params.value("width", 20.0); h = params.value("height", 20.0);
        if (w <= 0 || h <= 0) throw std::runtime_error("width and height must be > 0");
    }
    doc.checkpoint();
    int s = sk >= 0 ? sk
          : has_prof ? doc.add_sketch_profile(prof, pl, "Sketch")
                     : doc.add_sketch(SketchShape::Rectangle, pl, w, h, 0.0, "Sketch");
    int e = doc.add_extrude(s, d, /*symmetric*/false, mode, "Extrude");
    // End condition (Onshape parity). apply reads extrude_end directly; the kernel `symmetric`
    // bool is unused, so set the field here. up_to_face id comes from query_topology on the
    // target (last) body. taper_deg lofts the side wall; flip negates the direction.
    const std::string end = params.value("end", std::string("blind"));
    CadFeature& fe = doc.features[e];
    fe.flip      = params.value("flip", false);
    fe.taper_deg = params.value("taper", 0.0);
    if (params.contains("entities")) {
        if (sk < 0 || !params["entities"].is_array() || params["entities"].empty() || params["entities"].size() > 256) { doc.undo(); throw std::runtime_error("entities needs an existing 'sketch' and a non-empty array of entity indices"); }
        for (const json& x : params["entities"]) {
            if (!x.is_number_integer() || x.get<int>() < 0) { doc.undo(); throw std::runtime_error("entities must hold entity indices"); }
            fe.extrude_entities.push_back(x.get<int>());
        }
    }
    fe.thin_in   = params.value("thin_in", 0.0);
    fe.thin_out  = params.value("thin_out", 0.0);
    if (fe.thin_in < 0 || fe.thin_out < 0 || fe.thin_in > 1e6 || fe.thin_out > 1e6) { doc.undo(); throw std::runtime_error("thin_in and thin_out must be >= 0"); }
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

    const int sk = sketch_ref_arg(params, doc);
    SketchProfile prof;
    bool has_prof = sk < 0 && profile_from(params, prof);
    double w = 0, h = 0;
    if (!has_prof && sk < 0) {   // ponytail: rectangle centred on the axis may self-overlap; offset via `profile`
        w = params.value("width", 20.0); h = params.value("height", 10.0);
        if (w <= 0 || h <= 0) throw std::runtime_error("width and height must be > 0");
    }
    doc.checkpoint();
    int s = sk >= 0 ? sk
          : has_prof ? doc.add_sketch_profile(prof, pl, "Sketch")
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
    int f = doc.add_fillet(radius, edges, "Fillet");
    if (bi >= 0) doc.features[f].target_body = bi;   // edge id resolved against THIS body's shape
    {   // size law: radius_end = a radius running linearly from `radius` (at the end nearest `start`) to this; chord = `radius` is a width
        CadFeature& cf = doc.features[f];
        if (params.contains("radius_end")) {
            if (!params["radius_end"].is_number() || params["radius_end"].get<double>() < 0) { doc.undo(); throw std::runtime_error("radius_end must be a number >= 0"); }
            cf.dressup_size2 = params["radius_end"].get<double>();
        }
        if (params.contains("radii") || params.contains("knots")) {
            auto nums = [&](const char* k) {
                std::vector<double> v;
                if (!params.contains(k) || !params[k].is_array() || params[k].empty() || params[k].size() > 64) { doc.undo(); throw std::runtime_error(std::string(k) + " must be a non-empty array of at most 64 numbers"); }
                for (const json& x : params[k]) { if (!x.is_number()) { doc.undo(); throw std::runtime_error(std::string(k) + " must hold numbers"); } v.push_back(x.get<double>()); }
                return v;
            };
            cf.dressup_radii = nums("radii");
            cf.dressup_knots = nums("knots");
        }
        cf.dressup_chord = params.value("chord", false);
        if (params.contains("fit")) {
            if (!params["fit"].is_string() || params["fit"].get<std::string>() != "taper") { doc.undo(); throw std::runtime_error("fit must be \"taper\""); }
            cf.dressup_fit_taper = true;
        }
        if (cf.dressup_chord && cf.dressup_size2 >= 0) { doc.undo(); throw std::runtime_error("chord and radius_end cannot be combined"); }
        if (params.contains("start")) {
            const json& s = params["start"];
            if (!s.is_array() || s.size() != 3 || !s[0].is_number() || !s[1].is_number() || !s[2].is_number()) { doc.undo(); throw std::runtime_error("start must be [x,y,z]"); }
            cf.dressup_has_start = true;
            cf.dressup_start     = Vec3d(s[0].get<double>(), s[1].get<double>(), s[2].get<double>());
        }
    }
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
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
    int c = doc.add_chamfer(dist, edges, "Chamfer");
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
    std::vector<std::pair<double, double>> pts;   // `points` [[x,y],...] = one hole each, one undo step; else the single x,y
    if (params.contains("points")) {
        if (!params["points"].is_array() || params["points"].empty()) throw std::runtime_error("'points' must be a non-empty [[x,y],...]");
        if (params["points"].size() > 500) throw std::runtime_error("at most 500 hole points per call");
        for (const json& p : params["points"]) {
            if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) throw std::runtime_error("each point must be [x, y]");
            pts.emplace_back(jn(p, 0), jn(p, 1));
        }
    } else pts.emplace_back(x, y);
    doc.checkpoint();
    int h = -1;
    const double tip = params.value("tip_angle", 0.0);
    if (tip < 0 || tip >= 180) throw std::runtime_error("tip_angle must be in [0, 180)");
    for (const auto& p : pts) { h = doc.add_hole(dia, depth, thru, p.first, p.second, pl, "Hole"); doc.features[size_t(h)].hole_tip_angle = tip; }
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
    std::vector<std::pair<double, double>> pts;   // `points` [[x,y],...] = one styled hole each, one undo step
    if (params.contains("points")) {
        if (!params["points"].is_array() || params["points"].empty()) throw std::runtime_error("'points' must be a non-empty [[x,y],...]");
        if (params["points"].size() > 500) throw std::runtime_error("at most 500 hole points per call");
        for (const json& p : params["points"]) {
            if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) throw std::runtime_error("each point must be [x, y]");
            pts.emplace_back(jn(p, 0), jn(p, 1));
        }
    } else pts.emplace_back(x, y);
    const double tip = params.value("tip_angle", 0.0);
    if (tip < 0 || tip >= 180) throw std::runtime_error("tip_angle must be in [0, 180)");
    doc.checkpoint();
    int h = -1;
    for (const auto& p : pts) {
        h = doc.add_hole_styled(dia, depth, thru, p.first, p.second, pl, style,
                                cbore_diameter, cbore_depth,
                                csink_diameter, csink_angle, standard, "Hole");
        doc.features[size_t(h)].hole_tip_angle = tip;
    }
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

// Sheet metal: sm_rule | sm_base | sm_flange | sm_contour | sm_hem | sm_unfold | sm_refold
json action_sheet(DesignPanel* panel, const std::string& m, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    auto num = [&](const char* k, double def, double lo, double hi) {
        const double v = params.value(k, def);
        if (!(v >= lo && v <= hi)) throw std::runtime_error(std::string(k) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
        return v;
    };
    const bool flip = params.value("flip", false);
    std::function<int()> make;
    if (m == "sm_rule") {
        const double t = num("thickness", 2.5, 1e-3, 1000), r = params.value("bend_radius", -1.0), k = num("k_factor", 0.44, 0.0, 1.0), g = num("gap", 0.01, 0.0, 100);
        make = [&, t, r, k, g] { return doc.add_sm_rule(t, r, k, g, "Sheet metal rule"); };
    } else if (m == "sm_base") {
        const int sk = sketch_ref_arg(params, doc);
        if (sk < 0) throw std::runtime_error("sm_base needs 'sketch' (a closed profile)");
        make = [&, sk] { return doc.add_sm_base(sk, flip, "Base flange"); };
    } else if (m == "sm_flange") {
        if (doc.bodies.empty()) throw std::runtime_error("no sheet body to flange");
        const std::vector<int> edges = edge_ids_arg(params, "sm_flange");
        const double h = num("height", 20.0, 1e-3, 1e5), a = num("angle", 90.0, 1e-3, 170.0);
        make = [&, edges, h, a] { return doc.add_sm_edge_flange(edges, h, a, flip, "Edge flange"); };
    } else if (m == "sm_contour") {
        const int sk = sketch_ref_arg(params, doc);
        if (sk < 0) throw std::runtime_error("sm_contour needs 'sketch' (an open chain of lines)");
        const double w = num("width", 20.0, 1e-3, 1e5);
        make = [&, sk, w] { return doc.add_sm_contour_flange(sk, w, flip, "Contour flange"); };
    } else if (m == "sm_hem") {
        if (doc.bodies.empty()) throw std::runtime_error("no sheet body to hem");
        const std::vector<int> edges = edge_ids_arg(params, "sm_hem");
        const double len = num("length", 8.0, 1e-3, 1e5);
        make = [&, edges, len] { return doc.add_sm_hem(edges.front(), len, "Hem"); };
    } else if (m == "sm_unfold") {
        make = [&] { return doc.add_sm_unfold("Unfold"); };
    } else {   // sm_refold
        const int u = params.value("unfold", -1);
        if (u < 0 || u >= int(doc.features.size()) || doc.features[size_t(u)].type != CadFeatureType::SmUnfold)
            throw std::runtime_error("sm_refold needs 'unfold': the index of an Unfold feature");
        make = [&, u] { return doc.add_sm_refold(u, "Refold"); };
    }
    const int bi = target_body_arg(params, doc);
    doc.checkpoint();
    const int idx = make();
    if (bi >= 0) doc.features[size_t(idx)].target_body = bi;
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
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
    if (params.contains("features")) {   // feature pattern: replays the effect of those features, with explicit world axes
        auto vec3 = [&](const char* k, const Vec3d& dflt) {
            if (!params.contains(k)) return dflt;
            const json& a = params[k];
            if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number()) { doc.undo(); throw std::runtime_error(std::string(k) + " must be [x,y,z]"); }
            return Vec3d(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
        };
        CadFeature& pf = doc.features[p];
        const json& fl = params["features"];
        if (!fl.is_array() || fl.empty() || fl.size() > 64) { doc.undo(); throw std::runtime_error("features must be a non-empty array of feature indices"); }
        for (const json& x : fl) {
            if (!x.is_number_integer() || x.get<int>() < 0 || x.get<int>() >= p) { doc.undo(); throw std::runtime_error("features: every index must name an earlier feature"); }
            pf.pattern_features.push_back(x.get<int>());
        }
        pf.pattern_axis    = vec3("axis", Vec3d(1, 0, 0));
        pf.pattern_origin  = vec3("origin", Vec3d(0, 0, 0)) + doc.modeling_origin;   // given in model coordinates, stored in world ones
        pf.pattern_axis2   = vec3("axis2", Vec3d(0, 1, 0));
        pf.pattern_mirror  = params.value("mirror", false);
        if (params.contains("path_sketch")) {   // spacing is then an arc length along that sketch curve
            const int ps = params["path_sketch"].get<int>(), pe = params.value("path_entity", 0);
            if (ps < 0 || ps >= p || doc.features[size_t(ps)].type != CadFeatureType::Sketch || pe < 0 || pe >= int(doc.features[size_t(ps)].entities.size())) {
                doc.undo(); throw std::runtime_error("path_sketch/path_entity must name an earlier sketch and one of its entities");
            }
            pf.pattern_curve_sketch = ps;
            pf.pattern_curve_entity = pe;
        }
        pf.pattern_count2  = std::clamp(params.value("count2", 1), 1, 500);
        pf.pattern_spacing2 = params.value("spacing2", 0.0);
    }
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
    if (params.contains("faces")) {
        if (!params["faces"].is_array() || params["faces"].size() > 64) { doc.undo(); throw std::runtime_error("faces must be an array of face ids"); }
        for (const json& x : params["faces"]) {
            if (!x.is_number_integer() || x.get<int>() < 0) { doc.undo(); throw std::runtime_error("faces must hold face ids"); }
            doc.features[s].shell_faces.push_back(x.get<int>());
        }
    }
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
    if (!params.contains("face") && !params.contains("faces")) throw std::runtime_error("draft needs 'face' or 'faces' (ids from query_topology)");
    const double angle = params.value("angle", 5.0);
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to draft");
    int bi = target_body_arg(params, doc);
    doc.checkpoint();
    int d = doc.add_draft(angle, params.contains("face") ? params["face"].get<int>() : -1, bi, "Draft");
    {
        CadFeature& df = doc.features[d];
        auto vec3 = [&](const char* k) {
            const json& a = params[k];
            if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number()) { doc.undo(); throw std::runtime_error(std::string(k) + " must be [x,y,z]"); }
            return Vec3d(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
        };
        if (params.contains("faces")) {
            if (!params["faces"].is_array() || params["faces"].size() > 64) { doc.undo(); throw std::runtime_error("faces must be an array of face ids"); }
            for (const json& x : params["faces"]) {
                if (!x.is_number_integer() || x.get<int>() < 0) { doc.undo(); throw std::runtime_error("faces must hold face ids"); }
                df.draft_faces.push_back(x.get<int>());
            }
        }
        if (params.contains("pull") || params.contains("neutral")) {
            df.draft_has_neutral = true;
            if (params.contains("pull")) df.draft_pull = vec3("pull");
            df.draft_neutral = params.contains("neutral") ? vec3("neutral") + doc.modeling_origin : doc.modeling_origin;   // model coordinates -> world
        }
    }
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
        return Vec2d(jn(a, 0), jn(a, 1));
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

// The face a sketch being drawn is attached to; written onto the Sketch feature by sketch_commit.
struct PendingAttach { int body{-1}; int face{-1}; };
PendingAttach g_pending_attach;

json action_sketch_begin(DesignPanel* panel, const json& params)
{
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr) throw std::runtime_error("no viewport");
    if (vp->is_sketching()) throw std::runtime_error("a sketch is already open");
    const std::string pl = params.value("plane", std::string("XY"));
    SketchPlane plane = SketchPlane::XY();
    int face_id = -1;
    g_pending_attach = {};
    if (params.contains("face")) {   // sketch ON a planar face of a body: the sketch then follows that face through upstream edits
        CadDocument& doc = panel->mcp_doc();
        if (!params["face"].is_number_integer()) throw std::runtime_error("'face' must be a face id from query_topology");
        const int body = params.value("body", int(doc.bodies.size()) - 1), face = params["face"].get<int>();
        if (body < 0 || body >= int(doc.bodies.size())) throw std::runtime_error("body index out of range");
        const TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[body].shape, face);
        if (fc.IsNull()) throw std::runtime_error("no such face on that body (ids expire when the model rebuilds)");
        if (!GeometryEngine::cylinder_of_face(fc).ok && GeometryEngine::face_normal_world(fc).norm() < 1e-9) throw std::runtime_error("face is not planar");
        BRepAdaptor_Surface surf(fc);
        if (surf.GetType() != GeomAbs_Plane) throw std::runtime_error("sketch_begin face: the face is not planar");
        Vec3d n = GeometryEngine::face_normal_world(fc).normalized();
        const Vec3d p = GeometryEngine::face_centroid_world(fc), mo = doc.modeling_origin;
        plane.normal = n;
        plane.origin = mo - n * ((mo - p).dot(n));           // the bed-centre point nearest the face, as every other plane
        Vec3d x = Vec3d(1, 0, 0) - n * n.x();
        if (x.norm() < 0.3) x = Vec3d(0, 1, 0) - n * n.y();
        x.normalize();
        plane.x_axis = x;
        plane.y_axis = n.cross(x);
        g_pending_attach = {body, face};
        face_id = face;
    }
    else if (pl == "XZ") plane = SketchPlane::XZ();
    else if (pl == "YZ") plane = SketchPlane::YZ();
    else if (pl.rfind("datum:", 0) == 0) {
        // "datum:N" = the Nth enabled datum plane, the same row (3+N) the Plane card's base combo names.
        const auto datums = panel->mcp_doc().resolve_datum_planes();
        const int  n      = std::atoi(pl.c_str() + 6);
        if (n < 0 || n >= int(datums.size())) throw std::runtime_error("no such datum plane: " + pl);
        plane = datums[n].second;
    }
    else if (pl != "XY") throw std::runtime_error("plane must be XY, XZ, YZ or datum:N");
    // Every plane passes through modeling_origin (the bed centre), as the GUI's, the datum planes', extrude{profile}'s,
    // hole's and mirror's already do. sketch_begin alone used the world origin, so a sketch and its datum plane disagreed.
    if (pl.rfind("datum:", 0) != 0 && !params.contains("face")) plane.origin = panel->mcp_doc().modeling_origin;
    vp->begin_sketch(plane, DesignSketchTool::Mode::Select);
    // The panel has to enter sketch mode too, or the app is half in it: the tool sketches, the
    // offer menu offers sketch verbs, and every sketch KEY is dead because key dispatch tests
    // m_ui_mode while the menu tests the viewport. Driving the socket must leave the GUI in the
    // state a user would be in, not a state only the socket can produce.
    panel->mcp_set_sketch_mode(true);
    // The plane as it is, not as it was asked for: a face is "face:<id>" with its frame, so a client never reads "XY" for a
    // sketch that lies on a tilted or offset face. origin is in model coordinates, like every sketch coordinate.
    auto v3 = [](const Vec3d& v) { return json::array({v.x(), v.y(), v.z()}); };
    return json{{"ok", true}, {"plane", face_id >= 0 ? "face:" + std::to_string(face_id) : pl},
                {"origin", v3(plane.origin - panel->mcp_doc().modeling_origin)}, {"normal", v3(plane.normal)}, {"x_axis", v3(plane.x_axis)}};
}

json action_sketch_commit(DesignPanel* panel, const json& params)
{
    (void)params;
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr || !vp->is_sketching()) throw std::runtime_error("no sketch is open");
    vp->finish_sketch();
    panel->mcp_set_sketch_mode(false);
    panel->mcp_after_change();
    CadDocument& cd = panel->mcp_doc();
    const int last = cd.last_added >= 0 && cd.last_added < int(cd.features.size()) ? cd.last_added : int(cd.features.size()) - 1;
    const bool is_sk = last >= 0 && cd.features[last].type == CadFeatureType::Sketch;
    if (is_sk && g_pending_attach.face >= 0) { cd.features[last].attach_body = g_pending_attach.body; cd.features[last].attach_face = g_pending_attach.face; }
    g_pending_attach = {};
    return json{{"ok", true}, {"features", int(cd.features.size())}, {"sketch_index", is_sk ? last : -1}};
}

json action_sketch_cancel(DesignPanel* panel, const json& params)
{
    (void)params;
    DesignCanvas* vp = panel->mcp_viewport();
    if (vp == nullptr || !vp->is_sketching()) throw std::runtime_error("no sketch is open");
    vp->cancel_sketch();
    panel->mcp_set_sketch_mode(false);
    g_pending_attach = {};
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
        const double x0 = jn(r, 0), y0 = jn(r, 1);
        const double x1 = jn(r, 2), y1 = jn(r, 3);
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
    int base;
    { PhaseLog p("sketch_add.add_entities"); base = t.add_entities_scripted(ents, params.value("infer", true)); }
    panel->mcp_viewport()->request_repaint();
    int dof;
    { PhaseLog p("sketch_add.dof"); dof = t.dof(); }
    return json{{"ok", base >= 0}, {"first_index", base}, {"added", int(ents.size())},
                {"entities", int(t.entities().size())}, {"dof", dof}};
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
        return Vec2d(jn(a, 0), jn(a, 1));
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
    // A profile is buildable when at least one loop closed, nothing is left dangling and no loop
    // crosses or folds back on itself.
    json defects = json::array();
    for (const auto& l : rep.loops)
        if (l.defect) defects.push_back(json::array({l.defect_at.x(), l.defect_at.y()}));
    const bool buildable = !rep.loops.empty() && rep.open_ends.empty() && defects.empty();
    return json{{"closed_loops", loops}, {"open_ends", open_ends}, {"defects", defects}, {"buildable", buildable}};
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

// One vocabulary for the agent: every tool (MCP method) and every offer verb (what the ribbon and the context menu run), with
// whether it can run now and, when it cannot, why - the same sentence the GUI shows.
json describe_tools();
json call_method(DesignPanel* panel, const std::string& method, const json& params);
json action_list_commands(DesignPanel* panel, const json& params)
{
    const std::string filter = params.value("filter", std::string());
    auto match = [&](const std::string& s) { return filter.empty() || s.find(filter) != std::string::npos; };
    json out = json::array();
    const json dt = describe_tools();
    for (const json& t : dt["tools"]) {
        const std::string n = t["name"].get<std::string>();
        if (n == "execute" || !match(n)) continue;
        out.push_back(json{{"command", n}, {"kind", "tool"}, {"summary", t.value("summary", std::string())}, {"read_only", t.value("readOnly", false)}, {"enabled", true}});
    }
    const int kind = panel->mcp_offer_selection_kind();
    const uint32_t bit = offer_bit(OfferSel(kind));
    for (int i = 0; i < kOfferVerbCount; ++i) {
        const OfferVerb& v = kOfferVerbs[i];
        if (!match(v.id) || v.action == nullptr) continue;
        const bool applies = (v.accepts & bit) != 0;
        json e{{"command", std::string("verb:") + v.id}, {"kind", "verb"}, {"summary", v.hint ? v.hint : ""}, {"read_only", false}, {"enabled", applies}};
        if (!applies) e["reason"] = v.refusal ? v.refusal : "does not apply to the current selection";
        out.push_back(std::move(e));
    }
    return json{{"ok", true}, {"count", out.size()}, {"commands", std::move(out)}};
}

// {command, params, verbose}: a tool by name, or an offer verb by id (what the ribbon would run on the current selection).
json action_execute(DesignPanel* panel, const json& params)
{
    if (!params.contains("command") || !params["command"].is_string()) throw std::runtime_error("execute needs 'command' (see list_commands)");
    const std::string cmd = params["command"].get<std::string>();
    const json p = params.contains("params") ? params["params"] : json::object();
    if (!p.is_object()) throw std::runtime_error("'params' must be an object");
    if (cmd == "execute") throw std::runtime_error("execute cannot run itself");
    json r;
    const json dt = describe_tools();
    bool is_tool = false;
    for (const json& t : dt["tools"]) is_tool = is_tool || t["name"] == cmd;
    const bool prefixed = cmd.rfind("verb:", 0) == 0;
    const std::string vid = prefixed ? cmd.substr(5) : cmd;
    bool is_verb = false;
    for (int i = 0; i < kOfferVerbCount; ++i) is_verb = is_verb || (vid == kOfferVerbs[i].id);
    if (is_tool && !prefixed) r = call_method(panel, cmd, p);
    else if (is_verb)         { json vp = p; vp["verb"] = vid; r = call_method(panel, "run_verb", vp); }
    else throw std::runtime_error("unknown command '" + cmd + "' (list_commands lists them; ribbon verbs are \"verb:<id>\")");
    if (!params.value("verbose", true) && r.is_object()) return json{{"ok", r.value("ok", true)}, {"error", r.value("error", std::string())}};
    return r;
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

json action_use(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const int src = params.value("source_sketch", -1);
    if (src < 0 || src >= int(doc.features.size())) throw std::runtime_error("source_sketch must be a sketch feature index");
    const std::string pl = params.value("plane", std::string("XY"));
    SketchPlane plane = SketchPlane::XY();
    if      (pl == "XZ") plane = SketchPlane::XZ();
    else if (pl == "YZ") plane = SketchPlane::YZ();
    else if (pl.rfind("datum:", 0) == 0) {
        const auto datums = doc.resolve_datum_planes();
        const int  n      = std::atoi(pl.c_str() + 6);
        if (n < 0 || n >= int(datums.size())) throw std::runtime_error("no such datum plane: " + pl);
        plane = datums[n].second;
    } else if (pl != "XY") throw std::runtime_error("plane must be XY, XZ, YZ or datum:N");
    std::vector<int> ents;
    if (params.contains("entities"))
        for (const auto& v : params.at("entities")) ents.push_back(v.get<int>());
    doc.checkpoint();
    const int idx = doc.add_use_sketch(src, ents, plane, "Use");
    const bool ok = doc.recompute();
    panel->mcp_after_change();
    return json{{"ok", ok}, {"feature_index", idx}, {"error", doc.error},
                {"entities", ok ? int(doc.features[idx].entities.size()) : 0}};
}

json action_sketch3d(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (!params.contains("points") || !params["points"].is_array() || params["points"].size() < 2)
        throw std::runtime_error("sketch3d needs 'points': [[x,y,z], ...] with at least two points");
    std::vector<Vec3d> pts;
    for (const auto& p : params["points"]) {
        if (!p.is_array() || p.size() < 3) throw std::runtime_error("each point must be [x, y, z]");
        pts.emplace_back(jn(p, 0), jn(p, 1), jn(p, 2));
    }
    doc.checkpoint();
    const int idx = doc.add_sketch3d(pts, params.value("smooth", false), params.value("closed", false), "Sketch3D");
    const bool ok = doc.recompute();
    panel->mcp_after_change();
    return json{{"ok", ok}, {"feature_index", idx}, {"error", doc.error}};
}

json action_new_document(DesignPanel* panel, const json& params)
{
    (void)params;
    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    doc.features.clear();
    doc.variables.clear();   // a new document starts with no variables and no history marker
    doc.rollback_index = -1;
    doc.recompute();
    panel->mcp_drop_selection();
    panel->mcp_after_change();
    return json{{"ok", true}, {"features", 0}};
}

json action_ui_option(DesignPanel* panel, const json& params)
{
    const std::string name = params.value("name", std::string());
    const bool        on   = params.value("value", false);
    if (name == "imgui_tree")   { panel->mcp_set_imgui_tree(on);   return json{{"ok", true}, {"imgui_tree", on}}; }
    if (name == "imgui_ribbon") { panel->mcp_set_imgui_ribbon(on); return json{{"ok", true}, {"imgui_ribbon", on}}; }
    if (name == "imgui_menu")   { panel->mcp_set_imgui_menu(on);   return json{{"ok", true}, {"imgui_menu", on}}; }
    if (name == "imgui_topbar") { panel->mcp_set_imgui_topbar(on); return json{{"ok", true}, {"imgui_topbar", on}}; }
    if (name == "imgui_sidebar") { panel->mcp_set_imgui_sidebar(on); return json{{"ok", true}, {"imgui_sidebar", on}}; }
    throw std::runtime_error("unknown ui option: " + name + " (imgui_tree, imgui_ribbon, imgui_menu, imgui_topbar, imgui_sidebar)");
}

json action_plane(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    const int idx = doc.add_plane(params.value("base", 0), params.value("offset", 0.0),
                                  params.value("tilt", 0.0), params.value("axis", 0), "Plane");
    doc.recompute();   // datum-only docs yield no body; that is expected
    panel->mcp_after_change();
    return json{{"ok", idx >= 0}, {"plane_index", idx}, {"datum_planes", int(doc.resolve_datum_planes().size())}};
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
        f.axis_p1 = Vec3d(jn(params["p1"], 0), jn(params["p1"], 1), jn(params["p1"], 2));
    if (params.contains("p2") && params["p2"].is_array() && params["p2"].size() >= 3)
        f.axis_p2 = Vec3d(jn(params["p2"], 0), jn(params["p2"], 1), jn(params["p2"], 2));
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
        pt = Vec3d(jn(params["point"], 0), jn(params["point"], 1), jn(params["point"], 2));
    doc.checkpoint();
    int idx = doc.add_coordsys(ct, pt, "CoordSys");
    CadFeature& f = doc.features[idx];
    f.coordsys_body  = params.value("body", -1);
    f.coordsys_face  = params.value("face", -1);
    f.coordsys_edge  = params.value("edge", -1);
    if (params.contains("x_hint") && params["x_hint"].is_array() && params["x_hint"].size() >= 3)
        f.coordsys_x_hint = Vec3d(jn(params["x_hint"], 0), jn(params["x_hint"], 1), jn(params["x_hint"], 2));
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
    { PhaseLog t("checkpoint"); doc.checkpoint(); }
    doc.variables[name] = expr;
    bool ok;
    { PhaseLog t("recompute"); ok = doc.recompute(); }
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

// ---- sketch_constrain ------------------------------------------------------------------------------------------
// "3" (entity), "3.start" / "3.end" / "3.center" / "3.point" (an entity's point), "origin", "x_axis", "y_axis".
struct SketchRefTok { int e{-1}; SketchPointRole r{SketchPointRole::P0}; bool has_role{false}; };

SketchRefTok sketch_ref_tok(const json& j, size_t n_entities)
{
    SketchRefTok t;
    std::string s = j.is_number_integer() ? std::to_string(j.get<int>()) : j.is_string() ? j.get<std::string>() : std::string();
    if (s.empty()) throw std::runtime_error("a constraint reference must be an entity index or a string like \"3.end\"");
    if      (s == "origin") { t.e = kSketchRefOrigin; t.has_role = true; return t; }
    else if (s == "x_axis") { t.e = kSketchRefAxisX;  return t; }
    else if (s == "y_axis") { t.e = kSketchRefAxisY;  return t; }
    const size_t dot = s.find('.');
    const std::string idx = s.substr(0, dot);
    if (idx.empty() || idx.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("bad constraint reference '" + s + "'");
    t.e = std::atoi(idx.c_str());
    if (t.e >= int(n_entities)) throw std::runtime_error("constraint reference '" + s + "': no such entity (sketch has " + std::to_string(n_entities) + ")");
    if (dot != std::string::npos) {
        const std::string r = s.substr(dot + 1);
        t.has_role = true;
        if      (r == "start" || r == "point") t.r = SketchPointRole::P0;
        else if (r == "end")                   t.r = SketchPointRole::P1;
        else if (r == "center")                t.r = SketchPointRole::Center;
        else throw std::runtime_error("bad point role '" + r + "' (start, end, center, point)");
    }
    return t;
}

// One {type, a, b, c, value[, orientation]} -> the solver's constraint record. Angles are degrees on the wire.
SketchEntityConstraintDef sketch_constraint_from(const json& j, const std::vector<SketchEntity>& ents)
{
    using CT = SketchConstraintType;
    const std::string ty = j.value("type", std::string());
    auto ref = [&](const char* k) {
        if (!j.contains(k)) throw std::runtime_error(ty + " needs '" + k + "'");
        return sketch_ref_tok(j.at(k), ents.size());
    };
    auto point = [&](const char* k) {   // a bare entity names its natural point
        SketchRefTok t = ref(k);
        if (!t.has_role) throw std::runtime_error(ty + ": '" + std::string(k) + "' must name a point (\"3.start\", \"3.end\", \"3.center\", \"origin\")");
        return t;
    };
    auto is_line  = [&](int i) { return i >= 0 && ents[i].type == SketchEntity::Type::Line; };
    auto is_round = [&](int i) { return i >= 0 && (ents[i].type == SketchEntity::Type::Circle || ents[i].type == SketchEntity::Type::Arc); };
    auto value = [&]() {
        if (!j.contains("value") || !j["value"].is_number()) throw std::runtime_error(ty + " needs a numeric 'value'");
        const double v = j["value"].get<double>();
        if (!std::isfinite(v)) throw std::runtime_error(ty + ": value must be finite");
        return v;
    };
    SketchEntityConstraintDef d;
    auto pts2 = [&](CT t) { auto a = point("a"), b = point("b"); d.type = t; d.ea = a.e; d.ra = a.r; d.eb = b.e; d.rb = b.r; };
    auto ents2 = [&](CT t) { d.type = t; d.ea = ref("a").e; d.eb = ref("b").e; };
    if (ty == "coincident" && j.contains("b") && !sketch_ref_tok(j.at("b"), ents.size()).has_role) {
        auto a = point("a"); auto b = ref("b");   // a point and a bare curve: the point lies on it
        d.type = is_line(b.e) ? CT::PointOnLine : CT::PointOnObject; d.ea = a.e; d.ra = a.r; d.eb = b.e;
    }
    else if (ty == "coincident")         pts2(CT::Coincident);
    else if (ty == "horizontal_points")  pts2(CT::Horizontal);
    else if (ty == "vertical_points")    pts2(CT::Vertical);
    else if (ty == "horizontal" || ty == "vertical") {   // one line: its two ends
        const SketchRefTok a = ref("a");
        if (!is_line(a.e)) throw std::runtime_error(ty + " needs a line (use " + ty + "_points for two points)");
        d.type = ty == "horizontal" ? CT::Horizontal : CT::Vertical;
        d.ea = d.eb = a.e; d.ra = SketchPointRole::P0; d.rb = SketchPointRole::P1;
    }
    else if (ty == "distance") {
        pts2(CT::Distance);
        const std::string o = j.value("orientation", std::string("aligned"));
        if      (o == "horizontal") d.type = CT::DistanceX;
        else if (o == "vertical")   d.type = CT::DistanceY;
        else if (o != "aligned")    throw std::runtime_error("orientation must be aligned, horizontal or vertical");
        d.value = value();
        if (d.type == CT::Distance && d.value < 0.0) throw std::runtime_error("an aligned distance must be >= 0 (horizontal/vertical distances are signed: b - a)");
    }
    else if (ty == "fix")     { auto a = point("a"); d.type = CT::Fix;   d.ea = a.e; d.ra = a.r; }
    else if (ty == "lock_x")  { auto a = point("a"); d.type = CT::LockX; d.ea = a.e; d.ra = a.r; d.value = value(); }
    else if (ty == "lock_y")  { auto a = point("a"); d.type = CT::LockY; d.ea = a.e; d.ra = a.r; d.value = value(); }
    else if (ty == "equal") {
        ents2(CT::EqualLength);
        if (is_round(d.ea) && is_round(d.eb)) d.type = CT::EqualRadius;
        else if (!is_line(d.ea) || !is_line(d.eb)) throw std::runtime_error("equal needs two lines or two circles/arcs");
    }
    else if (ty == "parallel")      ents2(CT::Parallel);
    else if (ty == "perpendicular") ents2(CT::Perpendicular);
    else if (ty == "collinear")     ents2(CT::Collinear);
    else if (ty == "tangent")       ents2(CT::Tangent);
    else if (ty == "concentric")    ents2(CT::Concentric);
    else if (ty == "midpoint")      { auto a = point("a"); auto b = ref("b"); d.type = CT::Midpoint; d.ea = a.e; d.ra = a.r; d.eb = b.e; }
    else if (ty == "symmetric")     { pts2(CT::Symmetric); d.ec = ref("c").e; }
    else if (ty == "symmetric_x")   pts2(CT::SymmetricAboutX);
    else if (ty == "symmetric_y")   pts2(CT::SymmetricAboutY);
    else if (ty == "point_on_line" || ty == "point_on_object") {
        auto a = point("a"); auto b = ref("b");
        d.type = ty == "point_on_line" ? CT::PointOnLine : CT::PointOnObject; d.ea = a.e; d.ra = a.r; d.eb = b.e;
        if (j.contains("value")) d.value = value();
    }
    else if (ty == "radius" || ty == "diameter") {
        d.ea = ref("a").e; d.type = ty == "radius" ? CT::Radius : CT::Diameter;
        if (!is_round(d.ea)) throw std::runtime_error(ty + " needs a circle or arc");
        d.value = value();
        if (d.value <= 0.0) throw std::runtime_error(ty + " must be positive");
    }
    else if (ty == "angle") {
        ents2(CT::Angle);
        if (!is_line(d.ea) || !is_line(d.eb)) throw std::runtime_error("angle needs two lines");
        d.value = value() * M_PI / 180.0;   // the model stores radians
    }
    else throw std::runtime_error("unknown constraint type '" + ty + "' (coincident, horizontal, vertical, horizontal_points, vertical_points, distance, fix, lock_x, lock_y, equal, parallel, perpendicular, collinear, tangent, concentric, midpoint, symmetric, symmetric_x, symmetric_y, point_on_line, point_on_object, radius, diameter, angle)");
    return d;
}

// A constraint in the vocabulary of the request that makes one: {type, a, b, c}, with the same point notation ("3.end",
// "origin", "x_axis"), so a conflict can be read back against what the client sent.
std::string constraint_text(const SketchEntityConstraintDef& c)
{
    using CT = SketchConstraintType;
    const bool pair_line = c.ea == c.eb && c.ea >= 0 && (c.type == CT::Horizontal || c.type == CT::Vertical);   // one line: its two ends
    const char* name = "constraint";
    bool pts = false;   // a and b (and c) name points rather than whole entities
    switch (c.type) {
    case CT::Fix: name = "fix"; pts = true; break;
    case CT::Coincident: name = "coincident"; pts = true; break;
    case CT::Horizontal: name = pair_line ? "horizontal" : "horizontal_points"; pts = !pair_line; break;
    case CT::Vertical: name = pair_line ? "vertical" : "vertical_points"; pts = !pair_line; break;
    case CT::Distance: name = "distance"; pts = true; break;
    case CT::DistanceX: name = "distance(horizontal)"; pts = true; break;
    case CT::DistanceY: name = "distance(vertical)"; pts = true; break;
    case CT::LockX: name = "lock_x"; pts = true; break;
    case CT::LockY: name = "lock_y"; pts = true; break;
    case CT::EqualLength: case CT::EqualRadius: name = "equal"; break;
    case CT::Parallel: name = "parallel"; break;
    case CT::Perpendicular: name = "perpendicular"; break;
    case CT::Collinear: name = "collinear"; break;
    case CT::Tangent: name = "tangent"; break;
    case CT::Concentric: name = "concentric"; break;
    case CT::Midpoint: name = "midpoint"; pts = true; break;
    case CT::Symmetric: name = "symmetric"; pts = true; break;
    case CT::SymmetricAboutX: name = "symmetric_x"; pts = true; break;
    case CT::SymmetricAboutY: name = "symmetric_y"; pts = true; break;
    case CT::PointOnLine: name = "point_on_line"; pts = true; break;
    case CT::PointOnObject: name = "point_on_object"; pts = true; break;
    case CT::Radius: name = "radius"; break;
    case CT::Diameter: name = "diameter"; break;
    case CT::Angle: name = "angle"; break;
    }
    auto ref = [&](int e, SketchPointRole r, bool point) -> std::string {
        if (e == kSketchRefOrigin) return "origin";
        if (e == kSketchRefAxisX) return "x_axis";
        if (e == kSketchRefAxisY) return "y_axis";
        if (e < 0) return "-";
        std::string t = std::to_string(e);
        if (point) t += r == SketchPointRole::P1 ? ".end" : r == SketchPointRole::Center ? ".center" : ".start";
        return t;
    };
    std::string s = name;
    if (c.ea != -1) s += " a=" + ref(c.ea, c.ra, pts && !(pair_line));
    if (c.eb != -1 && !pair_line) s += " b=" + ref(c.eb, c.rb, pts && c.type != CT::Midpoint && c.type != CT::PointOnLine && c.type != CT::PointOnObject);
    if (c.ec != -1) s += " c=" + ref(c.ec, c.rc, false);
    return s;
}

// Why the solver refuses `extra` on top of the sketch's constraints, in one shape for every constraint type:
//   kind       redundant_or_inconsistent | did_not_converge | too_many_unknowns
//   conflicts  [{of: "existing"|"request", index, constraint: "<type a=.. b=..>"}]; existing indexes count the sketch's
//              own constraints, request indexes count `extra`
// and the text that names them.
std::string solver_why(const DesignSketchTool& t, const std::vector<SketchEntityConstraintDef>& extra, json* out = nullptr)
{
    std::vector<SketchEntity> ents = t.entities();
    std::vector<SketchEntityConstraintDef> cons = t.constraints();
    const int base = int(cons.size());
    cons.insert(cons.end(), extra.begin(), extra.end());
    const SketchSolveResult r = sketch_solve(ents, cons);
    const char* kind = r.result == 2 ? "did_not_converge" : r.result == 3 ? "too_many_unknowns" : "redundant_or_inconsistent";
    std::string s = std::string("rejected by the solver: ") + (r.result == 2 ? "it did not converge" : r.result == 3 ? "too many unknowns"
                    : "redundant with, or inconsistent with, the constraints already in the sketch");
    json conflicts = json::array();
    for (size_t i = 0; i < r.bad.size(); ++i) {
        const bool req = r.bad[i] >= base;
        const int  idx = req ? r.bad[i] - base : r.bad[i];
        const std::string text = constraint_text(cons[size_t(r.bad[i])]);
        conflicts.push_back(json{{"of", req ? "request" : "existing"}, {"index", idx}, {"constraint", text}});
        s += (i == 0 ? "; conflicts: " : ", ") + std::string(req ? "this request's #" : "the sketch's existing #") + std::to_string(idx) + " (" + text + ")";
    }
    if (out) *out = json{{"kind", kind}, {"conflicts", conflicts}};
    return s;
}

// {constraints:[{type, a, b, c, value, orientation}], atomic:true}. atomic (default): all or nothing, solved as ONE
// system like the GUI's own batches. atomic:false: each is added and solved on its own, so one that conflicts is
// rejected alone (named in `rejected`) and the rest are kept. Unparseable entries are always reported.
json action_sketch_constrain(DesignPanel* panel, const json& params)
{
    DesignSketchTool& t = mcp_sketch(panel);
    if (!params.contains("constraints") || !params["constraints"].is_array())
        throw std::runtime_error("sketch_constrain needs 'constraints': [{type, a, b, value}, ...]");
    const bool atomic = params.value("atomic", true);
    json rejected = json::array();
    std::vector<SketchEntityConstraintDef> defs;
    std::vector<int> def_index;
    const json& list = params["constraints"];
    for (size_t i = 0; i < list.size(); ++i) {
        try { defs.push_back(sketch_constraint_from(list[i], t.entities())); def_index.push_back(int(i)); }
        catch (const std::exception& e) { rejected.push_back(json{{"index", int(i)}, {"kind", "malformed"}, {"conflicts", json::array()}, {"error", e.what()}}); }
    }
    int added = 0;
    if (atomic) {
        if (!rejected.empty()) { /* a malformed entry: add nothing */ }
        else if ([&] { PhaseLog p("sketch_constrain.try_add"); return t.try_add_constraints(defs); }()) added = int(defs.size());
        else { json why; const std::string text = solver_why(t, defs, &why);
               why["index"] = -1; why["error"] = text + " (atomic: none of the " + std::to_string(defs.size()) + " were added; atomic:false keeps the consistent ones)";
               rejected.push_back(why); }
    } else {
        for (size_t k = 0; k < defs.size(); ++k) {
            if (t.try_add_constraints({defs[k]})) ++added;
            else { json why; const std::string text = solver_why(t, {defs[k]}, &why);
                   why["index"] = def_index[k]; why["error"] = text; rejected.push_back(why); }
        }
    }
    panel->mcp_viewport()->request_repaint();
    int dof_now;
    { PhaseLog p("sketch_constrain.dof"); dof_now = t.dof(); }
    return json{{"ok", rejected.empty()}, {"added", added}, {"rejected", rejected}, {"dof", dof_now}, {"solved", t.solve_ok()},
                {"error", rejected.empty() ? std::string() : std::string("some constraints were rejected; see 'rejected'")}};
}

// ---- export ----------------------------------------------------------------------------------------------------
// Where an agent may write. ponytail: absolute path under $HOME or /tmp, no `..`, existing directory, canonicalised
// so a symlink cannot lead out; the --allow-path / per-launch scope of SolidCraft's MCP is a later slice.
// Roots an agent may read or write: $ORCA_CAD_MCP_ALLOW (colon separated), default $HOME and /tmp. The directory is
// canonicalised first, so `..` and symlinks cannot lead out.
bool path_in_scope(const std::string& dir)
{
    std::vector<std::string> roots;
    if (const char* a = std::getenv("ORCA_CAD_MCP_ALLOW"); a && *a) {
        std::string cur;
        for (const char* c = a;; ++c) {
            if (*c == ':' || *c == 0) { if (!cur.empty()) roots.push_back(cur); cur.clear(); if (!*c) break; }
            else cur.push_back(*c);
        }
    } else {
        roots.push_back("/tmp");
        if (const char* h = std::getenv("HOME")) roots.push_back(h);
    }
    for (const std::string& r0 : roots) {
        boost::system::error_code ec;
        const std::string r = boost::filesystem::canonical(r0, ec).string();
        if (!ec && !r.empty() && (dir == r || dir.rfind(r + "/", 0) == 0)) return true;
    }
    return false;
}

std::string checked_out_path(const json& params, const char* what)
{
    const std::string raw = params.value("path", std::string());
    if (raw.empty()) throw std::runtime_error(std::string(what) + " needs 'path'");
    namespace fs = boost::filesystem;
    const fs::path p(raw);
    if (!p.is_absolute()) throw std::runtime_error("path must be absolute");
    for (const auto& c : p) if (c == "..") throw std::runtime_error("path must not contain '..'");
    boost::system::error_code ec;
    const fs::path dir = fs::canonical(p.parent_path(), ec);
    if (ec) throw std::runtime_error("directory does not exist: " + p.parent_path().string());
    if (!path_in_scope(dir.string())) throw std::runtime_error("path is outside the allowed roots (default $HOME and /tmp; ORCA_CAD_MCP_ALLOW overrides)");
    return (dir / p.filename()).string();
}

json action_export(DesignPanel* panel, const json& params)
{
    const std::string fmt = params.value("format", std::string("step"));
    if (fmt != "step" && fmt != "iges" && fmt != "stl" && fmt != "obj") throw std::runtime_error("format must be step, iges, stl or obj (the project itself is `save`)");
    const std::string path = checked_out_path(params, "export");
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("nothing to export - the design has no bodies");
    const std::vector<Transform3d> xf(doc.bodies.size(), Transform3d::Identity());   // bodies as modelled, no Move-gizmo offsets
    std::string err;
    const bool ok = fmt == "step" ? doc.export_step(path, xf, err) : doc.export_other(fmt, path, xf, err);
    return json{{"ok", ok}, {"format", fmt}, {"path", path}, {"bodies", int(doc.bodies.size())}, {"error", err}};
}

// ---- sweep / loft / thread / cut -----------------------------------------------------------------------------
// A Sketch feature index in params[key], checked before any checkpoint.
int sketch_index_at(const json& v, const CadDocument& doc, const char* what)
{
    if (!v.is_number_integer()) throw std::runtime_error(std::string(what) + " must be a Sketch feature index (sketch_commit returns it as sketch_index)");
    const int s = v.get<int>();
    if (s < 0 || s >= int(doc.features.size()) || doc.features[s].type != CadFeatureType::Sketch)
        throw std::runtime_error(std::string(what) + " must be the index of a Sketch feature");
    return s;
}

// Run the just-added feature; on failure drop it (undo) and keep the reason.
json finish_feature(DesignPanel* panel, CadDocument& doc, json out)
{
    const bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    out["ok"] = ok; out["bodies"] = int(doc.bodies.size()); out["error"] = doc.error;
    return out;
}

// {profile:<sketch idx>, path:<sketch idx>, boolean}
json action_sweep(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (!params.contains("profile") || !params.contains("path")) throw std::runtime_error("sweep needs 'profile' and 'path' (Sketch feature indices)");
    const int pr = sketch_index_at(params["profile"], doc, "'profile'");
    int pa = -1;   // the path is a sketch, a helix or a 3D curve
    if (params["path"].is_number_integer() && params["path"].get<int>() >= 0 && params["path"].get<int>() < int(doc.features.size())
        && (doc.features[size_t(params["path"].get<int>())].type == CadFeatureType::Helix || doc.features[size_t(params["path"].get<int>())].type == CadFeatureType::Sketch3D))
        pa = params["path"].get<int>();
    else
        pa = sketch_index_at(params["path"], doc, "'path'");
    doc.checkpoint();
    const int i = doc.add_sweep(pr, pa, bool_from(params.value("boolean", std::string("new"))), "Sweep");
    return finish_feature(panel, doc, json{{"sweep_index", i}});
}

// {profiles:[<sketch idx>,...] (>=2, in order), ruled, boolean}
json action_loft(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (!params.contains("profiles") || !params["profiles"].is_array() || params["profiles"].size() < 2)
        throw std::runtime_error("loft needs 'profiles': at least two Sketch feature indices, in order");
    std::vector<int> refs;
    for (const json& v : params["profiles"]) refs.push_back(sketch_index_at(v, doc, "each 'profiles' entry"));
    doc.checkpoint();
    const int i = doc.add_loft(refs, params.value("ruled", false), bool_from(params.value("boolean", std::string("new"))), "Loft");
    return finish_feature(panel, doc, json{{"loft_index", i}});
}

// {radius (major), pitch, height, depth, internal, x, y, plane}
json action_thread(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to thread");
    const double r = params.value("radius", 5.0), pitch = params.value("pitch", 2.0), h = params.value("height", 10.0), d = params.value("depth", 1.0);
    if (r <= 0 || pitch <= 0 || h <= 0 || d <= 0) throw std::runtime_error("radius, pitch, height and depth must be > 0");
    SketchPlane pl = plane_from(params, doc);
    doc.checkpoint();
    const int i = doc.add_thread(r, pitch, h, d, params.value("internal", false), params.value("x", 0.0), params.value("y", 0.0), pl, "Thread");
    doc.features[i].thread_major_nominal = true;
    return finish_feature(panel, doc, json{{"thread_index", i}});
}

// {plane, offset, flip, keep: "upper"|"lower"|"both", body}: trim a body by a plane (Onshape split by plane).
json action_cut(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw std::runtime_error("no body to cut");
    const std::string keep = params.value("keep", std::string("upper"));
    if (keep != "upper" && keep != "lower" && keep != "both") throw std::runtime_error("keep must be upper, lower or both");
    SketchPlane pl = plane_from(params, doc);
    const int bi = target_body_arg(params, doc);
    doc.checkpoint();
    const int i = doc.add_cut(pl, params.value("offset", 0.0), params.value("flip", false), keep != "lower", keep != "upper", bi, "Cut");
    return finish_feature(panel, doc, json{{"cut_index", i}});
}

// ---- save / open / screenshot ----------------------------------------------------------------------------------
// The recipe travels inside the project 3MF, so saving the project saves the design.
json action_save(DesignPanel*, const json& params)
{
    const std::string path = checked_out_path(params, "save");
    if (boost::algorithm::to_lower_copy(boost::filesystem::path(path).extension().string()) != ".3mf") throw std::runtime_error("path must end in .3mf");
    const int rc = wxGetApp().plater()->export_3mf(path);
    return json{{"ok", rc == 0}, {"path", path}, {"error", rc == 0 ? std::string() : "export_3mf returned " + std::to_string(rc)}};
}

// Replaces the open project. Refused while it has unsaved changes unless discard:true, because the app would ask.
json action_open(DesignPanel*, const json& params)
{
    const std::string raw = params.value("path", std::string());
    if (raw.empty()) throw std::runtime_error("open needs 'path'");
    boost::system::error_code ec;
    const boost::filesystem::path p = boost::filesystem::canonical(raw, ec);
    if (ec || !boost::filesystem::is_regular_file(p)) throw std::runtime_error("no such file: " + raw);
    if (!path_in_scope(p.parent_path().string())) throw std::runtime_error("path is outside the allowed roots (default $HOME and /tmp; ORCA_CAD_MCP_ALLOW overrides)");
    if (boost::algorithm::to_lower_copy(p.extension().string()) != ".3mf") throw std::runtime_error("only .3mf projects can be opened");
    Plater* pl = wxGetApp().plater();
    const bool discard = params.value("discard", false);
    // The same predicate load_project asks before it opens its "save changes?" dialog, which would block the app.
    if (!pl->up_to_date(false, false) && !discard) throw std::runtime_error("the open project has unsaved changes: save first, or pass discard:true");
    wxGetApp().CallAfter([pl, p, discard]() {   // may rebuild the panel: not inline
        AppConfig* cfg = wxGetApp().app_config;
        const std::string prev = cfg->get("save_project_choise");
        if (discard) cfg->set("save_project_choise", "no");   // "remembered choice: don't save" for the duration of this one load
        pl->load_project(wxString::FromUTF8(p.string()));
        if (discard) cfg->set("save_project_choise", prev);
    });
    return json{{"ok", true}, {"path", p.string()}, {"dispatched", true}};
}

// PNG of the design viewport, read back from the screen. ponytail: X11 only (a Wayland compositor returns black);
// the window must be visible. Camera control: `view` (iso/top/front/...) and `fit`.
json action_screenshot(DesignPanel* panel, const json& params)
{
    DesignCanvas* v = panel->mcp_viewport();
    if (!v->IsShownOnScreen()) {   // the Design tab is not the front page: bring it up and let it lay out first
        wxGetApp().mainframe->select_tab(wxString(TAB_ID_DESIGN));
        if (wxEventLoopBase* loop = wxEventLoopBase::GetActive()) loop->YieldFor(wxEVT_CATEGORY_UI);
    }
    if (params.contains("view")) v->set_view(params["view"].get<std::string>());
    if (params.value("fit", false)) v->fit_view();
    v->Refresh(); v->Update();
    const std::string path = checked_out_path(params, "screenshot");
    const wxSize sz = v->GetClientSize();
    if (sz.x < 8 || sz.y < 8) throw std::runtime_error("the design viewport is not visible");
    wxScreenDC sdc;
    wxBitmap bmp(sz.x, sz.y);
    wxMemoryDC mdc(bmp);
    const wxPoint o = v->ClientToScreen(wxPoint(0, 0));
    mdc.Blit(0, 0, sz.x, sz.y, &sdc, o.x, o.y);
    mdc.SelectObject(wxNullBitmap);
    const bool ok = bmp.ConvertToImage().SaveFile(wxString::FromUTF8(path), wxBITMAP_TYPE_PNG);
    return json{{"ok", ok}, {"path", path}, {"width", sz.x}, {"height", sz.y}, {"error", ok ? std::string() : "could not write the PNG"}};
}

// ---- ui.* : synthetic input, through the same handlers a real mouse and keyboard reach --------------------------------
// ui.move/click/drag/key/text/worldToScreen/inspect. The events are built as wx events and processed by the live widgets
// on the MAIN thread, one step at a time with a pause between steps, so ImGui renders the frames it needs to see a press
// before its release. The pauses are why this runs on the client's thread, not inside one main-thread call.
// Everything the lambda uses is captured BY VALUE: after the 15 s timeout it may still be queued, and it is then dropped
// unrun (the same queued / running / abandoned state as dispatch_once) rather than reading the caller's freed locals.
template<class F> auto on_main(F f) -> decltype(f())
{
    using R = decltype(f());
    auto prom  = std::make_shared<std::promise<R>>();
    auto fut   = prom->get_future();
    auto state = std::make_shared<std::atomic<int>>(0);   // 0 queued, 1 running, 2 abandoned
    wxGetApp().CallAfter([prom, state, f]() mutable {
        int queued = 0;
        if (!state->compare_exchange_strong(queued, 1)) return;
        try { if constexpr (std::is_void_v<R>) { f(); prom->set_value(); } else prom->set_value(f()); }
        catch (...) { prom->set_exception(std::current_exception()); }
    });
    if (fut.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
        int queued = 0;
        if (state->compare_exchange_strong(queued, 2)) throw std::runtime_error("main-thread timeout: the command was NOT run");
        fut.wait();   // already running: wait for it
    }
    return fut.get();
}

struct UiState { int buttons{0}; };   // bit 0 left, 1 middle, 2 right: held, so a drag's motion events carry them
UiState g_ui;

void ui_mouse(DesignPanel* panel, int kind /*0 move,1 down,2 up,3 dclick*/, int button /*1 L,2 M,3 R*/, int x, int y, int mods, int clicks)
{
    wxWindow* w = panel->mcp_viewport()->input_window();
    if (!w) throw std::runtime_error("no viewport");
    const int bit = button == 1 ? 1 : button == 2 ? 2 : 4;
    if (kind == 1 || kind == 3) g_ui.buttons |= bit;
    if (kind == 2) g_ui.buttons &= ~bit;
    wxEventType t = wxEVT_MOTION;
    if (kind != 0) {
        const bool L = button == 1, M = button == 2;
        t = kind == 1 ? (L ? wxEVT_LEFT_DOWN : M ? wxEVT_MIDDLE_DOWN : wxEVT_RIGHT_DOWN)
          : kind == 2 ? (L ? wxEVT_LEFT_UP   : M ? wxEVT_MIDDLE_UP   : wxEVT_RIGHT_UP)
                      : (L ? wxEVT_LEFT_DCLICK : M ? wxEVT_MIDDLE_DCLICK : wxEVT_RIGHT_DCLICK);
    }
    wxMouseEvent e(t);
    e.SetEventObject(w); e.SetId(w->GetId()); e.SetPosition(wxPoint(x, y));
    e.m_leftDown = g_ui.buttons & 1; e.m_middleDown = g_ui.buttons & 2; e.m_rightDown = g_ui.buttons & 4;
    e.SetShiftDown(mods & 1); e.SetControlDown(mods & 2); e.SetAltDown(mods & 4);
    e.m_clickCount = clicks;
    panel->mcp_viewport()->tree_overlay().test_mods = mods;   // ImGui takes modifiers from key events, which a synthetic click has none of
    e.SetTimestamp(long(wxGetLocalTimeMillis().GetLo()));
    w->GetEventHandler()->ProcessEvent(e);
    panel->mcp_viewport()->request_repaint();   // a person moving the mouse repaints; ImGui samples the pointer once per frame
}

int ui_keycode(const std::string& k)
{
    static const std::map<std::string, int> names = {
        {"Escape", WXK_ESCAPE}, {"Esc", WXK_ESCAPE}, {"Enter", WXK_RETURN}, {"Return", WXK_RETURN}, {"Tab", WXK_TAB}, {"Backspace", WXK_BACK},
        {"Delete", WXK_DELETE}, {"Space", WXK_SPACE}, {"Left", WXK_LEFT}, {"Right", WXK_RIGHT}, {"Up", WXK_UP}, {"Down", WXK_DOWN},
        {"Home", WXK_HOME}, {"End", WXK_END}, {"PageUp", WXK_PAGEUP}, {"PageDown", WXK_PAGEDOWN},
        {"F1", WXK_F1}, {"F2", WXK_F2}, {"F3", WXK_F3}, {"F4", WXK_F4}, {"F5", WXK_F5}, {"F6", WXK_F6}, {"F7", WXK_F7}, {"F8", WXK_F8}, {"F9", WXK_F9}, {"F10", WXK_F10}, {"F11", WXK_F11}, {"F12", WXK_F12}};
    auto it = names.find(k);
    if (it != names.end()) return it->second;
    if (k.size() == 1) return std::toupper(static_cast<unsigned char>(k[0]));
    throw std::runtime_error("unknown key '" + k + "' (a single character, or Escape/Enter/Tab/Backspace/Delete/Space/arrows/Home/End/PageUp/PageDown/F1-F12)");
}

// One key press as the keyboard delivers it: CHAR_HOOK first (the panel's shortcut gate lives there); if nobody takes it,
// KEY_DOWN, CHAR, KEY_UP go to the focused window. `unicode` != 0 sends text instead (CHAR only).
// phase: 0 = press and release together, 1 = press only, 2 = release only. ImGui samples keys once per frame,
// so a press that is released before the next frame is invisible to it: ui.key holds the key across a frame.
void ui_key(DesignPanel* panel, int code, int unicode, int mods, int phase = 0)
{
    wxWindow* w = wxWindow::FindFocus();
    if (!w) w = panel->mcp_viewport()->input_window();
    // wx's FindFocus() is a record of focus EVENTS; on a rig with no window manager it can lag the real GTK focus (a real
    // key goes to GTK's focus widget, which open_palette/the inline editor moved to the canvas). While ImGui holds a text
    // field, the canvas is where a real key would land, so that is where the synthetic one goes.
    if (wxGetApp().imgui() && wxGetApp().imgui()->want_text_input() && panel->mcp_viewport()->input_window())
        w = panel->mcp_viewport()->input_window();
    auto make = [&](wxEventType t) {
        wxKeyEvent e(t);
        e.SetEventObject(w); e.SetId(w->GetId());
        e.m_keyCode = code; e.m_uniChar = unicode ? unicode : (code < 128 && std::isalpha(code) && !(mods & 1) ? std::tolower(code) : code);
        e.m_shiftDown = mods & 1; e.m_controlDown = mods & 2; e.m_altDown = mods & 4;
        e.SetTimestamp(long(wxGetLocalTimeMillis().GetLo()));
        return e;
    };
    if (phase == 2) {   // the release also drops the modifiers, as a real keyboard does (ImGui reads Ctrl/Shift off the key events)
        wxKeyEvent up = make(wxEVT_KEY_UP);
        up.m_shiftDown = up.m_controlDown = up.m_altDown = false;
        w->GetEventHandler()->ProcessEvent(up);
        return;
    }
    if (!unicode) {
        wxKeyEvent hook = make(wxEVT_CHAR_HOOK);
        if (w->GetEventHandler()->ProcessEvent(hook) && !hook.GetSkipped()) return;   // consumed as a shortcut
        wxKeyEvent down = make(wxEVT_KEY_DOWN);
        w->GetEventHandler()->ProcessEvent(down);
    }
    wxKeyEvent ch = make(wxEVT_CHAR);
    if (unicode) { ch.m_keyCode = unicode; ch.m_uniChar = unicode; }
    w->GetEventHandler()->ProcessEvent(ch);
    if (!unicode && phase == 0) { wxKeyEvent up = make(wxEVT_KEY_UP); w->GetEventHandler()->ProcessEvent(up); }
}

int ui_mods(const json& p)
{
    int m = 0;
    if (p.contains("mods")) {
        if (!p["mods"].is_array()) throw std::runtime_error("'mods' must be an array of shift|ctrl|alt");
        for (const json& v : p["mods"]) { const std::string s = v.is_string() ? v.get<std::string>() : ""; m |= s == "shift" ? 1 : s == "ctrl" ? 2 : s == "alt" ? 4 : 0; }
    }
    return m;
}
int ui_int(const json& a, size_t i) { return int(std::lround(jn(a, i))); }

json ui_card_state(DesignPanel* panel)
{
    return json{{"tool", panel->mcp_active_tool()}, {"confirm", panel->mcp_confirm_enabled()}, {"status", panel->mcp_status()}, {"rollback", panel->mcp_doc().rollback_index}};
}

json ui_inspect(DesignPanel* panel)
{
    DesignCanvas* v = panel->mcp_viewport();
    wxWindow* in = v ? v->input_window() : nullptr;
    wxWindow* f = wxWindow::FindFocus();
    json out{{"ok", true}, {"sketch_open", v != nullptr && v->is_sketching()}, {"selection_kind", panel->mcp_offer_selection_kind()}};
    out["focus"] = {{"class", f ? wxString(f->GetClassInfo()->GetClassName()).utf8_string() : std::string()}, {"is_canvas", f != nullptr && f == in},
                    {"text_entry", f != nullptr && (dynamic_cast<wxTextCtrl*>(f) != nullptr)}};
    {   // who has the keyboard, innermost first, and whether the canvas believes it is focusable: for focus bug reports
        std::string chain;
        for (wxWindow* c = f; c != nullptr; c = c->GetParent()) chain += (chain.empty() ? "" : " < ") + wxString(c->GetClassInfo()->GetClassName()).utf8_string() + (c == in ? "*" : "");
        out["focus"]["chain"] = chain;
        if (in) out["focus"]["canvas_accepts"] = in->AcceptsFocus();
    }
    out["card"] = ui_card_state(panel);
    if (auto* ig = wxGetApp().imgui())
        out["imgui"] = {{"want_text_input", ig->want_text_input()}, {"want_keyboard", ig->want_keyboard()}, {"want_mouse", ig->want_mouse()}};
    if (v) {
        const DesignPalette& pal = v->palette();
        json labels = json::object();
        for (const std::string& id : pal.shown()) labels[id] = pal.label_of(id);   // translated: a rig test types from these
        out["palette"] = {{"open", pal.is_open()}, {"query", pal.query()}, {"shown", pal.shown()}, {"labels", labels}, {"cursor", pal.cursor()}};
    }
    if (v) {   // what is picked on the solid right now: lets a rig test see click cycling move
        json faces = json::array();
        for (const auto& bf : v->mcp_sketch_tool().selected_faces()) faces.push_back({bf.first, bf.second});
        const DesignCandidateMenu& om = const_cast<DesignCanvas*>(v)->other_menu();
        out["other_menu"] = {{"open", om.is_open()}, {"labels", om.labels()}, {"hover", om.hovered()}};
        out["solid_selection"] = {{"kind", v->mcp_sketch_tool().solid_sel_kind()}, {"faces", faces}, {"edges", v->mcp_sketch_tool().selected_edges()}};
    }
    if (v) out["hud"] = v->readout();
    if (v) {
        json hits = json::array();
        for (const auto& hh : v->select_panel_hits()) hits.push_back(json::array({hh.first, hh.second}));
        out["select_panel"] = {{"checks", hits}};
    }
    if (v && v->tree_overlay_on()) {   // the on-canvas tree: where each row is, so a rig test can drag one
        const DesignTreeOverlay& to = const_cast<DesignCanvas*>(v)->tree_overlay();
        out["tree"] = {{"rows_y", to.row_mids()}, {"x", to.row_x()}, {"selected", panel->mcp_tree_selection()}};
    }
    if (in) {
        const wxSize sz = in->GetClientSize();
        out["canvas"] = {{"width", sz.x}, {"height", sz.y}, {"shown", in->IsShownOnScreen()}};
    }
    return out;
}

json ui_dispatch(const std::string& method, const json& p)
{
    auto pause = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    auto panelp = []() { DesignPanel* d = DesignPanel::ensure(); if (!d) throw std::runtime_error("Design panel not ready"); return d; };
    const int mods = ui_mods(p);
    if (method == "ui.inspect") return on_main([=]() { return ui_inspect(panelp()); });
    if (method == "ui.candidates") {   // what a click at "at" could take, nearest first; and sets the click mode / SELECT filter
        return on_main([=]() {
            DesignCanvas* v = panelp()->mcp_viewport();
            if (!v) throw std::runtime_error("no viewport");
            DesignSketchTool& t = v->mcp_sketch_tool();
            if (p.contains("filter")) {
                const json f = p["filter"];
                t.set_pick_filter({f.value("face", true), f.value("edge", true), f.value("vertex", true)});
            }
            if (p.contains("cycle")) t.set_click_cycle(p["cycle"].get<bool>());
            if (p.contains("other_hover"))  v->other_menu().request_hover(p["other_hover"].get<int>());    // drive a Select Other row
            if (p.contains("other_choose")) v->other_menu().request_choose(p["other_choose"].get<int>());
            json list = json::array();
            if (p.contains("at")) {
                const json at = p["at"];
                for (const auto& c : v->pick_candidates(jn(at, 0), jn(at, 1)))
                    list.push_back({{"kind", c.kind == 4 ? "vertex" : c.kind == 3 ? "edge" : c.kind == 2 ? "face" : "body"},
                                    {"body", c.body}, {"face", c.face}, {"edge", c.edge}});
            }
            return json{{"ok", true}, {"candidates", list}, {"cycle", t.click_cycle()},
                        {"filter", {{"face", t.pick_filter().face}, {"edge", t.pick_filter().edge}, {"vertex", t.pick_filter().vertex}}}};
        });
    }
    if (method == "ui.worldToScreen") {
        const Vec3d w(jn(p.value("point", json()), 0), jn(p.value("point", json()), 1), jn(p.value("point", json()), 2));
        return on_main([=]() {
            const Camera& cam = wxGetApp().plater()->get_camera();
            const Vec4d c = cam.get_projection_matrix().matrix() * cam.get_view_matrix().matrix() * Vec4d(w.x(), w.y(), w.z(), 1.0);
            if (std::abs(c.w()) < 1e-12) throw std::runtime_error("point is on the camera plane");
            const auto& vp = cam.get_viewport();
            return json{{"ok", true}, {"x", (c.x() / c.w() + 1.0) * 0.5 * vp[2]}, {"y", (1.0 - c.y() / c.w()) * 0.5 * vp[3]}, {"in_front", c.w() > 0}};
        });
    }
    if (method == "ui.move" || method == "ui.click" || method == "ui.drag") {
        const json& at = method == "ui.drag" ? p.value("from", json()) : p;
        const int x = ui_int(method == "ui.drag" ? at : p.value("at", json()), 0), y = ui_int(method == "ui.drag" ? at : p.value("at", json()), 1);
        const int button = p.value("button", 1), clicks = p.value("count", 1);
        if (button < 1 || button > 3 || clicks < 1 || clicks > 2) throw std::runtime_error("button is 1|2|3 and count is 1|2");
        auto mouse = [&](int kind, int bx, int by, int n = 1) { on_main([=]() { ui_mouse(panelp(), kind, button, bx, by, mods, n); }); };
        mouse(0, x, y); pause(60);
        if (method == "ui.move") return json{{"ok", true}};
        if (method == "ui.click") {
            mouse(1, x, y); pause(std::clamp(p.value("hold_ms", 60), 0, 5000)); mouse(2, x, y); pause(60);
            if (clicks == 2) { mouse(3, x, y, 2); pause(30); mouse(2, x, y); pause(60); }
            return json{{"ok", true}};
        }
        const int tx = ui_int(p.value("to", json()), 0), ty = ui_int(p.value("to", json()), 1), steps = std::clamp(p.value("steps", 8), 1, 200);
        const int step_ms = std::clamp(p.value("step_ms", 25), 5, 1000);   // ImGui reads the pointer once per painted frame: slow software GL needs longer
        pause(step_ms); mouse(0, x, y); pause(step_ms);   // a frame must see the pointer over the start before the press, or ImGui presses whatever it hovered last
        mouse(1, x, y); pause(std::max(60, step_ms));
        for (int i = 1; i <= steps; ++i) { mouse(0, x + (tx - x) * i / steps, y + (ty - y) * i / steps); pause(step_ms); }
        pause(step_ms); mouse(2, tx, ty); pause(60);
        return json{{"ok", true}};
    }
    if (method == "ui.tree") {   // rig driver for the tree's row commands: {event: copy|paste|delete|toggle, row}
        const std::string ev = p.value("event", std::string());
        if (ev != "copy" && ev != "paste" && ev != "delete" && ev != "toggle") throw std::runtime_error("ui.tree event: copy | paste | delete | toggle");
        const int row = p.value("row", -1);
        on_main([=]() { panelp()->mcp_tree_event(ev, row); return true; });
        pause(150);
        return json{{"ok", true}};
    }
    if (method == "ui.card") {   // rig driver for the wx feature cards: open one, set fields, commit
        if (p.contains("open") && !(p["open"].is_string() && on_main([=]() { return panelp()->mcp_card_open(p["open"].get<std::string>()); })))
            throw std::runtime_error("ui.card open: dressup | extrude | shell | draft | pattern | sheet[:0-6]");
        if (p.contains("set")) {
            if (!p["set"].is_object()) throw std::runtime_error("ui.card set must be an object {field: number}");
            for (auto it = p["set"].begin(); it != p["set"].end(); ++it) {
                if (!it.value().is_number()) throw std::runtime_error("ui.card set values are numbers");
                const std::string k = it.key();
                const double      v = it.value().get<double>();
                if (!on_main([=]() { return panelp()->mcp_card_set(k, v); })) throw std::runtime_error("ui.card: unknown field or bad choice: " + k);
            }
        }
        if (p.contains("check")) {
            if (!p["check"].is_array()) throw std::runtime_error("ui.card check must be an array of feature indices");
            for (const json& x : p["check"])
                if (!x.is_number_integer() || !on_main([=]() { return panelp()->mcp_card_check_feature(x.get<int>(), true); })) throw std::runtime_error("ui.card check: not a repeatable feature");
        }
        bool confirmed = true;
        if (p.value("confirm", false)) {
            for (int i = 0; i < 50 && !on_main([=]() { return panelp()->mcp_card_confirm_enabled(); }); ++i) pause(100);   // the preview settles first
            confirmed = on_main([=]() { return panelp()->mcp_card_confirm(); });
        }
        return json{{"ok", confirmed}, {"card", ui_card_state(panelp())}};
    }
    if (method == "ui.key") {
        if (!p.contains("key") || !p["key"].is_string()) throw std::runtime_error("ui.key needs 'key'");
        const int code = ui_keycode(p["key"].get<std::string>());
        on_main([=]() { ui_key(panelp(), code, 0, mods, 1); });
        pause(120);   // long enough for a paint: ImGui reads the key state per frame
        on_main([=]() { ui_key(panelp(), code, 0, mods, 2); });
        pause(60);
        return json{{"ok", true}};
    }
    if (method == "ui.text") {
        if (!p.contains("text") || !p["text"].is_string()) throw std::runtime_error("ui.text needs 'text'");
        const std::string t = p["text"].get<std::string>();
        if (t.size() > 4096) throw std::runtime_error("text is limited to 4096 characters");
        for (const wxUniChar c : wxString::FromUTF8(t)) { const int u = int(c.GetValue()); on_main([=]() { ui_key(panelp(), u, u, 0); }); pause(25); }
        return json{{"ok", true}};
    }
    throw std::runtime_error("unknown ui method '" + method + "' (ui.inspect, ui.candidates, ui.worldToScreen, ui.move, ui.click, ui.drag, ui.key, ui.text, ui.tree, ui.card)");
}

// ---- variables, sketch dimensions, suppression, rollback ---------------------------------------------------------
json action_rename_variable(DesignPanel* panel, const json& params)
{
    if (!params.contains("from") || !params.contains("to") || !params["from"].is_string() || !params["to"].is_string())
        throw std::runtime_error("rename_variable needs 'from' and 'to' (names)");
    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    std::string err;
    bool ok = doc.rename_variable(params["from"].get<std::string>(), params["to"].get<std::string>(), err);
    if (ok) { ok = doc.recompute(); if (!ok) err = doc.error; }
    if (!ok) { doc.undo(); doc.error = err; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"error", err}};
}

// CSV of the variables: returned, and written to `path` when given.
json action_variables_export(DesignPanel* panel, const json& params)
{
    const std::string csv = panel->mcp_doc().variables_csv();
    json out{{"ok", true}, {"csv", csv}, {"count", int(panel->mcp_doc().variables.size())}};
    if (params.contains("path")) {
        const std::string path = checked_out_path(params, "variables_export");
        std::ofstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("cannot write " + path);
        f << csv;
        out["path"] = path;
    }
    return out;
}

// Adds or replaces variables from `csv` (text) or `path` (a file). All rows are checked before any is kept.
json action_variables_import(DesignPanel* panel, const json& params)
{
    std::string csv;
    if (params.contains("csv") && params["csv"].is_string()) csv = params["csv"].get<std::string>();
    else if (params.contains("path")) {
        const std::string path = checked_out_path(params, "variables_import");
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("cannot read " + path);
        csv.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    } else throw std::runtime_error("variables_import needs 'csv' or 'path'");
    if (csv.size() > (1u << 20)) throw std::runtime_error("CSV is limited to 1 MB");
    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    std::string err;
    const int n = doc.variables_from_csv(csv, err);
    bool ok = err.empty();
    if (ok) { ok = doc.recompute(); if (!ok) err = doc.error; }
    if (!ok) { doc.undo(); doc.error = err; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"imported", ok ? n : 0}, {"error", err}};
}

// The dimensions of a committed sketch: index (for set_feature_expr field "dim:<index>"), kind, value, bound expression.
json action_sketch_dimensions(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const int sk = sketch_ref_arg(params, doc);
    if (sk < 0) throw std::runtime_error("sketch_dimensions needs 'sketch' (a Sketch feature index)");
    const CadFeature& f = doc.features[sk];
    json dims = json::array();
    for (size_t i = 0; i < f.entity_constraints.size(); ++i) {
        const auto& c = f.entity_constraints[i];
        using T = SketchConstraintType;
        if (c.type != T::Distance && c.type != T::DistanceX && c.type != T::DistanceY && c.type != T::Angle && c.type != T::Radius && c.type != T::Diameter) continue;
        const std::string field = "dim:" + std::to_string(i);
        const char* kind = c.type == T::Angle ? "angle" : c.type == T::Radius ? "radius" : c.type == T::Diameter ? "diameter" : c.type == T::DistanceX ? "distance_x" : c.type == T::DistanceY ? "distance_y" : "distance";
        dims.push_back(json{{"index", int(i)}, {"field", field}, {"kind", kind}, {"value", c.value}, {"expr", f.expr.count(field) ? f.expr.at(field) : std::string()}});
    }
    return json{{"ok", true}, {"sketch", sk}, {"dimensions", dims}};
}

// Suppress (disable) or restore a feature; downstream features rebuild without it.
json action_suppress(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (!params.contains("feature") || !params["feature"].is_number_integer()) throw std::runtime_error("suppress needs 'feature' (index)");
    const int fi = params["feature"].get<int>();
    if (fi < 0 || fi >= int(doc.features.size())) throw std::runtime_error("feature index out of range");
    const bool suppressed = params.value("suppressed", true);
    doc.checkpoint();
    doc.features[fi].enabled = !suppressed;
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.undo(); doc.error = why; }
    panel->mcp_after_change();
    return json{{"ok", ok}, {"feature", fi}, {"suppressed", suppressed}, {"error", doc.error}};
}

json action_rename_feature(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (!params.contains("feature") || !params["feature"].is_number_integer() || !params.contains("name") || !params["name"].is_string())
        throw std::runtime_error("rename_feature needs 'feature' (index) and 'name'");
    const int fi = params["feature"].get<int>();
    if (fi < 0 || fi >= int(doc.features.size())) throw std::runtime_error("feature index out of range");
    const std::string name = params["name"].get<std::string>();
    if (name.empty() || name.size() > 128) throw std::runtime_error("name must be 1..128 characters");
    doc.checkpoint();
    doc.features[fi].name = name;
    panel->mcp_after_change();
    return json{{"ok", true}, {"feature", fi}, {"name", name}};
}

// Move a feature to history position `to`, one checked step at a time; stops (ok:false) where a dependency forbids the next step.
json action_move_feature(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (!params.contains("feature") || !params["feature"].is_number_integer() || !params.contains("to") || !params["to"].is_number_integer())
        throw std::runtime_error("move_feature needs 'feature' and 'to' (indices)");
    int cur = params["feature"].get<int>();
    const int to = params["to"].get<int>(), n = int(doc.features.size());
    if (cur < 0 || cur >= n || to < 0 || to >= n) throw std::runtime_error("feature index out of range");
    doc.checkpoint();
    bool ok = true;
    while (cur != to) {
        const int step = to > cur ? 1 : -1;
        if (!doc.move_feature(cur, step)) { ok = false; break; }
        cur += step;
    }
    const std::string why = ok ? std::string() : doc.error;
    panel->mcp_after_change();
    return json{{"ok", ok}, {"feature", cur}, {"error", why}};
}

// Roll the history back to just after feature `index` (later features are kept, not built); index -1 = the full history.
json action_rollback(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (!params.contains("index") || !params["index"].is_number_integer()) throw std::runtime_error("rollback needs 'index' (-1 = full history)");
    const int idx = params["index"].get<int>();
    if (idx < -1 || idx >= int(doc.features.size())) throw std::runtime_error("index out of range");
    doc.rollback_index = idx;
    const bool ok = doc.recompute();
    panel->mcp_after_change();
    return json{{"ok", ok}, {"rollback_index", doc.rollback_index}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

// ---- undo / redo / inspect_design / batch ----------------------------------------------------------------
struct MethodNotFound {};
json        call_method(DesignPanel* panel, const std::string& method, const json& params);   // below
std::string refuse_reason(DesignPanel* panel, const std::string& method, const json& params = json());   // below

json undo_redo_state(DesignPanel* panel, bool did)
{
    const CadDocument& doc = panel->mcp_doc();
    return json{{"ok", true}, {"did", did}, {"features", int(doc.features.size())}, {"bodies", int(doc.bodies.size())},
                {"undo_depth", int(doc.undo_depth())}, {"redo_depth", int(doc.redo_depth())}, {"error", doc.error}};
}
json action_undo(DesignPanel* panel, const json&) { bool did; { PhaseLog t("undo"); did = panel->mcp_doc().undo(); } panel->mcp_after_change(); return undo_redo_state(panel, did); }
json action_redo(DesignPanel* panel, const json&) { bool did; { PhaseLog t("redo"); did = panel->mcp_doc().redo(); } panel->mcp_after_change(); return undo_redo_state(panel, did); }

// One call that answers "what is the state of this design": scene + variables + per-body mass properties and
// topology counts (the numbers an agent checks after every step), plus undo depth and whether a sketch is open.
json action_inspect_design(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    json out = describe_scene(panel);
    const bool measure = params.value("measure", true);
    for (size_t i = 0; i < doc.bodies.size() && i < out["bodies"].size(); ++i) {
        const TopoDS_Shape& shape = doc.bodies[i].shape;
        if (shape.IsNull()) continue;
        json& b = out["bodies"][i];
        b["faces"] = int(GeometryEngine::faces_of(shape).size());
        b["edges"] = int(GeometryEngine::edges_of(shape).size());
        if (!measure) continue;
        const auto mp = GeometryEngine::mass_properties(shape);
        if (!mp.valid) continue;
        b["is_solid"] = mp.is_solid;
        b["surface_area"] = mp.surface_area;
        if (mp.is_solid) {
            b["volume"] = mp.volume;
            b["center_of_mass"] = json::array({mp.center_of_mass.x(), mp.center_of_mass.y(), mp.center_of_mass.z()});
        }
    }
    json vars = json::object();
    for (const auto& kv : doc.variables) vars[kv.first] = kv.second;
    out["variables"]     = std::move(vars);
    out["error_feature"] = doc.error_feature;
    out["undo_depth"]    = int(doc.undo_depth());
    out["redo_depth"]    = int(doc.redo_depth());
    out["recompute"]     = {{"replayed", doc.replay_stats.replayed}, {"restored", doc.replay_stats.restored}, {"feature_ms", doc.replay_stats.feature_ms}, {"mesh_ms", doc.replay_stats.mesh_ms}};
    DesignCanvas* vp = panel->mcp_viewport();
    out["sketch_open"] = vp != nullptr && vp->is_sketching();
    return out;
}

void inflight_step(const std::string& step);   // the watchdog block below

// {commands:[{command, params}, ...], rollback:true, verbose:true}. Runs the calls in order on the main thread,
// stops at the first refusal or error and (rollback) restores the features and variables it started with.
// ponytail: the restore is a copy, not an undo walk, so it is exact even past the 200-step undo cap, but the undo
// stack keeps the batch's intermediate steps. Sketch-session methods (sketch_begin ...) are not rolled back; run
// sketches to completion (sketch_commit) inside the batch.
json action_batch(DesignPanel* panel, const json& params)
{
    if (!params.contains("commands") || !params["commands"].is_array())
        throw std::runtime_error("batch needs 'commands': [{command, params}, ...]");
    const json& cmds = params["commands"];
    if (cmds.size() > 10000) throw std::runtime_error("batch is limited to 10000 commands");
    const bool rollback = params.value("rollback", true);
    const bool verbose  = params.value("verbose", true);
    CadDocument& doc = panel->mcp_doc();
    const auto saved_features  = doc.features;
    const auto saved_variables = doc.variables;
    const size_t undo_at_start = doc.undo_depth();
    json results = json::array(), failed;
    const auto t_start = std::chrono::steady_clock::now();
    const double budget_s = params.value("time_budget_s", 30.0);   // the window is frozen while a batch runs: bound it
    for (size_t i = 0; i < cmds.size() && failed.is_null(); ++i) {
        const json& c = cmds[i];
        const std::string name = c.is_object() ? c.value("command", c.value("method", std::string())) : std::string();
        const json p = (c.is_object() && c.contains("params")) ? c["params"] : json::object();
        std::string err;
        json r;
        inflight_step(std::to_string(i + 1) + "/" + std::to_string(cmds.size()) + " " + name);
        if (i > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count() > budget_s)
            err = "time budget of " + std::to_string(int(budget_s)) + " s used up (the UI is frozen while a batch runs); "
                  + std::to_string(cmds.size() - i) + " commands were not run: send them in a new batch, or pass a larger time_budget_s";
        else if (name.empty() || name == "batch") err = "each entry needs a 'command' (and a batch cannot nest)";
        else if (!(err = refuse_reason(panel, name, p)).empty()) {}
        else {
            try { r = call_method(panel, name, p); }
            catch (const MethodNotFound&)    { err = "unknown command: " + name; }
            catch (const Standard_Failure& e) { err = std::string("OCCT: ") + (*e.what() ? e.what() : "failure"); }
            catch (const std::exception& e)   { err = e.what(); }
        }
        if (err.empty() && r.is_object() && r.contains("ok") && r["ok"] == false)
            err = r.value("error", std::string("refused"));
        if (!err.empty()) failed = json{{"index", int(i)}, {"command", name}, {"error", err}};
        else              results.push_back(verbose ? r : json{{"ok", true}});
    }
    bool undone = false;
    if (!failed.is_null() && rollback) {
        doc.features = saved_features;
        doc.variables = saved_variables;
        doc.drop_undo_to(undo_at_start);   // the steps the failed batch pushed describe a history that no longer happened
        doc.recompute();
        panel->mcp_after_change();
        undone = true;
    }
    json out{{"ok", failed.is_null()}, {"completed", int(results.size())}, {"results", std::move(results)}, {"rolled_back", undone}};
    if (!failed.is_null()) out["failed"] = failed;
    return out;
}

json call_method_core(DesignPanel* panel, const std::string& method, const json& params);

// The one dispatch point: the socket handler and `batch` both come through here. A call that changes the design
// reports WHAT it changed - created/changed/removed bodies, features added, warnings - so a caller need not diff
// describe_scene after every step.
json call_method(DesignPanel* panel, const std::string& method, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    auto bodies_now = [&]() { std::vector<const void*> v; for (const CadBody& b : doc.bodies) v.push_back(b.shape.IsNull() ? nullptr : b.shape.TShape().get()); return v; };
    const size_t nf = doc.features.size();
    const std::vector<const void*> before = bodies_now();
    json r = call_method_core(panel, method, params);
    if (!r.is_object() || !r.contains("ok") || read_only_method(method) || method.rfind("sketch_", 0) == 0 || method == "batch" || method == "undo" || method == "redo"
        || method == "open" || method == "new_document" || method == "run_verb" || method == "execute")
        return r;
    const std::vector<const void*> after = bodies_now();
    json created = json::array(), changed = json::array();
    for (size_t i = 0; i < after.size(); ++i) {
        if (i >= before.size()) created.push_back(int(i));
        else if (after[i] != before[i]) changed.push_back(int(i));
    }
    json warn = json::array();
    // An edit of a variable or a field rebuilds features that already existed: a reference one of them could not follow
    // (it "may point elsewhere") is what the caller most needs to hear, so those calls report every warning, named by feature.
    const bool edits_existing = method == "set_variable" || method == "set_parameter" || method == "set_feature_expr" || method == "rename_variable"
        || method == "variables_import" || method == "suppress" || method == "move_feature" || method == "rollback";
    for (const auto& w : doc.feature_warnings) {
        if (w.first >= int(nf)) warn.push_back(w.second);
        else if (edits_existing) warn.push_back("feature " + std::to_string(w.first) + ": " + w.second);
    }
    r["delta"] = {{"features_added", int(doc.features.size()) - int(nf)}, {"created_bodies", created}, {"changed_bodies", changed},
                  {"removed_bodies", before.size() > after.size() ? int(before.size() - after.size()) : 0}, {"warnings", warn}};
    return r;
}

json call_method_core(DesignPanel* panel, const std::string& method, const json& params)
{
    if (method == "describe_tools") return describe_tools();
    if (method == "describe_scene") return describe_scene(panel);
    if (method == "query_topology") return query_topology(panel, params);
    if (method == "measure")        return measure(panel, params);
    if (method == "mass_properties") return mass_properties(panel, params);
    if (method == "slice_body")     return slice_body(panel, params);
    if (method == "import_step")    return import_step(panel, params);
    if (method == "import_mesh")    return import_mesh(panel, params);
    if (method == "validate_against") return validate_against(panel, params);
    if (method == "extrude")        return action_extrude(panel, params);
    if (method == "revolve")        return action_revolve(panel, params);
    if (method == "fillet")         return action_fillet(panel, params);
    if (method == "chamfer")        return action_chamfer(panel, params);
    if (method == "hole")           return action_hole(panel, params);
    if (method == "hole_styled")   return action_hole_styled(panel, params);
    if (method == "hole_standard") return action_hole_standard(panel, params);
    if (method == "boolean")        return action_boolean(panel, params);
    if (method == "pattern")        return action_pattern(panel, params);
    if (method == "pattern_on_curve") return action_pattern_on_curve(panel, params);
    if (method == "shell")          return action_shell(panel, params);
    if (method == "rib")            return action_rib(panel, params);
    if (method == "draft")          return action_draft(panel, params);
    if (method == "mirror")         return action_mirror(panel, params);
    if (method == "sketch_begin")   return action_sketch_begin(panel, params);
    if (method == "sketch_commit")  return action_sketch_commit(panel, params);
    if (method == "sketch_cancel")  return action_sketch_cancel(panel, params);
    if (method == "sketch_add")     return action_sketch_add(panel, params);
    if (method == "sketch_select")  return action_sketch_select(panel, params);
    if (method == "sketch_delete")  return action_sketch_delete(panel, params);
    if (method == "sketch_construction") return action_sketch_construction(panel, params);
    if (method == "sketch_offset")  return action_sketch_offset(panel, params);
    if (method == "sketch_mirror")  return action_sketch_mirror(panel, params);
    if (method == "sketch_describe") return action_sketch_describe(panel, params);
    if (method == "sketch_validate") return action_sketch_validate(panel, params);
    if (method == "sketch_heal")    return action_sketch_heal(panel, params);
    if (method == "sketch_set_value") return action_sketch_set_value(panel, params);
    if (method == "list_verbs")     return action_list_verbs(panel, params);
    if (method == "run_verb")       return action_run_verb(panel, params);
    if (method == "transform")      return action_transform(panel, params);
    if (method == "thicken")        return action_thicken(panel, params);
    if (method == "split")          return action_split(panel, params);
    if (method == "project")        return action_project(panel, params);
    if (method == "delete_face")    return action_delete_face(panel, params);
    if (method == "bridge")         return action_bridge(panel, params);
    if (method == "plane")          return action_plane(panel, params);
    if (method == "ui_option")      return action_ui_option(panel, params);
    if (method == "sketch_constrain") return action_sketch_constrain(panel, params);
    if (method == "export")         return action_export(panel, params);
    if (method == "list_commands")  return action_list_commands(panel, params);
    if (method == "execute")        return action_execute(panel, params);
    if (method == "set_parameter") {   // set_variable, plus how much of the model the change rebuilt
        json r = action_set_variable(panel, params);
        const CadDocument& d = panel->mcp_doc();
        r["recomputed"] = d.replay_stats.replayed;
        r["restored"]   = d.replay_stats.restored;
        return r;
    }
    if (method == "rename_variable")   return action_rename_variable(panel, params);
    if (method == "variables_export")  return action_variables_export(panel, params);
    if (method == "variables_import")  return action_variables_import(panel, params);
    if (method == "sketch_dimensions") return action_sketch_dimensions(panel, params);
    if (method == "suppress")          return action_suppress(panel, params);
    if (method == "rename_feature")    return action_rename_feature(panel, params);
    if (method == "move_feature")      return action_move_feature(panel, params);
    if (method == "rollback")          return action_rollback(panel, params);
    if (method == "sweep")          return action_sweep(panel, params);
    if (method == "loft")           return action_loft(panel, params);
    if (method == "thread")         return action_thread(panel, params);
    if (method == "sm_rule" || method == "sm_base" || method == "sm_flange" || method == "sm_contour" || method == "sm_hem"
        || method == "sm_unfold" || method == "sm_refold")
        return action_sheet(panel, method, params);
    if (method == "cut")            return action_cut(panel, params);
    if (method == "save")           return action_save(panel, params);
    if (method == "open")           return action_open(panel, params);
    if (method == "screenshot")     return action_screenshot(panel, params);
    if (method == "undo")           return action_undo(panel, params);
    if (method == "redo")           return action_redo(panel, params);
    if (method == "inspect_design") return action_inspect_design(panel, params);
    if (method == "batch")          return action_batch(panel, params);
    if (method == "new_document")   return action_new_document(panel, params);
    if (method == "sketch3d")       return action_sketch3d(panel, params);
    if (method == "use")            return action_use(panel, params);
    if (method == "axis")           return action_axis(panel, params);
    if (method == "coordsys")       return action_coordsys(panel, params);
    if (method == "helix")          return action_helix(panel, params);
    if (method == "set_variable")     return action_set_variable(panel, params);
    if (method == "set_feature_expr") return action_set_feature_expr(panel, params);
    if (method == "surface_extrude")  return action_surface_extrude(panel, params);
    if (method == "surface_revolve")  return action_surface_revolve(panel, params);
    if (method == "thicken_surface")  return action_thicken_surface(panel, params);
    if (method == "surface_offset")   return action_surface_offset(panel, params);
    if (method == "surface_loft")    return action_surface_loft(panel, params);
    if (method == "surface_fill")    return action_surface_fill(panel, params);
    if (method == "mate")          return action_mate(panel, params);
    if (method == "check_interference") return action_check_interference(panel, params);
    throw MethodNotFound{};
}

// Why `method` may not run now ("" = it may). Methods that only LOOK always may. Everything else changes the document
// or the live sketch, and must not do it under a GUI editor, a sketch session it does not own, or a rebuild in
// progress (the GUI's worker thread holds the document then).
std::string refuse_reason(DesignPanel* panel, const std::string& method, const json& params)
{
    if (read_only_method(method)) return "";
    // a sketch-mode verb (a relation, a drawing tool) works ON the open sketch, like the sketch_* methods do
    bool sketchish = method.rfind("sketch_", 0) == 0;
    if (method == "run_verb" && params.is_object() && params.contains("verb") && params["verb"].is_string()) {
        const std::string id = params["verb"].get<std::string>();
        for (int i = 0; i < kOfferVerbCount; ++i) if (id == kOfferVerbs[i].id && kOfferVerbs[i].sketch_mode) sketchish = true;
    }
    std::string why;
    return panel->mcp_busy(sketchish, why) ? "Design tab busy: " + why : std::string();
}

// ---- is the UI thread still answering, and what was it doing? --------------------------------------------------------
// Every MCP method runs on the UI thread, so a slow one freezes the window. GNOME offers "Wait / Force Quit" after about 5 s,
// and the kill that follows leaves no log line, no core and no kernel message. So each call is logged with its duration, the
// call in flight is remembered (a batch names the command it is on), and a watchdog thread says what was running when the
// UI stopped answering, with the thread's kernel state and, on Linux, its stack. The watchdog makes no wx calls: it only
// reads a clock that a timer on the UI thread keeps fresh.
struct InFlight {
    std::mutex m;
    std::string method, step;
    std::chrono::steady_clock::time_point since;
};
InFlight g_inflight;
std::atomic<int64_t> g_ui_alive_ms{0};   // last time the UI thread's timer fired

int64_t steady_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void inflight_step(const std::string& step)
{
    std::lock_guard<std::mutex> l(g_inflight.m);
    g_inflight.step = step;
}

// handle_on_main() with the call remembered while it runs and logged when it ends (slow ones as a warning).
std::string handle_on_main(const std::string& method, const json& params, const json& id);
std::string timed_handle(const std::string& method, const json& params, const json& id)
{
    struct Scope {
        explicit Scope(const std::string& m) {
            std::lock_guard<std::mutex> l(g_inflight.m);
            g_inflight.method = m; g_inflight.step.clear(); g_inflight.since = std::chrono::steady_clock::now();
        }
        ~Scope() {
            std::lock_guard<std::mutex> l(g_inflight.m);
            g_inflight.method.clear(); g_inflight.step.clear();
        }
    } scope(method);
    const auto t0 = std::chrono::steady_clock::now();
    std::string reply = handle_on_main(method, params, id);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::string what = method;
    if (method == "batch" && params.is_object() && params.contains("commands") && params["commands"].is_array())
        what += " (" + std::to_string(params["commands"].size()) + " commands)";
    if (ms >= 2000) BOOST_LOG_TRIVIAL(warning) << "MCP: " << what << " took " << ms << " ms on the UI thread (" << reply.size() << " bytes)";
    else            BOOST_LOG_TRIVIAL(info)    << "MCP: " << what << " " << ms << " ms (" << reply.size() << " bytes)";
    return reply;
}

#ifdef __linux__
long      g_main_tid = 0;
pthread_t g_main_pthread{};
int       g_stall_fd = -1;
int       g_stall_sig = 0;
// Async-signal-safe enough for a diagnostic: backtrace() is primed once at install time so it does not allocate here.
void stall_stack_handler(int) { void* f[40]; const int n = backtrace(f, 40); if (g_stall_fd >= 0) backtrace_symbols_fd(f, n, g_stall_fd); }

std::string proc_text(const std::string& path)
{
    std::ifstream in(path);
    std::string t;
    std::getline(in, t);
    return t;
}
#endif

class UiBeat : public wxTimer {
public:
    void Notify() override { g_ui_alive_ms = steady_ms(); }
};

// MCP work for the UI thread goes through this FIFO, one call per timer tick, instead of one wxApp::CallAfter per call.
// wx drains its whole pending-event list before it returns to the GTK main loop, and a client (or four) that sends the next
// call as soon as it has the last reply refills that list as fast as it empties: timers, repaints and the user's keys and
// mouse then wait for as long as the flood lasts, and the window is "not responding". A tick per call leaves the loop a turn
// in between, at a cost of about a millisecond a call.
class MainPump : public wxTimer {
public:
    void post(std::function<void()> f)
    {
        bool kick;
        { std::lock_guard<std::mutex> l(m_m); m_q.push_back(std::move(f)); kick = !m_armed; m_armed = true; }
        if (kick) wxGetApp().CallAfter([this] { StartOnce(1); });   // a wxTimer is started on the UI thread
    }
    void Notify() override
    {
        std::function<void()> f;
        bool more;
        {
            std::lock_guard<std::mutex> l(m_m);
            if (m_q.empty()) { m_armed = false; return; }
            f = std::move(m_q.front()); m_q.pop_front(); more = !m_q.empty(); if (!more) m_armed = false;
        }
        if (more) StartOnce(1);
        f();
    }
private:
    std::mutex m_m;
    std::deque<std::function<void()>> m_q;
    bool m_armed{false};   // a tick is already scheduled
};
MainPump* g_main_pump = nullptr;   // created on the UI thread by install_ui_watchdog()

void ui_watchdog()
{
    bool reported = false;
    int64_t next_report = 0, worst = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const int64_t now = steady_ms(), silent = now - g_ui_alive_ms.load();
        if (silent < 5000) {
            if (reported) BOOST_LOG_TRIVIAL(warning) << "UI: the main thread answers again (it was silent for about " << worst / 1000 << " s)";
            reported = false; worst = 0;
            continue;
        }
        worst = std::max(worst, silent);
        if (reported && now < next_report) continue;
        std::string what = "no MCP call in flight";
        {
            std::lock_guard<std::mutex> l(g_inflight.m);
            if (!g_inflight.method.empty())
                what = "MCP call in flight: " + g_inflight.method + (g_inflight.step.empty() ? "" : " [" + g_inflight.step + "]")
                     + ", running for " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_inflight.since).count()) + " s";
        }
        std::string kernel;
#ifdef __linux__
        const std::string tid = std::to_string(g_main_tid);
        const std::string stat = proc_text("/proc/self/task/" + tid + "/stat");
        const size_t rp = stat.rfind(')');
        kernel = "; main thread state " + (rp != std::string::npos && rp + 2 < stat.size() ? std::string(1, stat[rp + 2]) : std::string("?"))
               + " (R running, S/D waiting), waiting in " + proc_text("/proc/self/task/" + tid + "/wchan");
#endif
        BOOST_LOG_TRIVIAL(warning) << "UI: the main thread has not answered for " << silent / 1000 << " s; " << what << kernel;
#ifdef __linux__
        if (!reported && g_stall_fd >= 0 && g_stall_sig > 0) {   // the stack, once per stall
            const off_t from = ::lseek(g_stall_fd, 0, SEEK_END);
            ::pthread_kill(g_main_pthread, g_stall_sig);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            char buf[4096];
            const ssize_t n = from >= 0 ? ::pread(g_stall_fd, buf, sizeof(buf) - 1, from) : -1;
            if (n > 0) { buf[n] = 0; BOOST_LOG_TRIVIAL(warning) << "UI: main thread stack (resolve with addr2line -f -C -e orca-slicer):\n" << buf; }
            else       BOOST_LOG_TRIVIAL(warning) << "UI: the main thread did not run the stack handler (blocked in the kernel or with signals masked)";
        }
#endif
        reported = true; next_report = now + 30000;
    }
}

// Called once, on the UI thread.
void install_ui_watchdog()
{
    g_ui_alive_ms = steady_ms();
    (new UiBeat)->Start(1000);   // leaked on purpose: it lives as long as the application
    g_main_pump = new MainPump;  // likewise
#ifdef __linux__
    g_main_tid = long(::syscall(SYS_gettid));
    g_main_pthread = ::pthread_self();
    g_stall_sig = SIGRTMIN + 4;   // glibc keeps the first few real-time signals for itself
    const std::string path = "/tmp/orca-cad-stall-" + std::to_string(::getpid()) + ".txt";
    g_stall_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (g_stall_fd >= 0) ::unlink(path.c_str());   // the watchdog reads the stack back through the fd and logs it: nothing is left in /tmp
    void* prime[1]; (void) ::backtrace(prime, 1);
    struct sigaction sa{};
    sa.sa_handler = stall_stack_handler;
    sa.sa_flags = SA_RESTART;
    ::sigemptyset(&sa.sa_mask);
    if (g_stall_fd < 0 || ::sigaction(g_stall_sig, &sa, nullptr) != 0) g_stall_sig = 0;
#endif
    std::thread(ui_watchdog).detach();
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
    // A project opened without ever showing the Design tab has its recipe only in the Model;
    // load it, as showing the tab would, before anything reads or writes the document.
    panel->hydrate_from_model();

    if (const std::string why = refuse_reason(panel, method, params); !why.empty())
        return rpc_error(id, -32002, why, busy_data(why));

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
        return rpc_result(id, call_method(panel, method, params));
    } catch (const MethodNotFound&) {
        return rpc_error(id, -32601, "Unknown method: " + method);
    } catch (const Standard_Failure& ex) {   // derives from std::exception, so it comes first
        return rpc_error(id, -32000, std::string("OCCT: ") + (*ex.what() ? ex.what() : "failure"));
    } catch (const std::exception& ex) {
        return rpc_error(id, -32000, ex.what());
    }
}

std::string dispatch_once(const std::string& method, const json& params, const json& id);

// Marshal a request to the main thread and block (with a timeout) for the reply.
// The generation of the topology this connection last read (query_topology, describe_scene). One thread serves one
// connection, so thread_local is per client. A call that takes a face or edge id and does not say which generation its ids
// come from is checked against it: the ids it carries can only have come from that read.
thread_local uint64_t t_seen_generation = 0;
thread_local bool     t_has_seen_generation = false;

bool takes_topology_ids(const json& params)
{
    if (!params.is_object()) return false;
    for (const char* k : {"edge", "edges", "face", "faces", "up_to_face"}) {
        const auto it = params.find(k);
        if (it == params.end()) continue;
        if (it->is_number_integer() && it->get<long long>() >= 0) return true;
        if (it->is_array() && !it->empty()) return true;
    }
    return false;
}

std::string dispatch_request(const std::string& line)
{
    // Nesting is bounded BEFORE parsing: copying or destroying a document a few hundred thousand levels deep recurses
    // off the end of the thread's stack, and the app dies. No real request nests past a handful of levels.
    {
        int depth = 0; bool in_str = false;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (in_str) { if (c == '\\') ++i; else if (c == '"') in_str = false; }
            else if (c == '"') in_str = true;
            else if (c == '[' || c == '{') { if (++depth > 64) return rpc_error(nullptr, -32600, "request nests deeper than 64 levels"); }
            else if (c == ']' || c == '}') --depth;
        }
    }
    json req;
    try { req = json::parse(line); }
    catch (const std::exception& ex) { return rpc_error(nullptr, -32700, std::string("parse error: ") + ex.what()); }

    // A request that parses but is not shaped like one ({"method": 5}, a bare number, ...) must be refused, not thrown:
    // this runs on a detached client thread, where an escaping exception is std::terminate.
    if (!req.is_object()) return rpc_error(nullptr, -32600, "a request must be a JSON object");
    json id = req.contains("id") ? req["id"] : json(nullptr);
    if (!req.contains("method") || !req["method"].is_string()) return rpc_error(id, -32600, "'method' must be a string");
    std::string method = req["method"].get<std::string>();
    json params        = req.contains("params") ? req["params"] : json::object();
    if (method.empty()) return rpc_error(id, -32600, "missing method");
    // Optional per-launch secret: when ORCA_CAD_MCP_TOKEN is set every request must carry it as "token". The socket's
    // 0600 mode already keeps other users out; this also keeps out other processes of the same user that were not told.
    if (const char* tok = std::getenv("ORCA_CAD_MCP_TOKEN"); tok && *tok && (req.contains("token") && req["token"].is_string() ? req["token"].get<std::string>() : std::string()) != tok)
        return rpc_error(id, -32003, "unauthorized: this app was launched with ORCA_CAD_MCP_TOKEN; send it as \"token\"");
    // A transient refusal (a rebuild in progress) is waited out here, on the client's own thread, never on the UI thread.
    if (method.rfind("ui.", 0) == 0 && method != "ui.screenshot") {
        if (const char* on = std::getenv("ORCA_CAD_MCP_UI"); on == nullptr || std::string(on) != "1")   // synthetic input is for test rigs, not for every agent on the socket
            return rpc_error(id, -32601, "the ui.* test drivers are off: start the app with ORCA_CAD_MCP_UI=1");
        try { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", ui_dispatch(method, params)}}.dump(); }
        catch (const std::exception& ex) { return rpc_error(id, -32000, ex.what()); }
    }
    if (method == "ui.screenshot") method = "screenshot";
    if (t_has_seen_generation && params.is_object() && !params.contains("generation") && takes_topology_ids(params))
        params["generation"] = t_seen_generation;   // an id the client did not date is dated by its own last read
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::string r = dispatch_once(method, params, id);
        if (r.find("\"transient\":true") == std::string::npos || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(15)) {
            if (method == "query_topology" || method == "describe_scene") {   // what this client has now seen
                try {
                    const json rr = json::parse(r);
                    if (rr.contains("result") && rr["result"].is_object() && rr["result"].contains("generation") && rr["result"]["generation"].is_number_unsigned()) {
                        t_seen_generation = rr["result"]["generation"].get<uint64_t>();
                        t_has_seen_generation = true;
                    }
                } catch (const std::exception&) {}
            }
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::string dispatch_once(const std::string& method, const json& params, const json& id)
{
    auto prom = std::make_shared<std::promise<std::string>>();
    auto fut  = prom->get_future();
    // 0 = queued, 1 = running, 2 = abandoned. A request that timed out while still QUEUED must
    // never run: the client has been told it failed, and a retry would otherwise apply it twice.
    auto state = std::make_shared<std::atomic<int>>(0);
    // Nothing may escape this lambda. It is invoked by the wx event loop, which has no
    // handler of its own, so an escaping exception is std::terminate — the socket would
    // become a way for any client to kill the application. handle_on_main() catches what
    // it knows about; this catches what it does not, and still answers the caller.
    auto run = [prom, state, method, params, id]() {
        int queued = 0;
        if (!state->compare_exchange_strong(queued, 1)) return;   // abandoned by a timeout
        try {
            prom->set_value(timed_handle(method, params, id));
        } catch (const std::exception& ex) {
            prom->set_value(rpc_error(id, -32000, std::string("internal error: ") + ex.what()));
        } catch (...) {
            prom->set_value(rpc_error(id, -32000, "internal error: unknown exception"));
        }
    };
    if (g_main_pump != nullptr) g_main_pump->post(std::move(run));   // one call per event-loop turn
    else                        wxGetApp().CallAfter(std::move(run));
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
            std::string reply;
            try { reply = dispatch_request(line); }
            catch (...) { reply = rpc_error(nullptr, -32603, "internal error"); }   // a client thread must never throw
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
            // A socket that still answers belongs to a running process: never take its path over.
            const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe >= 0) {
                sockaddr_un pa{};
                pa.sun_family = AF_UNIX;
                std::strncpy(pa.sun_path, sock_path.c_str(), sizeof(pa.sun_path) - 1);
                const bool live = ::connect(probe, reinterpret_cast<sockaddr*>(&pa), sizeof(pa)) == 0;
                ::close(probe);
                if (live) {
                    BOOST_LOG_TRIVIAL(warning) << "MCP: " << sock_path << " is in use by another process; not listening";
                    return;
                }
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
    if (::listen(sfd, 8) < 0) { BOOST_LOG_TRIVIAL(error) << "MCP: listen() failed"; ::close(sfd); return; }
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
        static std::atomic<int> clients{0};
        if (clients.load() >= 8) { ::close(cfd); continue; }   // a runaway client must not exhaust threads
        ++clients;
        std::thread([cfd]() { serve_client(cfd); ::close(cfd); --clients; }).detach();
    }
}

} // namespace

void start_mcp_control_if_enabled()
{
    // On by default (the Design tab must be enabled to get here); ORCA_CAD_MCP=0|off opts out.
    const char* env = std::getenv("ORCA_CAD_MCP");
    if (env && (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0)) return;
    const std::string path = (!env || !*env || std::strcmp(env, "1") == 0) ? "/tmp/orca-cad-mcp.sock" : env;
    static bool started = false;
    if (started) return;
    started = true;
    install_ui_watchdog();
    std::thread(server_thread, path).detach();
}

}} // namespace Slic3r::GUI

#else  // _WIN32

namespace Slic3r { namespace GUI {
void start_mcp_control_if_enabled() {}   // ponytail: no Windows transport yet
}}

#endif
