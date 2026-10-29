// cad_call: the Design tab's control surface (sketches, features, measurements) through MCP.

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/CAD/McpControl.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

void register_cad_tools(McpService& service)
{
    service.registry().add(mutate_tool(
        "cad_call", "Use the Design tab",
        "Call a method of the Design (CAD) tab's control surface. Method \"describe_tools\" lists the methods and their "
        "params; narrow it with params {\"method\": name or [names]} or {\"query\": text}. Typical calls: describe_scene, "
        "describe_feature / set_feature (read and edit a feature as its card does), sketch_begin / sketch_add (entities, "
        "polygon, slot, ellipse, spline) / sketch_commit then extrude or revolve {sketch}; in the sketch, sketch_fillet, "
        "sketch_trim, sketch_transform (move, rotate, array) and sketch_constrain / sketch_constraints; hole, thread, sweep (along a sketch or helix), loft, split {plane}, "
        "fillet, boolean, plane (datum), query_topology "
        "(face kinds, adjacency, filters), measure (min_distance), commit "
        "(Commit to Plate), undo / redo (the Design history; run_action undo is Prepare's). A method "
        "that fails is an error and leaves the design as it was; the older ones in describe_tools' legacy_ok_false "
        "answer ok false instead. thread, sweep and loft can take seconds, and their reply may be a job. Pass the "
        "\"generation\" from describe_scene or query_topology with face and edge ids so ids read before the model "
        "changed are refused. Needs the CAD feature enabled in Preferences (it "
        "takes effect after a restart).",
        object_schema({{"method", prop("string", "Method name.")}, {"params", {{"type", "object"}, {"description", "Its parameters."}}}},
                      {"method"}),
        [](const json& args, ToolContext& ctx) {
            const std::string method = arg_string(args, "method");
            const json        params = args.value("params", json::object());
            // `generation` guards ids within this session only: a replay rebuilds the same ids by
            // replaying the history, but reaches another generation number, so a recorded one
            // would refuse the replay. Journal the call without it.
            if (params.is_object() && params.contains("generation")) {
                json recorded = args;
                recorded["params"].erase("generation");
                ctx.set_journal_args(std::move(recorded));
            }
            json result = ctx.run_ui([&] {
                if (!wxGetApp().is_enable_cad_feature())
                    throw ToolFailure("The CAD feature is off; enable it in Preferences > Features and restart OrcaSlicer");
                try {
                    return cad_control_call(method, params);
                } catch (const CadCallError& e) {
                    throw ToolFailure(e.what(), {{"code", e.code}});
                }
            });
            settle(ctx);
            return ToolResult::ok({{"result", std::move(result)}});
        }));
}

}}} // namespace Slic3r::GUI::Mcp
