#include "McpPersonRun.hpp"
#include "McpService.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

PersonRun::PersonRun(McpService& service, Events events) : m_service(service), m_events(std::move(events)) {}

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
        m_service.request_stop(job);
}

void PersonRun::stop()
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
    // The step's own operation stops too, as cancel_job stops it.
    m_service.cancel_operations_of(job);
}

void PersonRun::detach() { m_detached = true; }

void PersonRun::post_step_started(std::size_t index)
{
    m_service.post_ui([self = shared_from_this(), index] {
        if (!self->m_detached && self->m_events.step_started)
            self->m_events.step_started(index);
    });
}

void PersonRun::post_step_finished(nlohmann::json entry)
{
    m_service.post_ui([self = shared_from_this(), entry = std::move(entry)] {
        if (!self->m_detached && self->m_events.step_finished)
            self->m_events.step_finished(entry);
    });
}

void PersonRun::post_finished(nlohmann::json result, nlohmann::json jobs)
{
    m_service.post_ui([self = shared_from_this(), result = std::move(result), jobs = std::move(jobs)] {
        if (!self->m_detached && self->m_events.finished)
            self->m_events.finished(result, jobs);
    });
}

}}} // namespace Slic3r::GUI::Mcp
