#pragma once

#include "McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>

// The contexts a command runs with besides the MCP core's own: a person's pick, and a Speed Dial tile that
// run_action runs. No wx here, so that the core's tests can include it.
namespace Slic3r { namespace GUI { namespace Mcp {

// A person's pick from the Speed Dial or a menu, on the UI thread, which no MCP service needs to run: UI
// closures run in place, and there is no job, no journal entry and no operation to hold.
class PersonContext final : public ToolContext
{
public:
    bool               person() const override { return true; }
    // The MCP service, started without its endpoint when the server is off, as the Run Script panel starts
    // it. Throws ToolFailure when it cannot start: OrcaSlicer is closing, or no app runs (a unit test).
    McpService&        service() override;
    // Starts command `key` as a job on the service's core (McpCore::submit()), under the admission a client's
    // call has, and returns at once: an Info notice "Started: <title>", or an Error when no job started. The
    // job's result is shown as a notification once it ends.
    ToolResult         submit(const std::string& key, const nlohmann::json& args) override;
    nlohmann::json     run_ui(const std::function<nlohmann::json()>& fn) override { return fn(); }
    nlohmann::json     observe_ui(const std::function<nlohmann::json()>& fn) override { return fn(); }
    const std::string& job_id() const override;
    // Whether OrcaSlicer is closing.
    bool                  cancelled() const override;
    // A person's pick makes no child calls: throws ToolFailure.
    ToolResult            call_child(const std::string& tool, const nlohmann::json& args) override;
    std::set<std::string> lineage() const override { return {}; }
    void                  set_journal_args(nlohmann::json /*args*/) override {}
    void                  begin_operation() override {}
};

// What run_action runs a command's tile with: the context of run_action's own call, except that a command
// that rewrites its journal entry does so inside run_action's, as {"id": <the tile's action id>, "args": ...},
// so that a replay runs the tile again through run_action.
class TileContext final : public ToolContext
{
public:
    TileContext(ToolContext& call, std::string action_id) : m_call(call), m_id(std::move(action_id)) {}

    bool           person() const override { return m_call.person(); }
    McpService&    service() override { return m_call.service(); }
    ToolResult     submit(const std::string& key, const nlohmann::json& args) override { return m_call.submit(key, args); }
    nlohmann::json run_ui(const std::function<nlohmann::json()>& fn) override { return m_call.run_ui(fn); }
    nlohmann::json observe_ui(const std::function<nlohmann::json()>& fn) override { return m_call.observe_ui(fn); }
    const std::string& job_id() const override { return m_call.job_id(); }
    bool               cancelled() const override { return m_call.cancelled(); }
    ToolResult call_child(const std::string& tool, const nlohmann::json& args) override { return m_call.call_child(tool, args); }
    std::set<std::string> lineage() const override { return m_call.lineage(); }
    void                  set_journal_args(nlohmann::json args) override
    {
        m_call.set_journal_args({{"id", m_id}, {"args", std::move(args)}});
    }
    void               begin_operation() override { m_call.begin_operation(); }
    void               works_in_dialog(std::uint64_t dialog) override { m_call.works_in_dialog(dialog); }
    const std::string& caller_plugin() const override { return m_call.caller_plugin(); }

private:
    ToolContext& m_call;
    std::string  m_id;
};

}}} // namespace Slic3r::GUI::Mcp
