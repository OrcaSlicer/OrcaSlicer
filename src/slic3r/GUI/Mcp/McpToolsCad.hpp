#pragma once

// The Design tab's commands, one per entry of its table (kDesignTools, DesignOffer.hpp, generated from
// scripts/CAD/tool_atlas.json) but align: its tools, sketches, features, measurements and edits, which an
// agent finds with search_tools (group Design) and runs with call_tool, and a person picks as Speed Dial
// tiles while the Design tab is shown, those of the mode it is in. With no arguments, an entry with an
// action does what the right-click menu's row does, in the mode the row belongs to (open_refusal()); with
// arguments, or silent: true, an entry with a tool block runs that block's silent form. They run on the UI
// thread through the GUI's own CadDocument (no engine of their own), behind one gate (cad_gate()): every
// command is refused (-32002) during a rebuild, whose worker owns the document, and a silent form that
// changes the document also while the Design tab is busy with it, an open feature card or a sketch session;
// face and edge ids read at another generation are refused (-32010). The catalog holds the commands only
// while the CAD feature is on (NativeCommands::register_all_commands()). The unit tests call the cad_*
// helpers directly.

#include "slic3r/GUI/CAD/DesignOffer.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class CadDocument;
struct CadFeature;
struct SketchEntity;
struct SketchEntityConstraintDef;
struct SketchPlane;
struct SketchProfile;
class Model;

namespace GUI {

class DesignPanel;
class DesignSketchTool;

namespace Mcp {

// A Design command's silent form: what it does given its arguments, on the panel cad_gate() returned.
using CadRun = nlohmann::json (*)(DesignPanel* panel, const nlohmann::json& params);

// The silent forms, by the id of the table's entry whose tool block describes them: one per block.
extern const std::pair<const char*, CadRun> kSilentForms[];
extern const std::size_t                    kSilentFormCount;

// A Design command: group Design, not listed, its function run on the UI thread. For a person's pick
// (ToolContext::person()) `fn` runs in place, and a refusal it throws is shown in the UI's language where its
// text is a msgid (an entry's refusal). For an automated call the journal records the arguments without
// `generation`, which numbers ids within this session only, and with `silent`, which it adds where `schema`
// takes it and the call gave nothing else, so that a replay runs the silent form too; then `fn` runs in one
// UI closure and the call settles. The reply is {"result": fn's value}; a ToolFailure is the call's error.
NativeCommand cad_command(std::string key, std::string title, nlohmann::json schema,
                          std::function<nlohmann::json(const nlohmann::json& args)> fn);

// Adds the Design commands to `catalog`, one per entry of kDesignTools that has an action or a tool block:
// its id as the key, its name as the title, and its icon. An entry with an action is in the palette (its tile
// is materialised only while the Design tab is shown in the entry's mode,
// ActionRegistry::materialize_design_actions()). One with a block is read_only for Read access, and its
// description is the block's summary, details and, for a silent form that reports a rejected rebuild in its
// reply, how that shows; its examples are the block's and its schema cad_input_schema() of it, through
// with_silent() when the entry has an action too. One with an action only takes no arguments, and its
// description is its hint and what the call does. undo and redo are the shared commands of NativeCommands.cpp
// and not among them.
void register_cad_commands(CommandCatalog& catalog);

// The input schema of a tool block: an object whose properties are the block's params, a type written "a|b"
// as the list of both, min, exclusive_min and max as minimum, exclusiveMinimum and maximum, enum and default
// as written, and the unit joined to the description; a param with no default is required. `generation`, the
// generation the call's face and edge ids were read at, is an optional property of every one. Arguments it
// does not name are not refused: a tool may read more, such as a param in radians also in degrees under its
// name with _deg.
nlohmann::json cad_input_schema(const nlohmann::json& block);

// The checks every Design command passes before its function, in this order: the main window is up (not
// shutting down or still starting), the app is the editor and not the G-code viewer (-32001), the CAD
// feature is on, and the Design panel can be built (-32001); then the busy rules of `access` (-32002; with
// None only a rebuild's), the project's recipe loaded into an empty document
// (DesignPanel::hydrate_from_model()), as showing the tab would, and a `generation` in `args` that is not an
// unsigned integer (-32602) or not the model's (-32010). Returns the panel. Throws ToolFailure, whose details
// carry the code. UI thread only.
DesignPanel* cad_gate(CadAccess access, const nlohmann::json& args);

// The Design table's entry `id`, or nullptr.
const DesignTool* find_design_tool(const std::string& id);

// Whether the Design tab offers entry `t` in the mode it is in, `sketching` being the right-click menu's
// DesignPanel::sketch_map_applies(): an entry of the sketch's vocabulary in a sketch, one of the model's
// outside it, as the menu offers them. A sketch in Constrain mode is a sketch to the menu, though the tab's keys
// there are the model's. Only then is its tile shown and its person's form run.
bool offered_in_mode(const DesignTool& t, bool sketching);

// The commands of `catalog` that have a Speed Dial tile while the Design tab is shown in the mode `sketching`
// names (as offered_in_mode() takes it), in the catalog's order: those of group kDesignGroup with in_palette whose
// entry is offered in that mode. None while the CAD feature is off (`cad_on` false): the tab stays until a
// restart, but cad_gate() would refuse every pick of its tiles.
std::vector<const NativeCommand*> design_tiles(const CommandCatalog& catalog, bool sketching, bool cad_on);

// Why the person's form of entry `t` is refused before its action runs, or nothing: `t` is not offered in the
// tab's mode (offered_in_mode()), and the message says which mode it belongs to and what to do; or `t`'s
// action is a btn: or fly: one, which the menu greys for a selection it does not accept, and selection `kind`
// (OfferSel as int) is not one it accepts, and the details name the selection and those it accepts. A key:
// action runs whatever is selected, as its key does. The message is a msgid, the code -32000.
std::optional<ToolFailure> open_refusal(const DesignTool& t, bool sketching, int kind);

// Runs the silent form of Design command `id` with `args` past cad_gate() with its entry's access, and
// returns its result. One that fails is -32000, with "OCCT: " before a kernel exception's message; an id with
// no silent form is -32601. UI thread only.
nlohmann::json run_cad_tool(const std::string& id, const nlohmann::json& args);

// Edit > Undo, or Redo, in the Design tab, past cad_gate() with Sketch access: the Design history, or in a
// sketch session the sketch's own, which takes off or puts back the last drawn shape. Returns {ok, scope}
// with "sketch" and the sketch's entities, or "design" and the undo and redo depth, features, bodies and
// generation. Fails (-32000) when there is nothing to take back or put back. UI thread only.
nlohmann::json cad_undo_redo(bool redo);

// get_app_state's cad_note: how a person turns the CAD feature on, in the editor while the Design commands
// cannot run, because `catalog` holds none of them (the feature was off at startup) or the feature is off
// now; null otherwise. UI thread.
nlohmann::json cad_note(const CommandCatalog& catalog);

// The `plane` param shared by the plane-taking Design commands: "XY", "XZ" or "YZ" through the modeling
// origin, {"datum": feature index}, or {"body": b, "face": f} on a planar face. A face gives the
// sketch-on-face plane (outward normal), or with `drill` the Hole card's inward plane; `face_body`
// then receives b. Throws std::runtime_error with the message the call fails with.
SketchPlane cad_resolve_plane(const nlohmann::json& plane, const CadDocument& doc, bool drill,
                              int* face_body = nullptr);

// describe_feature's reply for features[index]: what its card shows (`params`), the fields
// set_feature takes (`editable`) with the card's limits (`ranges`), its expression bindings and
// references, a sketch's geometry, and the features (`used_by`) and bodies (`bodies_made`) it
// feeds. Throws std::runtime_error for an index out of range.
nlohmann::json cad_feature_json(const CadDocument& doc, int index);

// set_feature's edit: overwrite the fields of `f` named in `fields` ({name: value}). Throws
// std::runtime_error, leaving `f` as it was, for a field its card does not edit, a field bound to
// an expression, a value of the wrong type, or one outside the card's range.
void cad_apply_feature_fields(CadFeature& f, const nlohmann::json& fields);

// commit's `replace`: the model's object indices to delete once the design is on the plate, sorted
// ascending. Throws std::runtime_error for a value that is not an index, one out of range or
// repeated, and an object that is part of a cut (deleting it asks the person first).
std::vector<int> cad_commit_replace(const nlohmann::json& replace, const Model& model);

// sketch_describe {sketch}: committed sketch features[sketch] as the open sketch is described, its
// entities, constraint count and loops, numbered as extrude's `loop` takes them; `tool` only
// finds the loops. Throws std::runtime_error for an index that is not a sketch with entities.
nlohmann::json cad_sketch_describe(const CadDocument& doc, const DesignSketchTool& tool, int sketch);

// The Design commands that work on the open sketch alone, run on `tool`'s session: sketch_add,
// sketch_fillet, sketch_chamfer, sketch_trim, sketch_transform, sketch_constrain,
// sketch_constraints (the open sketch's) and sketch_unconstrain. Each runs the sketch tool's own
// step for the button or gesture it stands for, checks every argument before it changes anything,
// and throws std::runtime_error with the reason, leaving the sketch as it was. Needs no Design panel.
nlohmann::json cad_sketch_call(DesignSketchTool& tool, const std::string& name, const nlohmann::json& params);

// sketch_constrain's constraints on the open sketch's entities `ents`, as the CONSTRAIN bar would
// add them. params.type is a SketchConstraintType in snake_case ("equal_length"; "equal" is the
// bar's Equal). With params.entities (or else `selection`) the bar's planner chooses the points
// (plan_entity_constraint), and a value it would ask for is params.value or the current one; with
// params.refs the references are taken as written, {entity, role} for a point ("origin" too) and
// {entity} for a whole entity ("x_axis", "y_axis" too), and params.value is required where the type
// carries one. Values are mm, or degrees for an angle. Throws std::runtime_error with the reason.
std::vector<SketchEntityConstraintDef> cad_constraint_defs(const nlohmann::json& params, const std::vector<SketchEntity>& ents,
                                                           const std::vector<int>& selection);

// extrude {sketch} and revolve {sketch}: check params against `doc` and return the call that adds,
// without rebuilding, the feature the Extrude or Revolve card adds for the committed sketch
// params.sketch; it takes the feature's name and returns its index. Checking first lets a bad
// argument fail before the caller takes an undo checkpoint. Extrude takes the whole sketch, or with
// params.loop one of its loops with the loops inside it; its params.boolean "auto" (the default)
// joins the body the sketch touches, else makes a new body, as the card's default does. Revolve's
// axis is params.axis_entity, a line of the sketch, else the sketch's only centerline when
// params.axis is not given, else the plane axis params.axis. Throws std::runtime_error.
std::function<int(const std::string& name)> cad_sketch_extrude(CadDocument& doc, const DesignSketchTool& tool,
                                                               const nlohmann::json& params);
std::function<int(const std::string& name)> cad_sketch_revolve(CadDocument& doc, const nlohmann::json& params);

// The profile revolve turns when it is given no sketch: params.profile, else a params.width by
// params.height rectangle (default 20 x 10) beside the plane axis params.axis, one edge on it, the
// width along it centred on the origin and the height out on its positive side, so a full turn is a
// cylinder `width` long of radius `height`. Throws std::runtime_error.
SketchProfile cad_revolve_profile(const nlohmann::json& params);

// extrude {face}: the Extrude card's push/pull of a picked face. Checks params against `doc` and
// returns the call that adds the face extrude of params.face, a planar face of params.body (default
// the last body), joined to that body unless params.boolean says otherwise; `frame` receives the
// face's plane. Throws std::runtime_error.
std::function<int(const std::string& name)> cad_face_extrude(CadDocument& doc, const nlohmann::json& params,
                                                             SketchPlane& frame);

// query_topology's reply for body params.body of `doc`: its faces (kind, area, centroid, normal and
// the numbers of an analytic surface) and edges (kind, length, ends), narrowed by the face filters
// (kinds, direction, at_extreme) and edge filters (edge_kinds, direction, convex); with adjacency,
// the faces' edges and the edges' faces and convexity, and a list whose filters were not given holds
// the neighbours of the other's matches. Both lists are paged by params.limit and params.offset.
// Throws std::runtime_error.
nlohmann::json cad_query_topology(const CadDocument& doc, const nlohmann::json& params);

// measure's reply for refs params.a and params.b, each a face, an edge (of its own body, else
// params.body), a point, or a cylindrical face's axis: the distance between their representative
// points, the minimum distance and the closest points, their angle and, for two parallel planar
// faces, the gap between them. Throws std::runtime_error naming the ref that does not resolve.
nlohmann::json cad_measure(const CadDocument& doc, const nlohmann::json& params);

// Where a hole command drills: `plane` from params.plane (a face gives the Hole card's inward plane),
// `target_body` the body (a face plane's own body, else params.body, else -1 for the kernel's last
// body), and the reply's {body, x, y}: params.x and params.y, or params.at (a world point, or a
// cylindrical face or circular edge whose axis the hole shares) projected onto the plane, with
// `off_plane`, how far it was moved. Throws std::runtime_error.
nlohmann::json cad_hole_placement(const CadDocument& doc, const nlohmann::json& params, SketchPlane& plane,
                                  int& target_body);

// plane: check params against `doc` as the Plane card's Confirm does (CadDocument::datum_plane_refusal)
// and return the call that adds the datum plane, without rebuilding. Throws std::runtime_error.
std::function<int(const std::string& name)> cad_datum_plane(CadDocument& doc, const nlohmann::json& params);

// thread, sweep, loft and split: check params against `doc` as their cards' Confirm does and return
// the call that adds the feature without rebuilding, as cad_datum_plane does.
// thread: on params.face (a cylindrical face) or params.edge (a circular edge) of params.body, along
// its axis (CadDocument::thread_target), internal for a bore; else on params.plane at x/y or `at`,
// placed as a hole is (cad_hole_placement), with params.internal required. The size is
// params.designation's, else the standard nearest the cylinder (nearest_thread_standard), else a
// custom params.diameter and params.pitch; diameter, pitch and depth override it, and length
// defaults to the face's. `resolved` receives what the call will build: designation (null when
// custom), internal, diameter, pitch, depth, length, x, y and the frame.
// sweep: committed sketch params.sketch along params.path, a sketch or a helix.
// loft: through the sketches params.profiles, in order, params.ruled.
// split: body params.body with params.plane, moved params.offset along its normal and turned over
// by params.flip (a Cut feature), or along the plane of params.face (of params.face_body); it keeps
// the pieces on the plane's normal side (keep_upper) and the other (keep_lower), and `resolved`
// receives the body split, the indices the pieces will have (upper_body, lower_body) and, for a
// plane, the frame cut along. A plane clear of the body is refused.
// Each throws std::runtime_error.
std::function<int(const std::string& name)> cad_thread(CadDocument& doc, const nlohmann::json& params, nlohmann::json& resolved);
std::function<int(const std::string& name)> cad_sweep(CadDocument& doc, const nlohmann::json& params);
std::function<int(const std::string& name)> cad_loft(CadDocument& doc, const nlohmann::json& params);
std::function<int(const std::string& name)> cad_split(CadDocument& doc, const nlohmann::json& params, nlohmann::json& resolved);

}}} // namespace Slic3r::GUI::Mcp
