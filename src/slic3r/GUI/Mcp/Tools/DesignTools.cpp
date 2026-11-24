// The place Design, the Design tab's commands, and its Document heading: describe_design, select_geometry, confirm_tool
// and cancel_tool, what the tab shows and has open; import_step and import_mesh; set_variable; and commit_to_plate,
// new_design and export_step. The Design table (kDesignTools, generated from scripts/CAD/tool_atlas.json) declares
// every command, its menu item, handler, access, tool block and the items folded into it; silent_forms() binds each
// block to the function that runs it, which is in the file of its heading (DesignSketchTools.cpp for Sketch,
// DesignSolidTools.cpp for Solid and so on). register_design_tools() makes a command of each entry that is one
// (is_design_command()): a call with the arguments that choose one of its items does what that menu item does
// (opened_item(), open_tool()), and any other has run_design_tool() run its silent form on the UI thread, through the
// GUI's own CadDocument and DesignPanel's mcp_* hooks, behind the Design tab's busy and stale-id checks
// (design_gate()). The helpers the other headings' files share are here too, declared in DesignCommon.hpp.

#include "slic3r/GUI/Mcp/McpToolsDesign.hpp"
#include "slic3r/GUI/Mcp/Tools/DesignCommon.hpp"
#include "libslic3r/Point.hpp"
#include <stdexcept>
#include <math.h>
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
#include <functional>
#include <initializer_list>
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
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpPaths.hpp"
#include "slic3r/GUI/Mcp/UiInspector.hpp"   // gizmo_name: the name app_state reports
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"
#include "slic3r/GUI/CAD/DesignOffer.hpp"   // the Design table: every command's menu item, handler and tool block

#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r_version.h"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include "libslic3r/BoundingBox.hpp"

#include <TopoDS_Shape.hxx>

using json = nlohmann::json;

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// ---- What the commands' descriptions and schemas add to the Design table ------------------------

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
// design_gate()'s refusal while the CAD feature is off in a run that started with it on.
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

} // namespace

// ---- Shared with the other Design files (DesignCommon.hpp) --------------------------------------

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

json placement_json(const DesignPanel& panel, int b)
{
    const Transform3d t = panel.mcp_body_placement(b);
    if (t.isApprox(Transform3d::Identity())) return json();
    json rows = json::array();
    for (int r = 0; r < 3; ++r)
        rows.push_back(json::array({t(r, 0), t(r, 1), t(r, 2), t(r, 3)}));
    return rows;
}

json frame_json(const SketchPlane& p)
{
    return json{{"origin", vec3(p.origin)}, {"normal", vec3(p.normal)},
                {"x_axis", vec3(p.x_axis)}, {"y_axis", vec3(p.y_axis)}};
}

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

json add_feature(DesignPanel* panel, const char* index_key, const std::function<int()>& add, bool slow)
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

std::string input_path(const json& value, const std::string& what)
{
    if (!value.is_string()) throw std::runtime_error(what + " must be an absolute path");
    std::string err;
    const auto path = Mcp::resolve_input_path(value.get<std::string>(), err);
    if (!path) throw std::runtime_error(err);
    return Mcp::path_to_utf8(*path);
}

void check_feature_index(int fi, const CadDocument& doc)
{
    if (fi < 0 || fi >= int(doc.features.size()))
        throw std::runtime_error("feature " + std::to_string(fi) + " is out of range; the design has " +
                                 std::to_string(doc.features.size()) + " features");
}

json param(const json& params, const char* key)
{
    return params.is_object() ? params.value(key, json()) : json();
}

namespace {

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
        // is refused if the topology has moved on. See the stale-id guard in design_gate().
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

// ---- Build: bring a reference part in (Import STEP as native B-rep bodies) ----------------------
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

// ---- Import a triangle mesh as a B-rep body (GeometryEngine::mesh_to_brep) ----------------------
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

// ---- Hand-off: Commit to Plate, New Design, Export STEP -----------------------------------------

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
        objects.push_back(json{{"index", int(it - model.objects.begin())}, {"object_id", o->id().id}, {"name", o->name},
                               {"parts", o->volumes.size()}});
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

} // namespace

// Every silent form, by the id of the Design table's entry whose tool block describes it: run_design_tool() and
// register_design_tools() read these, and the block gives its access. The Document heading's are here, the other
// headings' beside their functions.
const std::vector<SilentForm>& silent_forms()
{
    static const std::vector<SilentForm> forms = [] {
        std::vector<SilentForm> all = {
            {"describe_design",              [](DesignPanel* panel, const json&) { return describe_design(panel); }},
            {"select_geometry",              action_select_geometry},
            {"confirm_tool",                 action_confirm_tool},
            {"cancel_tool",                  action_cancel_tool},
            {"import_step",                  import_step},
            {"import_mesh",                  import_mesh},
            {"set_variable",                 action_set_variable},
            {"commit_to_plate",              action_commit_to_plate},
            {"new_design",                   action_new_design},
            {"export_step",                  action_export_step},
        };
        sketch_silent_forms(all);
        solid_silent_forms(all);
        surface_silent_forms(all);
        construct_silent_forms(all);
        inspect_silent_forms(all);
        body_silent_forms(all);
        history_silent_forms(all);
        return all;
    }();
    return forms;
}

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

std::optional<std::uint64_t> design_generation(const json& args)
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

// The checks every Design command passes before its function, in this order: the main window is up (not
// shutting down or still starting), the app is the editor and not the G-code viewer (design_unavailable),
// the CAD feature is on, and the Design panel can be built (design_unavailable for each); then the busy rules of
// `access` (design_busy, with a hint naming confirm_tool and cancel_tool; with Read, None and Ui only a rebuild's), the
// project's recipe loaded into an empty document (DesignPanel::hydrate_from_model()), as showing the tab would, and a
// `generation` in `args` that design_generation() refuses (invalid_argument) or that is not the model's (stale_ids).
// Returns the panel. Throws ToolFailure, whose details carry the code. UI thread only.
DesignPanel* design_gate(CadAccess access, const json& args);

// The silent form of entry `id`, or nullptr.
DesignRun find_silent_form(const std::string& id)
{
    for (const auto& [key, run] : silent_forms())
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
// its Speed Dial action runs it: past design_gate() with no access of its own, so that only a rebuild refuses it, as
// the Design tab's keys and menu are refused by nothing; each tool decides for itself what an open card means to it.
// The handler runs in place, so a modal dialog it opens holds the call, which the core reports as blocked by that
// dialog.
// It is refused, as open_refusal() says, in the mode the item does not belong to, where the right-click menu does not
// offer it, and for a btn: or fly: handler a selection the menu greys the item for. The reply claims nothing about
// what the handler did, which may be to open a card, start a sketch tool, act at once or tell the person why it
// cannot: it says what the tab shows after it, its busy reason (a card, a dialog or a sketch now open) and its
// status line. Its `tool` is the command called. A call that is not a person's pick, made while a row of the Feature
// list is selected and nothing is picked in the view, targets that row: an item whose handler acts on it (feature_row)
// is not refused for the selection then, and applies. A person's pick keeps the menu's rule, which greys the item.
json open_tool(const DesignTool& item, const char* command, const json& args, bool person)
{
    DesignPanel* panel      = design_gate(CadAccess::None, args);
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
NativeCommand table_command(const DesignTool& t)
{
    const json doc = t.doc != nullptr ? json::parse(t.doc) : json();
    json       schema;
    if (doc.is_null())
        schema = {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}};
    else
        schema = design_input_schema(doc);
    // The person's form is the call with no arguments where that opens an item's tool, the entry's own or one folded
    // into it: it requires nothing (with_silent()), and runs in that item's mode only.
    const DesignTool* person_item = opened_item(t, json::object());
    if (person_item != nullptr && !doc.is_null()) {
        schema = with_silent(std::move(schema));
        schema["properties"]["silent"]["description"] = std::string(kSilentNote) + " " + mode_note(*person_item, true);
    }

    const DesignTool* entry = &t;
    NativeCommand     c     = design_command(
        t.id, t.name, std::move(schema),
        [entry](const json& args, bool person) {
            if (const DesignTool* item = opened_item(*entry, args))
                return open_tool(*item, entry->id, args, person);
            return run_design_tool(entry->id, silent_args(args));
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
    // commit_to_plate's replace_ids names Prepare's objects by id.
    return with_target_ids(std::move(c));
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

namespace {

DesignPanel* design_gate(CadAccess access, const json& args)
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
    // The type is checked first (design_generation()), as a caller that skips the schema (run_design_tool() called
    // directly) may pass any: a bare get<uint64_t>() on `"generation": "x"` would surface as an internal error
    // instead of a parameter error.
    if (const std::optional<std::uint64_t> given = design_generation(args)) {
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

} // namespace

json run_design_tool(const std::string& id, const json& args)
{
    const DesignTool* entry = find_design_tool(id);
    const DesignRun      run   = find_silent_form(id);
    if (entry == nullptr || run == nullptr)
        throw ToolFailure("No Design command runs silently as " + id, {{"code", "no_silent_form"}});
    DesignPanel* panel = design_gate(entry->access, args);
    return run_guarded([&] { return run(panel, args); });
}

json design_undo_redo(bool redo)
{
    DesignPanel* panel = design_gate(CadAccess::Sketch, json::object());
    return run_guarded([&] { return undo_redo(panel, redo); });
}

json design_input_schema(const json& block)
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

NativeCommand design_command(std::string key, std::string title, json schema,
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

void register_design_tools(CommandCatalog& catalog)
{
    // Heading by heading, as tool_list's description indexes them, so that its listing of the group reads in the
    // same order. Every command has a heading (DesignTool::section).
    for (const char* section : kDesignSections)
        for (const DesignTool& t : kDesignTools)
            if (is_design_command(t) && t.section != nullptr && std::strcmp(t.section, section) == 0)
                catalog.add(table_command(t));
}

void leave_out_design_tools(CommandCatalog& catalog, bool editor)
{
    // A folded item's id too, which is no command but names an action.
    for (const DesignTool& t : kDesignTools)
        if (t.action != nullptr || t.doc != nullptr)
            catalog.leave_out(t.id, editor ? kCadOffAtStartup : kNoDesignInViewer, "design_unavailable");
}

std::string design_command_index()
{
    std::string index = " The Design tab's CAD commands (sketches, solid and surface features, bodies, measurements) follow "
                        "by heading; `tool_list {group: \"Design\"}` says what each does and `tool_describe {tool}` gives one's "
                        "manual. One whose schema has `silent` does what its menu item does when called with no arguments, "
                        "unless `silent` is true.";
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

}}} // namespace Slic3r::GUI::Mcp
