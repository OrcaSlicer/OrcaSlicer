#pragma once

// What the files of the Design commands share, and nothing else uses: the rest of OrcaSlicer and the tests use
// McpToolsDesign.hpp. Each Design heading has its file: DesignTools.cpp for Document, DesignSketchTools.cpp for Sketch,
// DesignSolidTools.cpp for Solid and so on. Each helper here is in the section of the file that defines it:
// DesignTools.cpp's serve every heading, the others' the headings that read the same arguments or describe the same
// things.

#include "slic3r/GUI/Mcp/McpToolsDesign.hpp"
#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/Point.hpp"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// ---- The helpers every heading uses: DesignTools.cpp's, and the inline ones here ------------

// A feature's type as the Design commands name it (design_describe's features[].type): its CadFeatureType
// enumerator's name.
const char* feature_type_name(CadFeatureType t);

// Where body `b` stands for Commit to Plate and Export STEP (body_place, the Move gizmo, Place on Face): null where the
// model has it, else three rows of four numbers, the rotation and the translation in mm, in the world frame.
nlohmann::json placement_json(const DesignPanel& panel, int b);

// A vector or point as the Design commands give one: [x, y, z].
inline nlohmann::json vec3(const Vec3d& v) { return nlohmann::json::array({v.x(), v.y(), v.z()}); }

// Every reply that used a plane says where it was: a face plane's x axis is not something a caller
// can guess, and the XZ base plane is left-handed (x cross y is -normal), as the GUI stores it.
nlohmann::json frame_json(const SketchPlane& p);

// A plane's frame, with `base` when it is the XY, XZ or YZ plane through the modeling origin
// (whatever its origin within that plane): the name the plane list gives it.
nlohmann::json plane_json(const SketchPlane& p, const Vec3d& modeling_origin);

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
nlohmann::json add_feature(DesignPanel* panel, const char* index_key, const std::function<int()>& add, bool slow = false);

// A file the call reads, checked as every path the MCP server takes: absolute UTF-8 (a relative
// one would resolve against the GUI's working directory) and existing. Returns its UTF-8 form.
std::string input_path(const nlohmann::json& value, const std::string& what);

// The Design commands' names of SketchConstraintType's values, in enum order. SketchConstraintType is
// append-only (recipes store it by position), and so is this list.
inline constexpr const char* kConstraintTypes[] = {
    "fix", "coincident", "horizontal", "vertical", "distance", "lock_x", "lock_y", "equal_length", "parallel",
    "perpendicular", "concentric", "tangent", "midpoint", "symmetric", "angle", "radius", "diameter", "point_on_line",
    "point_on_object", "equal_radius", "collinear", "distance_x", "distance_y", "symmetric_about_y", "symmetric_about_x"};
static_assert(std::size(kConstraintTypes) == size_t(SketchConstraintType::SymmetricAboutX) + 1,
              "every constraint type needs a name");

// The name of value `i` of an enum in `names`, one of the Design commands' tables of names (kConstraintTypes, a
// file's own): a recipe is read from a file, so an enum value may be one this build has no name for.
template<size_t N> const char* enum_name(const char* const (&names)[N], int i)
{
    return i >= 0 && i < int(N) ? names[i] : "unknown";
}

// The position of `v` in `names`, or an error naming the choices.
template<size_t N> int enum_index(const std::string& key, const nlohmann::json& v, const char* const (&names)[N])
{
    for (size_t i = 0; i < N; ++i)
        if (v.is_string() && v.get<std::string>() == names[i]) return int(i);
    std::string list;
    for (const char* n : names) list += (list.empty() ? "" : ", ") + std::string(n);
    throw std::runtime_error(key + " must be one of " + list);
}

// Throws std::runtime_error unless `fi` is the index of one of `doc`'s features.
void check_feature_index(int fi, const CadDocument& doc);

// params[key], or null when it is absent or params is not an object.
nlohmann::json param(const nlohmann::json& params, const char* key);

// ---- DesignSketchTools.cpp: a sketch feature as the model's headings describe it ------------

// Entity `e` of a sketch, its `index`th, as sketch_describe and feature_describe give it.
nlohmann::json sketch_entity_to(const SketchEntity& e, int index);

// A constraint's value as the Design commands state it: an angle in degrees (the recipe holds radians), a
// length in mm. Null for the types that carry none.
nlohmann::json constraint_value(SketchConstraintType t, double v);

// An entity constraint's references, as the solver reads them: a point names its entity and role,
// a whole entity (a line's direction, a circle's radius) only the entity.
nlohmann::json entity_constraint_json(const SketchEntityConstraintDef& c, int index);

// The `sketch` a Design command reads or builds from: a Sketch feature, or a Project feature, whose
// entities the kernel builds as a sketch's.
int sketch_arg(const nlohmann::json& v, const CadDocument& doc);

// ---- The names the model's headings share, in enum order ------------------------------------

// What a feature's result does to the bodies already there, in BooleanMode order: the commands' `operation`, in the
// words of Fusion's dialogs and the Extrude card.
inline constexpr const char* kOperations[] = {"new_body", "join", "cut", "intersect"};

// The names the Solid and Construct headings' commands take and the History heading's feature_describe and
// feature_edit show and set.
inline constexpr const char* kExtrudeEnds[]    = {"blind", "symmetric", "two_sided", "through_all", "up_to_face", "up_to_vertex"};
inline constexpr const char* kFaceGroups[]     = {"top", "bottom", "lateral", "all"};
inline constexpr const char* kPlaneTypes[]     = {"offset", "angle", "midplane", "tangent", "two_edges", "coincident"};
inline constexpr const char* kHoleTypes[]      = {"simple", "counterbore", "countersink"};    // CadFeature::hole_style
inline constexpr const char* kAlignTypes[]     = {"fastened", "planar", "revolute", "slider", "cylindrical"};   // CadFeature::mate_kind
inline constexpr const char* kPatternTypes[]   = {"linear", "circular", "curve"};             // a Pattern's layout

// kFaceGroups as a refusal lists them.
inline const std::string kFaceGroupWords = "top, bottom, lateral or all";

// ---- DesignInspectTools.cpp: the kinds of a body's faces and edges --------------------------

// What kind of curve an edge is, for the messages that refuse one.
const char* edge_kind_name(const TopoDS_Edge& e);

// What kind of surface a face is, for the messages that refuse one.
const char* face_kind_name(const TopoDS_Face& f);

// ---- DesignConstructTools.cpp: where a datum plane stands -----------------------------------

// A shown Plane feature's place among the datum planes: the kernel lists the enabled Plane features
// in order, and a datum reference holds 3 + that position.
int datum_ordinal(const CadDocument& doc, int fi);

// ---- DesignSolidTools.cpp: the arguments other headings read as Solid's commands do ---------

// Resolve an optional explicit body target. Default (no `body`, or <0) = last body, which is
// what the kernel picks anyway. Validated BEFORE any checkpoint so a bad index throws clean.
int target_body_arg(const nlohmann::json& params, const CadDocument& doc);

// The edge group a word names, the Fillet and Chamfer cards' Edges choice, or nothing for any other value.
std::optional<FaceGroup> face_group_named(const nlohmann::json& v);

// The `axis` a revolve turns about when it names no line of its sketch: 0 for the sketch plane's X axis, 1 for
// its Y axis.
int revolve_axis_arg(const nlohmann::json& params);

// Whether entity `e` of `ents`, a sketch's entities, is a line: what a revolve turns about and a rib grows from.
bool revolve_line(const std::vector<SketchEntity>& ents, int e);

// The line of sketch `si` a revolve of it turns about, or -1 for the plane axis: params.axis_entity; without one, the
// sketch's only centerline, as a fresh card offers it, unless the caller chose a plane axis.
int revolve_axis_entity_arg(const nlohmann::json& params, const CadDocument& doc, int si);

// ---- DesignHistoryTools.cpp: what the Feature tree's and the Bodies list's edits share ------

// The panel's tree edits put the features back, leave no undo step and say "Edit rejected" when the
// kernel refuses one; the call then fails with the kernel's reason.
[[noreturn]] void tree_edit_refused(const CadDocument& doc);

// `reply` with what every edit of the Feature tree or the Bodies list reports besides: ok, the feature and body
// counts and the topology generation after it.
nlohmann::json tree_edit_reply(const CadDocument& doc, nlohmann::json reply);

// A `name` as the rows' rename editor takes it: trimmed, and never empty.
std::string name_arg(const nlohmann::json& params);

// ---- Each heading's file but DesignTools.cpp: its silent forms ------------------------------

// Each adds its heading's silent forms to `forms`, which silent_forms() gathers with Document's.
void sketch_silent_forms(std::vector<SilentForm>& forms);
void solid_silent_forms(std::vector<SilentForm>& forms);
void surface_silent_forms(std::vector<SilentForm>& forms);
void construct_silent_forms(std::vector<SilentForm>& forms);
void inspect_silent_forms(std::vector<SilentForm>& forms);
void body_silent_forms(std::vector<SilentForm>& forms);
void history_silent_forms(std::vector<SilentForm>& forms);

}}} // namespace Slic3r::GUI::Mcp
