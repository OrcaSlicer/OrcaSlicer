#include "LaunchRun.hpp"

#include "McpOptions.hpp"
#include "McpPaths.hpp"
#include "AutomationRuntime.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/plugin/PythonScriptRunner.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/app.h>

#include <atomic>
#include <exception>
#include <ios>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

std::atomic<int> g_run_exit_code{-1};

void set_launch_exit_code(int code) { g_run_exit_code = code; }

// The run's end: its exit code, then the main window's close, which ends the event loop.
void end_launch_run(AutomationRuntime& runtime, int code)
{
    set_launch_exit_code(code);
    BOOST_LOG_TRIVIAL(info) << "--run finished with exit code " << code;
    if (runtime.stopping() || wxTheApp == nullptr)
        return;
    // Close without the unsaved-project prompt: the run owns this session.
    runtime.post_ui([] {
        if (wxTheApp != nullptr && wxGetApp().mainframe != nullptr)
            wxGetApp().mainframe->Close(true);
    });
}

// The exit code of the run's result: 0 passed, 1 failed, 2 invalid (the replay's result says so). An error built
// without details carries no object to read, and is a failed run.
int launch_run_code(const ToolResult& r, bool python)
{
    if (r.is_error)
        BOOST_LOG_TRIVIAL(error) << "--run: " << r.message;
    const bool fields = r.data.is_object();
    if (python) {
        const bool finished = !r.is_error && fields && r.data.value("status", "") == "finished";
        if (!r.is_error && !finished)
            BOOST_LOG_TRIVIAL(error) << "--run: " << (fields ? r.data.value("message", std::string()) : std::string()) << "\n"
                                     << (fields ? r.data.value("traceback", std::string()) : std::string());
        return finished ? 0 : 1;
    }
    if (r.is_error)
        return fields && r.data.value("invalid", false) ? 2 : 1;
    return fields && r.data.value("passed", false) ? 0 : 1;
}

} // namespace

int launch_exit_code() { return g_run_exit_code.load(); }

void start_launch_run(AutomationRuntime& runtime, const McpLaunchOptions& options)
{
    const std::string file      = options.run_file;
    const std::string extension = path_to_utf8(path_from_utf8(file).extension());
    const bool        python    = boost::algorithm::iequals(extension, ".py");
    const bool        replay    = boost::algorithm::iequals(extension, ".json");
    // A replay's steps and a script's tools drive the GUI.
    runtime.enable_gui_instruments();
    if (!options.run_out.empty())
        set_run_output_dir(path_from_utf8(options.run_out));

    FollowedCall call;
    call.command = *NativeCommands::instance().find(python ? "run_script" : "run_replay");
    call.person  = true;
    // The file is read off the UI thread.
    call.ready = [&runtime, file, out = options.run_out, python, replay](json& args) {
        try {
            if (!python && !replay)
                throw std::runtime_error(file + " is neither a replay (.json) nor a Python script (.py)");
            if (python) {
                boost::nowide::ifstream in(file.c_str(), std::ios::binary);
                if (!in)
                    throw std::runtime_error("Cannot read " + file);
                std::ostringstream raw;
                raw << in.rdbuf();
                std::string text, encoding, error;
                if (!decode_python_source(raw.str(), text, encoding, error))
                    throw std::runtime_error("Cannot read " + file + " as Python source: " + error);
                args = {{"code", text}, {"filename", file}};
            } else {
                args = {{"path", file}};
                if (!out.empty())
                    args["out_dir"] = out;
            }
            return true;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "--run: " << e.what();
            end_launch_run(runtime, 2);
            return false;
        }
    };
    call.ended = [&runtime, python](const std::optional<ToolResult>& r) {
        // The runtime stopped before the run ended: it did not pass.
        int code = 1;
        try {
            if (r)
                code = launch_run_code(*r, python);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "--run: " << e.what();
            code = 2;
        }
        end_launch_run(runtime, code);
    };
    runtime.on_ready([&runtime, call = std::move(call)]() mutable {
        // Refused while the runtime stops, or when the system cannot start a thread: the run never began.
        if (!runtime.follow(std::move(call)))
            end_launch_run(runtime, 1);
    });
}

}}} // namespace Slic3r::GUI::Mcp
