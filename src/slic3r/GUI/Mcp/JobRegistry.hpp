#pragma once

#include "McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// Bookkeeping for mutate tool calls that run on their own threads. A call that outlives its reply
// budget stays here until it finishes, so a later `wait {for:"job"}` can return its result; the
// most recent finished jobs are kept for that purpose, and so is a finished job whose parent (a
// running script) may still wait for it, up to kKeepFinishedMax in all. Thread-safe.
class JobRegistry
{
public:
    static constexpr std::size_t kMaxRunning  = 64;
    static constexpr std::size_t kKeepFinished = 64;
    static constexpr std::size_t kKeepFinishedMax = 1024;

    // Registers a running job. Throws ToolFailure when kMaxRunning jobs are already running.
    std::string create(const std::string& tool, const std::string& parent);
    void        finish(const std::string& id, ToolResult result);

    // The job's result once it has finished; nullopt when it is still running after `timeout` or
    // the registry was woken for shutdown. Throws ToolFailure for an unknown id.
    std::optional<ToolResult> wait(const std::string& id, std::chrono::milliseconds timeout);
    // The same, but gives up early once `stop` is true, which it checks every half second. It looks
    // at the job at least once, so a zero timeout still returns a finished job's result.
    std::optional<ToolResult> wait(const std::string& id, std::chrono::milliseconds timeout, const std::function<bool()>& stop);

    // `id` followed by its parent chain.
    std::set<std::string> lineage(const std::string& id) const;
    // The same chain in order: `id`, its parent, its parent's parent, ...
    std::vector<std::string> ancestry(const std::string& id) const;
    // A stop asked for `id` reaches every call it and its descendants make (ToolContext::cancelled).
    // Requests are kept for the session: ids are never reused, and a descendant may still run after
    // its root has finished.
    void request_stop(const std::string& id);
    // True when a stop was asked for `id` or one of its ancestors.
    bool stop_requested(const std::string& id) const;
    // True when a running job exists that is not in `excluded`.
    bool any_running_except(const std::set<std::string>& excluded) const;
    // Operation admission: a job marked as an operation holds the project until it finishes.
    // Marks `id` unless another operation is running, whose id it then returns ("" on success).
    std::string              begin_operation(const std::string& id);
    std::vector<std::string> running_operations() const;

    // [{id, tool, parent, state, age_ms}] for get_state.
    nlohmann::json list() const;

    // Wakes every waiter; later waits return nullopt immediately.
    void wake_all();

private:
    struct Job
    {
        std::string                           tool;
        std::string                           parent;
        // The parent chain, nearest first, recorded at creation: a finished ancestor may be forgotten
        // while this job still runs, and the chain must still reach the root (a stop asked for it).
        std::vector<std::string>              ancestors;
        bool                                  finished  = false;
        bool                                  operation = false;
        ToolResult                            result;
        std::chrono::steady_clock::time_point created;
    };

    mutable std::mutex          m_mutex;
    std::condition_variable     m_cv;
    std::map<std::string, Job>  m_jobs;
    std::deque<std::string>     m_finished_order;
    std::set<std::string>       m_stop_requests;
    uint64_t                    m_next = 1;
    bool                        m_woken = false;
};

}}} // namespace Slic3r::GUI::Mcp
