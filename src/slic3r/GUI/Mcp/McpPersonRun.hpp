#pragma once

#include "McpReplay.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

class AutomationRuntime;

// A run a person started from a window: a replay Action Replay starts through start_replay_run(), or a Run
// Script panel session through start_python_session(). An unjournaled job whose steps report as they start and
// end. The runtime follows it on a thread it owns (AutomationRuntime::follow()); the window holds it to stop it and
// drops its events when it closes.
class PersonRun : public std::enable_shared_from_this<PersonRun>
{
public:
    // Called on the UI thread, never after detach().
    struct Events
    {
        std::function<void(std::size_t index)>          step_started;
        std::function<void(const nlohmann::json& entry)> step_finished;
        // Exactly once: replay_run's result, or {"error": message} when the run failed as a call (refused,
        // or its job ended in an error or was lost); `jobs` are the jobs the run saved, for later steps,
        // or null when it was refused before it started.
        std::function<void(const nlohmann::json& result, const nlohmann::json& jobs)> finished;
    };

    PersonRun(AutomationRuntime& runtime, Events events);

    // UI thread. Latches the stop, so one asked before the job exists still counts, stops the job's lineage
    // once it is known (JobRegistry::request_stop), and cancels the run's operation (a slice, an export, a
    // background job) as job_cancel does. Does nothing once the runtime is stopping.
    void stop();
    // UI thread: the events are dropped from now on; the run carries on to its end.
    void detach();
    bool running() const { return m_running; }
    // A copy: the job thread sets it. "" until the job has started.
    std::string job() const;

    // Runtime side. The handler calls set_job() before any step runs; it applies a latched stop.
    void set_job(const std::string& job);
    void post_step_started(std::size_t index);
    void post_step_finished(nlohmann::json entry);
    void post_finished(nlohmann::json result, nlohmann::json jobs);
    void mark_ended() { m_running = false; }

private:
    AutomationRuntime& m_runtime;
    Events             m_events;           // UI thread
    bool               m_detached = false; // UI thread
    mutable std::mutex m_mutex;
    std::string        m_job;
    bool               m_stop = false;
    std::atomic<bool>  m_running{true};
};

// Runs a replay for the Action Replay window: an unjournaled mutate job, as replay_run runs one, whose steps
// report through `events`. `runtime` follows the job to its end, past a running or blocked_by_dialog reply, and
// posts `finished`. `only_step` runs one step (no result.json). `discard_authorized`: the user chose not to save
// before a run that replaces the project, so the replacing tools may drop unsaved changes while it runs. nullptr
// when the runtime is not started or is stopping, or when the system cannot start a thread. UI thread.
std::shared_ptr<PersonRun> start_replay_run(AutomationRuntime& runtime, Replay replay, ReplayContext sctx,
                                            std::optional<std::size_t> only_step, bool discard_authorized,
                                            PersonRun::Events events);

}}} // namespace Slic3r::GUI::Mcp
