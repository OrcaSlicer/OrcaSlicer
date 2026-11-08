#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// A piece of work a session runs in its namespace.
struct PythonWork
{
    enum class Kind { File, Cell, Console };
    Kind        kind = Kind::Cell;
    // The whole script (File), the cell's lines (Cell), or what was typed at the console (Console).
    std::string code;
    // The file line a Cell's code starts on, so its line numbers are the file's.
    int         first_line = 1;
    // The whole editor text, which the work's traceback quotes for frames in the session's file.
    std::string source;
    // A File work's cell first lines, ascending, starting at 1: its Cell events index them.
    std::vector<int> cell_starts;
};

struct PythonSessionEvent
{
    enum class Kind { Output, Cell, Result, Finished };
    Kind        kind = Kind::Output;
    // Output: what was printed, as it was printed. Result: a work's or a cell's result that JSON holds as is, as
    // compact JSON with json.dumps's separators; any other result is printed, so it comes as Output.
    std::string text;
    // Cell: the index in the running File work's cell_starts of the cell whose lines now run.
    int         cell = -1;
    // Finished: finished, raised, cancelled, timed_out or unavailable.
    std::string status;
    std::string message;
    std::string traceback;
    // The innermost traceback line in the session's file (a SyntaxError's line), 0 for none.
    int         error_line = 0;
    // False when the code never started: it didn't compile, or its dependencies didn't install.
    bool        executed = true;
    // Whether a File work sent Cell events.
    bool        cells_reported = false;
    // The names in the session's __main__ that don't start with '_', aren't `args` and aren't modules.
    int         variables = 0;
    // The JSON `result` a File work left, for run_python_code(); null otherwise.
    nlohmann::json result;
};

// One Python session: a __main__, folder, identity, output capture and watcher that outlive a single piece of work,
// hosted on one thread by host_python_session() (PythonScriptRunner.hpp). The person's side submits work and reads
// events from any thread; the class holds no Python and no wx.
class PythonSession
{
public:
    // Queues `work`. False while work is queued or running, after end(), or once the host returned.
    bool submit(PythonWork work);
    // Interrupts what runs now: the work, the session's threads and tasks, and once end() was asked its teardown.
    // Nothing when it is idle with none and not ending.
    void stop();
    // No more work: the session ends after the work running now. A stop() after it reaches the teardown, as a stopped
    // run's does, so whatever the teardown runs (atexit functions, finalizers) can be interrupted.
    void end();
    // Work is queued or running.
    bool working() const;
    // Threads, pool tasks or window callbacks the session started are still live.
    bool has_threads() const { return m_has_threads.load(); }
    // The host has returned: every event the session sends is queued.
    bool ended() const;
    std::vector<PythonSessionEvent> take_events();

    // Used only by host_python_session().
    // Waits up to `timeout` for queued work and takes it (it stays working() until work_done()), or nullopt.
    // `stops_before` is set to stop_requests() as the work was submitted: only later stops reach it.
    std::optional<PythonWork> take_work(std::chrono::milliseconds timeout, std::uint64_t& stops_before);
    // The stop() calls that asked for something so far.
    std::uint64_t stop_requests() const { return m_stop_requests.load(); }
    bool          end_requested() const;
    void          work_done();
    void          push_event(PythonSessionEvent event);
    void          set_has_threads(bool live) { m_has_threads.store(live); }
    // The host returns: nothing more is taken, and ended() turns true.
    void          host_returned();

private:
    mutable std::mutex              m_mutex;
    std::condition_variable         m_cv;
    std::optional<PythonWork>       m_queued;
    std::uint64_t                   m_queued_stops = 0;
    bool                            m_running  = false;
    bool                            m_end      = false;
    bool                            m_returned = false;
    std::vector<PythonSessionEvent> m_events;
    std::atomic<std::uint64_t>      m_stop_requests{0};
    std::atomic<bool>               m_has_threads{false};
};

} // namespace Slic3r
