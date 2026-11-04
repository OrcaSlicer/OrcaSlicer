#include "PluginUv.hpp"

#include "PythonInterpreter.hpp"

#include <boost/log/trivial.hpp>
#include <slic3r/Utils/BoostProcessCompat.hpp>

#include <chrono>
#include <exception>
#include <string>
#include <system_error>
#include <thread>

namespace Slic3r::plugin_uv {

bool run(const std::vector<std::string>& args, std::chrono::seconds timeout, std::string& error)
{
    error.clear();

    const std::string uv_path = PythonInterpreter::bundled_uv_path();
    if (uv_path.empty()) {
        error = "Bundled uv executable not found. Python package installation is unavailable.";
        return false;
    }

    try {
        namespace process = boost::process;

        process::ipstream std_err;
        process::child    child(uv_path, process::args(args),
#ifdef _WIN32
                                // uv.exe (and the python.exe it spawns) are console-subsystem programs.
                                // OrcaSlicer is a GUI app with no console of its own, so without this flag
                                // Windows allocates a fresh console window for the child that flashes on
                                // screen during startup plugin loading. Matches ProcessRunner/MediaPlayCtrl.
                                process::windows::create_no_window,
#endif
                                process::std_err > std_err);

        std::string err_output;
        std::thread stderr_reader([&std_err, &err_output]() {
            std::string line;
            while (std::getline(std_err, line)) {
                err_output += line + '\n';
            }
        });

        // NOTE: we poll child.running() instead of calling child.wait_for().
        // On macOS (which lacks sigtimedwait) boost.process v1 implements the
        // timed waits with a sigwait()-based fallback that deadlocks inside a
        // multi-threaded process: SIGCHLD is delivered to an arbitrary thread
        // and consumed by an async handler, so the worker thread's sigwait()
        // never returns and the timeout never fires — the plugin load hangs
        // forever (the "Loading" status never clears). child.running() uses
        // waitpid(WNOHANG), which behaves correctly on every platform.
        // (boost.process v2, in Boost >= 1.86, implements timed waits via Asio
        // and is unaffected — revisit this when the bundled Boost is upgraded.)
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        std::error_code run_ec;
        while (child.running(run_ec)) {
            if (run_ec) {
                stderr_reader.join();
                error = "Failed to query uv process status: " + run_ec.message();
                BOOST_LOG_TRIVIAL(error) << error;
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::error_code term_ec;
                child.terminate(term_ec);
                child.wait(term_ec);
                stderr_reader.join();
                error = "uv pip install timed out after " + std::to_string(timeout.count()) + "s";
                BOOST_LOG_TRIVIAL(error) << error;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        stderr_reader.join();

        const int exit_code = child.exit_code();
        if (exit_code != 0) {
            if (!err_output.empty())
                error = "uv pip install failed (exit code " + std::to_string(exit_code) + "): " + err_output;
            else
                error = "uv pip install failed with exit code " + std::to_string(exit_code);
            BOOST_LOG_TRIVIAL(error) << error;
            return false;
        }

        return true;
    } catch (const std::exception& ex) {
        error = std::string("Failed to run uv: ") + ex.what();
        BOOST_LOG_TRIVIAL(error) << error;
        return false;
    }
}

} // namespace Slic3r::plugin_uv
