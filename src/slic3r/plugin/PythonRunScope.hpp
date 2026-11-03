#pragma once

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace Slic3r {

class ToolBridge;

// What a thread is doing for the run bound to it.
enum class PythonRunThread {
    Main,     // the run's own thread, which runs its code
    Started,  // a thread the run started, or a pool worker running one of the run's tasks
    Callback, // a window callback of the run, on the UI thread
};

// One run_python run, shared by the thread that runs its code, the threads it starts, the pool tasks it submits and
// its window callbacks: who it runs as, its file, its tool bridge, the work it waits for before it ends, and whether
// it still takes new work and opens UI. Runs can overlap, so everything a run changes is keyed by it.
//
// Lock order: the GIL before this object's mutex. Nothing that holds the mutex waits for the GIL, so the watcher may
// raise interrupts under it while a thread of the run, holding the GIL, waits to record that it ended.
class PythonRunScope
{
public:
    PythonRunScope(std::uint64_t id, std::string identity, boost::filesystem::path file, ToolBridge* bridge);

    std::uint64_t                  id() const { return m_id; }
    // kScriptIdentityPerson or kScriptIdentityAgent for a person's or an agent's script, or a plugin's key.
    const std::string&             identity() const { return m_identity; }
    // The file the code was read from; empty for code sent as text.
    const boost::filesystem::path& file() const { return m_file; }
    // A script's windows belong to its run ("script/<id>"; '/' never occurs in a plugin key) and close when it ends;
    // a plugin's run_python leaves them to the plugin, which closes them when it unloads.
    bool                           owns_ui() const;
    std::string                    ui_owner() const;

    // orca.host.tools from any thread of the run, called without the GIL. Throws std::runtime_error once the run
    // has ended; a bridge's ToolCallError passes through.
    nlohmann::json call_tool(const std::string& tool, const nlohmann::json& args);
    bool           has_tools() const;
    bool           tools_closed() const;
    // Refuses new calls, then waits for the calls already inside the bridge, which holds the run's ToolContext.
    void           close_tools();

    // A thread the run starts is counted from the moment the starting thread asks for it, before the OS thread
    // exists, so the run never misses one started late. `waited`: the run waits for it before it ends. False once
    // the run's admission has closed.
    bool thread_starting(bool waited);
    void thread_start_failed(bool waited);
    void thread_started(unsigned long ident, bool waited);
    void thread_ended(unsigned long ident);

    // A window callback of the run, while it runs. False once the run's admission has closed: it does nothing.
    bool callback_entering();
    void callback_left();

    // A task the run submitted to a pool, from submit() until its work item has run or was cancelled. False once
    // the run's admission has closed.
    bool task_submitted();
    void task_done();
    // The pool worker running one of the run's tasks is an interrupt target for that call.
    void task_running(unsigned long ident);
    void task_finished(unsigned long ident);

    // Closes admission when nothing the run waits for is left: no waited thread pending or alive, no callback
    // running, no task outstanding. Returns whether admission is closed.
    bool close_admission_if_quiet();
    void close_admission();
    bool admission_closed() const;

    // Calls `fn` for every interrupt target of the run (its live threads and the workers running its tasks), under
    // the mutex, so a target can't end, and its ident be reused, while `fn` aims at it.
    void for_each_interrupt_target(const std::function<void(unsigned long)>& fn) const;

    // Set at the run's first stop or time-out: its tasks that a pool has not started yet don't run.
    bool stopped() const { return m_stopped.load(); }
    void mark_stopped() { m_stopped.store(true); }

    // Set when the run's UI is torn down: nothing more opens, and its callbacks do nothing.
    bool ui_closed() const { return m_ui_closed.load(); }
    void mark_ui_closed() { m_ui_closed.store(true); }

private:
    const std::uint64_t           m_id;
    const std::string             m_identity;
    const boost::filesystem::path m_file;

    mutable std::mutex      m_mutex;
    std::condition_variable m_calls_cv;
    ToolBridge*             m_bridge = nullptr;
    bool                    m_tools_closed = false;
    int                     m_calls_in_flight = 0;

    bool                                m_admission_closed = false;
    int                                 m_waited_pending = 0;
    std::map<unsigned long, bool>       m_live_threads; // ident -> waited
    int                                 m_callbacks_active = 0;
    int                                 m_tasks_outstanding = 0;
    std::multiset<unsigned long>        m_task_targets;

    std::atomic<bool> m_stopped{false};
    std::atomic<bool> m_ui_closed{false};
};

// The run bound to the calling thread, and what the thread does for it; `run` is null outside a run.
struct PythonRunBinding
{
    std::shared_ptr<PythonRunScope> run;
    PythonRunThread                 kind = PythonRunThread::Main;
};
const PythonRunBinding& current_python_run();

// Binds `run` to the calling thread for the scope's lifetime, restoring the previous binding after.
class ScopedPythonRun
{
public:
    ScopedPythonRun(std::shared_ptr<PythonRunScope> run, PythonRunThread kind);
    ~ScopedPythonRun();
    ScopedPythonRun(const ScopedPythonRun&)            = delete;
    ScopedPythonRun& operator=(const ScopedPythonRun&) = delete;

private:
    PythonRunBinding m_previous;
};

} // namespace Slic3r
