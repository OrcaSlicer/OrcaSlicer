#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Slic3r {

// A tool call that ended in an error result: its message, and the result's data as details.
struct ToolCallError : std::runtime_error
{
    nlohmann::json details;
    ToolCallError(const std::string& message, nlohmann::json details) : std::runtime_error(message), details(std::move(details)) {}
};

// How orca.host.tools reaches the MCP tools for the code running on one thread: a person's or an agent's
// script binds its run's bridge to its worker, so its calls run as child calls of the run.
class ToolBridge
{
public:
    virtual ~ToolBridge() = default;
    // Calls `tool` with `args` and returns the result's data, or a job envelope ({status, job}) for a call
    // that outlives its reply budget. Throws ToolCallError for an error result. Called without the GIL.
    virtual nlohmann::json call(const std::string& tool, const nlohmann::json& args) = 0;
};

// The bridge bound to the calling thread, or nullptr.
ToolBridge* current_tool_bridge();

// Binds `bridge` to the calling thread for the scope's lifetime, restoring the previous one after.
class ScopedToolBridge
{
public:
    explicit ScopedToolBridge(ToolBridge& bridge);
    ~ScopedToolBridge();
    ScopedToolBridge(const ScopedToolBridge&)            = delete;
    ScopedToolBridge& operator=(const ScopedToolBridge&) = delete;

private:
    ToolBridge* m_previous;
};

// Whether the calling thread may make a plugin's synchronous tool calls: a Script worker, a Python thread one
// starts (the thread shim carries the mark), or MCP's own thread running a plugin operation. Nothing on the
// UI thread waits for these, so a call that waits for the UI thread can't deadlock.
bool tool_caller_thread();
class ScopedToolCallerThread
{
public:
    ScopedToolCallerThread();
    ~ScopedToolCallerThread();
    ScopedToolCallerThread(const ScopedToolCallerThread&)            = delete;
    ScopedToolCallerThread& operator=(const ScopedToolCallerThread&) = delete;

private:
    bool m_previous;
};

// Calls a tool for the plugin `plugin_key`, as orca.host.tools does from a plugin's code: returns the
// result's data or a job envelope, throws ToolCallError for an error result and std::runtime_error when
// the plugin may not call tools. Called without the GIL.
using PluginToolCaller = std::function<nlohmann::json(const std::string& plugin_key, const std::string& tool, const nlohmann::json& args)>;
// Set once the GUI can run tool calls; until then a plugin's calls fail.
void set_plugin_tool_caller(PluginToolCaller caller);

} // namespace Slic3r
