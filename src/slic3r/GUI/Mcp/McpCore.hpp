#pragma once

#include "JobRegistry.hpp"
#include "McpTypes.hpp"
#include "UiInvoker.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

class Journal;

struct CoreTiming
{
    std::chrono::milliseconds soft_budget{1500};  // before probing for a blocking dialog
    std::chrono::milliseconds probe_timeout{1000};
    std::chrono::milliseconds join_timeout{5000}; // drain() in the destructor
    int                       blocking_slots = 32;
};

// Executes tool calls for the protocol layer and for scripts: observation tools inline on the
// calling thread, everything else as a job on a thread of its own, with the reply budget, the
// blocked-by-dialog probe, blocking slots, operation admission and the session journal. Independent
// of wx so the threading contract is unit-testable with a hand-driven UiDispatcher.
class McpCore : public ToolCaller
{
public:
    McpCore(const ToolRegistry& registry, UiInvoker& ui, JobRegistry& jobs, Journal* journal, CoreTiming timing = {});
    ~McpCore() override;

    // A call from a client. Mutate tools become top-level jobs.
    ToolResult call(const ToolDef& def, const nlohmann::json& args) override;
    // A call made by a running job (a script step). The new job's parent is `parent_job`, so a
    // `wait {for:"idle"}` from that job does not wait for it, and the journal leaves it out.
    ToolResult call_child(const ToolDef& def, const nlohmann::json& args, const std::string& parent_job);

    // Refuses new calls. The owner wakes UiInvoker and JobRegistry, which makes running calls return.
    void shutdown();
    // Waits until no call or job thread is inside the core; false when one is still running after
    // `timeout`. The owner may destroy the core, and what it uses, only after this returns true.
    bool drain(std::chrono::milliseconds timeout);

    const ToolRegistry& registry() const { return m_registry; }
    UiInvoker&          ui() { return m_ui; }
    JobRegistry&        jobs() { return m_jobs; }

private:
    ToolResult run(const ToolDef& def, const nlohmann::json& args, const std::string& parent);

    const ToolRegistry& m_registry;
    UiInvoker&          m_ui;
    JobRegistry&        m_jobs;
    Journal*            m_journal;
    CoreTiming          m_timing;

    // Requests that may block (a job within its reply budget, or wait) hold a slot, so the number of
    // blocked request threads stays below the connection limit and recovery calls always get through.
    bool acquire_slot();
    void release_slot();
    std::mutex m_slots_mutex;
    int        m_slots_used = 0;

    // Client and child calls inside run(), plus job threads.
    bool enter();
    void leave();
    std::mutex              m_threads_mutex;
    std::condition_variable m_threads_cv;
    int                     m_active   = 0;
    bool                    m_stopping = false;
};

}}} // namespace Slic3r::GUI::Mcp
