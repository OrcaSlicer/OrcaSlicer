#include "McpPersonRun.hpp"

#include "JobRegistry.hpp"
#include "McpReplay.hpp"
#include "AutomationRuntime.hpp"
#include "McpTools.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/CommandTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

PersonRun::PersonRun(AutomationRuntime& runtime, Events events) : m_runtime(runtime), m_events(std::move(events)) {}

std::string PersonRun::job() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_job;
}

void PersonRun::set_job(const std::string& job)
{
    bool stop = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_job = job;
        stop  = m_stop;
    }
    if (stop)
        m_runtime.jobs().request_stop(job);
}

void PersonRun::stop()
{
    std::string job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        job    = m_job;
    }
    if (job.empty() || m_runtime.stopping())
        return;
    m_runtime.jobs().request_stop(job);
    // The step's own operation stops too, as job_cancel stops it.
    m_runtime.cancel_operations_of(job);
}

void PersonRun::detach() { m_detached = true; }

void PersonRun::post_step_started(std::size_t index)
{
    m_runtime.post_ui([self = shared_from_this(), index] {
        if (!self->m_detached && self->m_events.step_started)
            self->m_events.step_started(index);
    });
}

void PersonRun::post_step_finished(nlohmann::json entry)
{
    m_runtime.post_ui([self = shared_from_this(), entry = std::move(entry)] {
        if (!self->m_detached && self->m_events.step_finished)
            self->m_events.step_finished(entry);
    });
}

void PersonRun::post_finished(nlohmann::json result, nlohmann::json jobs)
{
    m_runtime.post_ui([self = shared_from_this(), result = std::move(result), jobs = std::move(jobs)] {
        if (!self->m_detached && self->m_events.finished)
            self->m_events.finished(result, jobs);
    });
}

std::shared_ptr<PersonRun> start_replay_run(AutomationRuntime& runtime, Replay replay, ReplayContext sctx,
                                            std::optional<std::size_t> only_step, bool discard_authorized, PersonRun::Events events)
{
    const NativeCommand* run_replay_def = NativeCommands::instance().find("replay_run");
    if (run_replay_def == nullptr || runtime.stopping())
        return nullptr;

    auto run      = std::make_shared<PersonRun>(runtime, std::move(events));
    auto shared   = std::make_shared<Replay>(std::move(replay));
    auto ctx_held = std::make_shared<ReplayContext>(std::move(sctx));
    // replay_run's properties, outside the session journal: a person's run is not an agent's call.
    NativeCommand def = *run_replay_def;
    def.journal       = false;
    def.run           = [&runtime, run, shared, ctx_held, only_step, discard_authorized](const json&, ToolContext& ctx) {
        const std::string job = ctx.job_id();
        run->set_job(job);
        if (discard_authorized)
            runtime.jobs().allow_discard(job);
        ReplayRunOptions options;
        options.only_step     = only_step;
        options.write_result  = !only_step;
        options.step_started  = [run](std::size_t index) { run->post_step_started(index); };
        options.step_finished = [run](const json& entry) { run->post_step_finished(entry); };
        return ToolResult::ok(run_replay_job(runtime, ctx, *shared, *ctx_held, options));
    };

    FollowedCall call;
    call.command = std::move(def);
    call.origin  = {{"source", "action_replay"}};
    call.person  = true;
    call.ended   = [&runtime, run, ctx_held](const std::optional<ToolResult>& r) {
        run->mark_ended();
        if (!r || runtime.stopping())
            return;
        // The handler has returned, so the jobs it saved are settled. A run refused before its handler started
        // (another operation holds the project) saved none and replaces none.
        run->post_finished(r->is_error ? json{{"error", r->message}} : r->data, run->job().empty() ? json() : ctx_held->jobs);
    };
    call.stop = [run] { run->stop(); };
    if (!runtime.follow(std::move(call)))
        return nullptr;
    return run;
}

}}} // namespace Slic3r::GUI::Mcp
