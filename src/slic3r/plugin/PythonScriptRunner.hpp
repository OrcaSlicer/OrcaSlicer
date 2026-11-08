#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pybind11 {
class handle;
}

namespace Slic3r {

class PythonRunScope;
class PythonSession;

// A Python script a person or an agent starts (run_python, a .py in the Run Script dialog or
// --mcp-script), or a plugin's run_python: code run on the plugin interpreter without being packaged
// as a plugin.
struct PythonRunRequest
{
    // UTF-8 text; a file's bytes go through decode_python_source() first. A coding declaration in it changes
    // nothing, as for any code string Python compiles.
    std::string    code;
    // Bound to the module-level name `args`.
    nlohmann::json args = nlohmann::json::object();
    // The audit identity the code runs under: kScriptIdentityPerson or kScriptIdentityAgent for a
    // person's or an agent's script, which every audited access is allowed to apart from the categorical
    // denies, or a plugin's key, whose grants then apply.
    std::string    identity;
    // Shown in tracebacks. An existing file's path makes it a file run, which runs as `python file.py` would run
    // it: `__file__` and `sys.argv` name it, and its folder serves its imports. Any other name, such as the default
    // <run_python>, is code sent as text.
    std::string    filename = "<run_python>";
    // The run is interrupted as timed_out once it has run this long.
    std::chrono::milliseconds budget{std::chrono::minutes(5)};
    // Polled while the code runs, from another thread; true interrupts it as cancelled.
    std::function<bool()> cancelled;
    // Called once, from another thread, at the run's first stop or time-out, to cancel its tool calls in flight.
    std::function<void()> stop_calls;
};

struct PythonRunOutcome
{
    // finished, raised, cancelled, timed_out, or unavailable (no interpreter).
    std::string    status;
    // The value the code left in `result`, read once when the module body returned; null when it set
    // none or the run did not finish.
    nlohmann::json result;
    // What the run printed to stdout and stderr, in order: its own thread, the threads it starts and its window
    // callbacks.
    std::string    output;
    // The formatted traceback of a raised run.
    std::string    traceback;
    // Why a run did not finish, in one line.
    std::string    message;
};

// Runs `request.code` as the main program on the calling thread and returns how it ended. Holds an
// interpreter lease and the GIL for the run, under `request.identity`'s audit scope, in a fresh `__main__`
// module of its own where `args` is bound and `orca` is importable. Cancellation and the budget raise an
// exception in the running code at its next bytecode, again every second until it ends, so code
// blocked inside a C call stops once the call returns. A `result` that is not JSON (a dict with str
// keys, a list, a str, a finite number, a bool or None, nested) makes the run raised; SystemExit with
// code 0 or None finishes it.
PythonRunOutcome run_python_code(const PythonRunRequest& request);

// How a session is hosted: who it runs as, its file, and what its output, tracebacks and stops do.
struct PythonSessionRequest
{
    // As PythonRunRequest::identity.
    std::string    identity;
    // As PythonRunRequest::filename, fixed for the session: an existing file makes it a file session, whose works
    // run with `__file__`, `sys.argv` and its folder's imports, and whose first work installs its PEP 723 dependencies.
    std::string    filename = "<run_python>";
    // Bound to the module-level name `args`.
    nlohmann::json args = nlohmann::json::object();
    // The session is stopped and ended as timed_out once it has run this long, measured from its start; 0 is none.
    std::chrono::milliseconds budget{0};
    // Polled from another thread; true stops and ends the session as cancelled.
    std::function<bool()> cancelled;
    // Called from another thread at each stop or time-out, to cancel the tool calls in flight.
    std::function<void()> stop_calls;
    // A stop closes every window of the run, as a run's stop does, instead of only its message boxes and modal
    // windows, which a session's stop ends so the code waiting in them can be interrupted.
    bool stop_closes_ui = false;
    // Output events as the code prints, each work and each stretch between works capped at 256 KiB with a note;
    // false: one Output event at the session's end, capped at 256 KiB as a run's output is.
    bool stream_output = true;
    // Tracebacks quote the work's `source` for frames in the session's file, never the file on disk or linecache.
    bool quote_source = true;
    // File works send a Cell event as each cell's lines start running.
    bool report_cells = true;
    // A File work shows the result of each cell whose last statement is a bare expression, as a Cell work shows its
    // own: `_` names it, and it comes as a Result event, or printed as its repr.
    bool cell_results = true;
    // Run on the hosting thread without the GIL, before and after each work.
    std::function<void()> work_started, work_ended;
};

// Hosts `session` on the calling thread until it ends: one interpreter lease, audit scope, __main__, folder, output
// capture and watcher for all its works, and the teardown a run has (threads, pools, atexit, the run's windows, the
// folder's helpers) once at its end. A Cell or Console work echoes the value of a trailing bare expression and binds it
// to `_`; a File work runs as run_python_code() runs its code, and when the session goes on afterwards it reports once
// the waited threads it started have ended. Stop interrupts what runs now and the session's threads and tasks, and
// for five seconds after it, while the session is idle, its threads and tasks; the session goes on.
void host_python_session(const PythonSessionRequest& request, PythonSession& session);

// Installs a file run's PEP 723 dependencies; false with `error` when that fails. plugin_loader::install_packages
// unless a test sets another; nullptr restores it.
using PythonDependencyInstaller = std::function<bool(const std::vector<std::string>& packages, std::string& error)>;
void set_python_dependency_installer_for_testing(PythonDependencyInstaller installer);

// For the thread shim, with the GIL held. Whether `function`, the callable _thread.start_new_thread was given,
// starts a concurrent.futures pool's worker; and whether a run waits for the thread it starts before it ends: a
// non-daemon threading.Thread that isn't a pool worker. Python's own exit doesn't wait for a raw _thread thread.
bool python_run_is_pool_worker(const pybind11::handle& function);
bool python_run_waits_for(const pybind11::handle& function);
// For a pool worker a thread of `run` starts: the worker stays unbound, as pools are shared across runs, but the
// pool's initializer, which runs on it before any task, runs inside `run` as the task it was started for does (its
// binding, its identity, one of its interrupt targets), counted as the run's work from now until it returns, so the
// run waits for it even when that task is cancelled. Counted: the caller uncounts it with PythonRunScope::task_done()
// if the thread then fails to start. Refused: the run's admission has closed, so the worker must not start, since its
// initializer would run outside any run and identity.
enum class PoolInitializerBinding { None, Counted, Refused };
PoolInitializerBinding python_run_bind_pool_initializer(const pybind11::handle& function, const std::shared_ptr<PythonRunScope>& run);

// A .py file's bytes as UTF-8 text, decoded as Python reads a source file: by its BOM, else its PEP 263
// encoding declaration, else as UTF-8. Sets `encoding` to the codec used, which encode_python_source() takes
// to save the text back. False with `error` when the bytes are not text in that encoding.
bool decode_python_source(const std::string& bytes, std::string& text, std::string& encoding, std::string& error);
// UTF-8 `text` encoded in `encoding` (from decode_python_source(), or "utf-8" for a new file). False with
// `error` when a character has no form in it.
bool encode_python_source(const std::string& text, const std::string& encoding, std::string& bytes, std::string& error);

} // namespace Slic3r
