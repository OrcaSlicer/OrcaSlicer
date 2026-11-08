#include "PythonSession.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r {

bool PythonSession::submit(PythonWork work)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queued || m_running || m_end || m_returned)
            return false;
        m_queued       = std::move(work);
        m_queued_stops = m_stop_requests.load();
    }
    m_cv.notify_all();
    return true;
}

void PythonSession::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_returned || (!m_queued && !m_running && !m_end && !m_has_threads.load()))
            return;
        ++m_stop_requests;
    }
    m_cv.notify_all();
}

void PythonSession::end()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_end = true;
    }
    m_cv.notify_all();
}

bool PythonSession::working() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queued.has_value() || m_running;
}

bool PythonSession::ended() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_returned;
}

std::vector<PythonSessionEvent> PythonSession::take_events()
{
    std::lock_guard<std::mutex>     lock(m_mutex);
    std::vector<PythonSessionEvent> events;
    events.swap(m_events);
    return events;
}

std::optional<PythonWork> PythonSession::take_work(std::chrono::milliseconds timeout, std::uint64_t& stops_before)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait_for(lock, timeout, [this] { return m_queued.has_value() || m_end; });
    if (!m_queued)
        return std::nullopt;
    stops_before                   = m_queued_stops;
    std::optional<PythonWork> work = std::move(m_queued);
    m_queued.reset();
    m_running = true;
    return work;
}

bool PythonSession::end_requested() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_end;
}

void PythonSession::work_done()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_running = false;
}

void PythonSession::push_event(PythonSessionEvent event)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back(std::move(event));
}

void PythonSession::host_returned()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_returned = true;
        m_running  = false;
        m_queued.reset();
    }
    m_has_threads.store(false);
    m_cv.notify_all();
}

} // namespace Slic3r
