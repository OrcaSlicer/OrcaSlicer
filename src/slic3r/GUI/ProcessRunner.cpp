#include "ProcessRunner.hpp"

#include <boost/process/env.hpp>
#include <boost/process.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#endif

namespace Slic3r { namespace GUI {

ProcessRunner::ProcessRunner()
    : m_poll_timer(this, wxID_ANY)
{
    Bind(wxEVT_TIMER, &ProcessRunner::on_timer, this);
}

ProcessRunner::~ProcessRunner()
{
    terminate();
}

void ProcessRunner::join_launch_thread()
{
    if (m_launch_thread.joinable())
        m_launch_thread.join();
}

void ProcessRunner::start_reader_threads()
{
    join_reader_threads();

    if (m_stdout_pipe) {
        m_stdout_thread = std::thread([this]() {
            std::string line;
            while (std::getline(*m_stdout_pipe, line))
                enqueue_output(std::move(line), false);
        });
    }

    if (m_stderr_pipe) {
        m_stderr_thread = std::thread([this]() {
            std::string line;
            while (std::getline(*m_stderr_pipe, line))
                enqueue_output(std::move(line), true);
        });
    }
}

void ProcessRunner::join_reader_threads()
{
    if (m_stdout_thread.joinable())
        m_stdout_thread.join();
    if (m_stderr_thread.joinable())
        m_stderr_thread.join();
}

void ProcessRunner::enqueue_output(std::string line, bool is_stderr)
{
    std::lock_guard<std::mutex> lock(m_output_mutex);
    m_output_queue.push_back({std::move(line), is_stderr});
}

std::vector<ProcessRunner::OutputLine> ProcessRunner::drain_output_queue()
{
    std::lock_guard<std::mutex> lock(m_output_mutex);
    std::vector<OutputLine> batch;
    batch.swap(m_output_queue);
    return batch;
}

// Clean up stored process state after the child has exited or has already been stopped.
void ProcessRunner::finish_process(int exit_code)
{
    join_launch_thread();
    join_reader_threads();

    std::vector<OutputLine> final_batch = drain_output_queue();
    if (!final_batch.empty() && m_on_output)
        m_on_output(final_batch);

    m_process.reset();
    m_stdout_pipe.reset();
    m_stderr_pipe.reset();
    m_stdin_pipe.reset();

    if (m_on_done) {
        auto cb = std::move(m_on_done);
        m_on_done = nullptr;
        cb(exit_code);
    }
}

// Stop a running child process, then use finish_process() for shared cleanup.
void ProcessRunner::terminate()
{
    m_poll_timer.Stop();

    join_launch_thread();

    if (m_process && m_process->running()) {
        std::error_code ec;
        m_process->terminate(ec);
        m_process->wait(ec);
    }

    if (m_process) {
        finish_process(m_process->exit_code());
    } else if (m_launch_failed.exchange(false) || m_on_done) {
        finish_process(-1);
    }
}

bool ProcessRunner::run_command_line_async(const std::string& command_line,
                                           OutputCallback on_output,
                                           DoneCallback on_done)
{
    if (!m_launching.load())
        join_launch_thread();

    if (is_running())
        return false;

    m_on_output = std::move(on_output);
    m_on_done   = std::move(on_done);
    drain_output_queue();
    m_launch_failed.store(false);
    m_launching.store(true);

    m_launch_thread = std::thread([this, command_line]() {
        namespace bp = boost::process;

        try {
            auto stdout_pipe = std::make_unique<bp::ipstream>();
            auto stderr_pipe = std::make_unique<bp::ipstream>();
            auto stdin_pipe  = std::make_unique<bp::opstream>();

            auto process = std::make_unique<bp::child>(
                bp::cmd = command_line,
                bp::env["PYTHONUNBUFFERED"] = "1",
                bp::env["PYTHONIOENCODING"] = "utf-8",
                bp::env["PYTHONUTF8"] = "1",
#ifdef _WIN32
                bp::windows::create_no_window,
#endif
                bp::std_in < *stdin_pipe,
                bp::std_out > *stdout_pipe,
                bp::std_err > *stderr_pipe);

            m_stdout_pipe = std::move(stdout_pipe);
            m_stderr_pipe = std::move(stderr_pipe);
            m_stdin_pipe  = std::move(stdin_pipe);
            m_process     = std::move(process);
            start_reader_threads();
        } catch (const std::exception&) {
            m_stdout_pipe.reset();
            m_stderr_pipe.reset();
            m_stdin_pipe.reset();
            m_process.reset();
            m_launch_failed.store(true);
        }

        m_launching.store(false);
    });

    m_poll_timer.Start(50);
    return true;
}

void ProcessRunner::on_timer(wxTimerEvent& /*event*/)
{
    if (m_launching.load())
        return;

    join_launch_thread();

    if (m_launch_failed.exchange(false)) {
        m_poll_timer.Stop();
        finish_process(-1);
        return;
    }

    if (!m_process) {
        m_poll_timer.Stop();
        return;
    }

    std::vector<OutputLine> batch = drain_output_queue();
    if (!batch.empty() && m_on_output)
        m_on_output(batch);

    // Check if process exited
    if (!m_process->running()) {
        m_poll_timer.Stop();
        m_process->wait();
        const int exit_code = m_process->exit_code();
        finish_process(exit_code);
    }
}

void ProcessRunner::write_stdin(const std::string& data)
{
    if (m_launching.load() || !m_stdin_pipe || !m_process || !m_process->running())
        return;

    *m_stdin_pipe << data;
    m_stdin_pipe->flush();
}

bool ProcessRunner::is_running() const
{
    return m_launching.load() || (m_process && m_process->running());
}

ProcessRunner::SyncResult ProcessRunner::run_sync(const std::string& executable,
                                                    const std::vector<std::string>& args)
{
    SyncResult result;

    try {
        namespace bp = boost::process;

        bp::ipstream std_out;
        bp::ipstream std_err;

        bp::child child(executable, bp::args(args),
                        bp::std_out > std_out,
                        bp::std_err > std_err);

        // Read both streams on separate threads while the process runs
        std::thread stdout_reader([&]() {
            std::string line;
            while (std::getline(std_out, line))
                result.stdout_output += line + '\n';
        });

        std::thread stderr_reader([&]() {
            std::string line;
            while (std::getline(std_err, line))
                result.stderr_output += line + '\n';
        });

        child.wait();
        stdout_reader.join();
        stderr_reader.join();

        result.exit_code = child.exit_code();
    } catch (const std::exception&) {
        result.exit_code = -1;
    }

    return result;
}

}} // namespace Slic3r::GUI
