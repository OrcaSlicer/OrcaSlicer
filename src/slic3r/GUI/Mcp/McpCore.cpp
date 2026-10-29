#include "McpCore.hpp"

#include "McpJournal.hpp"
#include "McpTypes.hpp"
#include "UiInvoker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// What a mutating call is told while another job's operation holds the project.
std::string busy_message(const std::string& op)
{
    return "Busy: job " + op + " is running an operation (slice, export, arrange or another background job); wait for it or cancel it";
}

class CallContext final : public ToolContext
{
public:
    CallContext(McpCore& core, const ToolDef& def, std::string job, std::string parent, Journal* journal, uint64_t journal_seq)
        : m_core(core), m_def(def), m_job(std::move(job)), m_parent(std::move(parent)), m_journal(journal), m_seq(journal_seq)
    {}

    nlohmann::json run_ui(const std::function<nlohmann::json()>& fn) override
    {
        if (m_def.guard == GuardClass::Inspect)
            return m_core.ui().run(m_def.guard, m_def.name, fn);
        const bool admit = m_def.guard == GuardClass::Mutate && !m_def.allowed_during_operation;
        return m_core.ui().run(m_def.guard, m_def.name, [&]() -> nlohmann::json {
            // Admission again on the UI thread, where operations and worker jobs start: a call admitted
            // before one began must not change the project under it.
            if (admit)
                m_core.admit(m_job);
            if (!m_core.worker_busy())
                m_saw_idle = true;
            nlohmann::json out;
            try {
                out = fn();
            } catch (...) {
                claim_worker();
                throw;
            }
            claim_worker();
            return out;
        });
    }
    nlohmann::json observe_ui(const std::function<nlohmann::json()>& fn) override
    {
        // A job the call's change queued for later (a toolbar event, a render) starts between its
        // closures; the next one the call makes claims it.
        return m_core.ui().run(GuardClass::Inspect, m_def.name, [&]() -> nlohmann::json {
            nlohmann::json out = fn();
            claim_worker();
            return out;
        });
    }
    const std::string& job_id() const override { return m_job; }
    bool cancelled() const override { return m_core.ui().is_shutdown(); }

    ToolResult call_child(const std::string& tool, const nlohmann::json& args) override
    {
        const ToolDef* def = m_core.registry().find(tool);
        if (def == nullptr)
            return ToolResult::error("Unknown tool: " + tool);
        if (std::string err = validate_arguments(def->input_schema, args); !err.empty())
            return ToolResult::error("Invalid arguments: " + err);
        // An inline caller has no job of its own; its children are rooted at its parent.
        return m_core.call_child(*def, args, m_job.empty() ? m_parent : m_job);
    }

    std::set<std::string> lineage() const override
    {
        return m_core.jobs().lineage(m_job.empty() ? m_parent : m_job);
    }

    void set_journal_args(nlohmann::json args) override
    {
        if (m_journal != nullptr && m_seq != 0)
            m_journal->set_args(m_seq, std::move(args));
    }

    void begin_operation() override
    {
        if (m_job.empty())
            return;
        if (const std::string other = m_core.jobs().begin_operation(m_job); !other.empty())
            throw ToolFailure(busy_message(other), {{"job", other}});
    }

private:
    // A worker job is the call's own when the worker was idle as one of its changes began and is busy
    // now. UI thread.
    void claim_worker()
    {
        if (m_saw_idle)
            m_core.claim_worker(m_job);
    }

    McpCore&       m_core;
    const ToolDef& m_def;
    std::string    m_job;
    std::string    m_parent;
    Journal*       m_journal;
    uint64_t       m_seq;
    bool           m_saw_idle = false; // UI thread
};

ToolResult invoke(const ToolDef& def, const nlohmann::json& args, ToolContext& ctx)
{
    try {
        return def.handler(args, ctx);
    } catch (const ToolFailure& f) {
        return ToolResult::error(f.what(), f.details);
    } catch (const std::exception& e) {
        return ToolResult::error(std::string("Internal error: ") + e.what());
    } catch (...) {
        return ToolResult::error("Internal error");
    }
}

} // namespace

McpCore::McpCore(const ToolRegistry& registry, UiInvoker& ui, JobRegistry& jobs, Journal* journal, CoreTiming timing, WorkerHooks worker)
    : m_registry(registry), m_ui(ui), m_jobs(jobs), m_journal(journal), m_timing(timing), m_worker(std::move(worker))
{}

McpCore::~McpCore()
{
    shutdown();
    std::unique_lock<std::mutex> lock(m_threads_mutex);
    m_threads_cv.wait(lock, [this] { return m_active == 0; });
}

bool McpCore::enter()
{
    std::lock_guard<std::mutex> lock(m_threads_mutex);
    if (m_stopping)
        return false;
    ++m_active;
    return true;
}

void McpCore::leave()
{
    // Notified under the lock: once the count reaches zero, drain() may return and the owner free
    // the core, so nothing here may touch it after the unlock.
    std::lock_guard<std::mutex> lock(m_threads_mutex);
    --m_active;
    m_threads_cv.notify_all();
}

ToolResult McpCore::call(const ToolDef& def, const nlohmann::json& args) { return run(def, args, {}); }

ToolResult McpCore::call_child(const ToolDef& def, const nlohmann::json& args, const std::string& parent_job)
{
    return run(def, args, parent_job);
}

ToolResult McpCore::run(const ToolDef& def, const nlohmann::json& args, const std::string& parent)
{
    if (m_ui.is_shutdown() || !enter())
        return ToolResult::error("OrcaSlicer is shutting down");
    struct Leave { McpCore* core; ~Leave() { core->leave(); } } leave_on_return{this};

    // Only client calls are journaled; a script's own steps replay through the script.
    const bool     journal = m_journal != nullptr && parent.empty() && def.journal;
    const uint64_t seq     = journal ? m_journal->begin(def.name, args, def.guard == GuardClass::Recover && !m_ui.open_dialogs().empty()) : 0;
    auto           finish  = [&](ToolResult r) {
        if (journal)
            m_journal->end(seq, r);
        return r;
    };

    if (def.inline_call) {
        CallContext ctx(*this, def, {}, parent, m_journal, seq);
        // Any inline call can wait for the UI thread, so each holds a blocking slot.
        if (!acquire_slot())
            return finish(ToolResult::ok({{"status", "pending"}, {"note", "Too many calls are waiting right now; try again shortly."}}));
        ToolResult r = invoke(def, args, ctx);
        release_slot();
        return finish(std::move(r));
    }

    // Operation admission: an MCP operation holds the project until it finishes; only recovery
    // input and the tools that may run during an operation (cancel) get through.
    if (def.guard == GuardClass::Mutate && !def.allowed_during_operation)
        if (const auto busy = m_jobs.running_operations(); !busy.empty())
            return finish(ToolResult::error(busy_message(busy.front()) + " before changing the project", {{"job", busy.front()}}));

    std::string job;
    try {
        job = m_jobs.create(def.name, parent);
    } catch (const ToolFailure& f) {
        return finish(ToolResult::error(f.what()));
    }
    if (journal)
        m_journal->job_started(seq, job);

    if (!enter()) {
        m_jobs.finish(job, ToolResult::error("OrcaSlicer is shutting down"));
        return finish(ToolResult::error("OrcaSlicer is shutting down"));
    }
    // The job thread owns copies of what it needs; the core outlives it, as the owner destroys the
    // core only once drain() has seen every job thread leave.
    try {
        std::thread([this, def, args, job, parent, seq]() {
            CallContext ctx(*this, def, job, parent, m_journal, seq);
            ToolResult  result = await_worker(job, def.name, invoke(def, args, ctx));
            // Before finish(): an export_script right after the wait that sees the result must know it.
            if (seq != 0)
                m_journal->job_ended(seq, result);
            m_jobs.finish(job, std::move(result));
            leave();
        }).detach();
    } catch (const std::system_error& e) {
        const ToolResult failed = ToolResult::error(std::string("Cannot start the call: ") + e.what());
        m_jobs.finish(job, failed);
        leave();
        return finish(failed);
    }

    auto envelope = [&](const char* status, nlohmann::json extra) {
        nlohmann::json out{{"status", status}, {"job", job}, {"tool", def.name}};
        out.update(extra);
        return ToolResult::ok(std::move(out));
    };
    const std::string running_note = "The call is still running and will finish on its own; do not retry it. "
                                     "Use wait {for:\"job\", job:\"" + job + "\"} for its result.";
    // dialog_respond can't press a native dialog's buttons, so its note asks for a person instead.
    auto blocked = [&](nlohmann::json dialogs, bool ui_held) {
        std::string note;
        if (topmost_dialog_is_native(dialogs)) {
            note = "The call is waiting on a native dialog, which only a person can answer (dialog_respond can't); do not retry it. ";
            if (ui_held)
                note += "Until it closes, tools that need the UI thread fail. ";
            note += "Once it is answered, wait {for:\"job\", job:\"" + job +
                    "\"}. Register dialog_auto_answer before a step that opens a native dialog.";
        } else {
            note = "The call is waiting on a dialog and continues after it closes; do not retry it. "
                   "Answer the dialog (ui_snapshot, dialog_respond), then wait {for:\"job\", job:\"" + job + "\"}.";
        }
        return envelope("blocked_by_dialog", {{"dialogs", std::move(dialogs)}, {"note", note}});
    };
    if (!acquire_slot())
        return finish(envelope("running", {{"note", running_note}}));
    struct SlotGuard { McpCore* core; ~SlotGuard() { core->release_slot(); } } slot_guard{this};

    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(def.budget_ms);
    auto       next_probe = start + m_timing.soft_budget;
    for (;;) {
        const auto now  = std::chrono::steady_clock::now();
        const auto stop = std::min(deadline, next_probe);
        if (auto r = m_jobs.wait(job, std::chrono::duration_cast<std::chrono::milliseconds>(stop > now ? stop - now : std::chrono::milliseconds(0))))
            return finish(std::move(*r));
        if (m_ui.is_shutdown())
            return finish(ToolResult::error("OrcaSlicer is shutting down"));
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        // Past the soft budget: if a modal dialog is open, the job is almost certainly waiting on it.
        const auto p = m_ui.probe(m_timing.probe_timeout);
        // A stop makes the probe return nothing, which must not read as a held UI thread.
        if (m_ui.is_shutdown())
            return finish(ToolResult::error("OrcaSlicer is shutting down"));
        if (p && p->guards.modal) {
            // A dialog can close between the probe and here; a finished job wins.
            if (auto r = m_jobs.wait(job, std::chrono::milliseconds(0)))
                return finish(std::move(*r));
            return finish(blocked(p->guards.dialogs, false));
        }
        // An unanswered probe with a native dialog on top: that dialog holds the UI thread, so the
        // snapshot published for other threads is all there is to report.
        if (!p) {
            if (nlohmann::json dialogs = m_ui.open_dialogs(); topmost_dialog_is_native(dialogs)) {
                if (auto r = m_jobs.wait(job, std::chrono::milliseconds(0)))
                    return finish(std::move(*r));
                return finish(blocked(std::move(dialogs), true));
            }
        }
        next_probe = std::chrono::steady_clock::now() + m_timing.soft_budget;
    }
    return finish(envelope("running", {{"note", running_note}}));
}

void McpCore::admit(const std::string& job) const
{
    for (const std::string& op : m_jobs.running_operations())
        if (op != job)
            throw ToolFailure(busy_message(op) + " before changing the project", {{"job", op}});
    if (!worker_busy())
        return;
    {
        std::lock_guard<std::mutex> lock(m_worker_mutex);
        if (!job.empty() && m_worker_job == job)
            return;
    }
    throw ToolFailure("Busy: a background job (arrange, orient, fill bed, or one a gizmo started, such as Text typing) is "
                      "running in OrcaSlicer; wait {for:\"idle\"} before changing the project");
}

bool McpCore::worker_busy() const { return m_worker.busy && m_worker.busy(); }

bool McpCore::claim_worker(const std::string& job)
{
    if (job.empty() || !worker_busy())
        return false;
    {
        std::lock_guard<std::mutex> lock(m_worker_mutex);
        if (!m_worker_job.empty())
            return m_worker_job == job;
    }
    // Under another call's operation (a dialog answer during a slice) the job stays unclaimed; the
    // busy worker still keeps other changes out.
    if (!m_jobs.begin_operation(job).empty())
        return false;
    std::lock_guard<std::mutex> lock(m_worker_mutex);
    m_worker_job       = job;
    m_worker_cancelled = false;
    return true;
}

bool McpCore::cancel_worker(const std::string& job)
{
    {
        std::lock_guard<std::mutex> lock(m_worker_mutex);
        if (job.empty() || m_worker_job != job)
            return false;
        m_worker_cancelled = true;
    }
    if (m_worker.cancel)
        m_worker.cancel();
    return true;
}

ToolResult McpCore::await_worker(const std::string& job, const std::string& tool, ToolResult result)
{
    {
        std::lock_guard<std::mutex> lock(m_worker_mutex);
        if (m_worker_job != job)
            return result;
    }
    // Worker::cancel() does not wait, so after a cancel the job keeps admission until the worker is
    // really idle: until then the job may still touch the model.
    const auto deadline  = std::chrono::steady_clock::now() + m_timing.worker_timeout;
    bool       timed_out = false;
    while (!m_ui.is_shutdown()) {
        bool busy = true;
        try {
            busy = m_ui.run(GuardClass::Inspect, tool, [&] {
                if (!timed_out && std::chrono::steady_clock::now() >= deadline)
                    timed_out = cancel_worker(job);
                return nlohmann::json(worker_busy());
            }).get<bool>();
        } catch (const ToolFailure&) {
            // Not run (a native dialog holds the UI thread, or the UI is being rebuilt); ask again.
        }
        if (!busy)
            break;
        std::this_thread::sleep_for(m_timing.worker_poll);
    }
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_worker_mutex);
        cancelled = m_worker_cancelled;
        m_worker_job.clear();
        m_worker_cancelled = false;
    }
    if (m_ui.is_shutdown())
        return ToolResult::error("OrcaSlicer is shutting down");
    if (result.is_error)
        return result;
    if (timed_out)
        return ToolResult::error("The background job this call started did not finish within " +
                                     std::to_string(std::chrono::duration_cast<std::chrono::seconds>(m_timing.worker_timeout).count()) +
                                     " s, so it was cancelled",
                                 {{"job", job}});
    if (cancelled)
        return ToolResult::error("The background job this call started was cancelled", {{"job", job}});
    return result;
}

bool McpCore::acquire_slot()
{
    std::lock_guard<std::mutex> lock(m_slots_mutex);
    if (m_slots_used >= m_timing.blocking_slots)
        return false;
    ++m_slots_used;
    return true;
}

void McpCore::release_slot()
{
    std::lock_guard<std::mutex> lock(m_slots_mutex);
    --m_slots_used;
}

void McpCore::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_threads_mutex);
        m_stopping = true;
    }
    m_threads_cv.notify_all();
}

bool McpCore::drain(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_threads_mutex);
    return m_threads_cv.wait_for(lock, timeout, [this] { return m_active == 0; });
}

}}} // namespace Slic3r::GUI::Mcp
