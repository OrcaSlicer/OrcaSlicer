// The Design tab's commands. The Design table (kDesignTools, generated from scripts/CAD/tool_atlas.json)
// declares every one, its menu item, handler, access, tool block and the items folded into it; kSilentForms binds a
// block to the function that runs it. register_cad_commands() makes a command of each entry that is one
// (is_design_command()): a call with the arguments that choose one of its items does what that menu item does
// (opened_item(), open_tool()), and any other has run_cad_tool() run its silent form on the UI thread, through the
// GUI's own CadDocument and DesignPanel's mcp_* hooks, behind the Design tab's busy and stale-id checks
// (cad_gate()).

#include "McpToolsCad.hpp"
#include "libslic3r/Point.hpp"
#include <stdexcept>
#include <math.h>
#include <TopAbs_ShapeEnum.hxx>
#include <Standard_TypeDef.hxx>
#include <exception>
#include <cstdint>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <utility>
#include <cctype>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include <nlohmann/json.hpp>
#include <wx/string.h>
#include <wx/translation.h>
#include <Standard_Failure.hxx>   // OCCT base error

#include "slic3r/GUI/GUI.hpp"   // into_u8
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"   // itObject
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "McpArguments.hpp"
#include "McpTools.hpp"
#include "McpPaths.hpp"
#include "UiInspector.hpp"   // gizmo_name: the name app_state reports
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"
#include "slic3r/GUI/CAD/DesignOffer.hpp"   // the Design table: every command's menu item, handler and tool block

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/SketchSolver.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/CAD/SketchImport.hpp"   // svg_to_regions, transform_regions
#include "libslic3r/CAD/ThreadStandards.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r_version.h"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include "libslic3r/BoundingBox.hpp"

#include <gp_Pln.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
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

namespace Slic3r { namespace GUI { namespace Mcp {

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

// --- What the commands' descriptions and schemas add to the Design table ---------------

// The `generation` property of every Design command with a silent form: how face and edge ids expire, and
// how a call keeps from acting on a stale one.
const char* const kGenerationNote =
    "The generation (describe_design, body_describe) the face and edge ids this call passes were read at. Ids "
    "expire when a feature rebuilds the model, and a stale id still names a real face or edge: with the "
    "generation, a call whose ids were read before the model changed is refused.";

// How a silent form whose entry has ok_false reports a rebuild the kernel rejects.
const char* const kFailureNote = "A rejected rebuild is reported in the reply: ok is false or error is not empty.";

// The rest of the description of a command whose entry has a handler and no silent form, after its hint and
// before mode_note().
const char* const kOpensToolNote =
    "It takes no arguments and does what the Design tab's right-click menu item does, as a person's pick does: open "
    "a tool, start a sketch tool, act at once, or tell the person why it cannot. The reply's status is what the "
    "tab's status line shows then.";

// The `silent` property of a command whose call with no arguments opens a tool, before mode_note() of that tool's
// item.
const char* const kSilentNote =
    "true runs the tool with the other arguments and opens nothing. A call with no arguments does what the Design "
    "tab's menu item does, as a person's pick does.";

// The mode an entry with a handler runs in, as the Design tab's right-click menu offers it (offered_in_mode()).
// A sketch being constrained counts as open, and refuses sketch_finish and sketch_cancel: the tab's own Confirm
// and Cancel buttons end it. A call that takes no arguments runs only there; `no_arguments` words it for the call
// with no arguments of a command that also runs from arguments, whose argument form may run in the other mode
// (sketch_text, sketch_project).
std::string mode_note(const DesignTool& t, bool no_arguments = false)
{
    return std::string(no_arguments ? "Called with no arguments, it runs only " : "It runs only ") +
           (t.sketch_mode ? "in an open sketch (sketch_create opens one)."
                          : "outside a sketch (sketch_finish or sketch_cancel ends one; while the tab constrains a sketch, "
                            "its Confirm or Cancel button ends that).");
}

// cad_note()'s text, given while the feature is off and while it is on but was off at startup. The
// catalog holds the Design commands only when the feature was on at startup (the preference, or the command
// line's --cad), and the main window creates the Design page only then too (its panel is built on first use),
// so turning the feature on takes a restart.
const char* const kCadOffNote = "tool_call runs no Design command now: they need the CAD feature on when OrcaSlicer "
                                "started, in Preferences > Features or with --cad. A person turns it on there if it is "
                                "off and restarts OrcaSlicer, or starts it with --cad.";

// Why a catalog built without the Design commands leaves them out, after "Unknown tool: <key>; " or an action's
// "<key> is a Design tab action, and " (unknown_action()): in the editor the CAD feature was off at startup; the
// G-code viewer has no Design tab whatever the preference says.
const char* const kCadOffAtStartup = "the CAD feature was off when OrcaSlicer started: a person turns it on in "
                                     "Preferences > Features and restarts OrcaSlicer, or starts it with --cad";
const char* const kNoDesignInViewer = "the G-code viewer has no Design tab: a person opens the project in OrcaSlicer's editor";
// cad_gate()'s refusal while the CAD feature is off in a run that started with it on.
const char* const kCadOffNow = "The CAD feature is off; enable it in Preferences > Features and restart OrcaSlicer";

// The details of a ToolFailure a silent form passes on, with invalid_argument as their code where they name no cause,
// as a plain exception a silent form throws reads (run_guarded(), add_feature()).
json with_code(json details)
{
    if (!details.is_object())
        details = json::object();
    if (!details.contains("code"))
        details["code"] = "invalid_argument";
    return details;
}

// What describe_design and select_geometry say is selected. The selection: what the right-click menu would be built
// for, its kind (an OfferSel) and its id in the atlas, which the entries' accepts name.
json selection_json(const DesignPanel& panel)
{
    const int kind = panel.mcp_offer_selection_kind();
    return json{{"kind", kind}, {"id", kDesignSelectionIds[kind]}};
}

// The reference plane picked in the view, which the next Sketch opens on: "XY", "XZ", "YZ" or null.
json picked_plane_json(const DesignPanel& panel)
{
    static const char* const names[] = {"XY", "XZ", "YZ"};
    const int plane = panel.mcp_picked_plane();
    return plane >= 0 ? json(names[plane]) : json();
}

// The selected row of the Feature list, a feature index, which the menu items that act on it take while nothing is picked
// in the view; or null.
json feature_row_json(const DesignPanel& panel)
{
    const int row = panel.mcp_feature_row();
    return row >= 0 ? json(row) : json();
}

// Where body `b` stands for Commit to Plate and Export STEP (body_place, the Move gizmo, Place on Face): null where the
// model has it, else three rows of four numbers, the rotation and the translation in mm, in the world frame.
json placement_json(const DesignPanel& panel, int b)
{
    const Transform3d t = panel.mcp_body_placement(b);
    if (t.isApprox(Transform3d::Identity())) return json();
    json rows = json::array();
    for (int r = 0; r < 3; ++r)
        rows.push_back(json::array({t(r, 0), t(r, 1), t(r, 2), t(r, 3)}));
    return rows;
}

json describe_design(DesignPanel* panel)
{
    CadDocument& doc = panel->mcp_doc();

    json features = json::array();
    for (size_t i = 0; i < doc.features.size(); ++i) {
        const CadFeature& f = doc.features[i];
        json jf{{"index", int(i)}, {"type", feature_type_name(f.type)}, {"name", f.name}, {"suppressed", !f.enabled}};
        // The fields an expression drives: feature_edit refuses them, feature_describe has the expressions.
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
               // Where it stands (body_place, the Move gizmo, Place on Face), which bbox, the recipe's body, does not
               // include.
               {"moved", panel->mcp_body_moved(int(i))},
               {"placement", placement_json(*panel, int(i))}};
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
        // is refused if the topology has moved on. See the stale-id guard in cad_gate().
        {"generation", doc.topo_generation},
        {"undo_depth", doc.undo_depth()},
        {"redo_depth", doc.redo_depth()},
        // Whether sketch endpoints that nearly meet are welded: the document's rule, which changes
        // the loops sketch_draw's entities make.
        {"auto_close_loops", doc.auto_close_loops},
    };
    out["selection"]    = selection_json(*panel);
    out["picked_plane"] = picked_plane_json(*panel);
    out["feature_row"]  = feature_row_json(*panel);
    // The Commit button's mode, which commit_to_plate uses when given none.
    out["commit_mode"] = panel->commit_mode() == DesignPanel::CommitMode::Bodies ? "bodies" : "assembly";
    if (!var_error.empty()) out["variables_error"] = var_error;
    // The aligns (Mate features) that cannot hold with those before them.
    if (!doc.mate_conflicts.empty()) {
        json conflicts = json::array();
        for (const auto& [feature, reason] : doc.mate_conflicts)
            conflicts.push_back(json{{"feature", feature}, {"reason", reason}});
        out["align_conflicts"] = std::move(conflicts);
    }
    return out;
}

// --- Measure layer (read-only "evidence" half of the RE loop) ------------------
inline json vec3(const Vec3d& v) { return json::array({v.x(), v.y(), v.z()}); }

// Shared Build helpers.

// The kinds body_describe gives faces, in GeomAbs_SurfaceType order, and edges.
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
// failed call can be left out of an exported replay. The error's code tells the causes apart, as
// run_guarded() does: rebuild_failed for a rebuild that reports failure, kernel_failure, precondition or
// invalid_argument, or the code of a ToolFailure `add` throws.
// `slow` rebuilds as the cards do, off the UI thread behind the progress dialog (recompute_guarded),
// for features whose helical pipes and skins can take seconds: the window keeps painting and
// inspection calls keep answering meanwhile, while the rebuild refuses every Design call.
json add_feature(DesignPanel* panel, const char* index_key, const std::function<int()>& add, bool slow = false)
{
    CadDocument& doc = panel->mcp_doc();
    doc.checkpoint();
    int         idx = -1;
    std::string why;
    json        details = {{"code", "rebuild_failed"}};
    try {
        idx = add();
        if (!(slow ? panel->recompute_guarded(_L("Rebuilding model…")) : doc.recompute()))
            why = doc.error.empty() ? "the model could not be rebuilt" : doc.error;
    } catch (const ToolFailure& ex) {   // a refusal with its own code, such as no_design_view()
        why     = ex.what();
        details = with_code(ex.details);
    } catch (const Standard_Failure& ex) {   // derives from std::exception, so it comes before it
        why     = std::string("OCCT: ") + (*ex.what() ? ex.what() : "failure");
        details = {{"code", "kernel_failure"}};
    } catch (const CadKernelFailure& ex) {
        why     = std::string("OCCT: ") + ex.what();
        details = {{"code", "kernel_failure"}};
    } catch (const CadPrecondition& ex) {
        why     = ex.what();
        details = {{"code", "precondition"}};
    } catch (const std::exception& ex) {
        why     = *ex.what() ? ex.what() : "the feature could not be added";
        details = {{"code", "invalid_argument"}};
    }
    if (!why.empty()) {
        doc.rollback();
        doc.error = why;   // the rollback's rebuild cleared it; the status line reads it
        panel->mcp_after_change(false);
        throw ToolFailure(why, std::move(details));
    }
    panel->mcp_after_change();
    return json{{"ok", true}, {index_key, idx}, {"bodies", int(doc.bodies.size())}};
}

// What a feature's result does to the bodies already there, in BooleanMode order: the commands' `operation`, in the
// words of Fusion's dialogs and the Extrude card.
const char* const kOperations[] = {"new_body", "join", "cut", "intersect"};

// The operations each command takes (operation_arg()): extrude's, auto among them; revolve's, sweep's and loft's;
// boolean's, which combines two bodies there and makes none; mirror's, which keeps its image apart or joins it.
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

// Resolve an optional explicit body target. Default (no `body`, or <0) = last body, which is
// what the kernel picks anyway. Validated BEFORE any checkpoint so a bad index throws clean.
int target_body_arg(const json& params, const CadDocument& doc)
{
    int bi = params.value("body", -1);
    if (bi >= int(doc.bodies.size()))
        throw std::runtime_error("body index out of range (have " + std::to_string(doc.bodies.size()) + ")");
    return bi;   // <0 -> kernel uses the last body
}

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
    CadDocument& doc = panel->mcp_doc();
    const auto   add = cad_surface_revolve(doc, params);
    doc.checkpoint();
    int idx = add(panel->mcp_feature_name(_L("Surface Revolve")));
    bool ok = doc.recompute();
    if (!ok) { const std::string why = doc.error; doc.rollback(); doc.error = why; }
    panel->mcp_after_change(ok);
    return json{{"ok", ok}, {"feature_index", idx}, {"bodies", int(doc.bodies.size())}, {"error", doc.error}};
}

json action_surface_offset(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no sheet body to offset");
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

json action_surface_patch(DesignPanel* panel, const json& params)
{
    if (!params.contains("sketch")) throw std::runtime_error("surface_patch needs 'sketch' (feature index)");
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

// --- A feature as its card shows it: feature_describe and feature_edit ------------------------

// Names in the Design commands' vocabulary, in enum order (BooleanMode's are kOperations).
const char* const kShapes[]         = {"rectangle", "circle"};                     // SketchShape
const char* const kExtrudeEnds[]    = {"blind", "symmetric", "two_sided", "through_all", "up_to_face", "up_to_vertex"};
const char* const kFaceGroups[]     = {"top", "bottom", "lateral", "all"};
const char* const kPlaneTypes[]     = {"offset", "angle", "midplane", "tangent", "two_edges", "coincident"};
const char* const kAxisTypes[]      = {"two_points", "face_normal", "cylinder", "plane_intersection", "along_edge"};
const char* const kCoordSysTypes[]  = {"point_world", "face_and_direction"};
const char* const kHoleTypes[]      = {"simple", "counterbore", "countersink"};    // CadFeature::hole_style
const char* const kAlignTypes[]     = {"fastened", "planar", "revolute", "slider", "cylindrical"};   // CadFeature::mate_kind
const char* const kPatternTypes[]   = {"linear", "circular", "curve"};             // a Pattern's layout
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

// The edge group a word names, the Fillet and Chamfer cards' Edges choice, or nothing for any other value.
std::optional<FaceGroup> face_group_named(const json& v)
{
    for (size_t i = 0; i < std::size(kFaceGroups); ++i)
        if (v.is_string() && v.get<std::string>() == kFaceGroups[i]) return FaceGroup(i);
    return std::nullopt;
}
const std::string kFaceGroupWords = "top, bottom, lateral or all";

// A constraint's value as the Design commands state it: an angle in degrees (the recipe holds radians), a
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
// writes, named as the CadFeature members (the names feature_set_expression binds), but for the
// members that hold a variant, read and written under their command's argument in its words: mode
// as `operation` (kOperations), and hole_style, mate_kind and pattern_circular as `type`; `ranges` the limits
// its spin controls set on the numeric ones; `refs` what it refers to (feature and body indices, face
// and edge ids, read in the model before it but for a datum's), which feature_edit's refs re-points
// (cad_apply_feature_refs()); `expr` the bindings.
// feature_describe shows it and feature_edit checks against it, so the two cannot disagree.
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
            // Text or SVG art, vectorised once: its placement edits, and a Text feature's words, font and height,
            // which feature_edit outlines again.
            if (f.is_text()) {
                edit("text_string", f.text_string);
                edit("text_font", f.text_font);
                edit("text_height", f.text_height);
            }
            show("regions", int(f.imported_regions.size()));
            // Per region, its closed loops and the area it fills as placed, in mm². A region keeps its
            // outer loop first and its holes after it, so it has 1 + holes loops, and it fills the
            // outer loop's area less its holes', times the placement's scale. A glyph or path that
            // vectorised into a sliver or with a stray hole shows here, where the region count alone
            // does not.
            json loops = json::array(), areas = json::array();
            for (const auto& region : f.imported_regions) {
                loops.push_back(int(region.size()));
                double filled = 0.0;
                for (size_t c = 0; c < region.size(); ++c) {
                    const std::vector<Vec2d>& loop  = region[c];
                    double                    twice = 0.0;   // the shoelace sum: twice the signed area
                    for (size_t i = 0; i < loop.size(); ++i) {
                        const Vec2d& a = loop[i];
                        const Vec2d& b = loop[(i + 1) % loop.size()];
                        twice += a.x() * b.y() - b.x() * a.y();
                    }
                    filled += (c == 0 ? 0.5 : -0.5) * std::abs(twice);
                }
                areas.push_back(filled * std::abs(f.import_scale_x * f.import_scale_y));
            }
            show("region_loops", std::move(loops));
            show("region_areas", std::move(areas));
            edit("import_offset", json::array({f.import_offset.x(), f.import_offset.y()}));
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
        edit("operation", enum_name(kOperations, int(f.mode)));
        ref("sketch_ref", f.sketch_ref);
        ref("target_body", f.target_body);
        ref("up_to_face", f.up_to_face);
        ref("extrude_src_face", f.extrude_src_face);
        break;
    case T::Revolve:
        num("revolve_angle", f.revolve_angle, 1, 360);
        edit("revolve_axis", f.revolve_axis);
        edit("flip", f.flip);
        edit("operation", enum_name(kOperations, int(f.mode)));
        ref("sketch_ref", f.sketch_ref);
        ref("revolve_axis_entity", f.revolve_axis_entity);
        ref("target_body", f.target_body);
        break;
    case T::Sweep:
        edit("operation", enum_name(kOperations, int(f.mode)));
        ref("sketch_ref", f.sketch_ref);
        ref("sweep_path_ref", f.sweep_path_ref);
        ref("target_body", f.target_body);
        break;
    case T::Loft:
        edit("operation", enum_name(kOperations, int(f.mode)));
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
        // The seat, which the card does not show; hole's type and designation set it.
        edit("type", enum_name(kHoleTypes, f.hole_style));
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
        // A pattern along a curve follows its sketch's entity, which no field changes.
        if (f.pattern_curve_sketch >= 0)
            show("type", enum_name(kPatternTypes, 2));
        else
            edit("type", enum_name(kPatternTypes, f.pattern_circular ? 1 : 0));
        num("pattern_count", f.pattern_count, 1, 999);
        num("pattern_spacing", f.pattern_spacing, 0.01, 100000);
        edit("pattern_dir", f.pattern_dir);
        num("pattern_angle", f.pattern_angle, 1, 360);
        ref("pattern_curve_sketch", f.pattern_curve_sketch);
        ref("pattern_curve_entity", f.pattern_curve_entity);
        ref("target_body", f.target_body);
        break;
    case T::Boolean:
        edit("operation", enum_name(kOperations, int(f.mode)));
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
        edit("operation", enum_name(kOperations, int(f.mode)));
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
        edit("revolve_axis", f.revolve_axis);
        edit("flip", f.flip);
        ref("sketch_ref", f.sketch_ref);
        ref("revolve_axis_entity", f.revolve_axis_entity);
        break;
    case T::SurfaceOffset:
        num("plane_offset", f.plane_offset, -100000, 100000);
        ref("target_body", f.target_body);
        break;
    case T::Mate:
        edit("type", enum_name(kAlignTypes, f.mate_kind));
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
    if (k == "pattern_dir")          return choice(f.pattern_dir, 1);
    if (k == "revolve_axis") {
        // Choosing the plane's X or Y axis on the card drops a sketch line chosen as the axis.
        choice(f.revolve_axis, 1);
        f.revolve_axis_entity = -1;
        return;
    }
    if (k == "xf_translate") return point3(f.xf_translate, false);
    if (k == "xf_pivot")     return point3(f.xf_pivot, false);
    if (k == "xf_axis")      return point3(f.xf_axis, true);
    if (k == "import_offset") {
        if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
            throw std::runtime_error(k + " must be [x, y]");
        const Vec2d p(v[0].get<double>(), v[1].get<double>());
        if (!(p.cwiseAbs().maxCoeff() <= 100000)) throw std::runtime_error(k + " must be between -100000 and 100000 on each axis");
        f.import_offset = p;
        return;
    }
    // A Text feature's words, height and font, as sketch_text takes them. Written here only: the outline is the
    // panel's (DesignPanel::mcp_outline_text()), which feature_edit runs on them.
    if (k == "text_string") {
        if (!v.is_string() || v.get<std::string>().empty()) throw std::runtime_error("text_string must be the words to outline");
        f.text_string = v.get<std::string>();
        return;
    }
    if (k == "text_height") {
        if (!v.is_number() || !(v.get<double>() >= 0.5 && v.get<double>() <= 500.0))
            throw std::runtime_error("text_height must be 0.5 to 500 mm");
        f.text_height = v.get<double>();
        return;
    }
    if (k == "text_font") {
        if (!v.is_string() || v.get<std::string>().empty())
            throw std::runtime_error("text_font must be a font's face name, or the stored value feature_describe reports");
        f.text_font = v.get<std::string>();
        return;
    }
    if (k == "shape") {
        f.shape = SketchShape(enum_index(k, v, kShapes));
        return;
    }
    if (k == "type") {
        // A hole's seat, an align's kind and a pattern's layout, in their command's words.
        if (f.type == CadFeatureType::Hole) {
            f.hole_style = enum_index(k, v, kHoleTypes);
            return;
        }
        if (f.type == CadFeatureType::Mate) {
            f.mate_kind = enum_index(k, v, kAlignTypes);
            return;
        }
        if (f.type == CadFeatureType::Pattern) {
            const int layout = enum_index(k, v, kPatternTypes);
            if (layout == 2)
                throw std::runtime_error("a pattern's type is linear or circular; a curve pattern is made with pattern {type: \"curve\"}");
            f.pattern_circular = layout == 1;
            return;
        }
    }
    if (k == "operation") {
        // The member mode, in the words the feature's card offers: a Boolean combines two bodies, so it makes no new
        // one; a Mirror adds its mirrored body or joins it back.
        const BooleanMode m = BooleanMode(enum_index(k, v, kOperations));
        if (f.type == CadFeatureType::Boolean && m == BooleanMode::New)
            throw std::runtime_error("a Boolean's operation is join, cut or intersect");
        if (f.type == CadFeatureType::Mirror && m != BooleanMode::New && m != BooleanMode::Add)
            throw std::runtime_error("a Mirror's operation is new_body or join");
        f.mode = m;
        return;
    }
    if (k == "extrude_end") {
        // The face or vertex an end goes up to is picked on the canvas; feature_edit keeps the
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

// The `feature` param (or `index`, as agents often call it): an index into describe_design's features.
int feature_arg(const json& params, const CadDocument& doc)
{
    const json v = !params.is_object() ? json() : params.contains("feature") ? params["feature"] : params.value("index", json());
    if (!v.is_number_integer()) throw std::runtime_error("feature must be a feature index (describe_design's features[].index)");
    check_feature_index(v.get<int>(), doc);
    return v.get<int>();
}

json action_feature_describe(DesignPanel* panel, const json& params)
{
    const CadDocument& doc = panel->mcp_doc();
    return cad_feature_json(doc, feature_arg(params, doc));
}

// A card's Confirm in edit mode (DesignPanel::confirm_tool): `fields`, then `refs`, written to a copy of the feature,
// a Text feature outlined again when its words, height or font change, then one undo step and replace_feature(),
// which rebuilds and puts the old features back if the rebuild fails. Every argument is checked before anything
// changes.
json action_feature_edit(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const int    fi  = feature_arg(params, doc);
    // The card's values go in fields; params and values are what agents often call them.
    const char* key    = params.contains("fields") ? "fields" : params.contains("params") ? "params" : params.contains("values") ? "values" : nullptr;
    json        fields = key != nullptr ? params[key] : json::object();
    const json  refs   = params.value("refs", json::object());
    if (!fields.is_object()) throw std::runtime_error("fields must be an object, as {\"distance\": 15}");
    if (!refs.is_object()) throw std::runtime_error("refs must be an object, as {\"sweep_path_ref\": 2}");
    if (fields.empty() && refs.empty())
        throw std::runtime_error("feature_edit needs fields or refs, as {\"fields\": {\"distance\": 15}} or {\"refs\": {\"sweep_path_ref\": 2}}");
    const CadFeature was    = doc.features[fi];
    CadFeature       edited = was;
    // A Text feature's Bold and Italic go with a face name in text_font, as the Text dialog's boxes go with its Font
    // list: they are no field of the feature, whose stored font holds them.
    bool bold = false, italic = false, styled = false;
    if (was.is_text())
        for (const auto& [k, flag] : {std::pair<const char*, bool*>{"text_bold", &bold}, {"text_italic", &italic}}) {
            if (!fields.contains(k)) continue;
            if (!fields[k].is_boolean()) throw std::runtime_error(std::string(k) + " must be true or false");
            *flag  = fields[k].get<bool>();
            styled = true;
            fields.erase(k);
        }
    if (!fields.empty()) cad_apply_feature_fields(edited, fields);
    if (was.is_text() && (styled || fields.contains("text_string") || fields.contains("text_height") || fields.contains("text_font"))) {
        // A text_font that is the stored value keeps the feature's font; any other names a face.
        const std::string face = fields.contains("text_font") && edited.text_font != was.text_font ? edited.text_font : std::string();
        if (styled && face.empty()) throw std::runtime_error("text_bold and text_italic go with text_font given as a face name");
        edited.text_font = was.text_font;
        std::string why;
        if (!panel->mcp_outline_text(edited, edited.text_string, face, bold, italic, edited.text_height, why))
            throw std::runtime_error(why);
    }
    if (!refs.empty()) cad_apply_feature_refs(edited, refs, cad_document_before(doc, fi), doc, fi);
    const json described = cad_feature_json(doc, fi);
    doc.checkpoint();
    if (!doc.replace_feature(fi, edited)) {
        // The features are back already; rollback() also takes back the undo step and restores
        // the redo branch, so the refused call leaves the history as it found it.
        const std::string why = doc.error.empty() ? "the model could not be rebuilt" : doc.error;
        doc.rollback();
        doc.error = why;
        panel->mcp_after_change(false);
        throw ToolFailure(why, {{"code", "rebuild_failed"}});
    }
    panel->mcp_after_change();
    return json{{"ok", true},
                {"feature", fi},
                {"changed", cad_feature_changes(described, cad_feature_json(doc, fi))},
                {"bodies", int(doc.bodies.size())},
                {"generation", doc.topo_generation},
                {"error", doc.error}};
}

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

// The `sketch` a Design command reads or builds from: a Sketch feature, or a Project feature, whose
// entities the kernel builds as a sketch's.
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

// --- Solids from a profile: extrude and revolve ------------------------------------------------
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

int revolve_axis_arg(const json& params)
{
    const int axis = params.value("axis", 0);
    if (axis != 0 && axis != 1) throw std::runtime_error("axis must be 0 (plane X) or 1 (plane Y)");
    return axis;
}

// Whether entity `e` of `ents`, a sketch's entities, is a line: what a revolve turns about and a rib grows from.
bool revolve_line(const std::vector<SketchEntity>& ents, int e)
{
    return e >= 0 && e < int(ents.size()) && ents[size_t(e)].type == SketchEntity::Type::Line;
}

// The line of sketch `si` a revolve of it turns about, or -1 for the plane axis: params.axis_entity; without one, the
// sketch's only centerline, as a fresh card offers it, unless the caller chose a plane axis.
int revolve_axis_entity_arg(const json& params, const CadDocument& doc, int si)
{
    const json ae = params.value("axis_entity", json());
    if (ae.is_null()) return params.contains("axis") ? -1 : DesignPanel::default_revolve_axis_entity(doc.features[si].entities);
    if (!ae.is_number_integer()) throw std::runtime_error("axis_entity must be an entity index of the sketch, or -1 for the plane axis");
    if (ae.get<int>() != -1 && !revolve_line(doc.features[si].entities, ae.get<int>()))
        throw std::runtime_error("axis_entity " + std::to_string(ae.get<int>()) + " is not a line of sketch " + std::to_string(si));
    return ae.get<int>();
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

json action_transform(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    if (doc.bodies.empty()) throw CadPrecondition("no body to transform");
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

json action_delete_face(DesignPanel* panel, const json& params)
{
    if (!params.contains("faces")) throw std::runtime_error("delete_face needs 'faces' (array of face ids)");
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

json action_feature_set_expression(DesignPanel* panel, const json& params)
{
    CadDocument& doc = panel->mcp_doc();
    const int    fi  = feature_arg(params, doc);
    if (!params.contains("field")) throw std::runtime_error("feature_set_expression needs 'field' name");
    if (!params.contains("expr"))  throw std::runtime_error("feature_set_expression needs 'expr' string");
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
// sketch's own, which takes off or puts back the last drawn shape. The project's history is undo's
// with history "project".
json undo_redo(DesignPanel* panel, bool redo)
{
    DesignCanvas* vp     = panel->mcp_viewport();
    const bool    sketch = vp != nullptr && vp->is_sketching();
    if (!panel->menu_can_undo_redo(redo)) {
        if (sketch) throw CadPrecondition(redo ? "Nothing to redo in the open sketch" : "Nothing to undo in the open sketch");
        throw CadPrecondition(redo ? "Nothing to redo in the Design history (history \"project\" redoes the project's)"
                                      : "Nothing to undo in the Design history (history \"project\" undoes the project's)");
    }
    panel->menu_undo_redo(redo);
    if (sketch) return json{{"scope", "sketch"}, {"entities", vp->mcp_sketch_tool().entities().size()}};
    const CadDocument& doc = panel->mcp_doc();
    return json{{"scope", "design"},
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
    throw ToolFailure(doc.error.empty() ? "the Design tab refused the edit" : doc.error, {{"code", "rebuild_failed"}});
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

// The `body` param: an index into describe_design's bodies.
int body_arg(const json& params, const CadDocument& doc)
{
    const json v = param(params, "body");
    if (!v.is_number_integer()) throw std::runtime_error("body must be a body index (describe_design's bodies[].index)");
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
        throw CadPrecondition("feature " + std::to_string(fi) + " is already " + (to < 0 ? "first" : "last"));
    if (!panel->move_feature(fi, delta.get<int>())) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"feature", to}, {"from", fi}});
}

// The row's eye: it shows a feature that is not suppressed and hides one that is.
json action_feature_suppress(DesignPanel* panel, const json& params)
{
    const CadDocument& doc        = panel->mcp_doc();
    const int          fi         = feature_arg(params, doc);
    const json         suppressed = param(params, "suppressed");
    if (!suppressed.is_boolean()) throw std::runtime_error("suppressed must be true or false");
    const bool shown   = !suppressed.get<bool>();
    const bool changed = doc.features[fi].enabled != shown;
    if (!panel->show_feature(fi, shown)) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"feature", fi}, {"suppressed", suppressed}, {"changed", changed}});
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
        throw CadPrecondition("body " + std::to_string(b) + " has no feature to delete; use new_design to start over");
    const json   deleted{{"index", src}, {"type", feature_type_name(doc.features[src].type)}, {"name", doc.features[src].name}};
    const size_t before = doc.features.size();
    if (!panel->delete_body(b)) tree_edit_refused(doc);
    return tree_edit_reply(doc, {{"deleted_feature", deleted}, {"removed", before - doc.features.size()}});
}

// The Bodies row's Move and Place on Face from arguments: where a body stands, outside the history. Every argument is
// read before the body moves.
json action_body_place(DesignPanel* panel, const json& params)
{
    const int  b      = body_arg(params, panel->mcp_doc());
    const json move   = param(params, "move");
    const json rotate = param(params, "rotate");
    const json face   = param(params, "face");
    const json reset  = param(params, "reset");
    if (!reset.is_null() && !reset.is_boolean()) throw std::runtime_error("reset must be true or false");
    const bool resets = reset.is_boolean() && reset.get<bool>();
    const int  forms  = int(!move.is_null() || !rotate.is_null()) + int(!face.is_null()) + int(resets);
    if (forms == 0) throw std::runtime_error("body_place takes move and rotate, face, or reset: true");
    if (forms > 1) throw std::runtime_error("move and rotate, face and reset are three forms: give one of them");

    std::string why;
    if (!face.is_null()) {
        if (!face.is_number_integer() || face.get<int>() < 0) throw std::runtime_error("face must be a face id (body_describe)");
        if (!panel->mcp_place_body_on_face(b, face.get<int>(), why)) throw std::runtime_error(why);
    } else if (resets) {
        panel->mcp_reset_body_placement(b);
    } else {
        Vec3d d = Vec3d::Zero();
        if (!move.is_null()) {
            if (!move.is_array() || move.size() != 3 || !std::all_of(move.begin(), move.end(), [](const json& v) { return v.is_number(); }))
                throw std::runtime_error("move must be [dx, dy, dz] in mm");
            d = Vec3d(move[0].get<double>(), move[1].get<double>(), move[2].get<double>());
        }
        int    axis  = -1;
        double angle = 0.0;
        if (!rotate.is_null()) {
            static const std::map<std::string, int> axes{{"x", 0}, {"y", 1}, {"z", 2}};
            const json a = param(rotate, "axis");
            const json g = param(rotate, "angle");
            const auto named = a.is_string() ? axes.find(a.get<std::string>()) : axes.end();
            if (!rotate.is_object() || named == axes.end() || !g.is_number() || rotate.size() != 2)
                throw std::runtime_error("rotate must be {\"axis\": \"x\", \"y\" or \"z\", \"angle\": degrees}");
            axis  = named->second;
            angle = g.get<double>();
        }
        if (!panel->mcp_place_body(b, d, axis, angle, why)) throw std::runtime_error(why);
    }
    return json{{"ok", true}, {"body", b}, {"moved", panel->mcp_body_moved(b)}, {"placement", placement_json(*panel, b)}};
}

// --- Hand-off: Commit to Plate, New Design, Export STEP ----------------------------------

// Commit to Plate as the split button does, in `mode` (by default the button's, which this leaves
// alone), then delete the `replace` objects. With `replace` the deletions and the commit are one
// Prepare undo step: the commit's own snapshot is suppressed under the outer one.
json action_commit_to_plate(DesignPanel* panel, const json& params)
{
    using Mode = DesignPanel::CommitMode;
    Mode mode = panel->commit_mode();
    const json m = params.value("mode", json(""));
    if (m == "assembly") mode = Mode::Assembly;
    else if (m == "bodies") mode = Mode::Bodies;
    else if (m != "") throw std::runtime_error("mode must be \"assembly\" or \"bodies\"");

    Plater*     plater   = wxGetApp().plater();
    ObjectList* obj_list = wxGetApp().obj_list();
    if (plater == nullptr || obj_list == nullptr) throw CadPrecondition("Prepare is not ready");
    Model& model = plater->model();
    const std::vector<int> replace = cad_commit_to_plate_replace(params.value("replace", json::array()), model);
    // The Delete key does not pull an object from under an open gizmo either.
    if (!replace.empty())
        for (GLCanvas3D* canvas : {plater->get_view3D_canvas3D(), plater->get_assmeble_canvas3D()})
            if (canvas != nullptr && canvas->get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined)
                throw CadPrecondition("close the " + Mcp::gizmo_name(canvas->get_gizmos_manager().get_current_type()) +
                                         " gizmo first: replace deletes objects");

    // commit_to_plate()'s own refusals, checked first so a refused call leaves no undo step.
    const CadDocument& doc = panel->mcp_doc();
    if (doc.display_mesh.its.indices.empty())
        throw CadPrecondition("Nothing to commit: the design has no body; add a feature first");
    bool any_shown = doc.display_body_meshes.size() <= 1;   // a lone body is sent even when hidden
    for (size_t b = 0; !any_shown && b < doc.display_body_meshes.size(); ++b)
        any_shown = panel->mcp_body_shown(int(b)) && !doc.display_body_meshes[b].its.indices.empty();
    if (!any_shown) throw CadPrecondition("Every body is hidden; show one first with body_set {body, visible: true}");

    json replaced = json::array();
    for (int i : replace) replaced.push_back(json{{"index", i}, {"name", model.objects[i]->name}});

    std::vector<int>                sent;
    std::vector<const ModelObject*> made;
    {
        std::optional<Plater::TakeSnapshot> snapshot;
        if (!replace.empty()) snapshot.emplace(plater, _u8L("Commit to Plate"));
        for (size_t i : panel->commit_to_plate(mode, &sent)) made.push_back(model.objects[i]);
        if (made.empty()) throw CadPrecondition("Commit to Plate sent nothing");
        std::vector<ItemForDelete> items;
        for (int i : replace) items.emplace_back(itObject, i, -1);
        obj_list->delete_from_model_and_list(items);
    }

    // The new objects' indices once the replaced ones are gone.
    json objects = json::array();
    for (const ModelObject* o : made) {
        const auto it = std::find(model.objects.begin(), model.objects.end(), o);
        objects.push_back(json{{"index", int(it - model.objects.begin())}, {"name", o->name}, {"parts", o->volumes.size()}});
    }
    json hidden = json::array();
    for (int b = 0; b < int(doc.bodies.size()); ++b)
        if (std::find(sent.begin(), sent.end(), b) == sent.end() && !panel->mcp_body_shown(b)) hidden.push_back(b);
    return json{{"ok", true}, {"mode", mode == Mode::Bodies ? "bodies" : "assembly"}, {"objects", std::move(objects)},
                {"bodies_sent", sent}, {"bodies_hidden", std::move(hidden)}, {"replaced", std::move(replaced)},
                {"tab", "prepare"}};
}

// New Design without its confirmation dialog: discard_changes confirms, as project_new's does.
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
    if (doc.bodies.empty()) throw CadPrecondition("Nothing to export: the design has no body; add a feature first");
    const json path_arg = params.value("path", json());
    const json overwrite = params.value("overwrite", json(false));
    if (!path_arg.is_string()) throw std::runtime_error("export_step needs 'path', an absolute file path");
    if (!overwrite.is_boolean()) throw std::runtime_error("overwrite must be true or false");
    std::string err;
    const auto path = Mcp::resolve_output_path(path_arg.get<std::string>(), overwrite.get<bool>(), err);
    if (!path) throw std::runtime_error(err);
    const std::string path_utf8 = Mcp::path_to_utf8(*path);
    if (!panel->export_step_to(path_utf8, err)) throw ToolFailure("STEP export failed: " + err, {{"code", "export_failed"}});

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

// --- Text and SVG art ------------------------------------------------------------------

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

// select_geometry: what a click selects in the model at rest, by id (DesignPanel::mcp_select()). An argument given as
// null is left out, as its null default says. The reply is what describe_design reports of the selection.
json action_select_geometry(DesignPanel* panel, const json& params)
{
    using Kind = DesignSelection::Kind;
    std::set<std::string> named;
    for (const char* key : {"body", "face", "edges", "vertex", "plane", "sketch", "loop", "feature"})
        if (!param(params, key).is_null()) named.insert(key);
    const auto is_form = [&named](std::initializer_list<const char*> form) {
        return named == std::set<std::string>(form.begin(), form.end());
    };
    const auto index = [&params](const char* key) { return params.at(key).get<int>(); };
    DesignSelection s;
    if (named.empty()) {
        s.kind = Kind::None;
    } else if (is_form({"body"})) {
        s.kind = Kind::Body;
    } else if (is_form({"body", "face"})) {
        s.kind = Kind::Face;
        s.face = index("face");
    } else if (is_form({"body", "edges"})) {
        s.kind = Kind::Edges;
        for (const json& e : params.at("edges")) {
            if (!e.is_number_integer()) throw std::runtime_error("edges is a list of edge ids");
            s.edges.push_back(e.get<int>());
        }
    } else if (is_form({"body", "vertex"})) {
        s.kind   = Kind::Vertex;
        s.vertex = index("vertex");
    } else if (is_form({"plane"})) {
        const std::string plane = params.at("plane").get<std::string>();
        s.kind                  = Kind::Plane;
        s.plane                 = plane == "XY" ? 0 : plane == "XZ" ? 1 : plane == "YZ" ? 2 : -1;
    } else if (is_form({"sketch", "loop"})) {
        s.kind   = Kind::Loop;
        s.sketch = index("sketch");
        s.loop   = index("loop");
    } else if (is_form({"feature"})) {
        s.kind    = Kind::Feature;
        s.feature = index("feature");
    } else {
        throw std::runtime_error("select_geometry takes one of: body; body with face, edges or vertex; plane; sketch with "
                                 "loop; feature; or nothing, which clears the selection");
    }
    if (named.count("body") != 0) s.body = index("body");
    std::string why;
    if (!panel->mcp_select(s, why)) throw std::runtime_error(why);
    json r{{"ok", true},
           {"selection", selection_json(*panel)},
           {"plane", picked_plane_json(*panel)},
           {"feature", feature_row_json(*panel)}};
    if (s.kind == Kind::Vertex) {
        r["point"]        = vec3(cad_body_vertices(panel->mcp_doc(), s.body).at(size_t(s.vertex)));
        r["placed_point"] = vec3(panel->mcp_viewport()->selected_vertex_point());
    }
    return r;
}

// The tab's Confirm on what is open, as Enter presses it, or the Text dialog's OK.
json action_confirm_tool(DesignPanel* panel, const json&)
{
    DesignPanel::ConfirmOutcome out;
    std::string                 why;
    if (!panel->mcp_confirm_tool(out, why)) throw CadPrecondition(why);
    return json{{"was", out.was},
                {"ok", out.ok},
                {"feature", out.feature >= 0 ? json(out.feature) : json()},
                {"open", out.open.empty() ? json() : json(out.open)},
                {"status", into_u8(panel->mcp_status())}};
}

// The tab's Cancel on what is open, with no dialog, or the Text dialog's Cancel.
json action_cancel_tool(DesignPanel* panel, const json&)
{
    std::string was, why;
    if (!panel->mcp_cancel_tool(was, why)) throw CadPrecondition(why);
    return json{{"was", was}, {"status", into_u8(panel->mcp_status())}};
}

// --- dispatcher ---------------------------------------------------------------------

} // namespace

// Every silent form, by the id of the Design table's entry whose tool block describes it: run_cad_tool() and
// register_cad_commands() read this, and the block gives its access.
const std::pair<const char*, CadRun> kSilentForms[] = {
    {"describe_design",              [](DesignPanel* panel, const json&) { return describe_design(panel); }},
    {"select_geometry",              action_select_geometry},
    {"confirm_tool",                 action_confirm_tool},
    {"cancel_tool",                  action_cancel_tool},
    {"feature_describe",             action_feature_describe},
    {"body_describe",                body_describe},
    {"measure",                      measure},
    {"body_mass_properties",         body_mass_properties},
    {"body_section",                 body_section},
    {"body_compare",                 body_compare},
    {"interference",                 action_check_interference},
    {"sketch_describe",              action_sketch_describe},
    {"sketch_validate",              action_sketch_validate},
    {"sketch_list_constraints",      action_sketch_list_constraints},
    {"import_step",                  import_step},
    {"import_mesh",                  import_mesh},
    {"extrude",                      action_extrude},
    {"revolve",                      action_revolve},
    {"fillet",                       action_fillet},
    {"chamfer",                      action_chamfer},
    {"hole",                         action_hole},
    {"boolean",                      action_boolean},
    {"pattern",                      action_pattern},
    {"shell",                        action_shell},
    {"rib",                          action_rib},
    {"draft",                        action_draft},
    {"mirror",                       action_mirror},
    {"body_transform",               action_transform},
    {"thicken",                      action_thicken},
    {"split",                        action_split},
    {"thread",                       action_thread},
    {"sweep",                        action_sweep},
    {"loft",                         action_loft},
    {"sketch_project",               action_sketch_project},
    {"sketch_text",                  action_sketch_text},
    {"sketch_insert_svg",            action_sketch_insert_svg},
    {"delete_face",                  action_delete_face},
    {"sketch_blend_curve",           action_sketch_blend_curve},
    {"construct_axis",               action_construct_axis},
    {"construct_coordinate_system",  action_construct_coordinate_system},
    {"construct_plane",              action_construct_plane},
    {"helix",                        action_helix},
    {"align",                        action_align},
    {"list_align_types",             action_list_align_types},
    {"set_variable",                 action_set_variable},
    {"feature_set_expression",       action_feature_set_expression},
    {"feature_edit",                 action_feature_edit},
    {"feature_delete",               action_feature_delete},
    {"feature_move",                 action_feature_move},
    {"feature_suppress",             action_feature_suppress},
    {"feature_rename",               action_feature_rename},
    {"body_set",                     action_body_set},
    {"body_delete",                  action_body_delete},
    {"body_place",                   action_body_place},
    {"commit_to_plate",              action_commit_to_plate},
    {"new_design",                   action_new_design},
    {"export_step",                  action_export_step},
    {"surface_extrude",              action_surface_extrude},
    {"surface_revolve",              action_surface_revolve},
    {"surface_loft",                 action_surface_loft},
    {"surface_patch",                action_surface_patch},
    {"surface_offset",               action_surface_offset},
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
};
const std::size_t kSilentFormCount = std::size(kSilentForms);

json run_guarded(const std::function<json()>& run)
{
    try {
        return run();
    } catch (const ToolFailure& ex) {
        // Its own refusal, code included; one that names no cause is read as a plain exception is.
        if (ex.details.is_object() && ex.details.contains("code"))
            throw;
        throw ToolFailure(ex.what(), with_code(ex.details));
    } catch (const Standard_Failure& ex) {   // derives from std::exception, so it comes before it
        throw ToolFailure(std::string("OCCT: ") + (*ex.what() ? ex.what() : "failure"), {{"code", "kernel_failure"}});
    } catch (const CadKernelFailure& ex) {
        throw ToolFailure(std::string("OCCT: ") + ex.what(), {{"code", "kernel_failure"}});
    } catch (const CadPrecondition& ex) {
        throw ToolFailure(ex.what(), {{"code", "precondition"}});
    } catch (const std::exception& ex) {
        throw ToolFailure(ex.what(), {{"code", "invalid_argument"}});
    }
}

ToolFailure no_design_tab()
{
    if (!wxGetApp().is_editor())
        return ToolFailure("The G-code viewer has no Design tab", {{"code", "design_unavailable"}});
    if (!wxGetApp().is_enable_cad_feature())
        return ToolFailure(kCadOffNow, {{"code", "design_unavailable"}});
    return ToolFailure("The Design tab isn't available in this window yet; retry, and if it stays unavailable restart "
                       "OrcaSlicer (turning the CAD feature on takes effect after a restart)",
                       {{"code", "design_unavailable"}});
}

ToolFailure no_design_view()
{
    return ToolFailure("The Design view is not built in this window; restart OrcaSlicer if it stays so",
                       {{"code", "design_unavailable"}});
}

ToolFailure design_busy(const std::string& why)
{
    return ToolFailure("Design tab busy: " + why,
                       {{"code", "design_busy"},
                        {"hint", "confirm_tool keeps what is open when its Confirm is on (a card, the Text dialog, a body Move, "
                                 "a sketch, Constrain mode) and cancel_tool discards it; a rebuild ends by itself, so call "
                                 "again after it"}});
}

std::optional<std::uint64_t> cad_generation(const json& args)
{
    if (!args.is_object() || !args.contains("generation"))
        return std::nullopt;
    // The numbers the schema's integer type takes (is_whole_number(): a whole float within int64 among them), not
    // negative.
    const json& g = args["generation"];
    if (!is_whole_number(g) || (!g.is_number_unsigned() && g.get<double>() < 0))
        throw ToolFailure("generation must be an integer of 0 or more, as describe_design or body_describe gave it",
                          {{"code", "invalid_argument"}});
    return g.is_number_float() ? std::uint64_t(g.get<double>()) : g.get<std::uint64_t>();
}

namespace {

// The silent form of entry `id`, or nullptr.
CadRun find_silent_form(const std::string& id)
{
    for (const auto& [key, run] : kSilentForms)
        if (id == key) return run;
    return nullptr;
}

// Why a menu-only entry (a btn: or fly: handler) is refused for a selection it does not accept.
// TRN Refusal of a Design tab tool picked for a selection its right-click menu item is greyed for (Constrain
// sketch with a face selected).
const char* const kDoesNotApply = L("This tool does not apply to what is selected");
// Why an entry of the sketch's vocabulary is refused outside a sketch, and one of the model's inside one.
// TRN Refusal of a Design tab tool picked outside a sketch, when it is a sketch tool (a line, a rectangle).
const char* const kSketchTool = L("This tool works in a sketch: open a sketch first");
// TRN Refusal of a Design tab tool picked while a sketch is open, when it works on the model (Extrude, Shell).
const char* const kModelTool = L("This tool works on the model: finish or cancel the sketch first");

// The selection kinds the Design tab reports as what is selected (DesignPanel::offer_selection_kind()), as OfferSel
// bits: in a sketch, what is picked there; on the model, nothing, a face, an edge, a vertex, a body or a committed
// sketch's loop. Menu items accept other kinds too (two bodies, a datum, art, a sketch line on the model), which the tab
// never reports, so no selection can be one of them.
uint32_t reported_kinds(bool sketching)
{
    using S = OfferSel;
    static const S in_sketch[] = {S::SkNone, S::SkLine, S::SkArc, S::SkPoint, S::Sk2Ent};
    static const S on_model[]  = {S::None,     S::FacePlanar, S::FaceCyl,   S::FaceOther, S::EdgeStr,
                                  S::EdgeCirc, S::Vertex,     S::BodySolid, S::BodySheet, S::SkLoop};
    uint32_t bits = 0;
    if (sketching)
        for (const S kind : in_sketch) bits |= offer_bit(kind);
    else
        for (const S kind : on_model) bits |= offer_bit(kind);
    return bits;
}

// Item `item`'s handler, for a call of `command` that opens it (opened_item()), as a person's pick of the item or of
// its Speed Dial action runs it: past cad_gate() with no access of its own, so that only a rebuild refuses it, as the
// Design tab's keys and menu are refused by nothing; each tool decides for itself what an open card means to it. The
// handler runs in place, so a modal dialog it opens holds the call, which the core reports as blocked by that dialog.
// It is refused, as open_refusal() says, in the mode the item does not belong to, where the right-click menu does not
// offer it, and for a btn: or fly: handler a selection the menu greys the item for. The reply claims nothing about
// what the handler did, which may be to open a card, start a sketch tool, act at once or tell the person why it
// cannot: it says what the tab shows after it, its busy reason (a card, a dialog or a sketch now open) and its
// status line. Its `tool` is the command called. A call that is not a person's pick, made while a row of the Feature
// list is selected and nothing is picked in the view, targets that row: an item whose handler acts on it (feature_row)
// is not refused for the selection then, and applies. A person's pick keeps the menu's rule, which greys the item.
json open_tool(const DesignTool& item, const char* command, const json& args, bool person)
{
    DesignPanel* panel      = cad_gate(CadAccess::None, args);
    const int    kind       = panel->mcp_offer_selection_kind();
    const bool   row_target = !person && panel->mcp_feature_row() >= 0 && kind == int(OfferSel::None) &&
                            panel->mcp_picked_plane() < 0;
    const bool applies = (row_target && item.feature_row) || (item.accepts & offer_bit(OfferSel(kind))) != 0;
    if (std::optional<ToolFailure> refused = open_refusal(item, panel->mcp_sketch_map_applies(), kind, row_target))
        throw *refused;
    const bool text_was_open = panel->mcp_text_dialog_open();
    panel->mcp_run_verb(item.id);
    // A dialog the handler opened held the call until it was answered, unless the handler opened the Text dialog,
    // which is modeless: the call returns with it open. A Text dialog open before the handler ran is not its dialog.
    const bool modal = item.modal && !(panel->mcp_text_dialog_open() && !text_was_open);
    std::string why;
    const bool  busy = panel->mcp_busy(false, false, why);
    return json{{"interactive", true},
                {"tool", command},
                {"applies", applies},
                {"modal", modal},
                {"selection_kind", kind},
                {"busy", busy ? json(why) : json()},
                {"status", into_u8(panel->mcp_status())}};
}

// The end of the description of entry `t`'s command: the argument sets that name a menu item, its own args and those of
// the items folded into it, each once with the names of the items it opens (items folded in with one set are one tool
// under several labels, as the value items are with {}), so that a search for an item's name finds the command; empty
// when there are none.
std::string opening_calls(const DesignTool& t)
{
    std::vector<std::pair<std::string, std::string>> sets;   // an argument set as JSON text, and its items' names
    const auto add = [&sets](const DesignTool& item) {
        const std::string args = json::parse(item.args).dump();
        const auto same = std::find_if(sets.begin(), sets.end(), [&args](const auto& set) { return set.first == args; });
        if (same == sets.end())
            sets.emplace_back(args, item.name);
        else
            same->second += std::string(", ") + item.name;
    };
    if (t.action != nullptr && t.args != nullptr)
        add(t);
    for (const DesignTool& item : kDesignTools)
        if (item.fold_into != nullptr && std::strcmp(item.fold_into, t.id) == 0)
            add(item);
    if (sets.empty())
        return {};
    std::string calls;
    for (const auto& [args, names] : sets)
        calls += (calls.empty() ? "" : ", ") + args + " (" + names + ")";
    return " Called with exactly one of these argument sets and nothing else, it opens that item's tool of the Design "
           "tab for a person, as the menu item does: " +
           calls + ".";
}

// The command of the Design table's entry `t`, which has a handler, a tool block or both, and is no folded item.
NativeCommand design_command(const DesignTool& t)
{
    const json doc = t.doc != nullptr ? json::parse(t.doc) : json();
    json       schema;
    if (doc.is_null())
        schema = {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}};
    else
        schema = cad_input_schema(doc);
    // The person's form is the call with no arguments where that opens an item's tool, the entry's own or one folded
    // into it: it requires nothing (with_silent()), and runs in that item's mode only.
    const DesignTool* person_item = opened_item(t, json::object());
    if (person_item != nullptr && !doc.is_null()) {
        schema = with_silent(std::move(schema));
        schema["properties"]["silent"]["description"] = std::string(kSilentNote) + " " + mode_note(*person_item, true);
    }

    const DesignTool* entry = &t;
    NativeCommand     c     = cad_command(
        t.id, t.name, std::move(schema),
        [entry](const json& args, bool person) {
            if (const DesignTool* item = opened_item(*entry, args))
                return open_tool(*item, entry->id, args, person);
            return run_cad_tool(entry->id, silent_args(args));
        },
        [entry](const json& args) { return opened_item(*entry, args) != nullptr; });
    c.has_own_action = t.action != nullptr;
    c.icon           = t.icon != nullptr ? t.icon : "";
    c.heading        = t.section != nullptr ? t.section : "";
    // The items folded into the command keep their actions, each a call of the command with the item's arguments,
    // and say what a pick does as a command with a handler only does.
    for (const DesignTool& item : kDesignTools)
        if (item.fold_into != nullptr && std::strcmp(item.fold_into, t.id) == 0)
            c.actions.push_back({{"key", item.id},
                                 {"title", item.name},
                                 {"icon", item.icon != nullptr ? item.icon : ""},
                                 {"description", std::string(item.hint) + ". " + kOpensToolNote + " " + mode_note(item)},
                                 {"args", json::parse(item.args)}});
    if (doc.is_null()) {
        c.description = std::string(t.hint) + ". " + kOpensToolNote + " " + mode_note(t);
        return c;
    }
    if (find_silent_form(t.id) == nullptr)
        throw std::logic_error(std::string("The Design table's entry ") + t.id + " has a tool block but no silent form");
    std::string description = doc.at("summary").get<std::string>();
    if (const std::string details = doc.at("details").get<std::string>(); !details.empty())
        description += ' ' + details;
    if (t.ok_false) description += std::string(" ") + kFailureNote;
    description += opening_calls(t);
    c.read_only   = t.access == CadAccess::Read;
    // A read answers the same however often it is asked, as the listed read-only tools say of themselves.
    c.idempotent  = c.read_only;
    c.description = std::move(description);
    c.examples    = doc.at("examples");
    return c;
}

} // namespace

const DesignTool* find_design_tool(const std::string& id)
{
    for (const DesignTool& t : kDesignTools)
        if (id == t.id) return &t;
    return nullptr;
}

bool is_design_command(const DesignTool& t) { return (t.action != nullptr || t.doc != nullptr) && t.fold_into == nullptr; }

const DesignTool* opened_item(const DesignTool& entry, const json& args)
{
    // A call with no arguments gives none, null as {}.
    const json given = person_form(args) ? json::object() : args;
    if (entry.action != nullptr && (given.empty() || (entry.args != nullptr && json::parse(entry.args) == given)))
        return &entry;
    for (const DesignTool& item : kDesignTools)
        if (item.fold_into != nullptr && std::strcmp(item.fold_into, entry.id) == 0 && item.args != nullptr &&
            json::parse(item.args) == given)
            return &item;
    return nullptr;
}

bool offered_in_mode(const DesignTool& t, bool sketching) { return t.sketch_mode == sketching; }

std::vector<DesignAction> design_actions(const CommandCatalog& catalog, bool sketching, bool cad_on)
{
    std::vector<DesignAction> actions;
    if (!cad_on) return actions;
    for (const DesignTool& t : kDesignTools) {
        if (t.action == nullptr || !offered_in_mode(t, sketching)) continue;
        const auto [command, action] = find_command_action(catalog, t.id);
        if (command != nullptr && command->group == kDesignGroup) actions.push_back({command, action});
    }
    return actions;
}

std::optional<ToolFailure> open_refusal(const DesignTool& t, bool sketching, int kind, bool row_target)
{
    // The message is the person's; the hint names the commands that change the mode.
    if (!offered_in_mode(t, sketching))
        return ToolFailure(t.sketch_mode ? kSketchTool : kModelTool,
                           {{"code", "wrong_mode"},
                            {"mode", t.sketch_mode ? "sketch" : "model"},
                            // A model tool's: Constrain mode is a sketch to the menu, and the busy gate refuses
                            // sketch_finish and sketch_cancel there, so the hint names the two that end either.
                            {"hint", t.sketch_mode ? "sketch_create {plane: \"XY\"} opens a sketch on a plane, sketch_edit "
                                                     "{sketch: i} a committed one" :
                                                     "confirm_tool keeps the open sketch, or ends Constrain mode, and "
                                                     "cancel_tool drops the sketch or leaves Constrain mode"}});
    // The item's handler acts on the Feature list's selected row, and says itself what it cannot do with it.
    if (row_target && t.feature_row)
        return std::nullopt;
    const std::string handler(t.action);
    const bool applies = (t.accepts & offer_bit(OfferSel(kind))) != 0;
    // Only an item that accepts a kind the tab can report in its mode is greyed for the others; one that accepts none
    // (Boolean's items take two bodies, Rib a sketch line) opens as its toolbar button does, whatever is selected.
    const bool greyable = (handler.rfind("btn:", 0) == 0 || handler.rfind("fly:", 0) == 0) &&
                          (t.accepts & reported_kinds(t.sketch_mode)) != 0;
    if (applies || !greyable)
        return std::nullopt;
    // The item's own refusal is about the document (no body, no second sketch), which the menu shows only for
    // an item that takes the selection; here the selection is the obstacle, so the reply names what to pick.
    json accepts = json::array();
    for (int sel = 0; sel < int(OfferSel::Count); ++sel)
        if ((t.accepts & offer_bit(OfferSel(sel))) != 0)
            accepts.push_back(kDesignSelectionIds[sel]);
    return ToolFailure(kDoesNotApply, {{"code", "wrong_selection"},
                                       {"selection_kind", kind},
                                       {"selection", kDesignSelectionIds[kind]},
                                       {"accepts", std::move(accepts)}});
}

DesignPanel* cad_gate(CadAccess access, const json& args)
{
    // The MCP server answers before the main window exists and while the app closes.
    ui_plater();
    if (!wxGetApp().is_editor() || !wxGetApp().is_enable_cad_feature())
        throw no_design_tab();
    // The panel is built on first use, and in a headless session nobody clicks the tab that would
    // build it, so it is built here rather than refused. It is null when no page was made at startup
    // (the CAD feature was turned on later), while the page is mid-build, and after its build failed.
    DesignPanel* panel = DesignPanel::ensure();
    if (panel == nullptr)
        throw no_design_tab();

    // Only Read commands, the opening of a tool and the Ui commands, which act on what is open, may run under a GUI
    // editor or a sketch session they do not own. None may run during a rebuild, not even these: the rebuild's busy
    // loop runs this call while the GUI's worker thread holds the document. Checked before hydrating, which writes it.
    std::string why;
    const bool  under_open = access == CadAccess::Read || access == CadAccess::None || access == CadAccess::Ui;
    if (panel->mcp_busy(under_open, access == CadAccess::Sketch, why))
        throw design_busy(why);

    // A project opened without ever showing the Design tab has its recipe only in the Model;
    // load it, as showing the tab would, before anything reads or writes the document.
    panel->hydrate_from_model();

    // Stale-id guard, checked here rather than in each command's function.
    //
    // Global face and edge ids are indices into TopExp::MapShapes, valid only against the
    // topology that produced them. Every dress-up rewrites those maps, so the natural way to
    // drive the Design tab — read the scene once, then issue several operations — silently
    // addresses the WRONG edge on every call after the first. Measured on a box: four chamfers
    // with ids re-read each time remove 0.400/0.397/0.397/0.395 mm3; the same four with ids
    // captured up front remove 0.400/0.008/0.397/0.280. Neither run errors, because a stale id
    // still resolves to a real edge — just not the one that was asked for.
    //
    // So a caller may pass back the "generation" it got from describe_design or body_describe,
    // and a mismatch is refused instead of silently obeyed. Optional by design: a call without it
    // is not checked, and supplying it is what buys the guarantee. One check here rather than one
    // per command, so a command added later cannot forget it.
    //
    // The type is checked first (cad_generation()), as a caller that skips the schema (run_cad_tool() called
    // directly) may pass any: a bare get<uint64_t>() on `"generation": "x"` would surface as an internal error
    // instead of a parameter error.
    if (const std::optional<std::uint64_t> given = cad_generation(args)) {
        const uint64_t want = *given;
        const uint64_t have = panel->mcp_doc().topo_generation;
        if (want != have)
            throw ToolFailure("stale face/edge ids: they were read at generation " + std::to_string(want)
                                  + " but the model is now at " + std::to_string(have)
                                  + ". Re-read body_describe and use the new ids — the old ones still name real "
                                    "edges, just not the ones you measured.",
                              {{"code", "stale_ids"}});
    }
    return panel;
}

json run_cad_tool(const std::string& id, const json& args)
{
    const DesignTool* entry = find_design_tool(id);
    const CadRun      run   = find_silent_form(id);
    if (entry == nullptr || run == nullptr)
        throw ToolFailure("No Design command runs silently as " + id, {{"code", "no_silent_form"}});
    DesignPanel* panel = cad_gate(entry->access, args);
    return run_guarded([&] { return run(panel, args); });
}

json cad_undo_redo(bool redo)
{
    DesignPanel* panel = cad_gate(CadAccess::Sketch, json::object());
    return run_guarded([&] { return undo_redo(panel, redo); });
}

json cad_input_schema(const json& block)
{
    json properties = json::object();
    json required   = json::array();
    for (const json& param : block.at("params")) {
        const std::string name = param.at("name").get<std::string>();
        const std::string type = param.at("type").get<std::string>();
        json              property;
        if (type.find('|') != std::string::npos) {
            property["type"] = json::array();
            for (std::size_t from = 0;;) {
                const std::size_t bar = type.find('|', from);
                property["type"].push_back(type.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
                if (bar == std::string::npos) break;
                from = bar + 1;
            }
        } else {
            property["type"] = type;
        }
        if (param.contains("min")) property["minimum"] = param["min"];
        if (param.contains("exclusive_min")) property["exclusiveMinimum"] = param["exclusive_min"];
        if (param.contains("max")) property["maximum"] = param["max"];
        if (param.contains("enum")) property["enum"] = param["enum"];
        if (param.contains("default")) property["default"] = param["default"];
        else required.push_back(name);
        // A param whose default is null is one its function reads as left out when it is null (no limit, the open
        // sketch), so a call may give null. One marked "nullable": false, whose function reads a null as given,
        // keeps its type.
        if (param.contains("default") && param["default"].is_null() && param.value("nullable", true)) {
            if (!property["type"].is_array()) property["type"] = json::array({property["type"]});
            property["type"].push_back("null");
            if (property.contains("enum")) property["enum"].push_back(nullptr);
        }
        std::string description = param.value("description", std::string());
        if (const std::string unit = param.value("unit", std::string()); !unit.empty())
            description = description.empty() ? "In " + unit + "." : description + " (" + unit + ")";
        if (!description.empty()) property["description"] = std::move(description);
        properties[name] = std::move(property);
    }
    properties["generation"] = {{"type", "integer"}, {"minimum", 0}, {"description", kGenerationNote}};
    json schema{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

namespace {

// A Design function's value as its command replies with it: without "ok": true and an empty "error", which say
// nothing that a reply other than an error doesn't. An ok false, and an error's text beside it, stay.
json design_reply(json value)
{
    if (value.is_object()) {
        if (const auto ok = value.find("ok"); ok != value.end() && *ok == true)
            value.erase(ok);
        if (const auto error = value.find("error"); error != value.end() && *error == "")
            value.erase(error);
    }
    return value;
}

} // namespace

NativeCommand cad_command(std::string key, std::string title, json schema,
                          std::function<json(const json& args, bool person)> fn, std::function<bool(const json& args)> opens)
{
    NativeCommand c;
    c.key          = std::move(key);
    c.title        = std::move(title);
    c.group        = kDesignGroup;
    c.input_schema = std::move(schema);
    c.run = [fn = std::move(fn), opens = std::move(opens)](const json& args, ToolContext& ctx) {
        if (ctx.person()) {
            try {
                return ToolResult::ok(design_reply(fn(args, true)));
            } catch (const ToolFailure& refused) {
                // What the person reads, in the UI's language where the text is a msgid (an entry's refusal). Looked
                // up in the application's catalogue only, as the right-click menu's items are (show_offer_menu()), so
                // that a wxWidgets entry cannot answer for it.
                throw ToolFailure(into_u8(wxGetTranslation(wxString::FromUTF8(refused.what()), SLIC3R_APP_KEY)), refused.details);
            }
        }
        // `generation` guards ids within this session only: a replay rebuilds the same ids by replaying
        // the history, but reaches another generation number, so a recorded one would refuse the replay.
        // Journal the call without it.
        json recorded = args;
        if (recorded.is_object()) recorded.erase("generation");
        // A call that ran may be recorded, without its generation, as one that opens a tool (one with no arguments):
        // with silent, its replay runs too.
        if (opens && recorded.is_object() && !opens(args) && opens(recorded))
            recorded["silent"] = true;
        ctx.set_journal_args(std::move(recorded));
        json result = ctx.run_ui([&] { return fn(args, false); });
        settle(ctx);
        return ToolResult::ok(design_reply(std::move(result)));
    };
    return c;
}

void register_cad_commands(CommandCatalog& catalog)
{
    // Heading by heading, as tool_list's description indexes them, so that its listing of the group reads in the
    // same order. Every command has a heading (DesignTool::section).
    for (const char* section : kDesignSections)
        for (const DesignTool& t : kDesignTools)
            if (is_design_command(t) && t.section != nullptr && std::strcmp(t.section, section) == 0)
                catalog.add(design_command(t));
}

void leave_out_cad_commands(CommandCatalog& catalog, bool editor)
{
    // A folded item's id too, which is no command but names an action.
    for (const DesignTool& t : kDesignTools)
        if (t.action != nullptr || t.doc != nullptr)
            catalog.leave_out(t.id, editor ? kCadOffAtStartup : kNoDesignInViewer, "design_unavailable");
}

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

std::string design_command_index()
{
    std::string index = " The Design tab's CAD commands follow by heading (sketches, solid and surface features, bodies, "
                        "measurements); `tool_list {group: \"Design\"}` lists them with what each does, and `tool_describe {tool}` gives "
                        "one's manual before it is called. A "
                        "command whose schema has `silent` does what "
                        "its menu item does when called with no arguments, unless `silent` is true.";
    for (const char* section : kDesignSections) {
        std::string names;
        for (const DesignTool& t : kDesignTools)
            if (t.section != nullptr && std::strcmp(t.section, section) == 0)
                names += (names.empty() ? "" : ", ") + std::string("`") + t.id + "`";
        index += std::string(" ") + section;
        if (std::strcmp(section, "Menu items") == 0)
            index += ", which take no arguments and do what their item of the right-click menu does";
        index += ": " + names + ".";
    }
    return index;
}

json cad_note(const CommandCatalog& catalog)
{
    if (!wxGetApp().is_editor())
        return nullptr;
    const std::vector<NativeCommand>& commands = catalog.commands();
    const bool registered = std::any_of(commands.begin(), commands.end(), [](const NativeCommand& c) { return c.group == kDesignGroup; });
    return registered && wxGetApp().is_enable_cad_feature() ? json() : json(kCadOffNote);
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

json cad_feature_json(const CadDocument& doc, int index)
{
    check_feature_index(index, doc);
    const CadFeature& f = doc.features[index];
    json j = feature_card(f);
    j["index"]      = index;
    j["type"]       = feature_type_name(f.type);
    j["name"]       = f.name;
    j["suppressed"] = !f.enabled;

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

    // The model its body indices and face and edge ids are read in: the one just before it, which feature_edit's
    // refs take, but for a datum, which the engine resolves against the finished model (for_each_body_ref()).
    j["ids_of"] = f.type == T::Plane || f.type == T::Axis || f.type == T::CoordSys ? "finished" : "before";
    // As the last rebuild recorded them, named as the bodies they became are; null where that rebuild did not reach.
    json before = nullptr;
    if (size_t(index) < doc.bodies_before.size() && doc.bodies_before[size_t(index)]) {
        before = json::array();
        const std::vector<CadDocument::BodyBefore>& was = *doc.bodies_before[size_t(index)];
        for (size_t b = 0; b < was.size(); ++b) {
            const int      now  = doc.body_index(was[b].id);
            const CadBody* body = now >= 0 ? &doc.bodies[size_t(now)] : nullptr;
            before.push_back(json{{"index", int(b)}, {"name", body != nullptr && body->has_user_name ? body->user_name : was[b].name}});
        }
    }
    j["bodies_before"] = std::move(before);
    return j;
}

json cad_feature_changes(const json& was, const json& now)
{
    json changed = json::object();
    for (const char* part : {"params", "refs"}) {
        const json before = was.value(part, json::object()), after = now.value(part, json::object());
        for (const auto& [k, v] : after.items()) {
            const json old = before.value(k, json());
            if (old != v) changed[k] = json::array({old, v});
        }
    }
    // The plane a Mirror, a Helix or a Cut builds on is reported beside them, and a re-point of it is a change too.
    const json old_plane = was.value("plane", json()), new_plane = now.value("plane", json());
    if (old_plane != new_plane) changed["plane"] = json::array({old_plane, new_plane});
    return changed;
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
        throw std::runtime_error(type + ": no field feature_edit can change; its inputs are geometry or picks");
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
                                         "remove the binding with feature_set_expression {field: '" + bound + "', expr: ''}");
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

CadDocument cad_document_before(const CadDocument& doc, int feature)
{
    check_feature_index(feature, doc);
    CadDocument before = doc.history_before(feature);
    if (!before.recompute())
        throw ToolFailure("the model before feature " + std::to_string(feature) + " (" + doc.features[size_t(feature)].name +
                              ") does not rebuild: " + (before.error.empty() ? std::string("no reason given") : before.error),
                          {{"code", "rebuild_failed"}});
    return before;
}

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

namespace {

// "feature 5 (Sweep 6)": the feature whose refs feature_edit writes, as its refusals name it.
std::string feature_label(const CadDocument& doc, int index)
{
    return "feature " + std::to_string(index) + " (" + doc.features[size_t(index)].name + ")";
}

// The names feature_edit's refs takes for a feature of type `t`: those of feature_describe's refs that its card can
// pick again when it edits. Empty for a type whose card picks nothing again, and for an Extrude, whose profile and
// target are fixed.
std::vector<std::string> ref_names(CadFeatureType t)
{
    using T = CadFeatureType;
    switch (t) {
    case T::Revolve:        return {"revolve_axis_entity", "target_body"};
    case T::SurfaceRevolve: return {"revolve_axis_entity"};
    case T::Sweep:          return {"sweep_path_ref", "target_body"};
    case T::Loft:           return {"loft_profile_refs", "target_body"};
    case T::SurfaceLoft:    return {"loft_profile_refs"};
    case T::Boolean:        return {"target_body", "bool_tool_body"};
    case T::Mirror:
    case T::Cut:
    case T::Hole:
    case T::Thread:         return {"plane", "target_body"};
    case T::Helix:          return {"plane"};
    case T::Rib:            return {"rib_sketch_ref", "rib_entity", "target_body"};
    case T::Mate:           return {"mate_cs_a", "mate_cs_b"};
    case T::Fillet:
    case T::Chamfer:        return {"edges", "face_group", "target_body"};
    case T::Shell:          return {"shell_face", "target_body"};
    case T::Draft:          return {"draft_face", "target_body"};
    case T::Thicken:        return {"thicken_face", "target_body"};
    case T::DeleteFace:     return {"delete_faces", "target_body"};
    case T::Plane:
        return {"plane_type", "plane_base", "plane_axis", "plane_face_body", "plane_face", "plane_face2_body", "plane_face2",
                "plane_edge_body", "plane_edge", "plane_edge2_body", "plane_edge2"};
    case T::Axis:
        return {"axis_type", "axis_p1", "axis_p2", "axis_body", "axis_face", "axis_edge", "axis_plane_a", "axis_plane_b"};
    case T::CoordSys:
        return {"coordsys_type", "coordsys_point", "coordsys_body", "coordsys_face", "coordsys_edge", "coordsys_x_hint"};
    case T::Pattern:
    case T::Transform:
    case T::ThickenSurface:
    case T::SurfaceOffset:  return {"target_body"};
    default:                return {};
    }
}

// Reads the values of feature_edit's refs for features[index] of `doc`, whose body indices and face and edge ids
// name those of `ids`: the model before the feature, or for a datum the finished model, `doc` itself.
struct RefReader
{
    const CadDocument& ids;
    const CadDocument& doc;
    int                index;
    bool               finished;   // `ids` is the finished model, a datum's
    std::string        at;         // feature_label() of the feature

    // Where `ids` stands, as a refusal says it.
    std::string where() const { return finished ? "in the finished model" : "before " + at; }

    int integer(const std::string& name, const json& v, const char* what) const
    {
        if (!v.is_number_integer()) throw std::runtime_error(name + " must be " + what);
        return v.get<int>();
    }

    // A feature before the one edited, of one of `types`, which `kinds` names.
    int earlier(const std::string& name, const json& v, std::initializer_list<CadFeatureType> types, const char* kinds) const
    {
        const int fi = integer(name, v, "a feature index (describe_design's features[].index)");
        if (fi < 0 || fi >= index)
            throw std::runtime_error(name + " " + std::to_string(fi) + " is not an earlier feature: it must come before " + at);
        const CadFeatureType t = doc.features[size_t(fi)].type;
        if (std::find(types.begin(), types.end(), t) == types.end())
            throw std::runtime_error(name + " " + std::to_string(fi) + " (" + feature_type_name(t) + ") is not " + kinds);
        return fi;
    }

    // A body of `ids`, or -1 where `unset` names what -1 stands for.
    int body(const std::string& name, const json& v, const char* unset) const
    {
        const int b = integer(name, v, unset != nullptr ? "a body index, or -1" : "a body index");
        if (unset != nullptr && b == -1) return b;
        const int n = int(ids.bodies.size());
        if (b < 0 || b >= n)
            throw std::runtime_error(name + " " + std::to_string(b) + " is not a body " + where() + ": there " +
                                     (finished ? (n == 1 ? "is 1 body" : "are " + std::to_string(n) + " bodies")
                                               : (n == 1 ? "was 1 body" : "were " + std::to_string(n) + " bodies")));
        return b;
    }

    // The body a feature's target_body names in `ids`: -1 is the last one.
    int target(int b) const
    {
        if (b >= 0) return b;
        if (ids.bodies.empty()) throw CadPrecondition("there is no body " + where());
        return int(ids.bodies.size()) - 1;
    }

    // Face (`edges` false) or edge ids, each of body `b` of `ids`.
    void on_body(const std::vector<int>& list, int b, bool edges) const
    {
        const TopoDS_Shape& shape = ids.bodies[size_t(b)].shape;
        const int           n     = edges ? GeometryEngine::edge_count(shape) : GeometryEngine::face_count(shape);
        const char*         kind  = edges ? "edge" : "face";
        for (const int id : list)
            if (id < 0 || id >= n)
                throw std::runtime_error(std::string(kind) + " " + std::to_string(id) + " of body " + std::to_string(b) + " does not exist " +
                                         where() + ": the body " + (finished ? "has " : "had ") + std::to_string(n) + " " + kind + "s" +
                                         (finished ? "" : " there"));
    }

    // One id, or for `many` a list of distinct ids.
    std::vector<int> id_list(const std::string& name, const json& v, bool many) const
    {
        std::vector<int> out;
        if (many && v.is_array()) {
            for (const json& e : v) {
                if (!e.is_number_integer()) throw std::runtime_error(name + " must be ids from body_describe");
                if (std::find(out.begin(), out.end(), e.get<int>()) != out.end())
                    throw std::runtime_error(name + ": " + std::to_string(e.get<int>()) + " is listed twice");
                out.push_back(e.get<int>());
            }
            if (out.empty()) throw std::runtime_error(name + " is an empty list");
            return out;
        }
        out.push_back(integer(name, v, many ? "an id or a list of ids from body_describe" : "an id from body_describe"));
        return out;
    }

    // A datum-plane reference: 0, 1 or 2 for XY, XZ or YZ, or 3 + N for the Nth shown Plane feature, which must come
    // before the feature edited; -1 where `unset` allows it.
    int datum(const std::string& name, const json& v, bool unset) const
    {
        const int d = integer(name, v, "0, 1 or 2 (XY, XZ or YZ), or 3 + N for the Nth shown datum plane");
        if ((unset && d == -1) || (d >= 0 && d < 3)) return d;
        int seen = 0;
        for (int i = 0; d >= 3 && i < index; ++i)
            if (doc.features[size_t(i)].type == CadFeatureType::Plane && doc.features[size_t(i)].enabled && seen++ == d - 3)
                return d;
        throw std::runtime_error(name + " " + std::to_string(d) + " names no datum plane before " + at);
    }
};

// A point or direction [x, y, z] of a datum's refs.
Vec3d vec3_ref(const std::string& name, const json& v, bool direction)
{
    if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number())
        throw std::runtime_error(name + " must be [x, y, z]");
    const Vec3d p(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
    if (direction && !(p.norm() > 1e-9)) throw std::runtime_error(name + " must not be zero");
    return p;
}

// The refs of a Plane, an Axis and a CoordSys: their types, points and datum references, and bodies, faces and edges
// of the finished model.
void apply_datum_refs(CadFeature& g, const json& refs, const RefReader& r)
{
    using T = CadFeatureType;
    const auto given = [&](const char* k) { return refs.contains(k); };
    const auto body  = [&](const char* k, int& field) { if (given(k)) field = r.body(k, refs[k], "unset"); };
    const auto id    = [&](const char* k, int& field) { if (given(k)) field = r.integer(k, refs[k], "an id from body_describe, or -1"); };
    // A face or an edge set on a body: one of that body's, in the finished model.
    const auto check = [&](int b, int ref, bool edges, const char* k) {
        if (ref < 0) return;
        if (b < 0) throw std::runtime_error(std::string(k) + " " + std::to_string(ref) + " is on no body: give its body too");
        r.on_body({ref}, b, edges);
    };
    if (g.type == T::Plane) {
        if (given("plane_type")) g.plane_type = PlaneType(enum_index("plane_type", refs["plane_type"], kPlaneTypes));
        if (given("plane_base")) g.plane_base = r.datum("plane_base", refs["plane_base"], false);
        if (given("plane_axis")) {
            g.plane_axis = r.integer("plane_axis", refs["plane_axis"], "0 (the base's X) or 1 (its Y)");
            if (g.plane_axis != 0 && g.plane_axis != 1) throw std::runtime_error("plane_axis must be 0 (the base's X) or 1 (its Y)");
        }
        body("plane_face_body", g.plane_face_body);
        id("plane_face", g.plane_face);
        body("plane_face2_body", g.plane_face2_body);
        id("plane_face2", g.plane_face2);
        body("plane_edge_body", g.plane_edge_body);
        id("plane_edge", g.plane_edge);
        body("plane_edge2_body", g.plane_edge2_body);
        id("plane_edge2", g.plane_edge2);
        check(g.plane_face_body, g.plane_face, false, "plane_face");
        check(g.plane_face2_body, g.plane_face2, false, "plane_face2");
        check(g.plane_edge_body, g.plane_edge, true, "plane_edge");
        check(g.plane_edge2_body, g.plane_edge2, true, "plane_edge2");
        // What the Plane card's Confirm refuses, which the rebuild would not: an unresolved plane falls back silently.
        if (const std::string why = r.doc.datum_plane_refusal(g); !why.empty()) throw std::runtime_error(why);
    } else if (g.type == T::Axis) {
        if (given("axis_type")) g.axis_type = AxisType(enum_index("axis_type", refs["axis_type"], kAxisTypes));
        if (given("axis_p1")) g.axis_p1 = vec3_ref("axis_p1", refs["axis_p1"], false);
        if (given("axis_p2")) g.axis_p2 = vec3_ref("axis_p2", refs["axis_p2"], false);
        body("axis_body", g.axis_body);
        id("axis_face", g.axis_face);
        id("axis_edge", g.axis_edge);
        if (given("axis_plane_a")) g.axis_plane_a = r.datum("axis_plane_a", refs["axis_plane_a"], true);
        if (given("axis_plane_b")) g.axis_plane_b = r.datum("axis_plane_b", refs["axis_plane_b"], true);
        check(g.axis_body, g.axis_face, false, "axis_face");
        check(g.axis_body, g.axis_edge, true, "axis_edge");
    } else {
        if (given("coordsys_type")) g.coordsys_type = CoordSysType(enum_index("coordsys_type", refs["coordsys_type"], kCoordSysTypes));
        if (given("coordsys_point")) g.coordsys_point = vec3_ref("coordsys_point", refs["coordsys_point"], false);
        if (given("coordsys_x_hint")) g.coordsys_x_hint = vec3_ref("coordsys_x_hint", refs["coordsys_x_hint"], true);
        body("coordsys_body", g.coordsys_body);
        id("coordsys_face", g.coordsys_face);
        id("coordsys_edge", g.coordsys_edge);
        check(g.coordsys_body, g.coordsys_face, false, "coordsys_face");
        check(g.coordsys_body, g.coordsys_edge, true, "coordsys_edge");
    }
}

// The refs of every other type, read in the model before the feature (`r.ids`).
void apply_model_refs(CadFeature& g, const json& refs, const RefReader& r)
{
    using T             = CadFeatureType;
    const auto given    = [&](const char* k) { return refs.contains(k); };
    const CadDocument& doc = r.doc;
    if (given("target_body")) g.target_body = r.body("target_body", refs["target_body"], g.type == T::Boolean ? nullptr : "the last body");

    if (given("revolve_axis_entity")) {
        // A line of the sketch the revolve turns, or of the profile it carries itself.
        const int  e     = r.integer("revolve_axis_entity", refs["revolve_axis_entity"], "a line of the feature's sketch (entity index), or -1 for the plane axis");
        const bool own   = g.sketch_ref < 0 || g.sketch_ref >= int(doc.features.size());
        if (e != -1 && !revolve_line(own ? g.entities : doc.features[size_t(g.sketch_ref)].entities, e))
            throw std::runtime_error("revolve_axis_entity " + std::to_string(e) + " is not a line of " +
                                     (own ? std::string("its profile") : "sketch " + std::to_string(g.sketch_ref)));
        g.revolve_axis_entity = e;
    }
    if (given("sweep_path_ref")) {
        g.sweep_path_ref = r.earlier("sweep_path_ref", refs["sweep_path_ref"], {T::Sketch, T::Helix}, "a Sketch or a Helix");
        if (g.sweep_path_ref == g.sketch_ref) throw std::runtime_error("sweep_path_ref cannot be the profile sketch itself");
    }
    if (given("loft_profile_refs")) {
        const json& v = refs["loft_profile_refs"];
        if (!v.is_array() || v.size() < 2) throw std::runtime_error("loft_profile_refs must be two or more sketch feature indices, in loft order");
        std::vector<int> profiles;
        for (const json& p : v) {
            const int s = r.earlier("loft_profile_refs", p, {T::Sketch, T::Project}, "a Sketch");
            if (std::find(profiles.begin(), profiles.end(), s) != profiles.end())
                throw std::runtime_error("loft_profile_refs: sketch " + std::to_string(s) + " is listed twice");
            profiles.push_back(s);
        }
        g.loft_profile_refs = std::move(profiles);
    }
    if (given("bool_tool_body")) g.bool_tool_body = r.body("bool_tool_body", refs["bool_tool_body"], nullptr);
    // A Boolean's two operands; for every other type bool_tool_body is unused, and target_body -1 is the last body.
    if (g.type == T::Boolean && (given("target_body") || given("bool_tool_body")) && g.target_body == g.bool_tool_body)
        throw std::runtime_error("target_body and bool_tool_body must be two different bodies");

    if (given("plane")) {
        // A face is one of the model before the feature; a datum's frame is the finished model's, as a card's plane
        // list resolves it. A hole or a thread drills the face's own body, as on its card.
        const json& v     = refs["plane"];
        const bool  drill = g.type == T::Hole || g.type == T::Thread;
        int         face_body = -1;
        if (v.is_object() && v.contains("datum")) {
            if (const json d = v["datum"]; d.is_number_integer() && (d.get<int>() < 0 || d.get<int>() >= r.index))
                throw std::runtime_error("plane's datum " + std::to_string(d.get<int>()) + " is not an earlier feature: it must come before " + r.at);
            g.plane = cad_resolve_plane(v, doc, drill);
        } else {
            g.plane = cad_resolve_plane(v, r.ids, drill, &face_body);
        }
        if (g.type == T::Cut) g.cut_face = g.cut_face_body = -1;   // a split along the plane, not along a face
        if (drill && face_body >= 0) {
            if (given("target_body") && g.target_body != face_body)
                throw std::runtime_error("the plane is a face of body " + std::to_string(face_body) + ", so that body is drilled, not body " +
                                         std::to_string(g.target_body));
            g.target_body = face_body;
        }
    }
    if (given("rib_sketch_ref")) g.rib_sketch_ref = r.earlier("rib_sketch_ref", refs["rib_sketch_ref"], {T::Sketch}, "a Sketch");
    if (given("rib_entity")) g.rib_entity = r.integer("rib_entity", refs["rib_entity"], "a line of the rib's sketch (entity index)");
    if ((given("rib_sketch_ref") || given("rib_entity")) &&
        !(g.rib_sketch_ref >= 0 && g.rib_sketch_ref < int(doc.features.size()) &&
          revolve_line(doc.features[size_t(g.rib_sketch_ref)].entities, g.rib_entity)))
        throw std::runtime_error("rib_entity " + std::to_string(g.rib_entity) + " is not a line of sketch " + std::to_string(g.rib_sketch_ref));
    if (given("mate_cs_a")) g.mate_cs_a = r.earlier("mate_cs_a", refs["mate_cs_a"], {T::CoordSys}, "a CoordSys");
    if (given("mate_cs_b")) g.mate_cs_b = r.earlier("mate_cs_b", refs["mate_cs_b"], {T::CoordSys}, "a CoordSys");
    if ((given("mate_cs_a") || given("mate_cs_b")) && g.mate_cs_a == g.mate_cs_b)
        throw std::runtime_error("mate_cs_a and mate_cs_b must be two different coordinate systems");

    if (given("edges") && given("face_group")) throw std::runtime_error("give edges or face_group, not both");
    if (given("edges")) {
        const std::vector<int> edges = r.id_list("edges", refs["edges"], true);
        g.dressup_edge  = edges.front();
        g.dressup_edges = edges.size() > 1 ? edges : std::vector<int>();
    }
    if (given("face_group")) {
        const std::optional<FaceGroup> group = face_group_named(refs["face_group"]);
        if (!group) throw std::runtime_error("face_group must be " + kFaceGroupWords);
        g.face_group    = *group;
        g.dressup_edge  = -1;
        g.dressup_edges.clear();
    }
    if (given("shell_face")) {
        g.shell_face = r.integer("shell_face", refs["shell_face"], "a face id from body_describe, or -1 for a closed hollow");
        if (g.shell_face < -1) throw std::runtime_error("shell_face must be a face id from body_describe, or -1 for a closed hollow");
    }
    if (given("draft_face")) g.draft_face = r.id_list("draft_face", refs["draft_face"], false).front();
    if (given("thicken_face")) g.thicken_face = r.id_list("thicken_face", refs["thicken_face"], false).front();
    if (given("delete_faces")) {
        if (!refs["delete_faces"].is_array()) throw std::runtime_error("delete_faces must be a list of face ids from body_describe");
        g.delete_faces = r.id_list("delete_faces", refs["delete_faces"], true);
    }

    // The faces and edges the feature works on, of its target body as it stood before the feature, whichever of the
    // two changed.
    const bool retarget = given("target_body");
    const int  shown    = g.target_body;
    if ((g.type == T::Fillet || g.type == T::Chamfer) && (retarget || given("edges")) && !g.dressup_edge_ids().empty())
        r.on_body(g.dressup_edge_ids(), r.target(shown), true);
    if (g.type == T::Shell && (retarget || given("shell_face")) && g.shell_face >= 0) r.on_body({g.shell_face}, r.target(shown), false);
    if (g.type == T::Draft && (retarget || given("draft_face"))) r.on_body({g.draft_face}, r.target(shown), false);
    if (g.type == T::Thicken && (retarget || given("thicken_face"))) r.on_body({g.thicken_face}, r.target(shown), false);
    if (g.type == T::DeleteFace && (retarget || given("delete_faces"))) r.on_body(g.delete_faces, r.target(shown), false);
}

} // namespace

void cad_apply_feature_refs(CadFeature& f, const json& refs, const CadDocument& before, const CadDocument& doc, int index)
{
    using T                = CadFeatureType;
    const std::string type = feature_type_name(f.type);
    if (f.type == T::Extrude) throw std::runtime_error("Extrude: its profile and its target are fixed");
    if (!refs.is_object() || refs.empty()) throw std::runtime_error("refs must name the references to change, as {\"sweep_path_ref\": 2}");
    const std::vector<std::string> names = ref_names(f.type);
    std::string                    list;
    for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
    for (const auto& [name, v] : refs.items())
        if (std::find(names.begin(), names.end(), name) == names.end())
            throw std::runtime_error(names.empty() ? type + ": its card picks nothing again, so refs takes no name; '" + name + "' is not one"
                                                   : type + " takes " + list + " in refs, not '" + name + "'");
    const bool      datum = f.type == T::Plane || f.type == T::Axis || f.type == T::CoordSys;
    const RefReader r{datum ? doc : before, doc, index, datum, feature_label(doc, index)};
    CadFeature      g = f;
    if (datum) apply_datum_refs(g, refs, r);
    else       apply_model_refs(g, refs, r);
    f = std::move(g);
}

std::vector<int> cad_commit_to_plate_replace(const json& replace, const Model& model)
{
    if (!replace.is_array()) throw std::runtime_error("replace takes a list of object indices (object_list)");
    const int count = int(model.objects.size());
    std::vector<int> out;
    for (const json& v : replace) {
        if (!v.is_number_integer()) throw std::runtime_error("replace takes object indices (object_list), not " + v.dump());
        const auto i = v.get<std::int64_t>();
        if (i < 0 || i >= count)
            throw std::runtime_error("object " + std::to_string(i) + " is out of range; the project has " + std::to_string(count) +
                                     " objects");
        if (std::find(out.begin(), out.end(), int(i)) != out.end())
            throw std::runtime_error("object " + std::to_string(i) + " is listed twice in replace");
        // Deleting one opens the "part of a cut object" warning, which the person answers.
        if (model.objects[size_t(i)]->is_cut())
            throw CadPrecondition("object " + std::to_string(i) +
                                     " is part of a cut object; delete it with object_select, then object_delete, which asks first");
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

std::function<int(const std::string&)> cad_surface_revolve(CadDocument& doc, const json& params)
{
    const int    si          = sketch_arg(params.value("sketch", json()), doc);
    const double angle       = params.value("angle", 360.0);
    const int    axis        = revolve_axis_arg(params);
    const bool   flip        = params.value("flip", false);
    const int    axis_entity = revolve_axis_entity_arg(params, doc, si);
    return [&doc, si, angle, axis, flip, axis_entity](const std::string& name) {
        const int r = doc.add_surface_revolve(si, angle, axis, name);
        doc.features[r].flip                = flip;
        doc.features[r].revolve_axis_entity = axis_entity;
        return r;
    };
}

json cad_align_types(const CadDocument& doc, const json& params)
{
    for (const char* k : {"cs_a", "cs_b"})
        if (!params.value(k, json()).is_number_integer())
            throw std::runtime_error(std::string("list_align_types needs '") + k + "' (CoordSys feature index)");
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
    if (!pv.is_number_integer()) throw std::runtime_error("path must be the feature index of a sketch or a helix (describe_design)");
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
        if (!v.is_number_integer()) throw std::runtime_error("profiles must hold sketch feature indices (describe_design)");
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

}}} // namespace Slic3r::GUI::Mcp
