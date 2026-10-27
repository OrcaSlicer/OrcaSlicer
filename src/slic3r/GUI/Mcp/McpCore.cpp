#include "McpCore.hpp"

#include "McpJournal.hpp"

#include <exception>
#include <memory>
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

    nlohmann::json run_ui(const std::function<nlohmann::json()>& fn) override { return m_core.ui().run(m_def.guard, m_def.name, fn); }
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

McpCore::~McpCore() { shutdown(); }

ToolResult McpCore::call(const ToolDef& def, const nlohmann::json& args) { return run(def, args, {}); }

ToolResult McpCore::call_child(const ToolDef& def, const nlohmann::json& args, const std::string& parent_job)
{
    return run(def, args, parent_job);
}

ToolResult McpCore::run(const ToolDef& def, const nlohmann::json& args, const std::string& parent)
{
    if (m_ui.is_shutdown())
        return ToolResult::error("OrcaSlicer is shutting down");

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
        if (const auto busy = m_jobs.running_of(m_operation_tools, {}); !busy.empty())
            return finish(ToolResult::error("Busy: job " + busy.front() +
                                                " (a slice or export) is running; wait for it or cancel it before changing the project",
                                            {{"job", busy.front()}}));

    std::string job;
    try {
        job = m_jobs.create(def.name, parent);
    } catch (const ToolFailure& f) {
        return finish(ToolResult::error(f.what()));
    }

    {
        std::lock_guard<std::mutex> lock(m_threads_mutex);
        if (m_stopping)
            return finish(ToolResult::error("OrcaSlicer is shutting down"));
        ++m_threads;
    }
    // The job thread owns copies of what it needs; the core itself outlives it (shutdown() waits).
    std::thread([this, def, args, job, parent, seq]() {
        CallContext ctx(*this, def, job, parent, m_journal, seq);
        ToolResult  result = invoke(def, args, ctx);
        m_jobs.finish(job, std::move(result));
        {
            std::lock_guard<std::mutex> lock(m_threads_mutex);
            --m_threads;
        }
        m_threads_cv.notify_all();
    }).detach();

    auto envelope = [&](const char* status, nlohmann::json extra) {
        nlohmann::json out{{"status", status}, {"job", job}, {"tool", def.name}};
        out.update(extra);
        return ToolResult::ok(std::move(out));
    };
    const std::string running_note = "The call is still running and will finish on its own; do not retry it. "
                                     "Use wait {for:\"job\", job:\"" + job + "\"} for its result.";
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
        if (auto p = m_ui.probe(m_timing.probe_timeout); p && p->guards.modal) {
            // A dialog can close between the probe and here; a finished job wins.
            if (auto r = m_jobs.wait(job, std::chrono::milliseconds(0)))
                return finish(std::move(*r));
            return finish(envelope("blocked_by_dialog",
                                   {{"dialogs", p->guards.dialogs},
                                    {"note", "The call is waiting on a dialog and continues after it closes; do not retry it. "
                                             "Answer the dialog (ui_snapshot, dialog_respond), then wait {for:\"job\", job:\"" + job + "\"}."}}));
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
    std::unique_lock<std::mutex> lock(m_threads_mutex);
    m_stopping = true;
    m_threads_cv.wait_for(lock, m_timing.join_timeout, [this] { return m_threads == 0; });
}

}}} // namespace Slic3r::GUI::Mcp
