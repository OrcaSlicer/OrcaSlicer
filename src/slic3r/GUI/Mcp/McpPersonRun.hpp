#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

class McpService;

// A run a person started from a window: a replay Action Replay starts through McpService::start_replay_run(),
// or a Run Script panel session through start_python_session(). An unjournaled job whose steps report as
// they start and end. The service owns the thread that follows it; the window holds it to stop it and drops
// its events when it closes.
class PersonRun : public std::enable_shared_from_this<PersonRun>
{
public:
    // Called on the UI thread, never after detach().
    struct Events
    {
        std::function<void(std::size_t index)>          step_started;
        std::function<void(const nlohmann::json& entry)> step_finished;
        // Exactly once: run_replay's result, or {"error": message} when the run failed as a call (refused,
        // or its job ended in an error or was lost); `jobs` are the jobs the run saved, for later steps,
        // or null when it was refused before it started.
        std::function<void(const nlohmann::json& result, const nlohmann::json& jobs)> finished;
    };

    PersonRun(McpService& service, Events events);

    // UI thread. Latches the stop, so one asked before the job exists still counts, asks the service to
    // stop the job's lineage once it is known, and cancels the run's operation (a slice, an export, a
    // background job) as cancel_job does. Does nothing once the service is stopping.
    void stop();
    // UI thread: the events are dropped from now on; the run carries on to its end.
    void detach();
    bool running() const { return m_running; }
    // A copy: the job thread sets it. "" until the job has started.
    std::string job() const;

    // Service side. The handler calls set_job() before any step runs; it applies a latched stop.
    void set_job(const std::string& job);
    void post_step_started(std::size_t index);
    void post_step_finished(nlohmann::json entry);
    void post_finished(nlohmann::json result, nlohmann::json jobs);
    void mark_ended() { m_running = false; }

private:
    McpService&       m_service;
    Events            m_events; // UI thread
    bool              m_detached = false; // UI thread
    mutable std::mutex m_mutex;
    std::string       m_job;
    bool              m_stop = false;
    std::atomic<bool> m_running{true};
};

}}} // namespace Slic3r::GUI::Mcp
