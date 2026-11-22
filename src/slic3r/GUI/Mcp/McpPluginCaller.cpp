// How a plugin's orca.host.tools calls reach the MCP tools.

#include "McpPluginCaller.hpp"

#include "McpArguments.hpp"
#include "McpProtocol.hpp"
#include "AutomationRuntime.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/host/PluginHostTools.hpp"

#include <boost/log/trivial.hpp>
#include <wx/app.h>
#include <wx/thread.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// The UI thread's answer to a plugin's call: the runtime with a lease the caller releases, or why not.
struct Admission
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    answered  = false;
    bool                    abandoned = false; // the caller stopped waiting: take no lease
    AutomationRuntime*      runtime   = nullptr;
    std::string             refusal;
};

// Admits a plugin's call on the UI thread, which owns the runtime and runs the plugin's unload: the plugin
// must still be loaded and its scope not ended, and the runtime is started without its endpoint when MCP is
// off, with the GUI instruments on, as Action Replay starts it. Returns the runtime with a lease taken,
// or throws. A caller that stops waiting because the app closes marks the admission abandoned, under the
// lock the UI thread answers it under, so no lease is taken that nobody releases.
AutomationRuntime* admit_plugin_call(const std::string& plugin)
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
        AutomationRuntime* runtime = wxGetApp().automation();
        if (runtime == nullptr || !runtime->begin_plugin_scope(plugin) || !runtime->acquire_lease()) {
            admission->refusal = "OrcaSlicer can't run the call now";
            return;
        }
        // A plugin's tools drive the GUI as an agent's do.
        runtime->enable_gui_instruments();
        admission->runtime = runtime;
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
    if (admission->runtime == nullptr)
        throw std::runtime_error(admission->refusal);
    return admission->runtime;
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
        AutomationRuntime* runtime = admit_plugin_call(plugin);
        struct Lease
        {
            AutomationRuntime* runtime;
            ~Lease() { runtime->release_lease(); }
        } lease{runtime};
        const CommandCatalog&                catalog = NativeCommands::instance();
        std::shared_ptr<const NativeCommand> def     = catalog.lookup(tool);
        if (def == nullptr)
            throw ToolCallError(catalog.unknown_command(tool),
                                {{"code", catalog.unknown_command_code(tool)}, {"did_you_mean", catalog.similar(tool)}});
        json arguments = args;
        if (std::string err = argument_error(*def, arguments); !err.empty())
            throw ToolCallError("Invalid arguments: " + err, json{{"code", "invalid_argument"}});
        if (const std::optional<ToolFailure> refused = resolve_call(catalog, def, arguments))
            throw ToolCallError(refused->what(), refused->details);

        const ToolResult r = runtime->core().call_plugin(*def, arguments, plugin);
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
