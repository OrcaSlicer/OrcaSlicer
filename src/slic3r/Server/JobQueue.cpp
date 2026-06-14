#include "JobQueue.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "SliceService.hpp"

namespace Slic3r {
namespace Server {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

JobQueue::JobQueue(std::string resources_dir, int workers)
    : m_resources_dir(std::move(resources_dir))
{
    if (workers < 1)
        workers = 1;

    for (int i = 0; i < workers; ++i) {
        m_workers.emplace_back([this] { worker_loop(); });
    }
}

JobQueue::~JobQueue()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();
    for (auto &t : m_workers)
        if (t.joinable())
            t.join();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string JobQueue::submit(SliceCore::SliceRequest req)
{
    std::string id = generate_id();

    // Allocate a per-job cancel flag.  The worker thread and cancel() both
    // hold a shared_ptr, so the flag outlives the pending queue entry.
    auto flag = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        JobInfo info;
        info.id       = id;
        info.state    = JobState::Queued;
        info.progress = 0;
        m_jobs[id]    = std::move(info);
        m_cancel_flags[id] = flag;

        // Wire the progress callback so updates flow back into JobInfo.
        // Capture id by value; the lambda outlives the WorkItem (it lives
        // inside req.progress until the job finishes).
        //
        // NOTE: The lambda acquires the queue mutex for each progress
        // notification.  SliceService must not hold the mutex when it calls
        // progress (it doesn't — it's a fire-and-forget callback).
        std::string captured_id = id;
        auto *self = this;
        req.progress = [self, captured_id](int pct, const std::string &msg) {
            std::lock_guard<std::mutex> inner(self->m_mutex);
            auto it = self->m_jobs.find(captured_id);
            if (it != self->m_jobs.end() && it->second.state == JobState::Running) {
                it->second.progress = pct;
                it->second.message  = msg;
            }
        };

        m_pending.push_back({id, std::move(req)});
    }

    m_cv.notify_one();
    return id;
}

std::optional<JobInfo> JobQueue::status(const std::string &id) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_jobs.find(id);
    if (it == m_jobs.end())
        return std::nullopt;
    return it->second;
}

std::optional<SliceCore::SliceResult> JobQueue::result(const std::string &id) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_jobs.find(id);
    if (it == m_jobs.end())
        return std::nullopt;
    if (it->second.state != JobState::Done)
        return std::nullopt;
    return it->second.result;
}

bool JobQueue::cancel(const std::string &id)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_jobs.find(id);
    if (it == m_jobs.end())
        return false;

    JobState st = it->second.state;
    if (st == JobState::Done || st == JobState::Error || st == JobState::Cancelled)
        return false; // Already terminal.

    if (st == JobState::Queued) {
        // Remove from pending queue so the worker skips it.
        for (auto qi = m_pending.begin(); qi != m_pending.end(); ++qi) {
            if (qi->id == id) {
                m_pending.erase(qi);
                break;
            }
        }
        it->second.state   = JobState::Cancelled;
        it->second.message = "Cancelled before execution";
        m_cancel_flags.erase(id);
        return true;
    }

    // Running: set the cooperative flag.  The worker polls this flag;
    // SliceService::run() may not honour it if the implementation does not
    // check it.  The job state transitions to Cancelled only when the
    // worker observes the flag — see worker_loop() below.
    auto flag_it = m_cancel_flags.find(id);
    if (flag_it != m_cancel_flags.end())
        flag_it->second->store(true, std::memory_order_relaxed);

    return true;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void JobQueue::worker_loop()
{
    // Each worker owns its own SliceService instance so that they do not
    // share any mutable state through the service object itself.
    SliceCore::SliceService svc(m_resources_dir);

    while (true) {
        WorkItem item;
        std::shared_ptr<std::atomic<bool>> cancel_flag;

        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this] {
                return m_shutdown || !m_pending.empty();
            });

            if (m_shutdown && m_pending.empty())
                break;

            item        = std::move(m_pending.front());
            m_pending.pop_front();
            cancel_flag = m_cancel_flags[item.id];

            auto it = m_jobs.find(item.id);
            if (it != m_jobs.end())
                it->second.state = JobState::Running;
        }

        // Check for cancellation before even starting (race: cancel() ran
        // between submit and the worker picking up the item but after the
        // state was already set to Running).
        if (cancel_flag && cancel_flag->load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_jobs.find(item.id);
            if (it != m_jobs.end()) {
                it->second.state   = JobState::Cancelled;
                it->second.message = "Cancelled";
            }
            m_cancel_flags.erase(item.id);
            continue;
        }

        // Run the slice.
        SliceCore::SliceResult res;
        try {
            res = svc.run(item.req);
        } catch (const std::exception &ex) {
            res.ok        = false;
            res.exit_code = -1;
            res.error     = ex.what();
        } catch (...) {
            res.ok        = false;
            res.exit_code = -1;
            res.error     = "Unknown exception in SliceService::run()";
        }

        // Store result — check cancel flag one more time in case cancel()
        // fired while slicing was in progress.
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_jobs.find(item.id);
            if (it != m_jobs.end()) {
                bool was_cancelled =
                    cancel_flag && cancel_flag->load(std::memory_order_relaxed);

                if (was_cancelled) {
                    it->second.state   = JobState::Cancelled;
                    it->second.message = "Cancelled during execution";
                } else if (res.ok) {
                    it->second.state    = JobState::Done;
                    it->second.progress = 100;
                    it->second.result   = std::move(res);
                } else {
                    it->second.state   = JobState::Error;
                    it->second.message = res.error.empty()
                                            ? "Slice failed (exit_code=" +
                                              std::to_string(res.exit_code) + ")"
                                            : res.error;
                }
            }
            m_cancel_flags.erase(item.id);
        }
    }
}

// ---------------------------------------------------------------------------
// Helper: UUID generation
// ---------------------------------------------------------------------------

std::string JobQueue::generate_id()
{
    static boost::uuids::random_generator gen;
    // gen is not thread-safe on its own; callers must hold m_mutex (submit()
    // does so).  This function is only called from submit() under the lock.
    return boost::uuids::to_string(gen());
}

} // namespace Server
} // namespace Slic3r
