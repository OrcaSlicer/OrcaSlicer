#include "LaunchRun.hpp"

#include "McpOptions.hpp"
#include "McpPaths.hpp"
#include "AutomationRuntime.hpp"
#include "ModalTracker.hpp"

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GUI.hpp"
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
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/evtloop.h>
#include <wx/msgdlg.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <ios>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

std::atomic<int> g_run_exit_code{-1};

void set_launch_exit_code(int code) { g_run_exit_code = code; }

// The dialogs the run's close ended, by ModalTracker serial, so none is ended twice. UI thread only.
std::set<std::uint64_t> g_ended_dialogs;
bool                    g_close_waits = false; // on_close_idle() is bound to the app's idle events

// Ends `dialog` as its Cancel or its close box does, so its own handler runs: a message box by its Cancel, else its No,
// else its OK. A native file picker can't be ended, and the close waits for the person.
void end_dialog(wxDialog* dialog)
{
    BOOST_LOG_TRIVIAL(info) << "--run: closing the app, so ending the dialog \"" << into_u8(dialog->GetTitle()) << "\"";
    if (is_picker(dialog))
        return;
    if (dynamic_cast<wxMessageDialogBase*>(dialog) != nullptr) {
        for (int id : {wxID_CANCEL, wxID_NO, wxID_OK})
            if (has_dialog_button(dialog, id)) {
                press_dialog_button(dialog, id);
                return;
            }
        return;
    }
    // A Cancel the dialog disabled or hid ends it all the same: the app is closing.
    const DialogPress pressed = press_dialog_button(dialog, wxID_CANCEL);
    if (pressed == DialogPress::Disabled || pressed == DialogPress::Hidden)
        dialog->EndModal(wxID_CANCEL);
}

// The runtime while the run's close can still go ahead: the app and its main window are up and the runtime is not
// stopping for an exit that is already under way. UI thread only.
AutomationRuntime* closing_runtime()
{
    AutomationRuntime* runtime = wxTheApp != nullptr ? wxGetApp().automation_if_started() : nullptr;
    return runtime != nullptr && !runtime->stopping() && wxGetApp().mainframe != nullptr ? runtime : nullptr;
}

// Whether the main window can close now: no dialog's ShowModal() runs and the main loop is the one running. Otherwise
// ends the topmost dialog, once.
bool modals_ended(ModalTracker& modals)
{
    wxDialog* const        top  = modals.top();
    const wxEventLoopBase* loop = wxEventLoopBase::GetActive();
    if (top == nullptr && (loop == nullptr || loop->IsMain()))
        return true;
    if (top != nullptr && g_ended_dialogs.insert(modals.serial(top)).second)
        end_dialog(top);
    return false;
}

void on_close_idle(wxIdleEvent& event);
void wait_for_idle(bool wait)
{
    if (wait == g_close_waits || wxTheApp == nullptr)
        return;
    if (wait)
        wxTheApp->Bind(wxEVT_IDLE, &on_close_idle);
    else
        wxTheApp->Unbind(wxEVT_IDLE, &on_close_idle);
    g_close_waits = wait;
}

// Closes the main window without the unsaved-project prompt (the run owns this session), once no dialog's ShowModal()
// runs. A closure posted to the UI thread also runs in a modal dialog's nested event loop, and the main window closed
// there is destroyed by that loop's idle processing with the dialog's ShowModal() still on the stack: its children,
// the dialog among them, are deleted under the code that showed it. So the topmost dialog is ended, and the next idle
// event, which each loop sends when it runs out of work, looks again (on_close_idle()).
void close_after_modals()
{
    AutomationRuntime* runtime = closing_runtime();
    const bool         ready   = runtime != nullptr && modals_ended(runtime->modals());
    wait_for_idle(runtime != nullptr && !ready);
    if (ready)
        wxGetApp().mainframe->Close(true);
}

// Once the dialogs' loops have returned, the close is queued as the run's end queues it, not made here: the windows
// it closes are deleted by this same idle pass, before the closures their close queued (a settings tab's) have run.
void on_close_idle(wxIdleEvent& event)
{
    event.Skip();
    AutomationRuntime* runtime = closing_runtime();
    if (runtime == nullptr || modals_ended(runtime->modals())) {
        wait_for_idle(false);
        if (runtime != nullptr)
            runtime->post_ui([] { close_after_modals(); });
    }
}

// The run's end: its exit code, then the main window's close, which ends the event loop.
void end_launch_run(AutomationRuntime& runtime, int code)
{
    set_launch_exit_code(code);
    BOOST_LOG_TRIVIAL(info) << "--run finished with exit code " << code;
    if (runtime.stopping() || wxTheApp == nullptr)
        return;
    runtime.post_ui([] { close_after_modals(); });
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
    call.command = *NativeCommands::instance().find(python ? "script_run" : "replay_run");
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
