#pragma once

#include "CodeEditorWindow.hpp"
#include "slic3r/plugin/PythonSession.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

class wxWebView;

namespace Slic3r { namespace GUI {

namespace Mcp {
class PersonRun;
}
class RunScriptPane;

// The Run Script panel: a dock pane under the 3D view (it can float) whose page (resources/web/dialog/RunScript) edits a
// Python script in `# %%` cells and runs them, one at a time or the whole file, in a live session (PythonSession) hosted
// by the MCP service as a person's run_script job. One per app, owned by GUI_App: the script's text, file and unsaved
// state, the session and the recent scripts live here, so they survive the pane, which a main-window rebuild (a language
// change) destroys and the panel builds again. The pane and everything in it are hidden from the automation tools.
// UI thread.
class RunScriptPanel final : public CodeEditorWindow
{
public:
    RunScriptPanel();
    ~RunScriptPanel() override;
    RunScriptPanel(const RunScriptPanel&)            = delete;
    RunScriptPanel& operator=(const RunScriptPanel&) = delete;

    // Hides the panel when it is on screen, else shows it, as File › Run Script, its shortcut and Speed Dial do.
    void toggle();
    // Builds the pane on first use and shows it, selecting Prepare when the current tab is neither Prepare nor Preview.
    void show();
    // The pane's window is shown on screen.
    bool on_screen() const;
    // Shows the panel and opens `path`, asking about unsaved changes first.
    void open_file(const boost::filesystem::path& path);
    // Asks about unsaved changes as the app quits; false to stay.
    bool confirm_close();

    bool                         owns_focus(const wxWindow* focus) const override;
    void                         forward_action(const std::string& action) override;
    wxWebView*                   web_view() const override;
    const std::set<std::string>& editor_keys() const override { return m_editor_keys; }

    // MainFrame::shutdown(), before the dock panes go: the session is stopped and ended, and from then on only its end is
    // watched; the pane goes.
    void main_window_closing();
    // The end of GUI_App::recreate_GUI(): the pane comes back, with an empty console, if it was wanted, once the old main
    // window has been deleted.
    void main_window_rebuilt();
    // The toggle's key changed (MainFrame::update_shortcut_labels()).
    void shortcuts_changed();

private:
    friend class RunScriptPane;

    void pane_destroyed(RunScriptPane* pane);
    // Hides the panel when it is on screen, else shows it.
    void flip();
    void handle(const nlohmann::json& payload);
    void dropped(const boost::filesystem::path& path);
    // A message for the page, sent once it has a pane; checked for liveness at its last hop.
    void send(const nlohmann::json& payload);

    nlohmann::json state() const;
    nlohmann::json script_message() const;
    nlohmann::json recent_message() const;
    nlohmann::json toggle_key() const;
    std::string    session_state() const;
    void           send_session_state();

    bool confirm_unsaved();
    bool save(bool pick);
    void load(const boost::filesystem::path& path);
    void new_script();
    void close_script();
    void remember_recent(const boost::filesystem::path& path);

    void run(const nlohmann::json& payload);
    void refuse(const std::string& message);
    // Starts a session on the script's file as it is now; false (and refused) when the service can't host one.
    bool start_session();
    // Ends the session and stops what it runs, its teardown included; it is dropped once it has ended.
    void end_session();
    // Submits `work` and tells the page with `started`, unless that is null: the page was told when the work had to wait.
    void submit(PythonWork work, const nlohmann::json& started);
    void tick();
    void finish_run(const PythonSessionEvent& done);
    void send_service_state();
    void read_api();

    std::shared_ptr<std::atomic<bool>> m_alive;
    RunScriptPane*                     m_pane   = nullptr;
    bool                               m_wanted = false;
    // The main window was rebuilt and the pane is wanted back: built at the next idle.
    bool                               m_rebuild_pending = false;

    // The script.
    std::string             m_text;
    boost::filesystem::path m_file;
    std::string             m_encoding = "utf-8";
    bool                    m_edited   = false;
    bool                    m_none     = true; // no script open

    // The session, and the run going in it.
    std::shared_ptr<PythonSession>     m_session;
    std::shared_ptr<Mcp::PersonRun>    m_session_run;
    std::string                        m_session_file; // the name the session's tracebacks use
    bool                               m_detached       = false;
    bool                               m_session_ending = false;
    std::optional<PythonWork>          m_waiting; // a Run all waiting for the old session to end
    std::deque<PythonSessionEvent>     m_backlog;
    bool                               m_running = false;
    std::chrono::steady_clock::time_point m_run_started;
    wxWeakRef<wxWindow>                m_run_focus; // the canvas the run gave the focus to
    std::string                        m_sent_session_state;
    wxTimer                            m_timer;
    wxTimer                            m_ready_timer;

    std::set<std::string>                 m_editor_keys;
    std::chrono::steady_clock::time_point m_host_toggle_at{};

    // The orca.host API's entries for the editor's suggestions, read once per app run.
    nlohmann::json m_api;
    bool           m_api_asked = false;
    std::thread    m_api_thread;
};

}} // namespace Slic3r::GUI
