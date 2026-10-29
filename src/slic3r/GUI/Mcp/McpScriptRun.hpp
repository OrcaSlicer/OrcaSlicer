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

// A script run the script runner dialog started through McpService::start_script_run(): an
// unjournaled job whose steps report as they start and end. The service owns the thread that follows
// it; the dialog holds it to stop it and drops its events when it closes.
class ScriptRun : public std::enable_shared_from_this<ScriptRun>
{
public:
    // Called on the UI thread, never after detach().
    struct Events
    {
        std::function<void(std::size_t index)>          step_started;
        std::function<void(const nlohmann::json& entry)> step_finished;
        // Exactly once: run_script's result, or {"error": message} when the run failed as a call (refused,
        // or its job ended in an error or was lost); `jobs` are the jobs the run saved, for later steps,
        // or null when it was refused before it started.
        std::function<void(const nlohmann::json& result, const nlohmann::json& jobs)> finished;
    };

    ScriptRun(McpService& service, Events events);

    // UI thread. Latches the stop, so one asked before the job exists still counts, asks the service to
    // stop the job's lineage once it is known, and cancels the run's operation (a slice, an export, a
    // background job) as the cancel tool does. Does nothing once the service is stopping.
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
