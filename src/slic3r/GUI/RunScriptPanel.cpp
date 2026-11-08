#include "RunScriptPanel.hpp"

#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "KeyChord.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Shortcuts.hpp"
#include "format.hpp"
#include "Mcp/McpPaths.hpp"
#include "Mcp/McpScriptRun.hpp"
#include "Mcp/McpService.hpp"
#include "Mcp/McpTools.hpp"
#include "Mcp/McpTypes.hpp"
#include "Mcp/PythonScriptsDialog.hpp"
#include "Widgets/WebView.hpp"
#include "Widgets/WebViewHostDialog.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonScriptRunner.hpp"
#include "slic3r/plugin/PythonSession.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <wx/app.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/webview.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <ios>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

constexpr int         kMaxRecent      = 10;
constexpr std::size_t kOutputPerTick  = 256 * 1024;
// A toggle the page asks for this soon after one the main frame dispatched is the same key press, delivered twice.
constexpr auto        kToggleEcho     = std::chrono::milliseconds(150);

Mcp::McpService* service()
{
    Mcp::McpService* s = wxGetApp().mcp_service();
    return s != nullptr && s->started() && !s->stopping() ? s : nullptr;
}

#ifdef __APPLE__
const char* const kEnter = "Return";
#else
const char* const kEnter = "Enter";
#endif

std::string run_cell_key() { return shortkey_shift_prefix() + kEnter; }
std::string run_all_key() { return shortkey_ctrl_prefix() + kEnter; }

// What New writes: one cell, from the first line, so Run cell runs it wherever the caret is, even in a new session: what
// the panel is, how cells and their keys work, the tools import, and a call whose value Run cell shows.
std::string starter_script()
{
    // TRN The title of the one cell a new script starts with, which lists the objects of the project.
    return "# %% " + _u8L("List the objects") + "\n# " +
           // TRN A comment line of a new script in the Run Script panel. orca.host is the name of OrcaSlicer's Python API.
           _u8L("Run Script runs Python with OrcaSlicer's tools and the orca.host API.") + "\n# " +
           // TRN A comment line of a new script. %1% is the cell marker "# %%", written as is; %2% and %3% are keys, such as
           // Shift+Enter and Ctrl+Enter.
           format(_u8L("A line that starts with %1% starts a cell. %2% runs the cell at the cursor, %3% all of them."), "# %%",
                  run_cell_key(), run_all_key()) +
           "\nfrom orca.host import tools\ntools.call(\"list_objects\")\n";
}

// The page's text: its own and the code editor's.
json page_strings()
{
    const std::string ctrl = shortkey_ctrl_prefix();
#ifdef __APPLE__
    const std::string control = u8"⌃+";
#else
    const std::string control = ctrl;
#endif
    return {
        {"key_run_cell", run_cell_key()},
        {"key_run_all", run_all_key()},
        {"key_save", ctrl + "S"},
        {"key_save_as", ctrl + shortkey_shift_prefix() + "S"},
        {"key_suggest", control + "Space"},
        // TRN A button of the Run Script panel that starts a new script.
        {"new", _u8L_CONTEXT("New", "Script")},
        {"new_tip", _u8L("Start a new script")},
        {"open", _u8L("Open…")},
        {"open_tip", _u8L("Open a Python script")},
        {"recent_scripts", _u8L("Recent scripts")},
        {"no_recent", _u8L("No recent scripts")},
        {"close_script", _u8L("Close script")},
        {"save", _u8L("Save")},
        {"save_as", _u8L("Save as…")},
        // TRN %s is a key, such as Ctrl+S.
        {"save_tip", _u8L("Save the script (%s)")},
        {"run_all", _u8L("Run all")},
        // TRN %s is a key, such as Ctrl+Enter.
        {"run_all_tip", _u8L("Restart the session and run every cell (%s)")},
        {"run_cell", _u8L("Run cell")},
        // TRN %s is a key, such as Shift+Enter.
        {"run_cell_tip", _u8L("Run the cell at the cursor (%s)")},
        {"stop", _u8L("Stop")},
        {"stop_tip", _u8L("Interrupt the code that runs now and the threads the session started")},
        {"restart", _u8L("Restart session")},
        {"restart_tip", _u8L("End the session: its variables, imports and threads go")},
        {"edited", _u8L("(edited)")},
        {"untitled_script", _u8L("Untitled script")},
        // TRN The first item of the Run Script panel's session line, before the number of cells run and of variables.
        {"session", _u8L("Session")},
        {"no_session", _u8L("No session yet")},
        {"running", _u8L("Running")},
        {"threads_running", _u8L("Threads are running")},
        {"session_ending", _u8L("Ending the session…")},
        // TRN The first %s is the number of the cell that runs, the second how many cells the file has.
        {"running_cell", _u8L("Running cell %s of %s")},
        {"running_file", _u8L("Running the file")},
        {"hands_off", _u8L("Don't use OrcaSlicer until the script finishes.")},
        // TRN The heading of the console's block for a cell; %s is its number.
        {"cell_n", _u8L("Cell %s")},
        {"no_output", _u8L("(no output)")},
        // TRN A link in the heading of Run all's block in the Run Script console: opens every cell's row to show its output.
        {"expand_all", _u8L("Expand all")},
        // TRN A link in the heading of Run all's block in the Run Script console: folds every cell's row to its heading.
        {"collapse_all", _u8L("Collapse all")},
        // TRN The + below the last line of the Run Script editor, which adds a "# %%" cell at the end of the script.
        {"add_cell", _u8L("Add a cell")},
        {"add_cell_tip", _u8L("Add a cell at the end of the script")},
        // TRN A duration; %s is a number of seconds such as 0.8.
        {"seconds", _u8L("%s s")},
        {"console_placeholder", _u8L("Python, run in the session")},
        {"clear", _u8L("Clear")},
        {"clear_tip", _u8L("Clear the console: the session and its variables stay")},
        // TRN A menu of the Run Script panel: how the editor shows the script's cells, the parts that start with "# %%".
        {"cells", _u8L("Cells")},
        {"cells_tip", _u8L("How the editor shows the script's cells")},
        // TRN The heading of the Run Script panel's Cells menu, above its styles: Margin bars only, Dividers, Header bands...
        {"show_cells_with", _u8L("Show cells with")},
        // TRN A style of the Run Script panel's Cells menu: only the coloured bar beside each cell's line numbers.
        {"cells_margin", _u8L("Margin bars only")},
        // TRN A style of the Run Script panel's Cells menu: a line across the editor above each cell.
        {"cells_dividers", _u8L("Dividers")},
        // TRN A style of the Run Script panel's Cells menu: each cell's first line, "# %% title", drawn as a shaded band.
        {"cells_bands", _u8L("Header bands")},
        // TRN A style of the Run Script panel's Cells menu: every other cell has a shaded background.
        {"cells_shaded", _u8L("Shaded cells")},
        {"cells_highlight", _u8L("Highlight the cell at the cursor")},
        // TRN For a screen reader: the divider between the Run Script panel's editor and its console, whose value is the
        // editor's share of the panel's width.
        {"split_label", _u8L("Width of the editor")},
        {"split_tip", _u8L("Drag to share the width between the editor and the console. Double-click to reset it.")},
        // TRN The arrows on the divider between the Run Script panel's editor and console, and the strip a hidden side leaves at
        // the panel's edge.
        {"hide_editor", _u8L("Hide the editor")},
        {"hide_console", _u8L("Hide the console")},
        {"show_editor", _u8L("Show the editor")},
        {"show_console", _u8L("Show the console")},
        // TRN The strip a hidden console leaves at the Run Script panel's edge, while a script runs or after one ran.
        {"show_console_running", _u8L("Show the console: a script is running")},
        {"show_console_new", _u8L("Show the console: it has new output")},
        {"show_console_failed", _u8L("Show the console: the last run failed")},
        // TRN The label of the strip a hidden console leaves at the Run Script panel's edge.
        {"console", _u8L("Console")},
        {"new_script", _u8L("New script")},
        {"open_script", _u8L("Open a script…")},
        {"empty_editor", _u8L("Open a script, or start a new one.")},
        {"start_script", _u8L("Start a script")},
        // TRN %s is the cell marker "# %%", written as is. orca.host is the name of OrcaSlicer's Python API.
        {"start_script_text", _u8L("A Python script runs inside OrcaSlicer and can call every tool an AI agent can, plus the whole "
                                   "orca.host API. Split it into cells with %s and try one cell at a time.")},
        // TRN A line in the console of the Run Script panel, where Run all started a new session.
        {"session_restarted", _u8L("Session restarted for Run all")},
        {"python_off_text", _u8L("You can edit and save this script, but it can't run until Python scripts are on.")},
        {"python_off", _u8L("Python scripts are off.")},
        {"python_access", _u8L("A script runs with OrcaSlicer's full access to your computer.")},
        {"turn_on_python", _u8L("Turn on Python scripts…")},
        {"starting", _u8L("OrcaSlicer is still starting…")},
        {"code_editor", WebViewHostDialog::code_editor_strings()},
    };
}

bool is_python_file(const fs::path& path) { return boost::iequals(path.extension().string(), ".py"); }

wxString python_wildcard() { return format_wxstr("%1% (*.py)|*.py", _L("Python scripts")); }

std::vector<std::string> recent_paths()
{
    std::vector<std::string> paths;
    try {
        const json list = json::parse(wxGetApp().app_config->get("run_script", "recent"));
        for (const json& item : list)
            if (item.is_string())
                paths.push_back(item.get<std::string>());
    } catch (const std::exception&) {
    }
    return paths;
}

void set_recent_paths(const std::vector<std::string>& paths) { wxGetApp().app_config->set("run_script", "recent", json(paths).dump()); }

// How the editor shows cells, as the Cells menu last set it: the page reads a missing or unknown style as its default, and
// the highlight is on unless it was turned off.
json cell_view()
{
    const AppConfig& config = *wxGetApp().app_config;
    return {{"style", config.get("run_script", "cell_style")}, {"highlight", config.get("run_script", "highlight_cell") != "0"}};
}

// The editor's share of the width and the side hidden, as the divider last left them; the page reads a missing or unknown
// value as its default.
json pane_layout()
{
    const AppConfig& config = *wxGetApp().app_config;
    return {{"split", config.get("run_script", "split")}, {"hidden", config.get("run_script", "hidden_side")}};
}

} // namespace

// The panel's dock pane: a web view on the page, hidden from the automation tools with everything inside it.
class RunScriptPane : public wxPanel
{
public:
    RunScriptPane(wxWindow* parent, RunScriptPanel& owner);
    ~RunScriptPane() override;
    RunScriptPane(const RunScriptPane&)            = delete;
    RunScriptPane& operator=(const RunScriptPane&) = delete;

    wxWebView* web() const { return m_web; }
    // HandleStudio(payload) on the page, after the current event and only while the pane lives.
    void send(const json& payload);

private:
    void on_navigating(wxWebViewEvent& event);

    RunScriptPanel&                    m_owner;
    wxWebView*                         m_web = nullptr;
    wxFileName                         m_page;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

RunScriptPane::RunScriptPane(wxWindow* parent, RunScriptPanel& owner)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL, Mcp::kHiddenFromAutomation), m_owner(owner)
{
    const fs::path page = fs::path(resources_dir()) / "web" / "dialog" / "RunScript" / "index.html";
    m_page              = wxFileName(from_path(page));
    // Created empty and loaded once the user scripts are in, so the first load has them, as a WebViewHostDialog's page does.
    m_web = WebView::CreateWebView(this, wxEmptyString);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    if (m_web != nullptr)
        sizer->Add(m_web, 1, wxEXPAND);
    SetSizer(sizer);
    if (m_web == nullptr)
        return;
    // WebView2's accelerator keys would reload the page (Ctrl+R, F5) and zoom it.
    m_web->EnableBrowserAcceleratorKeys(false);
    m_web->AddUserScript(wxString::FromUTF8(WebViewHostDialog::theme_user_script()));
    m_web->AddUserScript(wxString::FromUTF8(WebViewHostDialog::localized_strings_script(page_strings())));
    m_web->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [this](wxWebViewEvent& event) {
        json payload;
        try {
            payload = json::parse(event.GetString().utf8_string());
        } catch (const std::exception&) {
            return;
        }
        // Out of the web view's message callback: window work on its stack (file dialogs, prompts) is a crash class on GTK
        // and macOS.
        wxGetApp().CallAfter([owner = &m_owner, alive = m_alive, payload = std::move(payload)] {
            if (alive->load())
                owner->handle(payload);
        });
    });
    m_web->Bind(wxEVT_WEBVIEW_NAVIGATING, &RunScriptPane::on_navigating, this);
    // A theme toggle re-themes the page in place.
    m_web->Bind(EVT_WEBVIEW_RECREATED, [this](wxCommandEvent&) {
        WebView::RunScript(m_web, wxString::FromUTF8(WebViewHostDialog::theme_apply_script()));
    });
    wxString url = file_url_from_path(page);
    if (const wxString lang = wxGetApp().current_language_code_safe(); !lang.empty())
        url += "?lang=" + lang;
    WebView::LoadUrl(m_web, url);
}

RunScriptPane::~RunScriptPane()
{
    m_alive->store(false);
    m_owner.pane_destroyed(this);
}

void RunScriptPane::send(const json& payload)
{
    const wxString script = "HandleStudio(" + wxString::FromUTF8(payload.dump(-1, ' ', false, json::error_handler_t::ignore)) + ")";
    wxGetApp().CallAfter([this, script, alive = m_alive] {
        if (alive->load() && m_web != nullptr)
            WebView::RunScript(m_web, script);
    });
}

// The page goes nowhere but its own loads. A file dropped on it is a navigation to that file, opened after this event.
void RunScriptPane::on_navigating(wxWebViewEvent& event)
{
    if (WebViewHostDialog::own_page_navigation(event.GetURL(), m_page)) {
        event.Skip();
        return;
    }
    event.Veto();
    fs::path path = WebViewHostDialog::dropped_file(event.GetURL());
    if (path.empty())
        return;
    wxGetApp().CallAfter([owner = &m_owner, alive = m_alive, path = std::move(path)] {
        if (alive->load())
            owner->dropped(path);
    });
}

RunScriptPanel::RunScriptPanel() : m_alive(std::make_shared<std::atomic<bool>>(true))
{
    m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    m_ready_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        Mcp::McpService* s = service();
        if (s == nullptr || s->ready())
            m_ready_timer.Stop();
        send_service_state();
    });
    // The pane comes back after a rebuild once the old main window is gone: a WebView2 created while the window it
    // replaces still exists shows a blank page. recreate_GUI() Destroy()s that window, which wx deletes in its idle pass
    // after the app's idle event, so the pane is built from a call queued in that event.
    wxGetApp().Bind(wxEVT_IDLE, [this, alive = m_alive](wxIdleEvent& event) {
        event.Skip();
        if (!alive->load() || !std::exchange(m_rebuild_pending, false))
            return;
        wxGetApp().CallAfter([this, alive] {
            if (alive->load() && m_wanted && m_pane == nullptr)
                show();
        });
    });
}

RunScriptPanel::~RunScriptPanel()
{
    m_alive->store(false);
    m_timer.Stop();
    m_ready_timer.Stop();
    // The service, which hosts the session on a thread of its own, joins that thread as it stops.
    if (m_session) {
        m_session->end();
        m_session->stop();
    }
    if (m_api_thread.joinable())
        m_api_thread.join();
}

// ---- the pane --------------------------------------------------------------------------------------------------

void RunScriptPanel::toggle()
{
    m_host_toggle_at = std::chrono::steady_clock::now();
    flip();
}

void RunScriptPanel::flip()
{
    if (on_screen()) {
        m_wanted = false;
        if (Plater* plater = wxGetApp().plater(); plater != nullptr && m_pane != nullptr)
            plater->show_dock_pane(m_pane, false);
    } else
        show();
}

void RunScriptPanel::show()
{
    MainFrame* frame  = wxGetApp().mainframe;
    Plater*    plater = wxGetApp().plater();
    if (frame == nullptr || plater == nullptr)
        return;
    m_wanted = true;
    if (!frame->is_prepare_or_preview_tab())
        frame->select_tab(wxString(TAB_ID_PREPARE));
    if (m_pane == nullptr) {
        m_pane = new RunScriptPane(plater, *this);
        plater->add_dock_pane(
            m_pane, "run_script", _L("Run Script"), "bottom", wxSize(900, 320),
            [this, alive = m_alive] {
                if (alive->load())
                    m_wanted = false;
            },
            true);
    } else
        plater->show_dock_pane(m_pane, true);
    if (m_pane->web() != nullptr)
        m_pane->web()->SetFocus();
}

bool RunScriptPanel::on_screen() const { return m_pane != nullptr && m_pane->IsShownOnScreen(); }

void RunScriptPanel::pane_destroyed(RunScriptPane* pane)
{
    if (m_pane == pane) {
        m_pane = nullptr;
        m_editor_keys.clear();
    }
}

bool RunScriptPanel::owns_focus(const wxWindow* focus) const
{
    if (m_pane == nullptr)
        return false;
    for (const wxWindow* w = focus; w != nullptr; w = w->GetParent())
        if (w == m_pane)
            return true;
#ifdef _WIN32
    // WebView2 keeps the focus in windows of its own, which wx doesn't know.
    if (const HWND native = ::GetFocus(); native != nullptr && ::IsChild(static_cast<HWND>(m_pane->GetHWND()), native))
        return true;
#endif
    return false;
}

void RunScriptPanel::forward_action(const std::string& action) { send({{"command", "key"}, {"action", action}}); }

wxWebView* RunScriptPanel::web_view() const { return m_pane != nullptr ? m_pane->web() : nullptr; }

void RunScriptPanel::send(const json& payload)
{
    if (m_pane != nullptr)
        m_pane->send(payload);
}

void RunScriptPanel::main_window_closing()
{
    if (m_session) {
        m_session->end();
        m_session->stop();
        m_detached = true;
        m_backlog.clear();
    }
    m_waiting.reset();
    m_running = false;
    m_ready_timer.Stop();
    m_pane = nullptr;
    m_editor_keys.clear();
}

void RunScriptPanel::main_window_rebuilt()
{
    m_rebuild_pending = m_wanted;
    if (m_rebuild_pending)
        wxWakeUpIdle();
}

void RunScriptPanel::python_setting_changed()
{
    const bool on = Mcp::python_scripts_allowed(*wxGetApp().app_config);
    send({{"command", "python"}, {"on", on}});
    if (!on)
        end_session();
}

void RunScriptPanel::shortcuts_changed() { send({{"command", "shortcuts"}, {"toggle_key", toggle_key()}}); }

json RunScriptPanel::toggle_key() const
{
    const ShortcutRegistry& shortcuts = wxGetApp().shortcuts();
    return {{"chord", shortcuts.binding(Shortcut::ToggleRunScript).to_string()}, {"display", shortcuts.display(Shortcut::ToggleRunScript)}};
}

// ---- the page's messages -----------------------------------------------------------------------------------------

void RunScriptPanel::handle(const json& payload)
{
    const std::string command = payload.value("command", "");
    try {
        if (command == "init") {
            // The panel needs the service's tools and jobs, never its network endpoint.
            if (wxGetApp().mcp_service() == nullptr)
                wxGetApp().start_mcp_service(false);
            send(state());
            send_service_state();
            read_api();
        } else if (command == "editor_keys") {
            m_editor_keys.clear();
            for (const json& chord : payload.value("chords", json::array()))
                if (chord.is_string())
                    m_editor_keys.insert(chord.get<std::string>());
        } else if (command == "edit") {
            m_text   = payload.value("text", "");
            m_edited = payload.value("edited", false);
        } else if (command == "new") {
            if (!m_running && confirm_unsaved())
                new_script();
        } else if (command == "open") {
            if (m_running || !confirm_unsaved())
                return;
            wxFileDialog dialog(wxGetApp().mainframe, _L("Open a Python script"), wxEmptyString, wxEmptyString, python_wildcard(),
                                wxFD_OPEN | wxFD_FILE_MUST_EXIST, wxDefaultPosition, wxDefaultSize, Mcp::kHiddenFromAutomation);
            if (dialog.ShowModal() == wxID_OK)
                load(into_path(dialog.GetPath()));
        } else if (command == "open_recent") {
            if (!m_running && confirm_unsaved())
                load(Mcp::path_from_utf8(payload.value("path", "")));
        } else if (command == "close_script") {
            if (!m_running && confirm_unsaved())
                close_script();
        } else if (command == "save")
            save(payload.value("pick", false));
        else if (command == "run")
            run(payload);
        else if (command == "stop") {
            // A Run all still waiting for the old session to end is dropped before it starts, and reported stopped.
            if (m_waiting) {
                m_waiting.reset();
                PythonSessionEvent stopped;
                stopped.kind     = PythonSessionEvent::Kind::Finished;
                stopped.status   = "cancelled";
                stopped.executed = false;
                finish_run(stopped);
            }
            if (m_session)
                m_session->stop();
        } else if (command == "restart")
            end_session();
        else if (command == "toggle") {
            if (std::chrono::steady_clock::now() - m_host_toggle_at >= kToggleEcho)
                flip();
        } else if (command == "counts") {
            const int cells = payload.value("cells", 0), variables = payload.value("variables", 0);
            send({{"command", "counts"},
                  // TRN The session line of the Run Script panel: how many cells ran in the session.
                  {"cells_text", format(_L_PLURAL("%1% cell run", "%1% cells run", cells), cells)},
                  // TRN The session line of the Run Script panel: how many variables the session holds.
                  {"variables_text", format(_L_PLURAL("%1% variable", "%1% variables", variables), variables)}});
        } else if (command == "cell_view") {
            wxGetApp().app_config->set("run_script", "cell_style", payload.value("style", ""));
            wxGetApp().app_config->set("run_script", "highlight_cell", payload.value("highlight", true) ? "1" : "0");
        } else if (command == "pane_layout") {
            wxGetApp().app_config->set("run_script", "split", json(payload.value("split", 0.6)).dump());
            wxGetApp().app_config->set("run_script", "hidden_side", payload.value("hidden", ""));
        } else if (command == "turn_on_python") {
            // The setting's own window tells the panel about a change (python_setting_changed()).
            Mcp::show_python_scripts_dialog(wxGetApp().mainframe);
        }
    } catch (const std::exception& e) {
        send({{"command", "error"}, {"message", e.what()}});
    }
}

json RunScriptPanel::state() const
{
    Mcp::McpService* s = service();
    json             script = script_message();
    script.erase("command");
    return {{"command", "state"},
            {"script", std::move(script)},
            {"python_on", Mcp::python_scripts_allowed(*wxGetApp().app_config)},
            {"ready", s != nullptr && s->ready()},
            {"recent", recent_message()["items"]},
            {"session", session_state()},
            {"toggle_key", toggle_key()},
            {"cell_view", cell_view()},
            {"pane_layout", pane_layout()}};
}

json RunScriptPanel::script_message() const
{
    if (m_none)
        return {{"command", "script"}, {"none", true}};
    return {{"command", "script"},
            {"none", false},
            {"file_name", m_file.empty() ? std::string() : Mcp::path_to_utf8(m_file.filename())},
            {"folder", m_file.empty() ? std::string() : Mcp::path_to_utf8(m_file.parent_path())},
            {"text", m_text},
            {"edited", m_edited}};
}

json RunScriptPanel::recent_message() const
{
    std::vector<std::string> kept;
    json                     items = json::array();
    for (const std::string& path : recent_paths()) {
        boost::system::error_code ec;
        const fs::path            file = Mcp::path_from_utf8(path);
        if (!fs::is_regular_file(file, ec))
            continue;
        kept.push_back(path);
        items.push_back({{"path", path}, {"name", Mcp::path_to_utf8(file.filename())}, {"folder", Mcp::path_to_utf8(file.parent_path())}});
    }
    if (kept.size() != recent_paths().size())
        set_recent_paths(kept);
    return {{"command", "recent"}, {"items", std::move(items)}};
}

void RunScriptPanel::remember_recent(const fs::path& path)
{
    const std::string        utf8  = Mcp::path_to_utf8(path);
    std::vector<std::string> paths = recent_paths();
    paths.erase(std::remove(paths.begin(), paths.end(), utf8), paths.end());
    paths.insert(paths.begin(), utf8);
    if (paths.size() > std::size_t(kMaxRecent))
        paths.resize(kMaxRecent);
    set_recent_paths(paths);
}

std::string RunScriptPanel::session_state() const
{
    if (!m_session)
        return "none";
    if (m_detached || m_session_ending)
        return "ending";
    if (m_running || m_session->working())
        return "working";
    return m_session->has_threads() ? "threads" : "idle";
}

void RunScriptPanel::send_session_state()
{
    const std::string now = session_state();
    if (now == m_sent_session_state)
        return;
    m_sent_session_state = now;
    send({{"command", "session"}, {"state", now}});
}

void RunScriptPanel::send_service_state()
{
    Mcp::McpService* s     = service();
    const bool       ready = s != nullptr && s->ready();
    if (!ready && s != nullptr && !m_ready_timer.IsRunning())
        m_ready_timer.Start(500);
    send({{"command", "service"}, {"ready", ready}});
    if (ready)
        send({{"command", "catalog"}, {"tools", Mcp::editor_tool_catalog(s->registry())}});
}

// The orca.host API's entries, read once per app run on a thread of the panel's, which it joins.
void RunScriptPanel::read_api()
{
    if (m_api_asked) {
        if (!m_api.is_null())
            send({{"command", "api"}, {"entries", m_api}});
        return;
    }
    m_api_asked  = true;
    m_api_thread = std::thread([this, alive = m_alive] {
        json entries = json::array();
        try {
            if (PythonInterpreter::instance().is_initialized())
                if (PythonRuntimeLease lease = PythonInterpreter::instance().acquire_runtime_lease())
                    for (const json& e : Mcp::python_api_reference())
                        entries.push_back({{"name", e["name"]},
                                           {"kind", e["kind"]},
                                           {"signature", e["signature"]},
                                           {"summary", Mcp::first_sentence(e["doc"].get<std::string>())}});
        } catch (const std::exception&) {
            // No API suggestions: the editor still suggests the tools.
        }
        wxGetApp().CallAfter([this, alive, entries = std::move(entries)] {
            if (!alive->load())
                return;
            m_api = entries;
            send({{"command", "api"}, {"entries", m_api}});
        });
    });
}

// ---- the script ---------------------------------------------------------------------------------------------------

// Asks whether to save an edited script before it is closed or replaced. False to keep it: the person cancelled, or chose
// to save and nothing was saved.
bool RunScriptPanel::confirm_unsaved()
{
    if (m_none || !m_edited)
        return true;
    const wxString question = m_file.empty() ? _L("Save this script?\nIf you don't, your changes are lost.") :
                                               // TRN %1% is a script's file name.
                                               format_wxstr(_L("Save the changes to %1%?\nIf you don't, your changes are lost."),
                                                            from_path(m_file.filename()));
    MessageDialog dialog(wxGetApp().mainframe, question, _L("Run Script"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
    // The person's own question: no auto-answer rule answers it.
    dialog.SetName(Mcp::kHiddenFromAutomation);
    dialog.SetButtonLabel(wxID_YES, _L("Save"));
    dialog.SetButtonLabel(wxID_NO, _L("Don't save"));
    const int answer = dialog.ShowModal();
    return answer == wxID_YES ? save(false) : answer == wxID_NO;
}

// Writes the script to its file, or to one the person picks when it has none or `pick` asks for another, in the encoding
// it was read in. False when nothing was written.
bool RunScriptPanel::save(bool pick)
{
    if (m_none)
        return false;
    fs::path path = m_file;
    if (pick || path.empty()) {
        wxFileDialog dialog(wxGetApp().mainframe, _L("Save the script"), path.empty() ? wxString() : from_path(path.parent_path()),
                            path.empty() ? wxString("script.py") : from_path(path.filename()), python_wildcard(),
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT, wxDefaultPosition, wxDefaultSize, Mcp::kHiddenFromAutomation);
        if (dialog.ShowModal() != wxID_OK)
            return false;
        path = into_path(dialog.GetPath());
        // GTK's file dialog doesn't add the filter's extension, so it never asked about the file adding it names.
        if (!is_python_file(path)) {
            path += ".py";
            boost::system::error_code ec;
            if (fs::exists(path, ec)) {
                MessageDialog confirm(wxGetApp().mainframe,
                                      wxString::Format(_L("The file %s already exists.\nDo you want to replace it?"), from_path(path)),
                                      _L("Confirm Save As"), wxYES_NO | wxICON_WARNING);
                confirm.SetName(Mcp::kHiddenFromAutomation);
                if (confirm.ShowModal() != wxID_YES)
                    return false;
            }
        }
    }
    const std::string target = Mcp::path_to_utf8(path);
    std::string       contents, error;
    if (!encode_python_source(m_text, m_encoding, contents, error)) {
        // TRN %1% is a file name, %2% the text encoding it is in (such as cp1252), %3% why the text doesn't fit it.
        send({{"command", "error"}, {"message", format(_u8L("Can't save %1% in its encoding, %2%: %3%"), target, m_encoding, error)}});
        return false;
    }
    // Written beside the file, then renamed over it, so a save that fails leaves the file as it was.
    const std::string       temp = target + ".saving";
    boost::nowide::ofstream out(temp.c_str(), std::ios::binary | std::ios::trunc);
    out << contents;
    out.close();
    const bool written = bool(out);
    if (!written || rename_file(temp, target)) {
        boost::system::error_code ec;
        // Kept when a failed replacement removed the file first (some network mounts): it is the only copy.
        if (!written || fs::exists(path, ec))
            boost::nowide::remove(temp.c_str());
        send({{"command", "error"}, {"message", format(_u8L("Can't save %1%."), target)}});
        return false;
    }
    // A new file is the script's from the next session; the session keeps the file it started with.
    m_file   = path;
    m_edited = false;
    remember_recent(path);
    send({{"command", "saved"}, {"file_name", Mcp::path_to_utf8(path.filename())}, {"folder", Mcp::path_to_utf8(path.parent_path())},
          {"text", m_text}});
    send(recent_message());
    return true;
}

void RunScriptPanel::load(const fs::path& path)
{
    boost::nowide::ifstream in(Mcp::path_to_utf8(path).c_str(), std::ios::binary);
    std::ostringstream      bytes;
    if (in)
        bytes << in.rdbuf();
    std::string text, encoding, error;
    if (!in || !decode_python_source(bytes.str(), text, encoding, error)) {
        // TRN %1% is a script's file name.
        send({{"command", "error"},
              {"message", in ? format(_u8L("%1% can't be read as Python source: %2%"), Mcp::path_to_utf8(path.filename()), error) :
                               format(_u8L("Can't read %1%."), Mcp::path_to_utf8(path))}});
        send(recent_message());
        return;
    }
    end_session();
    m_text     = std::move(text);
    m_file     = path;
    m_encoding = encoding;
    m_edited   = false;
    m_none     = false;
    remember_recent(path);
    send(script_message());
    send(recent_message());
}

void RunScriptPanel::new_script()
{
    end_session();
    m_text     = starter_script();
    m_file     = fs::path();
    m_encoding = "utf-8";
    m_edited   = false;
    m_none     = false;
    send(script_message());
}

void RunScriptPanel::close_script()
{
    end_session();
    m_text.clear();
    m_file     = fs::path();
    m_encoding = "utf-8";
    m_edited   = false;
    m_none     = true;
    send(script_message());
}

void RunScriptPanel::open_file(const fs::path& path)
{
    show();
    if (!m_running && confirm_unsaved())
        load(path);
}

bool RunScriptPanel::confirm_close() { return confirm_unsaved(); }

void RunScriptPanel::dropped(const fs::path& path)
{
    // As Open, which is unavailable while a run goes.
    if (m_running || m_waiting || !is_python_file(path))
        return;
    if (confirm_unsaved())
        load(path);
}

// ---- running ------------------------------------------------------------------------------------------------------

void RunScriptPanel::refuse(const std::string& message) { send({{"command", "run_refused"}, {"message", message}}); }

void RunScriptPanel::run(const json& payload)
{
    if (m_running || m_waiting)
        return;
    if (!Mcp::python_scripts_allowed(*wxGetApp().app_config)) {
        refuse(_u8L("Python scripts are off. You can turn them on in Preferences > AI features > Python scripts."));
        return;
    }
    Mcp::McpService* s = service();
    if (s == nullptr || !s->ready()) {
        refuse(_u8L("Scripts can't run while OrcaSlicer is starting or closing."));
        return;
    }
    const std::string kind = payload.value("kind", "cell");
    PythonWork        work;
    work.source = payload.value("text", m_text);
    if (kind == "file") {
        work.kind        = PythonWork::Kind::File;
        work.code        = work.source;
        work.cell_starts = payload.value("cell_starts", std::vector<int>{1});
        if (work.cell_starts.empty())
            work.cell_starts = {1};
    } else {
        work.kind       = kind == "console" ? PythonWork::Kind::Console : PythonWork::Kind::Cell;
        work.code       = payload.value("code", "");
        work.first_line = std::max(1, payload.value("first_line", 1));
    }
    json started{{"command", "run_started"}, {"kind", kind}, {"first_line", work.first_line}};

    // The script's tool calls act on the main window, and keys it sends without a target go where the focus is.
    if (MainFrame* frame = wxGetApp().mainframe) {
        frame->Raise();
        if (Plater* plater = wxGetApp().plater())
            if (GLCanvas3D* canvas = plater->get_current_canvas3D()) {
                canvas->force_set_focus();
                m_run_focus = canvas->get_wxglcanvas();
            }
    }
    // Run all runs the file as `python file.py` does, in a session of its own.
    if (kind == "file" && m_session) {
        m_waiting     = std::move(work);
        m_running     = true;
        m_run_started = std::chrono::steady_clock::now();
        send(started);
        end_session();
        return;
    }
    if (!m_session && !start_session())
        return;
    submit(std::move(work), std::move(started));
}

bool RunScriptPanel::start_session()
{
    Mcp::McpService* s = service();
    if (s == nullptr) {
        refuse(_u8L("Scripts can't run while OrcaSlicer is starting or closing."));
        return false;
    }
    auto session           = std::make_shared<PythonSession>();
    Mcp::ScriptRun::Events events;
    // Only a job refused before the session was hosted (or one whose handler refused) reports here.
    events.finished = [this, alive = m_alive, session](const json& result, const json&) {
        if (!alive->load() || session != m_session || !m_running)
            return;
        m_running = false;
        send({{"command", "run_finished"}, {"status", "refused"}, {"message", result.value("error", std::string())}});
    };
    m_session_run = s->start_python_session(session, m_file, std::move(events));
    if (!m_session_run) {
        refuse(_u8L("Scripts can't run while OrcaSlicer is starting or closing."));
        return false;
    }
    m_session        = std::move(session);
    m_session_file   = m_file.empty() ? std::string("<run_python>") : Mcp::path_to_utf8(m_file);
    m_session_ending = false;
    m_timer.Start(100);
    send_session_state();
    return true;
}

void RunScriptPanel::end_session()
{
    if (!m_session)
        return;
    m_session_ending = true;
    m_session->end();
    m_session->stop();
    send_session_state();
}

void RunScriptPanel::submit(PythonWork work, const json& started)
{
    if (!m_session->submit(std::move(work))) {
        m_running = false;
        refuse(_u8L("The script could not run."));
        return;
    }
    if (!started.is_null()) {
        m_running     = true;
        m_run_started = std::chrono::steady_clock::now();
        send(started);
    }
    send_session_state();
}

// Every 100 ms while a session exists: its output (coalesced, at most kOutputPerTick a tick, in order with the cells, the
// results and the end of each work; a result is capped by the runner), its state, and its end.
void RunScriptPanel::tick()
{
    if (!m_session) {
        m_timer.Stop();
        return;
    }
    // Read before the events are taken: once the host has returned, every event it sends is queued, so a session seen
    // ended here leaves none behind this take.
    const bool ended = m_session->ended();
    for (PythonSessionEvent& event : m_session->take_events())
        m_backlog.push_back(std::move(event));
    // A session the main window let go of reports to no page.
    if (m_detached)
        m_backlog.clear();
    std::string output;
    const auto  flush = [&] {
        if (!output.empty())
            send({{"command", "output"}, {"text", std::move(output)}});
        output.clear();
    };
    while (!m_backlog.empty()) {
        PythonSessionEvent& event = m_backlog.front();
        if (event.kind == PythonSessionEvent::Kind::Output) {
            const std::size_t room = kOutputPerTick - output.size();
            if (event.text.size() > room) {
                // Cut at a character's start, so each piece is UTF-8 on its own.
                std::size_t cut = room;
                while (cut > 0 && (static_cast<unsigned char>(event.text[cut]) & 0xC0) == 0x80)
                    --cut;
                output += event.text.substr(0, cut);
                event.text.erase(0, cut);
                break;
            }
            output += event.text;
            m_backlog.pop_front();
            continue;
        }
        flush();
        if (event.kind == PythonSessionEvent::Kind::Cell)
            send({{"command", "cell"}, {"index", event.cell}});
        else if (event.kind == PythonSessionEvent::Kind::Result)
            send({{"command", "result"}, {"text", event.text}});
        else
            finish_run(event);
        m_backlog.pop_front();
    }
    flush();
    if (ended && m_backlog.empty()) {
        m_session.reset();
        m_session_run.reset();
        m_detached       = false;
        m_session_ending = false;
        if (m_waiting) {
            std::optional<PythonWork> work = std::move(m_waiting);
            m_waiting.reset();
            if (start_session())
                submit(std::move(*work), json());
            else {
                m_running = false;
                send({{"command", "run_finished"}, {"status", "refused"}, {"message", _u8L("Scripts can't run while OrcaSlicer is starting or closing.")}});
            }
        } else if (m_running) {
            // Ended without reporting its work: its job was refused, or the service stopped.
            m_running = false;
            send({{"command", "run_finished"}, {"status", "refused"}, {"message", _u8L("Scripts can't run while OrcaSlicer is starting or closing.")}});
        }
    }
    send_session_state();
}

void RunScriptPanel::finish_run(const PythonSessionEvent& done)
{
    m_running                = false;
    const std::string status = done.status;
    std::string       status_text;
    if (status == "cancelled" || status == "timed_out")
        status_text = _u8L("Stopped.");
    else if (status == "raised")
        status_text = _u8L("The script raised an exception.");
    else if (status == "unavailable")
        status_text = _u8L("Python is not available in this OrcaSlicer.");
    send({{"command", "run_finished"},
          {"status", status},
          {"status_text", status_text},
          {"message", done.message},
          {"traceback", done.traceback},
          {"error_line", done.error_line},
          {"executed", done.executed},
          {"cells_reported", done.cells_reported},
          {"script_file", m_session_file},
          {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_run_started).count()},
          {"variables", done.variables}});
    // The run lent the focus to the canvas; it comes back while it is still there.
    if (wxWebView* web = web_view(); web != nullptr && m_run_focus && wxWindow::FindFocus() == m_run_focus.get())
        web->SetFocus();
}

}} // namespace Slic3r::GUI
