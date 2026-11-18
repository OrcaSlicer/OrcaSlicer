#pragma once

#include "slic3r/GUI/CommandTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>

// The contexts a command runs with besides the MCP core's own: a person's pick, and a Speed Dial tile that
// action_run runs. No wx here, so that unit tests can include it (the slic3rutils tile case does).
namespace Slic3r { namespace GUI { namespace Mcp {

// A person's pick from the Speed Dial or a menu, on the UI thread and outside the automation runtime's dispatch
// core: UI closures run in place, and there is no job, no journal entry and no operation to hold. It needs no MCP
// endpoint, and the runtime starts only when the pick's command needs it: through this context (runtime(),
// submit()) or through GUI_App::automation().
class PersonContext final : public ToolContext
{
public:
    bool               person() const override { return true; }
    // The automation runtime, started without the MCP endpoint when the server is off, as the Run Script panel
    // starts it. Throws ToolFailure when it cannot start: OrcaSlicer is closing, or no app runs (a unit test).
    AutomationRuntime& runtime() override;
    // Starts command `key` as a job on the runtime's core (McpCore::submit()), under the admission a client's
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

// What action_run runs a command's tile with: the context of action_run's own call, except that a command
// that rewrites its journal entry does so inside action_run's, as {"id": <the tile's action id>, "args": ...},
// so that a replay runs the tile again through action_run. A tile of fixed arguments (an item of a command's tiles, a
// Design command's folded items among them, or a command's own tile outside the Design tab) records `tile_args` there in
// place of the command's rewrite: what action_run takes for it (tile_journal_args()). action_run refuses any other
// arguments for such a tile, and so would refuse the replay of a rewrite (object_arrange's adds the settings in
// force). A Design command's own tile, which takes its command's arguments, has none, and the command's rewrite stands.
class TileContext final : public ToolContext
{
public:
    TileContext(ToolContext& call, std::string action_id, nlohmann::json tile_args = nullptr)
        : m_call(call), m_id(std::move(action_id)), m_tile_args(std::move(tile_args))
    {}

    bool               person() const override { return m_call.person(); }
    AutomationRuntime& runtime() override { return m_call.runtime(); }
    ToolResult         submit(const std::string& key, const nlohmann::json& args) override { return m_call.submit(key, args); }
    nlohmann::json     run_ui(const std::function<nlohmann::json()>& fn) override { return m_call.run_ui(fn); }
    nlohmann::json     observe_ui(const std::function<nlohmann::json()>& fn) override { return m_call.observe_ui(fn); }
    const std::string& job_id() const override { return m_call.job_id(); }
    bool               cancelled() const override { return m_call.cancelled(); }
    ToolResult call_child(const std::string& tool, const nlohmann::json& args) override { return m_call.call_child(tool, args); }
    std::set<std::string> lineage() const override { return m_call.lineage(); }
    void                  set_journal_args(nlohmann::json args) override
    {
        m_call.set_journal_args({{"id", m_id}, {"args", m_tile_args.is_null() ? std::move(args) : m_tile_args}});
    }
    void               begin_operation() override { m_call.begin_operation(); }
    void               works_in_dialog(std::uint64_t dialog) override { m_call.works_in_dialog(dialog); }
    const std::string& caller_plugin() const override { return m_call.caller_plugin(); }

private:
    ToolContext&   m_call;
    std::string    m_id;
    nlohmann::json m_tile_args;
};

}}} // namespace Slic3r::GUI::Mcp
