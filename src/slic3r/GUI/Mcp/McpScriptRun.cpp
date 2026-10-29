#include "McpScriptRun.hpp"
#include "JobRegistry.hpp"
#include "McpCore.hpp"
#include "McpService.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

ScriptRun::ScriptRun(McpService& service, Events events) : m_service(service), m_events(std::move(events)) {}

std::string ScriptRun::job() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_job;
}

void ScriptRun::set_job(const std::string& job)
{
    bool stop = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_job = job;
        stop  = m_stop;
    }
    if (stop)
        m_service.request_stop(job);
}

void ScriptRun::stop()
{
    std::string job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        job    = m_job;
    }
    if (job.empty() || m_service.stopping())
        return;
    m_service.request_stop(job);
    // The step's own operation stops too, as the cancel tool stops it: a background job through the
    // worker, a slice or export through the plater.
    for (const std::string& op : m_service.jobs().running_operations())
        if (m_service.jobs().lineage(op).count(job) != 0 && !m_service.core().cancel_worker(op))
            if (Plater* plater = wxGetApp().plater())
                plater->cancel_slicing();
}

void ScriptRun::detach() { m_detached = true; }

void ScriptRun::post_step_started(std::size_t index)
{
    m_service.post_ui([self = shared_from_this(), index] {
        if (!self->m_detached && self->m_events.step_started)
            self->m_events.step_started(index);
    });
}

void ScriptRun::post_step_finished(nlohmann::json entry)
{
    m_service.post_ui([self = shared_from_this(), entry = std::move(entry)] {
        if (!self->m_detached && self->m_events.step_finished)
            self->m_events.step_finished(entry);
    });
}

void ScriptRun::post_finished(nlohmann::json result, nlohmann::json jobs)
{
    m_service.post_ui([self = shared_from_this(), result = std::move(result), jobs = std::move(jobs)] {
        if (!self->m_detached && self->m_events.finished)
            self->m_events.finished(result, jobs);
    });
}

}}} // namespace Slic3r::GUI::Mcp
