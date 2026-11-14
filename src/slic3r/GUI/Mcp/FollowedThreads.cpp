#include "FollowedThreads.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

FollowedThreads::~FollowedThreads() { join_all(); }

bool FollowedThreads::start(std::function<void()> work, std::function<void()> stop)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Under the lock join_all() closes with: a start either comes before the join takes the threads, and is
    // joined with them, or is refused.
    if (m_closed)
        return false;
    // A returned work's thread has only to release what the work held and end, and neither takes this lock.
    for (auto it = m_threads.begin(); it != m_threads.end();) {
        if (!*it->returned) {
            ++it;
            continue;
        }
        if (it->thread.joinable())
            it->thread.join();
        it = m_threads.erase(it);
    }
    auto        returned = std::make_shared<std::atomic<bool>>(false);
    std::thread thread;
    try {
        // What the work holds goes as the thread ends, after it counts as returned.
        thread = std::thread([work = std::move(work), returned] {
            work();
            *returned = true;
        });
    } catch (const std::system_error&) {
        // The system cannot start a thread: nothing is added to the list, and neither the work nor `stop` is kept.
        return false;
    }
    m_threads.push_back({std::move(thread), std::move(returned), std::move(stop)});
    return true;
}

void FollowedThreads::stop_all()
{
    std::vector<std::function<void()>> stops;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Followed& followed : m_threads)
            if (followed.stop && !*followed.returned)
                stops.push_back(followed.stop);
    }
    // Outside the lock: a stop runs its run's own code, which may call these threads too.
    for (const std::function<void()>& stop : stops)
        stop();
}

void FollowedThreads::join_all()
{
    std::list<Followed> joining;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        joining.splice(joining.end(), m_threads);
        m_joining += joining.size();
    }
    // Outside the lock: a start or size() from a work that is returning is answered, not kept waiting for the join.
    for (Followed& followed : joining)
        if (followed.thread.joinable())
            followed.thread.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_joining -= joining.size();
}

std::size_t FollowedThreads::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_threads.size() + m_joining;
}

}}} // namespace Slic3r::GUI::Mcp
