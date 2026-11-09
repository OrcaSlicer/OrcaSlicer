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
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// What a call of a plugin that has unloaded fails with.
inline constexpr char kPluginUnloaded[] = "The plugin that made this call has been unloaded";

// Bookkeeping for mutate tool calls that run on their own threads. A call that outlives its reply
// budget stays here until it finishes, so a later `wait {for:"job"}` can return its result; the
// most recent finished jobs are kept for that purpose, and so is a finished job whose parent (a
// running replay) may still wait for it, up to kKeepFinishedMax in all. Thread-safe.
class JobRegistry
{
public:
    static constexpr std::size_t kMaxRunning  = 64;
    static constexpr std::size_t kKeepFinished = 64;
    static constexpr std::size_t kKeepFinishedMax = 1024;

    // Registers a running job. `owner` is the plugin whose call it is ("" for anyone else's);
    // a child job takes its parent's. Throws ToolFailure when kMaxRunning jobs are already running, and
    // kPluginUnloaded when its plugin is unloaded(), which is asked once the job is registered: a plugin
    // found loaded then was loaded while the job was there for its unload to find, so every job of the
    // plugin is either refused or one the unload finds running.
    std::string create(const std::string& tool, const std::string& parent, const std::string& owner = {});
    // Whether the call's plugin (`owner`, else the owner of the job `parent`) is blocked or, by the liveness
    // hook, no longer loaded. False for a call that is not a plugin's.
    bool unloaded(const std::string& parent, const std::string& owner) const;
    // Whether a plugin is loaded, asked on the calling thread without the registry's lock held. It is the
    // plugin manager's own state, so it changes the moment the plugin unloads, while the block comes later
    // from the UI thread. Set once, before any job is created.
    void set_owner_liveness(std::function<bool(const std::string&)> loaded) { m_owner_loaded = std::move(loaded); }
    // The plugin that owns `id`, or "" for anyone else's job, or one no longer known.
    std::string owner(const std::string& id) const;
    // The running jobs `owner` (a plugin) owns.
    std::vector<std::string> running_owned_by(const std::string& owner) const;
    // While `owner` (an unloaded plugin) is blocked, create() refuses its jobs and its jobs' children. The
    // block is checked as the job is registered, so a job registered just before it is among the jobs
    // running_owned_by() returns, which the unload then stops.
    void block_owner(const std::string& owner);
    void unblock_owner(const std::string& owner);
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
    // True when a running job exists that is not in `excluded` and isn't set idle.
    bool any_running_except(const std::set<std::string>& excluded) const;
    // Marks a running job as one that leaves `wait {for: "idle"}` to the calls it makes: a run_script run, which may
    // wait for a person in its window for minutes. list() shows it as idle.
    void set_idle(const std::string& id);
    // Operation admission: a job marked as an operation holds the project until it finishes.
    // Marks `id` unless another operation is running, whose id it then returns ("" on success).
    std::string              begin_operation(const std::string& id);
    std::vector<std::string> running_operations() const;

    // [{id, tool, parent, state, age_ms}] for get_app_state.
    nlohmann::json list() const;
    // The tool of `id` while it runs; empty for an unknown or finished job.
    std::string running_tool(const std::string& id) const;

    // Wakes every waiter; later waits return nullopt immediately.
    void wake_all();

private:
    // create() without the liveness check.
    std::string register_job(const std::string& tool, const std::string& parent, const std::string& owner);

    struct Job
    {
        std::string                           tool;
        std::string                           parent;
        std::string                           owner;
        // The parent chain, nearest first, recorded at creation: a finished ancestor may be forgotten
        // while this job still runs, and the chain must still reach the root (a stop asked for it).
        std::vector<std::string>              ancestors;
        bool                                  finished  = false;
        bool                                  operation = false;
        bool                                  idle      = false;
        ToolResult                            result;
        std::chrono::steady_clock::time_point created;
    };

    mutable std::mutex          m_mutex;
    std::condition_variable     m_cv;
    std::map<std::string, Job>  m_jobs;
    std::deque<std::string>     m_finished_order;
    std::set<std::string>       m_stop_requests;
    std::set<std::string>       m_blocked_owners;
    std::function<bool(const std::string&)> m_owner_loaded;
    uint64_t                    m_next = 1;
    bool                        m_woken = false;
};

// A parent for calls, not a call itself: a running job under `parent`, set idle, so a stop asked for it reaches every
// call made under it, inline ones included, without stopping `parent`. renew() finishes it and makes a fresh one, whose
// calls run again; the destructor finishes it, and nothing else does. A Run Script session's calls hang under one.
// Any thread.
class LineageNode
{
public:
    // Throws ToolFailure as JobRegistry::create() does.
    LineageNode(JobRegistry& jobs, std::string tool, std::string parent);
    ~LineageNode();
    LineageNode(const LineageNode&)            = delete;
    LineageNode& operator=(const LineageNode&) = delete;

    std::string id() const;
    // Throws ToolFailure as JobRegistry::create() does, keeping the current node.
    void        renew();

private:
    std::string make() const;

    JobRegistry&       m_jobs;
    const std::string  m_tool;
    const std::string  m_parent;
    mutable std::mutex m_mutex;
    std::string        m_id;
};

// Refuses a plugin's call (ToolContext::caller_plugin()) about a job its own calls did not start: the
// session tools act only on a plugin's own jobs. `what` names the action, as in "waits for".
void require_own_job(const ToolContext& ctx, const JobRegistry& jobs, const std::string& job, const char* what);

}}} // namespace Slic3r::GUI::Mcp
