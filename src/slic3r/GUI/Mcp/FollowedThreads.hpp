#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

namespace Slic3r { namespace GUI { namespace Mcp {

// The threads the automation runtime owns to follow calls to their end (AutomationRuntime::follow()), each with what
// stops its work early. A work has returned once its function has returned; what it holds is released after
// that, on its own thread. Thread-safe.
class FollowedThreads
{
public:
    FollowedThreads() = default;
    FollowedThreads(const FollowedThreads&)            = delete;
    FollowedThreads& operator=(const FollowedThreads&) = delete;
    // Joins what is left, as join_all() does.
    ~FollowedThreads();

    // Joins the threads whose work has returned, then runs `work` on a new one. `stop`, when given, is what
    // stop_all() calls while the work has not returned. False, with nothing started and neither `work` nor `stop`
    // kept, after join_all() or when the system cannot start a thread. `work` must not throw: an exception that
    // leaves it ends the process.
    bool start(std::function<void()> work, std::function<void()> stop = {});
    // Calls `stop` of every work that has not returned, outside the lock.
    void stop_all();
    // Refuses later starts, then joins every thread. The owner has made the works return.
    void join_all();
    // Threads not yet joined.
    std::size_t size() const;

private:
    struct Followed
    {
        std::thread                        thread;
        std::shared_ptr<std::atomic<bool>> returned;
        std::function<void()>              stop;
    };

    mutable std::mutex  m_mutex;
    std::list<Followed> m_threads;
    // Taken out of m_threads by join_all(), which joins them outside the lock.
    std::size_t         m_joining = 0;
    bool                m_closed  = false;
};

}}} // namespace Slic3r::GUI::Mcp
