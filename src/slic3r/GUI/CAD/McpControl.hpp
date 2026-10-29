#ifndef slic3r_GUI_McpControl_hpp_
#define slic3r_GUI_McpControl_hpp_

// MCP control surface: a local JSON-RPC 2.0 server, line-delimited over a Unix domain
// socket, that lets an external MCP bridge drive and perceive the Design tab. Off unless the
// CAD feature is enabled in Preferences AND the env var ORCA_CAD_MCP is set:
//   ORCA_CAD_MCP=1                -> socket at /tmp/orca-cad-mcp.sock
//   ORCA_CAD_MCP=/path/to.sock    -> socket at that path (an existing non-socket file there is
//                                    left alone and the server does not start)
// The socket is created 0600 and removed at exit. All CAD work is marshalled onto the wx main
// thread and runs through the SAME CadDocument kernel the GUI uses (no parallel engine);
// describe_tools lists the methods. Every method is refused (-32002) during a rebuild, whose
// worker owns the document; one that changes the document is also refused while the Design tab
// is busy with it — an open feature card, a sketch session. `commit` changes Prepare's model, so
// the socket refuses it: only the MCP server runs it, under its mutate guard.
//
// The socket is POSIX only; on Windows the same methods are reached through the app's MCP server
// (its cad_call tool), which calls cad_control_call().

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

class CadDocument;
struct CadFeature;
struct SketchEntity;
struct SketchEntityConstraintDef;
struct SketchPlane;
class Model;

namespace GUI {

class DesignSketchTool;

// Start the server thread iff ORCA_CAD_MCP is set. Safe to call once the MainFrame exists —
// the Design panel is built on the first request. No-op when the env var is unset or on Windows.
void start_mcp_control_if_enabled();

// A control-surface method's JSON-RPC error.
struct CadCallError : std::runtime_error
{
    int code;
    CadCallError(int code, const std::string& message) : std::runtime_error(message), code(code) {}
};

// Runs one control-surface method (describe_tools lists them) with the socket's busy and stale-id
// checks and returns its result. Throws CadCallError. UI thread only.
nlohmann::json cad_control_call(const std::string& method, const nlohmann::json& params);

// The names of every method cad_control_call() dispatches.
std::vector<std::string> cad_control_methods();

// describe_tools' reply: one row per method with its params and access (read, change or sketch),
// narrowed by params `method` (a name or a list of names) and `query` (text in a name or summary).
// Throws std::runtime_error naming close methods for an unknown name. Needs no Design panel.
nlohmann::json cad_describe_tools(const nlohmann::json& params);

// The `plane` param shared by the plane-taking methods: "XY", "XZ" or "YZ" through the modeling
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

// The methods that work on the open sketch alone, run on `tool`'s session: sketch_add,
// sketch_fillet, sketch_chamfer, sketch_trim, sketch_transform, sketch_constrain,
// sketch_constraints (the open sketch's) and sketch_unconstrain. Each runs the sketch tool's own
// step for the button or gesture it stands for, checks every argument before it changes anything,
// and throws std::runtime_error with the reason, leaving the sketch as it was. Needs no Design panel.
nlohmann::json cad_sketch_call(DesignSketchTool& tool, const std::string& method, const nlohmann::json& params);

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

// Where a hole method drills: `plane` from params.plane (a face gives the Hole card's inward plane),
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

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_McpControl_hpp_
