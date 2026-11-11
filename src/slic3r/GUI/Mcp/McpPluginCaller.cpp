// How a plugin's orca.host.tools calls reach the MCP tools.

#include "McpProtocol.hpp"
#include "McpService.hpp"
#include "McpTypes.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/host/PluginHostTools.hpp"

#include <boost/log/trivial.hpp>
#include <wx/app.h>
#include <wx/thread.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// The UI thread's answer to a plugin's call: the service with a lease the caller releases, or why not.
struct Admission
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    answered  = false;
    bool                    abandoned = false; // the caller stopped waiting: take no lease
    McpService*             service   = nullptr;
    std::string             refusal;
};

// Admits a plugin's call on the UI thread, which owns the service and runs the plugin's unload: the plugin
// must still be loaded and its scope not ended, and the service is started in its catalog-only mode (no
// endpoint) when MCP is off, as Action Replay starts it. Returns the service with a lease taken,
// or throws. A caller that stops waiting because the app closes marks the admission abandoned, under the
// lock the UI thread answers it under, so no lease is taken that nobody releases.
McpService* admit_plugin_call(const std::string& plugin)
{
    auto admission = std::make_shared<Admission>();
    wxGetApp().CallAfter([admission, plugin] {
        std::lock_guard<std::mutex> lock(admission->mutex);
        if (admission->abandoned)
            return;
        admission->answered = true;
        admission->cv.notify_all();
        if (wxGetApp().is_closing()) {
            admission->refusal = "OrcaSlicer is closing";
            return;
        }
        if (!PluginManager::instance().is_plugin_loaded(plugin)) {
            admission->refusal = "This plugin is not loaded";
            return;
        }
        if (wxGetApp().mcp_service() == nullptr || !wxGetApp().mcp_service()->started())
            wxGetApp().start_mcp_service(false);
        McpService* service = wxGetApp().mcp_service();
        if (service == nullptr || !service->started() || !service->begin_plugin_scope(plugin) || !service->acquire_lease()) {
            admission->refusal = "OrcaSlicer can't run the call now";
            return;
        }
        admission->service = service;
    });
    std::unique_lock<std::mutex> lock(admission->mutex);
    while (!admission->cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return admission->answered; }))
        // The plugins shut down without the app closing at the end of a session on macOS, with the UI thread
        // in PluginManager::shutdown(), which waits for this Script worker.
        if (wxTheApp == nullptr || wxGetApp().is_closing() || PluginManager::instance().is_shutting_down()) {
            // A closing app may never run the closure; a closure that runs after this takes nothing.
            admission->abandoned = true;
            throw std::runtime_error("OrcaSlicer is closing");
        }
    if (admission->service == nullptr)
        throw std::runtime_error(admission->refusal);
    return admission->service;
}

// The paths a call's arguments name, as the log records them: the values of the arguments whose name
// says path, dir or file.
std::string named_paths(const json& args)
{
    std::string out;
    const auto  add = [&out](const json& v) {
        if (v.is_string())
            out += (out.empty() ? "" : ", ") + v.get<std::string>();
    };
    for (const auto& [key, value] : args.items()) {
        if (key.find("path") == std::string::npos && key.find("dir") == std::string::npos && key.find("file") == std::string::npos)
            continue;
        if (value.is_array())
            for (const json& v : value)
                add(v);
        else
            add(value);
    }
    return out;
}

} // namespace

void install_plugin_tool_caller()
{
    set_plugin_tool_caller([](const std::string& plugin, const std::string& tool, const json& args) -> json {
        PluginInstallState state;
        if (!PluginManager::instance().get_install_state(plugin, state) || !state.permissions.tools)
            throw std::runtime_error("This plugin has not been granted the tools permission; it asks for it with "
                                     "request_permissions(tools=True)");
        McpService* service = admit_plugin_call(plugin);
        struct Lease
        {
            McpService* service;
            ~Lease() { service->release_lease(); }
        } lease{service};
        std::shared_ptr<const ToolDef> def = service->registry().lookup(tool);
        if (def == nullptr)
            throw ToolCallError("Unknown tool: " + tool, {{"did_you_mean", service->registry().similar(tool)}});
        json arguments = args;
        if (std::string err = argument_error(*def, arguments); !err.empty())
            throw ToolCallError("Invalid arguments: " + err, json());
        if (std::string err = resolve_api_call(service->registry(), def, arguments); !err.empty())
            throw ToolCallError(err, json());

        const ToolResult r = service->core().call_plugin(*def, arguments, plugin);
        // The handlers raise no audit events, so this line is the record of what a plugin did; it is best
        // effort, since plugin code may write the log folder.
        const std::string outcome = r.is_error ? "error: " + r.message : r.data.is_object() ? r.data.value("status", "ok") : "ok";
        BOOST_LOG_TRIVIAL(info) << "[MCP plugin] plugin=" << plugin << " tool=" << tool << " outcome=" << outcome << " paths=["
                                << named_paths(arguments) << "]";
        if (r.is_error)
            throw ToolCallError(r.message, r.data);
        return r.data;
    });
}

}}} // namespace Slic3r::GUI::Mcp
