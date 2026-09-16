#ifndef slic3r_GUI_ProcessRunner_hpp_
#define slic3r_GUI_ProcessRunner_hpp_

#include <wx/event.h>
#include <wx/timer.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <functional>

#include <boost/process.hpp>

namespace Slic3r { namespace GUI {

class ProcessRunner : public wxEvtHandler
{
public:
    ProcessRunner();
    ~ProcessRunner() override;

    struct OutputLine {
        std::string text;
        bool is_stderr;
    };
    using OutputCallback = std::function<void(const std::vector<OutputLine>& lines)>;
    using DoneCallback   = std::function<void(int exit_code)>;

    // Non-blocking command-line style launch. Boost parses command_line into
    // executable + arguments.
    // Output is batched per timer tick (50ms) and delivered via on_output on the GUI thread.
    // on_done fires on the GUI thread with the exit code.
    // Returns false if a process is already running.
    bool run_command_line_async(const std::string& command_line,
                                OutputCallback on_output,
                                DoneCallback on_done);

    struct SyncResult {
        int exit_code = -1;
        std::string stdout_output;
        std::string stderr_output;
    };

    // Blocking: runs the process and waits for it to finish.
    // Safe to call from any thread (no wx dependency).
    static SyncResult run_sync(const std::string& executable,
                               const std::vector<std::string>& args);

    // Write data to the running process's stdin (async mode only).
    void write_stdin(const std::string& data);

    // True while an async process is launching or running.
    bool is_running() const;

    // Terminate the running process (async mode only).
    void terminate();

private:
    void on_timer(wxTimerEvent& event);
    void finish_process(int exit_code);
    void start_reader_threads();
    void join_reader_threads();
    void enqueue_output(std::string line, bool is_stderr);
    std::vector<OutputLine> drain_output_queue();
    // Join any completed background launcher thread before reusing or destroying it.
    void join_launch_thread();

    wxTimer m_poll_timer;

    // True while the launcher thread is still creating the child process; join only after this becomes false.
    std::atomic_bool m_launching{false};
    std::atomic_bool m_launch_failed{false};
    std::thread      m_launch_thread;
    std::thread      m_stdout_thread;
    std::thread      m_stderr_thread;
    std::mutex       m_output_mutex;
    std::vector<OutputLine> m_output_queue;

    std::unique_ptr<boost::process::child> m_process;
    std::unique_ptr<boost::process::ipstream> m_stdout_pipe;
    std::unique_ptr<boost::process::ipstream> m_stderr_pipe;
    std::unique_ptr<boost::process::opstream> m_stdin_pipe;

    OutputCallback m_on_output;
    DoneCallback m_on_done;
};

}} // namespace Slic3r::GUI

#endif
