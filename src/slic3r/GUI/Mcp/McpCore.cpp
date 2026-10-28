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

class CallContext final : public ToolContext
{
public:
    CallContext(McpCore& core, const ToolDef& def, std::string job, std::string parent, Journal* journal, uint64_t journal_seq)
        : m_core(core), m_def(def), m_job(std::move(job)), m_parent(std::move(parent)), m_journal(journal), m_seq(journal_seq)
    {}

    nlohmann::json run_ui(const std::function<nlohmann::json()>& fn) override
    {
        if (m_def.guard != GuardClass::Mutate || m_def.allowed_during_operation)
            return m_core.ui().run(m_def.guard, m_def.name, fn);
        // Admission again on the UI thread, where operations start: a call admitted before an
        // operation began must not change the project under it.
        return m_core.ui().run(m_def.guard, m_def.name, [&]() -> nlohmann::json {
            for (const std::string& op : m_core.jobs().running_operations())
                if (op != m_job)
                    throw ToolFailure("Busy: job " + op + " (a slice or export) is running; wait for it or cancel it before changing the project",
                                      {{"job", op}});
            return fn();
        });
    }
    nlohmann::json observe_ui(const std::function<nlohmann::json()>& fn) override
    {
        return m_core.ui().run(GuardClass::Inspect, m_def.name, fn);
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
            throw ToolFailure("Busy: job " + other + " (a slice or export) is running; wait for it or cancel it", {{"job", other}});
    }

private:
    McpCore&       m_core;
    const ToolDef& m_def;
    std::string    m_job;
    std::string    m_parent;
    Journal*       m_journal;
    uint64_t       m_seq;
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

McpCore::McpCore(const ToolRegistry& registry, UiInvoker& ui, JobRegistry& jobs, Journal* journal, CoreTiming timing)
    : m_registry(registry), m_ui(ui), m_jobs(jobs), m_journal(journal), m_timing(timing)
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
    const uint64_t seq     = journal ? m_journal->begin(def.name, args) : 0;
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

    // Operation admission: an MCP slice/export holds the project until it finishes; only recovery
    // input and the tools that may run during an operation (cancel) get through.
    if (def.guard == GuardClass::Mutate && !def.allowed_during_operation)
        if (const auto busy = m_jobs.running_operations(); !busy.empty())
            return finish(ToolResult::error("Busy: job " + busy.front() +
                                                " (a slice or export) is running; wait for it or cancel it before changing the project",
                                            {{"job", busy.front()}}));

    std::string job;
    try {
        job = m_jobs.create(def.name, parent);
    } catch (const ToolFailure& f) {
        return finish(ToolResult::error(f.what()));
    }

    if (!enter()) {
        m_jobs.finish(job, ToolResult::error("OrcaSlicer is shutting down"));
        return finish(ToolResult::error("OrcaSlicer is shutting down"));
    }
    // The job thread owns copies of what it needs; the core outlives it, as the owner destroys the
    // core only once drain() has seen every job thread leave.
    try {
        std::thread([this, def, args, job, parent, seq]() {
            CallContext ctx(*this, def, job, parent, m_journal, seq);
            ToolResult  result = invoke(def, args, ctx);
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
