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
        "Call a method of the Design (CAD) tab's control surface: method \"describe_tools\" lists them with their params "
        "(describe_scene, query_topology, measure, sketch_begin / sketch_add / sketch_commit, extrude, fillet, boolean, "
        "import_step, ...). Pass the \"generation\" from describe_scene or query_topology with face and edge ids so ids "
        "read before the model changed are refused. Needs the CAD feature enabled in Preferences (it takes effect after a "
        "restart).",
        object_schema({{"method", prop("string", "Method name.")}, {"params", {{"type", "object"}, {"description", "Its parameters."}}}},
                      {"method"}),
        [](const json& args, ToolContext& ctx) {
            const std::string method = arg_string(args, "method");
            const json        params = args.value("params", json::object());
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
