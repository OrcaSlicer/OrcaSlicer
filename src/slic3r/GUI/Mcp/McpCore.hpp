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
    int                       blocking_slots = 32;
    // How long a call waits for a worker job it started before cancelling it, and how often it looks.
    std::chrono::milliseconds worker_timeout{std::chrono::minutes(30)};
    std::chrono::milliseconds worker_poll{50};
};

// The app's UI job worker (arrange, orient, fill bed, the Text, SVG and texture gizmos' jobs), which a
// call can start as a person's click does. Both run on the UI thread; without them nothing is busy.
struct WorkerHooks
{
    std::function<bool()> busy;   // a job is queued, running or not yet finalized
    std::function<void()> cancel; // asks the running job to stop, without waiting for it
};

// Executes tool calls for the protocol layer and for scripts: observation tools inline on the
// calling thread, everything else as a job on a thread of its own, with the reply budget, the
// blocked-by-dialog probe, blocking slots, operation admission and the session journal. Independent
// of wx so the threading contract is unit-testable with a hand-driven UiDispatcher.
//
// A call that finds the UI job worker idle and leaves it busy has started a worker job, whichever
// tool or input did it, and that job is its operation: the call holds admission, and its job ends,
// only once the worker is idle again, finalization included, since the job keeps pointers into the
// model until then.
class McpCore : public ToolCaller
{
public:
    McpCore(const ToolRegistry& registry, UiInvoker& ui, JobRegistry& jobs, Journal* journal, CoreTiming timing = {},
            WorkerHooks worker = {});
    // Refuses new calls and waits for every call and job thread to leave, however long that takes:
    // they use the core. The owner drains first, so this returns at once.
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

    // Operation admission for a change made by `job` ("" for a call that holds nothing), on the UI
    // thread: throws ToolFailure while another job's operation holds the project, or while the UI job
    // worker runs a job `job` does not hold (a person's, such as Text typing). Mutate calls get it in
    // every UI closure; the GUI input tools apply it once their target is outside the topmost modal.
    void admit(const std::string& job) const;
    // UI thread. Whether the UI job worker has a job queued, running or left to finalize.
    bool worker_busy() const;
    // UI thread. Makes the busy worker's job `job`'s operation unless another call holds it or another
    // operation runs; true when `job` holds it.
    bool claim_worker(const std::string& job);
    // UI thread. When `job` holds the UI job worker, asks its job to stop and returns true. The call
    // keeps admission until the worker is idle, then fails as cancelled.
    bool cancel_worker(const std::string& job);

private:
    ToolResult run(const ToolDef& def, const nlohmann::json& args, const std::string& parent);
    // On `job`'s thread once its handler returned: when it holds the UI job worker, waits until the
    // worker is idle, cancelling the job after CoreTiming::worker_timeout, and then lets it go. The
    // result becomes a failure when the job was cancelled or timed out.
    ToolResult await_worker(const std::string& job, const std::string& tool, ToolResult result);

    const ToolRegistry& m_registry;
    UiInvoker&          m_ui;
    JobRegistry&        m_jobs;
    Journal*            m_journal;
    CoreTiming          m_timing;
    WorkerHooks         m_worker;

    // The job whose call started the worker's current job, and whether a cancel was asked for it.
    mutable std::mutex m_worker_mutex;
    std::string        m_worker_job;
    bool               m_worker_cancelled = false;

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
