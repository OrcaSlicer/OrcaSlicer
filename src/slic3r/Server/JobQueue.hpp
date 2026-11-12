#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "SliceTypes.hpp"

namespace Slic3r {
namespace Server {

enum class JobState {
    Queued,
    Running,
    Done,
    Error,
    Cancelled
};

struct JobInfo {
    std::string              id;
    JobState                 state   = JobState::Queued;
    int                      progress = 0;
    std::string              message;
    SliceCore::SliceResult   result;
};

// Thread-safe async job manager.
//
// Default worker count is 1 because Slic3r's slicing pipeline is not proven
// reentrant: static state, wxString conversions, and non-thread-safe caches
// inside Print.cpp and related translation units have not been audited for
// concurrent use.  A single worker serialises all slicing work safely.
//
// To increase throughput: set workers > 1 only after confirming that
// SliceService::run() is safe to call concurrently from multiple threads.
class JobQueue {
public:
    // resources_dir  — passed directly to SliceService (each worker owns one instance).
    // workers        — number of worker threads (default 1; see note above).
    explicit JobQueue(std::string resources_dir, int workers = 1);

    // Stops accepting new jobs, drains running jobs, joins all workers.
    ~JobQueue();

    // Submit a new slicing request.  Returns the opaque job id (UUID v4 hex).
    std::string submit(SliceCore::SliceRequest req);

    // Returns the current JobInfo snapshot, or nullopt if id is unknown.
    std::optional<JobInfo> status(const std::string &id) const;

    // Returns the SliceResult if the job reached Done state, else nullopt.
    std::optional<SliceCore::SliceResult> result(const std::string &id) const;

    // Cancel a job.
    //   - Queued  → marked Cancelled immediately; worker will skip it.
    //   - Running → sets a cooperative cancellation flag; the worker checks
    //               it between slice operations.  Slicing may not abort
    //               instantly if SliceService::run() does not poll the flag.
    //   Returns true if the job was found and could be cancelled; false if
    //   the id is unknown or the job already finished.
    bool cancel(const std::string &id);

private:
    // Internal work item queued to a worker.
    struct WorkItem {
        std::string               id;
        SliceCore::SliceRequest   req;
    };

    // Per-job cancel flag pointer stored alongside JobInfo so the running
    // worker can poll it.  Allocated on submit, freed after job completes.
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> m_cancel_flags;

    // Ordered work queue (FIFO).
    std::deque<WorkItem>        m_pending;

    // All known jobs, keyed by id.
    std::unordered_map<std::string, JobInfo> m_jobs;

    mutable std::mutex          m_mutex;
    std::condition_variable     m_cv;
    bool                        m_shutdown = false;

    std::string                 m_resources_dir;
    std::vector<std::thread>    m_workers;

    // Worker thread body.
    void worker_loop();

    // Generate a UUID v4 hex string using boost::uuids.
    static std::string generate_id();
};

} // namespace Server
} // namespace Slic3r
