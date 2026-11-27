#pragma once

// The Design tab's commands, one per entry of its table (kDesignTools, DesignOffer.hpp, generated from
// scripts/CAD/tool_atlas.json) that has a handler or a tool block and is no folded item (is_design_command()): its
// tools, sketches, features, measurements and edits, which an agent finds with tool_list (group Design) and
// runs with tool_call, and a person picks as Speed Dial actions while the Design tab is shown, those of the mode it
// is in. A call with exactly the arguments that choose one of the command's items (opened_item()) does what that
// right-click menu item does, in the mode the item belongs to (open_refusal()); any other call, or one with
// silent: true, runs the entry's tool block's silent form. They run on the UI thread through the GUI's own
// CadDocument (no engine of their own), behind one gate (design_gate(), DesignTools.cpp): every command is refused
// (design_busy) during a rebuild, whose worker owns the document, and a silent form that changes the document also
// while the Design tab is busy with it, an open feature card or a sketch session; face and edge ids read at another
// generation are refused (stale_ids). The catalog holds the commands only while the CAD feature is on
// (NativeCommands::register_all_commands()).
//
// The design_* names here are the place's command machinery: its registration, schema, runs, menu items and undo.
// The cad_* names are the parts of the commands' work that the unit tests call directly, on the model, its sketches
// and features, and the plate; cad_note() is app_state's report of the CAD feature, named for its JSON key.
//
// A failed call's error names its cause in `code`, a word as the other commands' refusals have:
// - design_unavailable: no Design tab to act on (the G-code viewer, the CAD feature off, a panel or a view that is not
//   built);
// - design_busy: a rebuild, an open card or tool or a sketch session holds the document;
// - stale_ids: face or edge ids read at another generation;
// - wrong_mode: a menu item of the other mode (a sketch tool with no sketch open, a model tool in a sketch);
// - wrong_selection: a menu item that does not take what is selected;
// - invalid_argument: an argument is wrong in itself, names nothing, or names an object whose kind or geometry the
//   operation does not take; the caller changes the arguments, or which object they name;
// - precondition: the arguments are right, but the state of what they name, of the design or of the session is not
//   one the operation takes (no body, a body hidden or without a shape, a sketch open or none, a tool open), as
//   CadPrecondition says; the caller changes the design or the session first;
// - rebuild_failed: a rebuild of the model reported failure: with the call's change, which is taken back, or at the
//   earlier state the call reads;
// - kernel_failure: a CAD kernel (OCCT) error reached the command's guard, wherever it was raised, its message after
//   "OCCT: ";
// - export_failed: design_export_step could not transfer or write the STEP file;
// - cut_object: design_commit_to_plate's replace names a part of a cut, which Prepare's Delete refuses too;
// - nothing_deleted: design_commit_to_plate sent the design to the plate and then deleted none of its replace
//   objects; one Prepare undo takes the commit back;
// - no_silent_form: an entry with no silent form called as one (run_design_tool() called directly).

#include "libslic3r/Point.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
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
struct DesignTool;

namespace Mcp {

// The refusal of a call whose arguments are right but whose design or session is in a state the operation does not
// take: no body to work on, the named body hidden or without a shape, no sketch open or one already open, nothing to
// undo, an open value field, tool or gizmo, the feature already first. State is what the Design tab tracks apart
// from the geometry, which a call cannot choose; what a named object is and where it lies (a face that is not planar,
// a plane that misses the body) is the call's choice, an argument error. Its error's code is precondition, where any
// other exception a silent form throws is invalid_argument (run_guarded()): the caller changes the design or the
// session, not the call.
struct CadPrecondition : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// The refusal of a call the CAD kernel could not compute though it threw nothing (a section or a distance it reports
// as not done): kernel_failure, as an OCCT exception is, its message after "OCCT: ".
struct CadKernelFailure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Runs `run`, a silent form or undo and redo, with its failure as the call's error: its own refusal (a ToolFailure)
// with its code (invalid_argument where it names none), a kernel exception or a CadKernelFailure as "OCCT: ..."
// (kernel_failure), a CadPrecondition as
// precondition, and any other exception as invalid_argument, since the silent forms check their arguments with
// plain exceptions.
nlohmann::json run_guarded(const std::function<nlohmann::json()>& run);

// Why there is no Design panel to act on, as design_gate() refuses it, each design_unavailable: the G-code viewer,
// which has no Design tab; the CAD feature off; else a panel that cannot be built (no page made at startup because
// the feature was turned on later, a page mid-build, or a build that failed), which a retry or a restart mends. UI
// thread.
ToolFailure no_design_tab();

// The refusal of a call that finds no Design view to act on (the panel's viewport, Feature list or parts list not
// built), which no change to the call mends: design_unavailable, as design_gate() refuses a panel that cannot be built.
ToolFailure no_design_view();

// The refusal of a call the Design tab is too busy to take, `why` saying with what (DesignPanel::mcp_busy()):
// design_busy, with a hint naming the commands that close what is open.
ToolFailure design_busy(const std::string& why);

// The `generation` a call's face and edge ids were read at, or nothing when `args` gives none: a number the schema's
// integer type takes (is_whole_number(): a JSON integer stored signed or unsigned, since a script's or plugin's Python
// integers reach the command as signed ones, or a whole float within int64) that is not negative. Anything else
// throws ToolFailure (invalid_argument).
std::optional<std::uint64_t> design_generation(const nlohmann::json& args);

// A Design command's silent form: what it does given its arguments, on the panel design_gate() returned.
using DesignRun = nlohmann::json (*)(DesignPanel* panel, const nlohmann::json& params);

// A silent form, by the id of the table's entry whose tool block describes it.
using SilentForm = std::pair<const char*, DesignRun>;

// The silent forms: one per block.
const std::vector<SilentForm>& silent_forms();

// A Design command: group Design, not listed, its function run on the UI thread and told whether it runs for a
// person's pick (ToolContext::person()), which keeps the menu's rules where a call may target the Feature list's row
// (open_tool()). For a person's pick `fn` runs in place, and a refusal it throws is shown in the UI's language where
// its text is a msgid (an entry's refusal). For an automated call the journal records the arguments without
// `generation`, which numbers ids within this session only, and with `silent` where `opens` says that what it
// records would open a tool and the call did not, so that a replay runs the silent form too (`opens` empty:
// nothing opens a tool); then `fn` runs in one UI closure and the call settles. The reply is fn's value, without
// an "ok": true or an empty "error", which say nothing; a ToolFailure is the call's error.
NativeCommand design_command(std::string key, std::string title, nlohmann::json schema,
                          std::function<nlohmann::json(const nlohmann::json& args, bool person)> fn,
                          std::function<bool(const nlohmann::json& args)> opens = {});

// Adds the Design commands to `catalog`, one per entry of kDesignTools that is a command (is_design_command()),
// under the headings of kDesignSections in its order and the table's order within each: its id as the key, its name as the title, and its icon. An entry with a handler has an action of its own
// (has_own_action), and an item folded into a command is one of the command's `actions`, with the item's id as its
// key, its name, icon and args, and as its description its hint and what a pick does (an action is materialised
// only while the Design tab is shown in its entry's mode, ActionRegistry::materialize_design_actions()). One with a
// block is read_only for Read access, and its description is the block's summary, details and, for a silent form
// that reports a rejected rebuild in its reply, how that shows, then the other calls that open a tool, each with its
// item's name; its examples are the block's and its schema design_input_schema() of it, through with_silent() when a
// call with no arguments opens a tool. One with a handler only takes no arguments, and its description is its hint
// and what the call does.
// undo and redo are the shared commands of NativeCommands.cpp and not among them.
void register_design_tools(CommandCatalog& catalog);

// Records in `catalog`, built without the Design commands, that it leaves out the keys register_design_tools()
// adds, and the ids of the items folded into them, which name actions, with code design_unavailable and why
// (CommandCatalog::leave_out()): in the editor (`editor`) the CAD feature was off when OrcaSlicer started, and how a
// person turns it on; in the G-code viewer, which has no Design tab whatever the preference, that a person opens the
// project in the editor.
void leave_out_design_tools(CommandCatalog& catalog, bool editor);

// What tool_list's description says of the Design commands, which tools/list does not show, so that an agent
// learns without a call that they exist and are CAD: a lead saying what they cover, that tool_list's group Design
// says what each does, that tool_describe gives one's manual, and that a command whose schema has silent does what its menu item does when called
// with no arguments unless silent is true; then each heading of kDesignSections with the keys of its commands
// (DesignTool::section) in the table's order, Menu items saying that its commands take no arguments. It starts with
// a space, to follow the description's last sentence.
std::string design_command_index();

// The entities a sketch command acts on: those `params` lists in "entities", else `selection`, else, where the
// command says so (`all_if_empty`), every one of the sketch's `entity_count`: what was pointed at wins, as in
// the menu, which never acts on the whole sketch unasked. Only an empty list, the schema's default, reads as left
// out: anything else that is given names what it holds, a null nothing.
std::vector<int> sketch_target_indices(const nlohmann::json& params, const std::vector<int>& selection, int entity_count,
                                       bool all_if_empty);

// The input schema of a tool block: an object whose properties are the block's params, a type written "a|b"
// (or "a|b|c") as the list of them, min, exclusive_min and max as minimum, exclusiveMinimum and maximum, enum
// and default as written, and the unit joined to the description; a param with no default is required. `generation`, the
// generation the call's face and edge ids were read at, is an optional property of every one. Every schema
// refuses an argument it does not name (additionalProperties false), so a misspelt argument is an error rather
// than a default the caller did not mean; a name agents often use for a param is read by normalize_arguments().
nlohmann::json design_input_schema(const nlohmann::json& block);

// The Design table's entry `id`, or nullptr.
const DesignTool* find_design_tool(const std::string& id);

// Whether entry `t` is a command of the catalog: it has a handler or a tool block, and is no folded item
// (DesignTool::fold_into), which is a way of calling the command it is folded into.
bool is_design_command(const DesignTool& t);

// The item whose tool a call of `entry`'s command with `args` opens, or nullptr when the call runs. A call with no
// arguments (null or {}) opens `entry`'s own item where it has a handler, else the first item folded into it with
// {}; any other opens `entry`'s item where it has a handler and its own args equal `args`, else the first item
// folded into it whose args equal `args`. Arguments are compared as JSON values, as they reach the command's
// function: `generation`, `silent` or any other argument beside the choosing ones makes the call run.
const DesignTool* opened_item(const DesignTool& entry, const nlohmann::json& args);

// Whether the Design tab offers entry `t` in the mode it is in, `sketching` being the right-click menu's
// DesignPanel::sketch_map_applies(): an entry of the sketch's vocabulary in a sketch, one of the model's
// outside it, as the menu offers them. A sketch in Constrain mode is a sketch to the menu, though the tab's keys
// there are the model's. Only then is its Speed Dial action shown and its person's form run.
bool offered_in_mode(const DesignTool& t, bool sketching);

// A Speed Dial action of the Design tab: `command`'s own (`action` null), or the item of command->actions that a menu
// item folded into it has.
struct DesignAction
{
    const NativeCommand*  command;
    const nlohmann::json* action;
};

// The Speed Dial actions the Design tab has while it is shown in the mode `sketching` names (as offered_in_mode()
// takes it): one per entry of the Design table with a handler offered in that mode, in the table's order, whose
// command `catalog` holds: a folded item's runs the command it is folded into with the item's args. None while the
// CAD feature is off (`cad_on` false): the tab stays until a restart, but design_gate() would refuse every pick of
// its actions.
std::vector<DesignAction> design_actions(const CommandCatalog& catalog, bool sketching, bool cad_on);

// Why the person's form of entry `t` is refused before its handler runs, or nothing: `t` is not offered in the
// tab's mode (offered_in_mode()), and the message says which mode it belongs to and what to do; or `t`'s
// handler is a btn: or fly: one, which the menu greys for a selection it does not accept, `t` accepts a kind the
// tab reports as the selection in its mode, and selection `kind` (OfferSel as int) is not one it accepts, and the
// details name the selection and those it accepts. An item that accepts no kind the tab reports there opens as its
// toolbar button does, and a key: handler runs whatever is selected, as its key does. With `row_target`, a call that
// targets the Feature list's selected row (nothing picked in the view), an item whose handler acts on that row
// (DesignTool::feature_row) is not refused for the selection: its handler says itself what it cannot do. The message
// is a msgid, the code wrong_mode, with a hint naming the commands that change the mode, or wrong_selection.
std::optional<ToolFailure> open_refusal(const DesignTool& t, bool sketching, int kind, bool row_target = false);

// Runs the silent form of Design command `id` with `args` past design_gate() with its entry's access, and
// returns its result. One that fails has the code of its cause (the list at the top), with "OCCT: " before a
// kernel exception's message; an id with no silent form is no_silent_form. UI thread only.
nlohmann::json run_design_tool(const std::string& id, const nlohmann::json& args);

// Edit > Undo, or Redo, in the Design tab, past design_gate() with Sketch access: the Design history, or in a
// sketch session the sketch's own, which takes off or puts back the last drawn shape. Returns its scope,
// "sketch" with the sketch's entities, or "design" and the undo and redo depth, features, bodies and
// generation. Fails (precondition) when there is nothing to take back or put back. UI thread only.
nlohmann::json design_undo_redo(bool redo);

// app_state's cad_note: how a person turns the CAD feature on, in the editor while the Design commands
// cannot run, because `catalog` holds none of them (the feature was off at startup) or the feature is off
// now; null otherwise. UI thread.
nlohmann::json cad_note(const CommandCatalog& catalog);

// The `plane` param shared by the plane-taking Design commands: "XY", "XZ" or "YZ" through the modeling
// origin, {"datum": feature index}, or {"body": b, "face": f} on a planar face. A face gives the
// sketch-on-face plane (outward normal), or with `drill` the Hole card's inward plane; `face_body`
// then receives b. Throws std::runtime_error with the message the call fails with.
SketchPlane cad_resolve_plane(const nlohmann::json& plane, const CadDocument& doc, bool drill,
                              int* face_body = nullptr);

// feature_describe's reply for features[index]: what its card shows (`params`), the fields
// feature_edit takes (`editable`) with the card's limits (`ranges`), its expression bindings and
// references, a sketch's geometry, and the features (`used_by`) and bodies (`bodies_made`) it
// feeds; the bodies of the model before it (`bodies_before`, {index, name}, as the last rebuild recorded
// them: null when it did not reach the feature), and the model its body, face and edge ids are read in
// (`ids_of`): "before" it, or "finished" for a Plane, an Axis and a CoordSys, which the engine resolves
// against the finished model.
// Throws std::runtime_error for an index out of range.
nlohmann::json cad_feature_json(const CadDocument& doc, int index);

// feature_edit's `changed`: what differs between `was` and `now`, two cad_feature_json() replies for one feature
// before and after an edit, as {name: [old, new]}: each of the card's params and refs, and as `plane` the plane the
// feature builds on, which the reply gives beside them.
nlohmann::json cad_feature_changes(const nlohmann::json& was, const nlohmann::json& now);

// feature_edit's edit: overwrite the fields of `f` named in `fields` ({name: value}). Throws
// std::runtime_error, leaving `f` as it was, for a field its card does not edit, a field bound to
// an expression, a value of the wrong type, or one outside the card's range. A Text feature's
// text_string, text_height and text_font are only written here; feature_edit outlines the text again.
void cad_apply_feature_fields(CadFeature& f, const nlohmann::json& fields);

// The model as it stood just before features[feature]: the history before it (CadDocument::history_before())
// rebuilt, as a card that edits a feature lists the bodies there (DesignPanel::populate_body_choices()).
// Its bodies, faces and edges are the ones that feature's body indices and face and edge ids name.
// Throws std::runtime_error for an index out of range and for a history that does not rebuild.
CadDocument cad_document_before(const CadDocument& doc, int feature);

// The vertices of body `body` of `doc`, in the order of TopExp::MapShapes() over its shape: a vertex's
// id is its place here, as a face's and an edge's id is theirs. Throws std::runtime_error for a body
// out of range.
std::vector<Vec3d> cad_body_vertices(const CadDocument& doc, int body);

// feature_edit's refs: overwrite the references of `f`, a copy of doc.features[index], named in `refs`
// ({name: value}), under the names feature_describe reports, as far as the feature's card can pick them
// again when it edits. Body indices and face and edge ids are those of `before`
// (cad_document_before(doc, index)), but for a Plane, an Axis and a CoordSys, whose ids are the finished
// model's, `doc`; a feature index names a feature before `index`. Throws std::runtime_error, leaving `f`
// as it was, for an Extrude (its profile and target are fixed), a name the type does not take (listing
// those it takes), a feature index that is not before `index` or not of the type wanted, a body index that
// model does not have, and a face or an edge id that body does not have there.
void cad_apply_feature_refs(CadFeature& f, const nlohmann::json& refs, const CadDocument& before, const CadDocument& doc,
                            int index);

// design_commit_to_plate's `replace`: the model's object indices to delete once the design is on the plate, sorted
// ascending. Throws std::runtime_error for a value that is not an index, one out of range or
// repeated. What Prepare's Delete refuses of the objects, a part of a cut, is the delete operation's to refuse.
std::vector<int> cad_commit_to_plate_replace(const nlohmann::json& replace, const Model& model);

// sketch_describe {sketch}: committed sketch features[sketch] as the open sketch is described, its
// entities, constraint count and loops, numbered as solid_extrude's `loop` takes them; `tool` only
// finds the loops. Throws std::runtime_error for an index that is not a sketch with entities.
nlohmann::json cad_sketch_describe(const CadDocument& doc, const DesignSketchTool& tool, int sketch);

// The Design commands that work on the open sketch alone, run on `tool`'s session: sketch_draw,
// sketch_fillet, sketch_chamfer, sketch_trim, sketch_extend, sketch_offset, sketch_mirror,
// sketch_transform, sketch_pattern, sketch_constrain, sketch_dimension, sketch_list_constraints (the
// open sketch's), sketch_delete_constraint and sketch_insert_svg (an SVG file's outlines, scaled and moved, as Insert
// SVG adds them to a sketch). Each runs the sketch tool's own step for the button or gesture it
// stands for, checks every argument before it changes anything, and throws std::runtime_error with
// the reason, leaving the sketch as it was; sketch_draw, whose tools can still refuse an item while
// drawing it, puts the sketch back as it was before the call when one does. Needs no Design panel.
nlohmann::json cad_sketch_call(DesignSketchTool& tool, const std::string& name, const nlohmann::json& params);

// sketch_project with a sketch open: the edges of body params.source_body of `doc` (-1, the default, the last body)
// that a Project feature takes, params.edges, else those of params.face, else all of them, projected onto the open
// sketch's plane (CadDocument::project_edges_on()) and added to `tool`'s session as construction entities, as a scripted
// add adds entities (add_entities_scripted()). Reply {ok, scope: "sketch", first_index, added, entities: the sketch's
// count}. Throws std::runtime_error, leaving the sketch as it was, for a params.plane that is not null (the open
// sketch's plane is the one), a document with no body, and what project_edges_on() refuses.
nlohmann::json cad_sketch_project(const CadDocument& doc, DesignSketchTool& tool, const nlohmann::json& params);

// sketch_constrain's and sketch_dimension's constraints on the open sketch's entities `ents`, as the
// CONSTRAIN bar would add them. params.type is a SketchConstraintType in snake_case ("equal_length";
// "equal" is the bar's Equal). With params.entities (or else `selection`) the bar's planner chooses
// the points (plan_entity_constraint), and a value it would ask for is params.value or the current
// one; with params.refs the references are taken as written, {entity, role} for a point ("origin"
// too) and {entity} for a whole entity ("x_axis", "y_axis" too), and params.value is required where
// the type carries one. Values are mm, or degrees for an angle. Throws std::runtime_error with the reason.
std::vector<SketchEntityConstraintDef> cad_constraint_defs(const nlohmann::json& params, const std::vector<SketchEntity>& ents,
                                                           const std::vector<int>& selection);

// solid_extrude {sketch} and solid_revolve {sketch}: check params against `doc` and return the call that adds,
// without rebuilding, the feature the Extrude or Revolve card adds for the committed sketch
// params.sketch; it takes the feature's name and returns its index. Checking first lets a bad
// argument fail before the caller takes an undo checkpoint. Extrude takes the whole sketch, or with
// params.loop one of its loops with the loops inside it; its params.operation "auto" (the default)
// joins the body the sketch touches, else makes a new body, as the card's default does. Revolve's
// axis is params.axis_entity, a line of the sketch, else the sketch's only centerline when
// params.axis is not given, else the plane axis params.axis. Throws std::runtime_error.
std::function<int(const std::string& name)> cad_sketch_extrude(CadDocument& doc, const DesignSketchTool& tool,
                                                               const nlohmann::json& params);
std::function<int(const std::string& name)> cad_sketch_revolve(CadDocument& doc, const nlohmann::json& params);

// surface_revolve: the Surface Revolve card's sheet of committed sketch params.sketch, turned params.angle degrees
// about the axis chosen and checked as cad_sketch_revolve() chooses it (params.axis_entity, params.axis), and the
// other way with params.flip; returned as cad_sketch_revolve() returns it. Throws std::runtime_error.
std::function<int(const std::string& name)> cad_surface_revolve(CadDocument& doc, const nlohmann::json& params);

// solid_list_align_types: for coordinate systems params.cs_a (fixed) and params.cs_b (on the body that moves), {ok, cs_a,
// cs_b, types}, the five align types in order, each {type, fits, reason}: whether it fits the pair and, where it does
// not, why (CadDocument::mate_options()); reason is null where it fits. Throws std::runtime_error for a cs_a or cs_b
// that is not an integer.
nlohmann::json cad_align_types(const CadDocument& doc, const nlohmann::json& params);

// The profile revolve turns when it is given no sketch: params.profile, else a params.width by
// params.height rectangle (default 20 x 10) beside the plane axis params.axis, one edge on it, the
// width along it centred on the origin and the height out on its positive side, so a full turn is a
// cylinder `width` long of radius `height`. Throws std::runtime_error.
SketchProfile cad_revolve_profile(const nlohmann::json& params);

// solid_extrude {face}: the Extrude card's push/pull of a picked face. Checks params against `doc` and
// returns the call that adds the face extrude of params.face, a planar face of params.body (default
// the last body), joined to that body unless params.operation says otherwise; `frame` receives the
// face's plane. Throws std::runtime_error.
std::function<int(const std::string& name)> cad_face_extrude(CadDocument& doc, const nlohmann::json& params,
                                                             SketchPlane& frame);

// body_describe's reply for body params.body of `doc`: its faces (kind, area, centroid, normal and
// the numbers of an analytic surface) and edges (kind, length, ends), narrowed by the face filters
// (kinds, direction, at_extreme) and edge filters (edge_kinds, direction, convex); with adjacency,
// the faces' edges and the edges' faces and convexity, and a list whose filters were not given holds
// the neighbours of the other's matches. Both lists are paged by params.limit and params.offset.
// With params.vertices true it adds every vertex, {id, p} (cad_body_vertices()). With params.before, a
// feature index, it reads the body as it stood just before that feature (cad_document_before()) and says
// `before`; its `generation` stays the live document's, which a call passing these ids is checked against.
// Throws std::runtime_error.
nlohmann::json cad_body_describe(const CadDocument& doc, const nlohmann::json& params);

// design_measure's reply for refs params.a and params.b, each a face, an edge (of its own body, else
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

// construct_plane: check params against `doc` as the Plane card's Confirm does (CadDocument::datum_plane_refusal)
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

// hole, thicken, pattern, align, fillet and chamfer, likewise.
// hole: of params.type simple, counterbore or countersink, its seat sized by cbore_* or csink_*, and of
// params.diameter, or of the standard clearance params.designation names, seat included
// (CadDocument::add_hole_standard), which a diameter or seat size beside it is refused with. `resolved` receives
// where it goes (cad_hole_placement) and the frame of its plane.
// thicken: face params.face of body params.body (type face), or the whole sheet body (type surface); with no type,
// face when params.face is given, else surface.
// pattern: body params.body in a row along a plane axis (type linear), around the plane's normal (circular), or
// along entity params.entity of sketch params.sketch (curve), which only a curve pattern takes and it requires.
// align: a Mate of params.type (fastened, planar, revolute, slider or cylindrical) that moves the body of
// coordinate system params.cs_b onto params.cs_a.
// cad_dressup: a fillet of params.radius, or with `chamfer` a chamfer of params.distance, on params.edges, an edge
// id or a list of them made one feature, of body params.body; or on an edge group, params.edges "top", "bottom",
// "lateral" or "all", the card's Edges choice (the engine's FaceGroup overloads).
// Each throws std::runtime_error.
std::function<int(const std::string& name)> cad_hole(CadDocument& doc, const nlohmann::json& params, nlohmann::json& resolved);
std::function<int(const std::string& name)> cad_thicken(CadDocument& doc, const nlohmann::json& params);
std::function<int(const std::string& name)> cad_pattern(CadDocument& doc, const nlohmann::json& params);
std::function<int(const std::string& name)> cad_align(CadDocument& doc, const nlohmann::json& params);
std::function<int(const std::string& name)> cad_dressup(CadDocument& doc, const nlohmann::json& params, bool chamfer);

}}} // namespace Slic3r::GUI::Mcp
