#include "AgentActivityPanel.hpp"

#include "ActionReplayDialog.hpp"
#include "CommandCatalog.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "NativeCommands.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Mcp/ActivityLog.hpp"
#include "Mcp/AutomationRuntime.hpp"
#include "Mcp/McpPaths.hpp"
#include "Mcp/McpReplay.hpp"
#include "Mcp/McpReplaySummary.hpp"
#include "Mcp/McpTools.hpp"
#include "Mcp/VideoOverlay.hpp"
#include "Mcp/WindowNames.hpp"
#include "Widgets/WebView.hpp"
#include "Widgets/WebViewHostDialog.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <wx/app.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/filename.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/translation.h>
#include <wx/utils.h>
#include <wx/webview.h>
#include <wx/window.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

// The least time between two refreshes of the page, and how often a pane off screen with changes waiting looks again.
constexpr std::chrono::milliseconds kRefreshEvery{100};
constexpr std::chrono::milliseconds kOffScreenRetry{500};

// The page's text.
json page_strings()
{
    json strings = {
        {"pane", _u8L("Agent Activity")},
        // TRN A button of the Agent Activity pane: it empties the list of calls.
        {"clear", _u8L("Clear list")},
        {"show", _u8L("Show")},
        // TRN Agent Activity pane: show every call, reads included.
        {"show_all", _u8L("Everything")},
        // TRN Agent Activity pane: show only the calls that change something.
        {"show_changes", _u8L("Changes")},
        // TRN Agent Activity pane: show only the calls that failed.
        {"show_failures", _u8L("Failures")},
        // TRN Agent Activity pane, before who made the calls listed, such as "Everyone" or "Claude Code".
        {"from", _u8L("From:")},
        {"everyone", _u8L("Everyone")},
        {"find", _u8L("Find a call")},
        {"empty", _u8L("Calls from agents, Action Replay, Run Script and plugins appear here as they run.")},
        {"no_match", _u8L("No call matches.")},
        // TRN Agent Activity pane, while a call runs. %s is who made it, such as "Claude Code" or "Action Replay".
        {"working", _u8L("%s is working")},
        {"idle", _u8L("Idle")},
        {"nothing_yet", _u8L("Nothing yet")},
        // TRN Agent Activity pane: how many calls are listed. %s is the number.
        {"calls", _u8L("Calls: %s")},
        // TRN Agent Activity pane: how many listed calls failed. %s is the number.
        {"failed", _u8L("Failed: %s")},
        // TRN Agent Activity pane: a button that scrolls to calls that came while the person read further up. %s is their number.
        {"new_calls", _u8L("New calls: %s")},
        // TRN A duration under a minute. %s is the seconds, with one decimal under ten.
        {"seconds", _u8L("%s s")},
        // TRN A duration of a minute or more: the minutes, then the seconds.
        {"minutes_seconds", _u8L("%s min %s s")},
        // TRN Agent Activity pane, after a running call. %s is its job, such as "j4".
        {"running_as_job", _u8L("running as job %s")},
        {"read_call", _u8L("A call that only reads")},
        {"change_call", _u8L("A call that changes something")},
        {"dialog_answered", _u8L("A dialog a rule answered")},
        {"request", _u8L("Request")},
        {"response", _u8L("Response")},
        {"copy_request", _u8L("Copy request")},
        {"copy_response", _u8L("Copy response")},
        {"copied", _u8L("Copied")},
        // TRN Agent Activity pane: a call that has no response yet.
        {"nothing_yet_response", _u8L("Nothing yet.")},
        // TRN Agent Activity pane: a request or response left out for its size. %s is its size in bytes.
        {"too_large", _u8L("Too large to show here (%s bytes).")},
        // TRN Agent Activity pane, beside an agent's request. The first %s is the agent, such as "Claude Code"; the second the time.
        {"tools_call_from", _u8L("tools/call from %s at %s")},
        // TRN Agent Activity pane, beside an agent's request sent through tool_call (keep both names). The first %s is the agent; the second the time.
        {"tools_call_via", _u8L("tools/call from %s through tool_call, at %s")},
        // TRN Agent Activity pane, beside a step's request. %s is the title of the run that made it, such as "Run a replay".
        {"made_by", _u8L("made by %s")},
        // TRN Agent Activity pane, beside the code of a cell or console line. %s is the time.
        {"work_at", _u8L("run in the Run Script panel at %s")},
        // TRN Agent Activity pane, beside a request. %s is the time.
        {"sent_at", _u8L("sent at %s")},
        // TRN Agent Activity pane, beside a response. %s is a duration.
        {"answered_in", _u8L("answered in %s")},
        // TRN Agent Activity pane, beside a response. The first %s is the job, such as "j4"; the second a duration.
        {"ran_as_job", _u8L("ran as job %s; finished after %s")},
        // TRN Agent Activity pane, while a call runs. %s is its job, such as "j4".
        {"running_no_result", _u8L("running as job %s; no result yet")},
        {"waiting_reply", _u8L("waiting for the reply")},
        // TRN Agent Activity pane, beside a failed call's response. %s is a duration.
        {"failed_after", _u8L("failed after %s")},
        // TRN Agent Activity pane: a dialog an auto-answer rule answered.
        {"dialog", _u8L("Dialog")},
        {"dialog_title", _u8L("Title")},
        {"dialog_message", _u8L("Message")},
        // TRN Agent Activity pane: the button an auto-answer rule pressed in a dialog.
        {"pressed", _u8L("Pressed")},
        // TRN Agent Activity pane: the auto-answer rule that answered a dialog.
        {"rule", _u8L("Rule")},
        // TRN Agent Activity pane, beside a rule. The first %s is the number of the call that set it; the second its title.
        {"set_by", _u8L("set by call %s, %s")},
        {"show_call", _u8L("Show that call")},
        // TRN Agent Activity pane, beside the rule that answered a dialog, when the call that set it isn't in the list.
        {"rule_gone", _u8L("the call that set it isn't listed")},
        // TRN Agent Activity pane: in place of a response that repeats an earlier call's result the pane can't mask.
        {"withheld", _u8L("Can't be shown: it repeats a call whose record the pane no longer has.")},
        {"show_in_folder", _u8L("Show in folder")},
    };
    strings.update(flag_strings());
    return strings;
}

// Who made an entry's calls, as the pane names them.
std::string source_label(const json& entry)
{
    const std::string source = Mcp::arg_string(entry, "source");
    if (source == "agent") {
        const std::string client = Mcp::arg_string(entry, "client");
        // TRN Agent Activity pane: an AI agent whose name the app doesn't know.
        return client.empty() ? _u8L("Agent") : client;
    }
    if (source == "action_replay")
        return _u8L("Action Replay");
    if (source == "run_script")
        return _u8L("Run Script");
    if (source == "command_line")
        // TRN Agent Activity pane: a replay or script OrcaSlicer was started with (--run).
        return _u8L("Command line");
    if (source == "plugin") {
        const std::string key = Mcp::arg_string(entry, "plugin");
        PluginDescriptor  descriptor;
        if (PluginManager::instance().try_get_plugin_descriptor(key, descriptor) && !descriptor.name.empty())
            return descriptor.name;
        return key;
    }
    // TRN Agent Activity pane: automation whose source the app doesn't know.
    return _u8L("Automation");
}

// The first line of `text`, shortened.
std::string first_line(const std::string& text) { return Mcp::shorten(text, 160); }

} // namespace

// The panel's dock pane: a web view on the page, hidden from the automation tools with everything inside it.
class AgentActivityPane : public wxPanel
{
public:
    AgentActivityPane(wxWindow* parent, AgentActivityPanel& owner);
    ~AgentActivityPane() override;
    AgentActivityPane(const AgentActivityPane&)            = delete;
    AgentActivityPane& operator=(const AgentActivityPane&) = delete;

    // HandleStudio(payload) on the page, after the current event and only while the pane lives.
    void send(const json& payload);

private:
    AgentActivityPanel&                m_owner;
    wxWebView*                         m_web = nullptr;
    wxFileName                         m_page;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

AgentActivityPane::AgentActivityPane(wxWindow* parent, AgentActivityPanel& owner)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL, Mcp::kHiddenFromAutomation), m_owner(owner)
{
    const fs::path page = fs::path(resources_dir()) / "web" / "dialog" / "AgentActivity" / "index.html";
    m_page              = wxFileName(from_path(page));
    m_web               = WebViewHostDialog::create_pane_web_view(
        this, page, page_strings(), m_alive, [owner = &m_owner](const json& payload) { owner->handle(payload); },
        // The page goes nowhere but its own loads.
        [this](wxWebViewEvent& event) {
            if (WebViewHostDialog::own_page_navigation(event.GetURL(), m_page))
                event.Skip();
            else
                event.Veto();
        });
}

AgentActivityPane::~AgentActivityPane()
{
    m_alive->store(false);
    m_owner.pane_destroyed(this);
}

void AgentActivityPane::send(const json& payload)
{
    wxString script = "HandleStudio(" + wxString::FromUTF8(payload.dump(-1, ' ', false, json::error_handler_t::replace)) + ")";
    wxGetApp().CallAfter([this, script = std::move(script), alive = m_alive] {
        if (alive->load() && m_web != nullptr)
            WebView::RunScript(m_web, script);
    });
}

AgentActivityPanel::AgentActivityPanel() : m_alive(std::make_shared<std::atomic<bool>>(true))
{
    m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { refresh(); });
    // The pane comes back after a rebuild once the old main window is gone, as the Run Script panel's does.
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

AgentActivityPanel::~AgentActivityPanel()
{
    m_alive->store(false);
    m_timer.Stop();
    // The panel goes at exit, before the runtime: nothing is recorded any more.
    if (Mcp::AutomationRuntime* runtime = wxGetApp().automation_if_started()) {
        runtime->activity().set_recording(false);
        runtime->set_activity_listener({});
    }
}

void AgentActivityPanel::set_wanted(bool wanted)
{
    m_wanted = wanted;
    // The log records while the person has the pane open, docked on another tab and across a main-window rebuild too.
    if (Mcp::AutomationRuntime* runtime = wxGetApp().automation_if_started())
        runtime->activity().set_recording(wanted);
}

void AgentActivityPanel::toggle()
{
    if (on_screen()) {
        set_wanted(false);
        if (Plater* plater = wxGetApp().plater(); plater != nullptr)
            plater->show_dock_pane(m_pane, false);
    } else
        show();
}

void AgentActivityPanel::show()
{
    MainFrame* frame  = wxGetApp().mainframe;
    Plater*    plater = wxGetApp().plater();
    if (frame == nullptr || plater == nullptr)
        return;
    // The log lives in the automation runtime, started here without the network endpoint when nothing has started it.
    Mcp::AutomationRuntime* runtime = wxGetApp().automation();
    if (runtime == nullptr)
        return;
    runtime->set_activity_listener([this, alive = m_alive] {
        if (alive->load())
            changed();
    });
    set_wanted(true);
    // Docked, the pane shows on Prepare and Preview only; floated, on every tab.
    const bool floating = m_pane != nullptr && plater->is_dock_pane_floating(m_pane);
    if (!floating && !frame->is_prepare_or_preview_tab())
        frame->select_tab(wxString(TAB_ID_PREPARE));
    if (m_pane == nullptr) {
        m_pane = new AgentActivityPane(plater, *this);
        plater->add_dock_pane(
            m_pane, "agent_activity", _L("Agent Activity"), "right", wxSize(420, 640),
            [this, alive = m_alive] {
                if (alive->load())
                    set_wanted(false);
            },
            true, true);
    } else
        plater->show_dock_pane(m_pane, true);
    refresh();
}

bool AgentActivityPanel::on_screen() const { return m_pane != nullptr && m_pane->IsShownOnScreen(); }

void AgentActivityPanel::forget_pane()
{
    m_pane       = nullptr;
    m_page_ready = false;
    m_version    = 0;
}

void AgentActivityPanel::pane_destroyed(AgentActivityPane* pane)
{
    if (m_pane == pane)
        forget_pane();
}

void AgentActivityPanel::main_window_closing()
{
    m_timer.Stop();
    forget_pane();
}

void AgentActivityPanel::main_window_rebuilt()
{
    m_rebuild_pending = m_wanted;
    if (m_rebuild_pending)
        wxWakeUpIdle();
}

void AgentActivityPanel::handle(const json& payload)
{
    const std::string command = Mcp::arg_string(payload, "command");
    if (command == "ready") {
        m_page_ready = true;
        m_version    = 0;
        refresh();
    } else if (command == "clear") {
        if (Mcp::AutomationRuntime* runtime = wxGetApp().automation_if_started())
            runtime->activity().clear();
    } else if (command == "copy") {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(Mcp::arg_string(payload, "text"))));
            wxTheClipboard->Close();
        }
    } else if (command == "open_file" || command == "show_file") {
        // A file of an entry's result, named by the entry and its place in the list the page was given, never by a path:
        // the image opens in the system's viewer, as Action Replay opens one, and any file shows in its folder.
        const auto id    = payload.find("id");
        const auto index = payload.find("index");
        if (id == payload.end() || !id->is_number_unsigned() || index == payload.end() || !index->is_number_unsigned())
            return;
        const auto files = m_files.find(id->get<uint64_t>());
        if (files == m_files.end() || index->get<uint64_t>() >= files->second.size())
            return;
        const std::string& path = files->second[index->get<std::size_t>()];
        if (command == "open_file")
            wxLaunchDefaultApplication(from_u8(path));
        else
            desktop_open_any_folder(Mcp::path_to_utf8(Mcp::path_from_utf8(path).parent_path()));
    }
}

void AgentActivityPanel::changed()
{
    if (m_timer.IsRunning())
        return;
    const auto since = std::chrono::steady_clock::now() - m_last_refresh;
    if (since >= kRefreshEvery)
        refresh();
    else
        m_timer.StartOnce(int(std::chrono::duration_cast<std::chrono::milliseconds>(kRefreshEvery - since).count()) + 1);
}

void AgentActivityPanel::refresh()
{
    if (m_pane == nullptr || !m_page_ready)
        return;
    // A pane off screen reads nothing, and the log reports no more changes until it is read: while the pane is wanted
    // (docked off Prepare and Preview, the app's window minimized), it looks again until it shows.
    if (!on_screen()) {
        if (m_wanted && !m_timer.IsRunning())
            m_timer.StartOnce(int(kOffScreenRetry.count()));
        return;
    }
    Mcp::AutomationRuntime* runtime = wxGetApp().automation_if_started();
    if (runtime == nullptr)
        return;
    m_last_refresh     = std::chrono::steady_clock::now();
    const json changes = runtime->activity().changes_since(m_version);
    // The page holds nothing yet at version 0.
    const bool reset = m_version == 0 || changes.value("reset", false);
    m_version        = changes.value("version", uint64_t(0));
    json entries     = json::array();
    for (const json& entry : changes["entries"]) {
        // Wording runs on the UI thread, where an exception would end the app: an entry it fails on shows by its tool.
        try {
            entries.push_back(page_entry(entry));
        } catch (const std::exception&) {
            json plain = entry;
            plain.erase("args");
            plain["title"]        = Mcp::arg_string(entry, "tool");
            plain["what"]         = "";
            plain["source_label"] = source_label(entry);
            plain["flags"]        = json::array();
            entries.push_back(std::move(plain));
        }
    }
    for (const json& id : changes["removed"]) {
        m_files.erase(id.get<uint64_t>());
        m_thumbs.erase(id.get<uint64_t>());
    }
    if (!reset && entries.empty() && changes["removed"].empty())
        return;
    m_pane->send({{"command", "entries"}, {"reset", reset}, {"entries", std::move(entries)}, {"removed", changes["removed"]}});
}

json AgentActivityPanel::page_entry(const json& entry)
{
    json              out     = entry;
    json              args    = out.contains("args") ? std::move(out["args"]) : json::object();
    const json&       request = entry["request"];
    const std::string kind    = Mcp::arg_string(entry, "kind");
    const std::string tool    = Mcp::arg_string(entry, "tool");
    out.erase("args");
    out["source_label"] = source_label(entry);
    out["flags"]        = json::array();
    if (kind == "dialog") {
        // TRN Agent Activity pane: the title of a step where an auto-answer rule answered a dialog.
        out["title"] = _u8L("Answered a dialog");
        out["what"]  = Mcp::arg_string(request, "title");
    } else if (kind == "work") {
        const std::string work  = Mcp::arg_string(request, "work");
        const std::string file  = Mcp::path_to_utf8(Mcp::path_from_utf8(Mcp::arg_string(request, "file")).filename());
        const std::string title = Mcp::arg_string(request, "title");
        const int         cell  = request.is_object() ? request.value("cell", 0) : 0;
        if (work == "file") {
            out["title"] = _u8L("Run all");
            out["what"]  = file;
        } else if (work == "console") {
            // TRN Agent Activity pane: the title of a step where a person ran a line in the Run Script panel's console.
            out["title"] = _u8L("Run a console line");
            out["what"]  = first_line(Mcp::arg_string(request, "code"));
        } else {
            // TRN Agent Activity pane: the title of a step where a person ran a cell in the Run Script panel.
            out["title"] = _u8L("Run a cell");
            // TRN Agent Activity pane: the cell a person ran. %1% is the script's file name, %2% the cell's number, %3% its title.
            out["what"] = title.empty() ? format(_u8L("%1%, cell %2%"), file, cell) : format(_u8L("%1%, cell %2%: %3%"), file, cell, title);
        }
    } else {
        const std::shared_ptr<const NativeCommand> def = NativeCommands::instance().lookup(tool);
        if (def == nullptr) {
            out["title"] = tool;
            out["what"]  = "";
        } else {
            // The arguments of a call the protocol refused were never checked, and the wording reads them by type: what
            // it can't read leaves the call titled by its tool, as one the catalog doesn't know.
            try {
                // Worded as the recording captions word the call.
                const auto [title, titled] = shown_title(NativeCommands::instance(), *def, args);
                const char* context        = title_context(*titled);
                out["title"] = into_u8(context != nullptr ? I18N::translate(title, context) : wxGetTranslation(from_u8(title)));
                out["what"]  = Mcp::caption_detail(tool, args);
                // A run's own steps show what they do: read before it runs, a run given no file reads as doing everything.
                if (!entry.value("read", false) && tool != "replay_run" && tool != "script_run") {
                    Mcp::ReplayStep step;
                    step.tool = tool;
                    step.args = args.is_object() ? std::move(args) : json::object();
                    for (const Mcp::StepFlag flag :
                         Mcp::describe_step(NativeCommands::instance(), step, Mcp::Replay{}, Mcp::ReplayContext{}).flags)
                        out["flags"].push_back(Mcp::flag_id(flag));
                }
            } catch (const std::exception&) {
                out["title"] = tool;
                out["what"]  = "";
                out["flags"] = json::array();
            }
        }
    }
    // A failure the log read from a run's result, which the call itself reported as a success.
    if (const json failure = entry.value("failure", json()); failure.contains("script")) {
        out["error"] = script_status_text(Mcp::arg_string(failure, "script"));
    } else if (failure.contains("why")) {
        const std::string why = Mcp::arg_string(failure, "why");
        if (const int step = failure.value("step", 0); step > 0)
            // TRN Agent Activity pane: why a replay failed. %1% is the number of the step that failed, %2% that step's
            // error, in English.
            out["error"] = why.empty() ? format(_u8L("Step %1% failed"), step) : format(_u8L("Step %1% failed: %2%"), step, why);
        else
            // TRN Agent Activity pane: a replay failed without saying which step.
            out["error"] = why.empty() ? _u8L("The replay failed.") : why;
    }
    // The files a finished call's result names, by name, and a screenshot's thumbnail, read once the call has written
    // them. A path the log masked names no file, and gets no link.
    const uint64_t id = entry.value("id", uint64_t(0));
    if (Mcp::arg_string(entry, "status") == "done") {
        auto files = m_files.find(id);
        if (files == m_files.end()) {
            std::vector<std::string> existing;
            for (const std::string& path : Mcp::result_files(entry["response"])) {
                boost::system::error_code ec;
                if (fs::is_regular_file(Mcp::path_from_utf8(path), ec))
                    existing.push_back(path);
            }
            files = m_files.emplace(id, std::move(existing)).first;
        }
        if (!files->second.empty()) {
            out["files"] = json::array();
            for (const std::string& path : files->second)
                out["files"].push_back(Mcp::path_to_utf8(Mcp::path_from_utf8(path).filename()));
        }
        if (tool == "window_screenshot" && !files->second.empty()) {
            auto thumb = m_thumbs.find(id);
            if (thumb == m_thumbs.end())
                thumb = m_thumbs.emplace(id, screenshot_thumbnail(files->second.front())).first;
            if (!thumb->second.empty())
                out["thumb"] = thumb->second;
        }
    }
    return out;
}

}} // namespace Slic3r::GUI
