#include "ActionReplayDialog.hpp"

#include "CommandCatalog.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "NativeCommands.hpp"
#include "Plater.hpp"
#include "RunScriptPanel.hpp"
#include "format.hpp"
#include "Mcp/McpPaths.hpp"
#include "Mcp/McpReplay.hpp"
#include "Mcp/McpPersonRun.hpp"
#include "Mcp/McpReplaySummary.hpp"
#include "Mcp/McpService.hpp"
#include "Mcp/McpTools.hpp"
#include "Mcp/McpTypes.hpp"
#include "Widgets/WebViewHostDialog.hpp"
#include "libslic3r/Utils.hpp"
#ifdef __APPLE__
#include "slic3r/Utils/MacDarkMode.hpp"
#endif

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/base64.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/webview.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

Mcp::McpService* service()
{
    Mcp::McpService* s = wxGetApp().mcp_service();
    return s != nullptr && s->started() && !s->stopping() ? s : nullptr;
}

std::string timestamp()
{
    const std::time_t t = std::time(nullptr);
    char              buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

// The file name a new replay is first offered under: the replay's name, with the characters file
// systems refuse replaced.
std::string file_name_for(const std::string& text)
{
    std::string err;
    const auto  replay = Mcp::parse_replay(text, err);
    std::string name   = replay ? sanitize_filename(replay->name) : std::string();
    boost::trim_if(name, boost::is_any_of(" ."));
    return (name.empty() ? std::string("replay") : name) + ".json";
}

wxString replay_wildcard() { return format_wxstr("%1% (*.json)|*.json|%2% (*.*)|*.*", _L("Replay files"), _L("All files")); }

bool is_replay_file(const fs::path& path) { return boost::iequals(path.extension().string(), ".json"); }

bool is_python_file(const fs::path& path) { return boost::iequals(path.extension().string(), ".py"); }

// A value as a person reads it: a string as its text, anything else as compact JSON.
std::string value_text(const json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

std::string seconds_text(long long ms)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", double(ms) / 1000.0);
    return buf;
}

std::string join(const json& items)
{
    std::string out;
    for (const json& item : items)
        out += (out.empty() ? "" : ", ") + value_text(item);
    return out;
}

// The step's main line and, for a command given arguments, the second line, from describe_step()'s `what`.
std::pair<std::string, std::string> what_text(const json& what)
{
    if (!what.is_object())
        return {};
    const std::string kind = what.value("kind", "");
    if (kind == "files")
        return {join(what["names"]), {}};
    if (kind == "settings")
        return {join(what["pairs"]), {}};
    if (kind == "command") {
        const std::string               key     = what.value("key", "");
        const auto*       command = NativeCommands::find(key);
        // Titled as its tile is, so a Design command's step reads as the Design tab names its tool.
        return {command != nullptr ? into_u8(NativeCommands::tile_title(*command)) : key,
                what.contains("args") ? join(what["args"]) : std::string()};
    }
    if (kind == "recent_project" || kind == "preset")
        return {what.value("name", ""), {}};
    if (kind == "plates") {
        const json& plates = what["plates"];
        if (plates == "current")
            return {_u8L("Current plate"), {}};
        if (plates == "all")
            return {_u8L("All plates"), {}};
        if (plates.is_number_integer())
            // TRN %1% is a plate number, counted from 1.
            return {format(_u8L("Plate %1%"), plates.get<int>() + 1), {}};
        return {value_text(plates), {}};
    }
    if (kind == "replay") {
        const int n = what.value("steps", 0);
        // TRN A step that runs another replay; %1% is how many steps that replay has.
        return {format(_L_PLURAL("A replay of %1% step", "A replay of %1% steps", n), n), {}};
    }
    if (kind == "value")
        return {what.value("text", ""), {}};
    return {};
}

// One check of a step's expect in a sentence; the pointer and operand are the replay's data.
std::string check_text(const std::string& pointer, const std::string& op, const json& operand)
{
    const std::string value = value_text(operand);
    // TRN In these messages %1% names the field of a step's result that the replay checks (a JSON
    // pointer such as /plates/0/outcome) and %2% the value the replay compares it with.
    if (op == "eq")
        return format(_u8L("Checks that %1% is %2%."), pointer, value);
    if (op == "ne")
        return format(_u8L("Checks that %1% is not %2%."), pointer, value);
    if (op == "gt")
        return format(_u8L("Checks that %1% is more than %2%."), pointer, value);
    if (op == "ge")
        return format(_u8L("Checks that %1% is at least %2%."), pointer, value);
    if (op == "lt")
        return format(_u8L("Checks that %1% is less than %2%."), pointer, value);
    if (op == "le")
        return format(_u8L("Checks that %1% is at most %2%."), pointer, value);
    if (op == "contains")
        return format(_u8L("Checks that %1% contains %2%."), pointer, value);
    if (op == "regex")
        return format(_u8L("Checks that %1% matches %2%."), pointer, value);
    if (op == "exists")
        return operand == false ? format(_u8L("Checks that %1% is missing."), pointer) : format(_u8L("Checks that %1% is there."), pointer);
    if (op == "empty")
        return operand == false ? format(_u8L("Checks that %1% is not empty."), pointer) : format(_u8L("Checks that %1% is empty."), pointer);
    // A matcher this version doesn't know, which the run reports: the replay's own words.
    return pointer + ": " + op + " " + value;
}

json checks_view(const json& expect)
{
    json out = json::array();
    for (const auto& [pointer, matchers] : expect.items()) {
        if (!matchers.is_object()) {
            out.push_back(pointer + ": " + matchers.dump());
            continue;
        }
        for (const auto& [op, operand] : matchers.items())
            out.push_back(check_text(pointer, op, operand));
    }
    return out;
}

// A failed expectation in a sentence; the pointer and values are the replay's data.
std::string expect_text(const json& r)
{
    const std::string pointer  = r.value("pointer", "");
    const std::string op       = r.value("op", "");
    const std::string actual   = value_text(r.value("actual", json()));
    const std::string expected = value_text(r.value("expected", json()));
    const bool        exists   = r.value("exists", false);
    // TRN In these messages %1% names the field of a step's result that the replay checked (a JSON
    // pointer such as /plates/0/outcome), %2% the value it had and %3% the value the replay expected.
    if (op == "exists")
        return r.value("expected", json(true)) == true ? format(_u8L("The step finished, but %1% was missing."), pointer) :
                                                         format(_u8L("The step finished, but %1% was there, which the replay did not expect."), pointer);
    if (!exists)
        return format(_u8L("The step finished, but %1% was missing."), pointer);
    if (op == "eq")
        return format(_u8L("The step finished, but %1% was %2%, not %3%."), pointer, actual, expected);
    if (op == "ne")
        return format(_u8L("The step finished, but %1% was %2%, which the replay did not expect."), pointer, actual);
    if (op == "gt")
        return format(_u8L("The step finished, but %1% was %2%, not more than %3%."), pointer, actual, expected);
    if (op == "ge")
        return format(_u8L("The step finished, but %1% was %2%, not at least %3%."), pointer, actual, expected);
    if (op == "lt")
        return format(_u8L("The step finished, but %1% was %2%, not less than %3%."), pointer, actual, expected);
    if (op == "le")
        return format(_u8L("The step finished, but %1% was %2%, not at most %3%."), pointer, actual, expected);
    if (op == "contains")
        return format(_u8L("The step finished, but %1% was %2%, which does not contain %3%."), pointer, actual, expected);
    if (op == "regex")
        return format(_u8L("The step finished, but %1% was %2%, which does not match %3%."), pointer, actual, expected);
    if (op == "empty")
        return r.value("expected", json(true)) == true ? format(_u8L("The step finished, but %1% was %2%, not empty."), pointer, actual) :
                                                         format(_u8L("The step finished, but %1% was empty."), pointer);
    return {};
}

// A screenshot as a 160 px wide PNG data URL for the step's row, or "" when it can't be read.
std::string thumbnail(const std::string& path)
{
    wxImage image;
    if (!image.LoadFile(from_u8(path), wxBITMAP_TYPE_PNG) || !image.IsOk() || image.GetWidth() <= 0)
        return {};
    constexpr int width = 160;
    if (image.GetWidth() > width)
        image.Rescale(width, std::max(1, image.GetHeight() * width / image.GetWidth()), wxIMAGE_QUALITY_HIGH);
    wxMemoryOutputStream out;
    if (!image.SaveFile(out, wxBITMAP_TYPE_PNG))
        return {};
    std::string bytes(out.GetSize(), '\0');
    out.CopyTo(bytes.data(), bytes.size());
    return "data:image/png;base64," + into_u8(wxBase64Encode(bytes.data(), bytes.size()));
}

} // namespace

ActionReplayDialog::ActionReplayDialog(wxWindow* parent) : WebViewHostDialog(parent)
{
    // Replays drive the main window through the MCP tools; the dialog keeps out of their sight.
    SetName(Mcp::kHiddenFromAutomation);
    // Replaying needs the MCP service's tools and jobs, never its network endpoint.
    if (service() == nullptr)
        wxGetApp().start_mcp_service(false);

    m_ready_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        Mcp::McpService* s = service();
        if (s == nullptr || s->ready())
            m_ready_timer.Stop();
        send_service_state();
    });
    Bind(wxEVT_CLOSE_WINDOW, &ActionReplayDialog::on_close, this);
    static const char page[] = "web/dialog/ActionReplay/index.html";
    m_page                   = wxFileName(from_path(fs::path(resources_dir()) / page));
    if (create_webview(page, _L("Action Replay"), wxSize(760, 720), wxSize(560, 480))) {
        // No reload: it would put the page back to its draft while the host keeps the file it replaces.
        browser()->EnableBrowserAcceleratorKeys(false);
        browser()->Bind(wxEVT_WEBVIEW_NAVIGATING, &ActionReplayDialog::on_navigating, this);
    }
}

ActionReplayDialog::~ActionReplayDialog()
{
    m_alive->store(false);
    m_ready_timer.Stop();
    // A parent's destruction (a language change, quitting) ends the run with the window.
    if (m_run) {
        m_run->stop();
        m_run->detach();
    }
}

bool ActionReplayDialog::running() const { return m_run && m_run->running(); }

bool ActionReplayDialog::owns_focus(const wxWindow* focus) const
{
    for (const wxWindow* w = focus; w != nullptr; w = w->GetParent())
        if (w == this)
            return true;
#ifdef _WIN32
    // WebView2 keeps the focus in windows of its own, which wx doesn't know.
    if (const HWND native = ::GetFocus(); native != nullptr && ::IsChild(static_cast<HWND>(GetHWND()), native))
        return true;
#endif
    return false;
}

void ActionReplayDialog::forward_action(const std::string& action) { call_web_handler({{"command", "key"}, {"action", action}}); }

nlohmann::json ActionReplayDialog::localized_strings() const
{
    // Shortcuts as the menus write them (⌘+S, Ctrl+S). The editor's suggestion key is Control+Space on
    // every platform, which the Mac writes ⌃.
    const std::string ctrl = shortkey_ctrl_prefix();
#ifdef __APPLE__
    const std::string control = u8"\u2303+", enter = "Return";
#else
    const std::string control = ctrl, enter = "Enter";
#endif
    return {
        {"key_suggest", control + "Space"},
        {"key_run", ctrl + enter},
        {"key_save", ctrl + "S"},
        {"key_save_as", ctrl + shortkey_shift_prefix() + "S"},
        {"open_replay", _u8L("Open replay…")},
        {"save", _u8L("Save")},
        // TRN Saves a new replay file: the button opens a file dialog.
        {"save_new", _u8L("Save…")},
        {"save_as", _u8L("Save as…")},
        // TRN Shown for a moment after a replay file is saved.
        {"saved", _u8L("Saved")},
        {"edited", _u8L("(edited)")},
        // TRN The name a new replay starts with, in the editor of the Action Replay window.
        {"untitled_replay", _u8L("Untitled replay")},
        {"steps", _u8L("Steps")},
        {"steps_hint", _u8L("Steps appear here as you write them.")},
        {"replay_source", _u8L("Replay file")},
        {"source_placeholder", _u8L("Write a replay here, or open one.")},
        // TRN The first comment line a new replay starts with. MCP is the protocol AI agents use to drive OrcaSlicer.
        {"draft_about", _u8L("A replay file runs OrcaSlicer's tools in order, the way an AI agent does through MCP.")},
        // TRN A comment line a new replay starts with. "tool", "args" and "expect" are the file's keys and stay in English.
        {"draft_steps", _u8L("Each step names a \"tool\" and its \"args\"; \"expect\" checks the result before the next step.")},
        // TRN %s is the placeholder ${out_dir}, written as is in the replay file.
        {"draft_out_dir", _u8L("%s is this run's output folder.")},
        // TRN The first %s is a key such as Ctrl+Space, the second one such as Ctrl+Enter.
        {"draft_keys", _u8L("%s shows suggestions, %s replays the file.")},
        {"run", _u8L("Replay")},
        {"run_again", _u8L("Replay again")},
        {"stop", _u8L("Stop")},
        {"close", _u8L("Close")},
        {"open_output", _u8L("Open output folder")},
        {"run_this_step", _u8L("Run this step")},
        {"show_raw", _u8L("Show raw result")},
        {"hide_raw", _u8L("Hide raw result")},
        {"show_in_folder", _u8L("Show in folder")},
        {"skipped", _u8L("Skipped")},
        {"checks_result", _u8L("Checks the result")},
        {"flag_writes_file", _u8L("Writes a file")},
        {"flag_changes_presets", _u8L("Changes presets")},
        {"flag_replaces_project", _u8L("Replaces the project")},
        {"flag_sends_to_printer", _u8L("Sends to a printer")},
        // TRN A duration; %s is a number of seconds such as 0.8.
        {"seconds", _u8L("%s s")},
        // TRN %s is the number of the step that is running.
        {"running_one", _u8L("Running step %s")},
        // TRN The first %s is the number of the step that is running, the second how many steps there are.
        {"running_step", _u8L("Running step %s of %s")},
        {"hands_off", _u8L("Don't use OrcaSlicer until the replay finishes.")},
        {"starting", _u8L("OrcaSlicer is still starting…")},
        {"code_editor", code_editor_strings()},
    };
}

void ActionReplayDialog::on_script_message(const nlohmann::json& payload)
{
    // Out of the web view's message callback: window work on its stack (file dialogs, prompts,
    // closing) is a crash class on GTK and macOS.
    wxGetApp().CallAfter([this, alive = m_alive, payload] {
        if (alive->load())
            handle(payload);
    });
}

void ActionReplayDialog::handle(const nlohmann::json& payload)
{
    const std::string command = payload.value("command", "");
    try {
        if (command == "init")
            send_service_state();
        else if (command == "open_file")
            open_file();
        else if (command == "edit") {
            m_text   = payload.value("text", "");
            m_edited = payload.value("edited", false);
        } else if (command == "editor_keys") {
            m_editor_keys.clear();
            for (const json& chord : payload.value("chords", json::array()))
                if (chord.is_string())
                    m_editor_keys.insert(chord.get<std::string>());
        } else if (command == "save")
            save(payload.value("pick", false));
        else if (command == "parse")
            parse(payload);
        else if (command == "run")
            run(payload);
        else if (command == "stop") {
            if (m_run)
                m_run->stop();
        } else if (command == "open_output")
            desktop_open_any_folder(Mcp::path_to_utf8(out_dir()));
        else if (command == "open_location")
            desktop_open_any_folder(Mcp::path_to_utf8(Mcp::path_from_utf8(payload.value("path", "")).parent_path()));
        else if (command == "open_image")
            wxLaunchDefaultApplication(from_u8(payload.value("path", "")));
        else if (command == "close_page")
            Close();
    } catch (const std::exception& e) {
        call_web_handler({{"command", "error"}, {"message", e.what()}});
    }
}

void ActionReplayDialog::send_service_state()
{
    Mcp::McpService* s     = service();
    const bool       ready = s != nullptr && s->ready();
    if (!ready && s != nullptr && !m_ready_timer.IsRunning())
        m_ready_timer.Start(500);
    json msg{{"command", "service"}, {"ready", ready}};
    if (ready)
        msg["tools"] = Mcp::editor_tool_catalog(s->catalog());
    call_web_handler(msg);
}

void ActionReplayDialog::open_file()
{
    if (!confirm_unsaved())
        return;
    wxFileDialog dialog(this, _L("Open a replay file"), wxEmptyString, wxEmptyString, replay_wildcard(), wxFD_OPEN | wxFD_FILE_MUST_EXIST,
                        wxDefaultPosition, wxDefaultSize, Mcp::kHiddenFromAutomation);
    const int result = dialog.ShowModal();
    if (!m_alive->load())
        return;
    restore_z_order();
    if (result != wxID_OK)
        return;
    const fs::path path = into_path(dialog.GetPath());
    if (is_python_file(path))
        offer_run_script(path);
    else
        load_file(path);
}

void ActionReplayDialog::load_file(const fs::path& path)
{
    boost::nowide::ifstream in(Mcp::path_to_utf8(path).c_str(), std::ios::binary);
    if (!in) {
        call_web_handler({{"command", "error"}, {"message", format(_u8L("Can't read %1%."), Mcp::path_to_utf8(path))}});
        return;
    }
    std::ostringstream text;
    text << in.rdbuf();
    m_file = path;
    m_jobs = json::object();
    call_web_handler({{"command", "source"}, {"file_name", Mcp::path_to_utf8(path.filename())}, {"text", text.str()}});
}

// The page goes nowhere but its own loads. A file dropped on it is a navigation to that file, handled after this event.
void ActionReplayDialog::on_navigating(wxWebViewEvent& event)
{
    if (own_page_navigation(event.GetURL(), m_page)) {
        event.Skip();
        return;
    }
    event.Veto();
    fs::path path = dropped_file(event.GetURL());
    if (path.empty())
        return;
    wxGetApp().CallAfter([this, alive = m_alive, path = std::move(path)] {
        if (alive->load())
            dropped(path);
    });
}

void ActionReplayDialog::dropped(const fs::path& path)
{
    // As Open, which is unavailable while a replay runs.
    if (running())
        return;
    if (is_python_file(path))
        offer_run_script(path);
    else if (is_replay_file(path) && confirm_unsaved())
        load_file(path);
}

void ActionReplayDialog::offer_run_script(const fs::path& path)
{
    // TRN %1% is a file name.
    MessageDialog dialog(this, format_wxstr(_L("%1% is a Python script. Python scripts run in the Run Script panel."), from_path(path.filename())),
                         _L("Action Replay"), wxOK | wxCANCEL | wxICON_INFORMATION);
    // The person's own question: no auto-answer rule answers it.
    dialog.SetName(Mcp::kHiddenFromAutomation);
    dialog.SetButtonLabel(wxID_OK, _L("Open in Run Script"));
    const int answer = dialog.ShowModal();
    if (!m_alive->load())
        return;
    if (answer == wxID_OK)
        wxGetApp().run_script_panel().open_file(path);
    else
        restore_z_order();
}

// Writes the source to its file, or to one the person picks when it has none or `pick` asks for another.
// False when nothing was written: the person cancelled, or the file can't be written.
bool ActionReplayDialog::save(bool pick)
{
    fs::path path = m_file;
    if (pick || path.empty()) {
        const wxString name = path.empty() ? from_u8(file_name_for(m_text)) : from_path(path.filename());
        wxFileDialog   dialog(this, _L("Save the replay file"), path.empty() ? wxString() : from_path(path.parent_path()), name,
                              replay_wildcard(), wxFD_SAVE | wxFD_OVERWRITE_PROMPT, wxDefaultPosition, wxDefaultSize,
                              Mcp::kHiddenFromAutomation);
        const int      result = dialog.ShowModal();
        if (!m_alive->load())
            return false;
        restore_z_order();
        if (result != wxID_OK)
            return false;
        path = into_path(dialog.GetPath());
        // GTK's file dialog doesn't add the filter's extension, so it never asked about the file adding it names.
        if (!is_replay_file(path)) {
            path += ".json";
            boost::system::error_code ec;
            if (fs::exists(path, ec)) {
                MessageDialog confirm(this, wxString::Format(_L("The file %s already exists.\nDo you want to replace it?"), from_path(path)),
                                      _L("Confirm Save As"), wxYES_NO | wxICON_WARNING);
                confirm.SetName(Mcp::kHiddenFromAutomation);
                const int answer = confirm.ShowModal();
                if (!m_alive->load() || answer != wxID_YES)
                    return false;
            }
        }
    }
    // Written beside the file, then renamed over it. Unlike write_file_atomically() nothing is ever written
    // in place, so a save that fails (a full disk, a folder that refuses new files) leaves the file as it was.
    const std::string       target = Mcp::path_to_utf8(path);
    const std::string       temp   = target + ".saving";
    boost::nowide::ofstream out(temp.c_str(), std::ios::binary | std::ios::trunc);
    out << m_text;
    out.close();
    const bool written = bool(out);
    if (!written || rename_file(temp, target)) {
        boost::system::error_code ec;
        // Kept when a failed replacement removed the file first (some network mounts): it is the only copy.
        if (!written || fs::exists(path, ec))
            boost::nowide::remove(temp.c_str());
        call_web_handler({{"command", "error"}, {"message", format(_u8L("Can't save %1%."), target)}});
        return false;
    }
    // Saved somewhere new, the replay is that file's: ${replay_dir} and the output folder follow it.
    const bool moved = path != m_file;
    m_file           = path;
    m_edited         = false;
    call_web_handler({{"command", "saved"}, {"file_name", Mcp::path_to_utf8(path.filename())}, {"moved", moved}, {"text", m_text}});
    return true;
}

// Asks whether to save an edited source before it is closed or replaced. False to keep it: the person
// cancelled, or chose to save and nothing was saved.
bool ActionReplayDialog::confirm_unsaved()
{
    if (!m_edited)
        return true;
    const wxString question = m_file.empty() ? _L("Save this replay file?\nIf you don't, your changes are lost.") :
                                               // TRN %1% is a replay file's name.
                                               format_wxstr(_L("Save the changes to %1%?\nIf you don't, your changes are lost."), from_path(m_file.filename()));
    MessageDialog dialog(this, question, _L("Action Replay"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
    // The person's own question: no auto-answer rule answers it.
    dialog.SetName(Mcp::kHiddenFromAutomation);
    dialog.SetButtonLabel(wxID_YES, _L("Save"));
    dialog.SetButtonLabel(wxID_NO, _L("Don't save"));
    const int answer = dialog.ShowModal();
    if (!m_alive->load())
        return false;
    return answer == wxID_YES ? save(false) : answer == wxID_NO;
}

// A file's runs write next to it, into <name>.out; a new replay's into one folder of the session's
// output, named when first needed.
fs::path ActionReplayDialog::out_dir() const
{
    if (!m_file.empty()) {
        fs::path dir = m_file.parent_path() / m_file.stem();
        dir += ".out";
        return dir;
    }
    if (m_draft_out_dir.empty())
        m_draft_out_dir = Mcp::output_dir() / ("replay-" + timestamp());
    return m_draft_out_dir;
}

Mcp::ReplayContext ActionReplayDialog::context() const
{
    Mcp::ReplayContext ctx;
    ctx.out_dir    = out_dir();
    ctx.replay_dir = m_file.empty() ? ctx.out_dir : m_file.parent_path();
    ctx.jobs       = m_jobs;
    return ctx;
}

json ActionReplayDialog::describe(const Mcp::Replay& replay) const
{
    Mcp::McpService* s = service();
    // ${jobs.NAME} stays as written: a full run starts from no saved jobs and a single step uses the
    // ones saved when it runs, so a job id resolved now would be one neither of them uses.
    Mcp::ReplayContext ctx = context();
    ctx.jobs               = json::object();
    json               steps = json::array();
    for (std::size_t i = 0; i < replay.steps.size(); ++i) {
        const Mcp::ReplayStep&      step = replay.steps[i];
        const Mcp::StepDescription  d    = Mcp::describe_step(step, replay, ctx);
        const NativeCommand*        def  = s != nullptr ? s->catalog().find(step.tool) : nullptr;
        json                        flags = json::array();
        for (Mcp::StepFlag f : d.flags)
            flags.push_back(Mcp::flag_id(f));
        json view{{"index", i + 1},
                  {"tool", step.tool},
                  {"title", def != nullptr ? into_u8(NativeCommands::tile_title(*def)) : step.tool},
                  {"flags", std::move(flags)},
                  {"checks_result", d.checks_result},
                  {"optional", step.optional},
                  {"args", Mcp::substitute_lenient(step.args, replay, ctx)},
                  {"expect", step.expect},
                  {"checks", checks_view(step.expect)}};
        const auto [what, detail] = what_text(d.what);
        if (!what.empty())
            view["what_text"] = what;
        if (!detail.empty())
            view["args_text"] = detail;
        steps.push_back(std::move(view));
    }
    return steps;
}

void ActionReplayDialog::parse(const nlohmann::json& payload)
{
    json reply{{"command", "parsed"}, {"seq", payload.value("seq", 0)}, {"ok", false}};
    try {
        Mcp::McpService* s = service();
        std::string      err;
        const auto       replay = Mcp::parse_replay(m_text, err);
        if (s == nullptr)
            reply["error"] = _u8L("Replays can't run while OrcaSlicer is starting or closing.");
        else if (!replay)
            reply["error"] = err;
        else {
            for (std::size_t i = 0; i < replay->steps.size() && !reply.contains("error"); ++i)
                if (s->catalog().lookup(replay->steps[i].tool) == nullptr)
                    // TRN %1% is a step number, %2% the name of the tool the replay asks for.
                    reply["error"] = format(_u8L("Step %1% uses a tool this version of OrcaSlicer doesn't have: %2%"), i + 1,
                                            replay->steps[i].tool);
            if (!reply.contains("error")) {
                const int n = int(replay->steps.size());
                reply.update({{"ok", true},
                              {"name", replay->name},
                              {"steps", describe(*replay)},
                              {"count_text", format(_L_PLURAL("%1% step", "%1% steps", n), n)}});
            }
        }
    } catch (const std::exception& e) {
        reply["error"] = e.what();
    }
    call_web_handler(reply);
}

void ActionReplayDialog::refuse(const std::string& message, const std::string& detail)
{
    call_web_handler({{"command", "run_refused"}, {"message", message}, {"detail", detail}});
}

void ActionReplayDialog::run(const nlohmann::json& payload)
{
    if (running())
        return;
    Mcp::McpService* s = service();
    if (s == nullptr || !s->ready()) {
        refuse(_u8L("Replays can't run while OrcaSlicer is starting or closing."));
        return;
    }
    std::string err;
    auto        replay = Mcp::parse_replay(m_text, err);
    if (!replay) {
        refuse(_u8L("The replay could not run."), err);
        return;
    }
    std::optional<std::size_t> only;
    if (payload.contains("step") && payload["step"].is_number_unsigned()) {
        only = payload["step"].get<std::size_t>();
        if (*only < 1 || *only > replay->steps.size()) {
            refuse(_u8L("The replay could not run."));
            return;
        }
    }
    // A full run starts from no saved jobs; the ones earlier runs saved stay until a run starts.
    Mcp::ReplayContext ctx = context();
    if (!only)
        ctx.jobs = json::object();

    // The runner asks about unsaved changes only before replacements it can answer for (steps that
    // check them themselves), or ones the replay may answer the app's own question for.
    bool checked = false, app_asked = false, answers = false;
    for (std::size_t i = only ? *only - 1 : 0; i < (only ? *only : replay->steps.size()); ++i) {
        const Mcp::StepDescription d = Mcp::describe_step(replay->steps[i], *replay, ctx);
        checked                      = checked || d.replace == "checked";
        app_asked                    = app_asked || d.replace == "app_asked";
        answers                      = answers || d.answers_dialogs;
    }
    bool    discard = false;
    Plater* plater  = wxGetApp().plater();
    if (plater != nullptr && plater->is_project_dirty() && (checked || (app_asked && answers))) {
        MessageDialog dialog(this, _L("This replay replaces the current project, which has unsaved changes. Save them before replaying it?"),
                             _L("Action Replay"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
        dialog.SetName(Mcp::kHiddenFromAutomation);
        const int answer = dialog.ShowModal();
        if (!m_alive->load())
            return;
        if (answer == wxID_YES) {
            if (plater->save_project() != wxID_YES) {
                // Saving a new project asks for a file name in a native panel, which leaves the
                // main window in front; the run isn't going ahead, so bring this dialog back.
                restore_z_order();
                refuse({});
                return;
            }
        } else if (answer == wxID_NO)
            discard = true;
        else {
            refuse({});
            return;
        }
    }

    // Steps act on the main window, and keys a replay sends without a target go where the focus is.
    if (MainFrame* frame = wxGetApp().mainframe) {
        frame->Raise();
        if (plater != nullptr)
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->force_set_focus();
    }

    Mcp::PersonRun::Events events;
    auto                   alive = m_alive;
    events.step_started          = [this, alive](std::size_t index) {
        if (alive->load())
            call_web_handler({{"command", "step_started"}, {"index", index}});
    };
    events.step_finished = [this, alive](const json& entry) {
        if (alive->load())
            call_web_handler(step_view(entry));
    };
    events.finished = [this, alive](const json& result, const json& jobs) {
        if (!alive->load())
            return;
        if (!jobs.is_null())
            m_jobs = jobs;
        call_web_handler(finished_view(result));
        // The run lent the focus to the main window; it comes back to this dialog, which shows the outcome.
#ifdef __APPLE__
        if (MainFrame* frame = wxGetApp().mainframe)
            keep_window_above(GetHandle(), frame->GetHandle(), false);
#endif
        restore_z_order();
    };
    m_single  = only;
    m_total   = replay->steps.size();
    m_started = std::chrono::steady_clock::now();
    m_run     = s->start_replay_run(std::move(*replay), ctx, only, discard, std::move(events));
    if (!m_run) {
        refuse(_u8L("Replays can't run while OrcaSlicer is starting or closing."));
        return;
    }
#ifdef __APPLE__
    // The run's progress and its Stop button stay in sight above the raised main window, as Windows and
    // GTK keep a dialog above its parent by themselves.
    if (MainFrame* frame = wxGetApp().mainframe)
        keep_window_above(GetHandle(), frame->GetHandle(), true);
#endif
    // Reaches the page before any report of this run: those come through the service's dispatcher
    // and then call_web_handler(), two CallAfter()s on the app's queue, after this one.
    json started{{"command", "run_started"}, {"total", m_total}};
    if (only)
        started["step"] = *only;
    call_web_handler(started);
}

json ActionReplayDialog::step_view(const json& entry) const
{
    const std::string tool = entry.value("tool", "");
    json              view{{"command", "step_finished"},
              {"index", entry.value("index", 0)},
              {"ok", entry.value("ok", false)},
              {"cancelled", entry.value("cancelled", false)},
              {"duration_ms", entry.value("duration_ms", 0)},
              {"raw", entry}};
    if (!entry.value("ok", false)) {
        const json        reason = entry.value("reason", json::object());
        const std::string kind   = reason.value("kind", "");
        if (kind == "cancelled")
            view["failure_text"] = _u8L("Stopped.");
        else if (kind == "timeout") {
            const int s = reason.value("seconds", 0);
            view["failure_text"] = format(_L_PLURAL("The step didn't finish within %1% second.", "The step didn't finish within %1% seconds.", s), s);
        } else if (kind == "expect") {
            view["failure_text"]   = _u8L("The step's result was not what the replay expected.");
            const std::string text = expect_text(reason);
            view["failure_detail"] = text.empty() ? entry.value("expect_failure", "") : text;
        } else if (kind == "expect_invalid") {
            view["failure_text"]   = _u8L("The replay's check of this step's result isn't valid.");
            view["failure_detail"] = entry.value("expect_failure", "");
        } else if (kind == "placeholder")
            // TRN %1% is a placeholder such as ${vars.model} that the replay uses.
            view["failure_text"] = format(_u8L("The replay refers to %1%, which has no value."), reason.value("text", ""));
        else if (kind == "missing_job") {
            if (reason.contains("saved_by") && reason["saved_by"].is_number())
                // TRN %1% is the number of the step that saves the job this step needs.
                view["failure_text"] = format(_u8L("This step needs the job saved by step %1%. Run that step first."), reason["saved_by"].get<int>());
            else
                view["failure_text"] = _u8L("This step needs a job that no step of this replay saves.");
        } else if (kind == "python") {
            const std::string status = reason.value("status", "");
            view["failure_text"]     = status == "cancelled" ? _u8L("The script was stopped.") :
                                       status == "timed_out" ? _u8L("The script ran out of time.") :
                                                               _u8L("The script raised an exception.");
            const json result      = entry.value("result", json::object());
            view["failure_detail"] = result.value("traceback", std::string()).empty() ? entry.value("error", std::string()) :
                                                                                       result.value("traceback", std::string());
        } else {
            // A tool's own refusal is the only account of what went wrong; it is agent-facing English.
            view["failure_text"]   = _u8L("This step failed.");
            view["failure_detail"] = entry.contains("error") ? value_text(entry["error"]) : entry.value("expect_failure", "");
        }
    }

    const json result = entry.value("result", json());
    json       lines  = json::array();
    if (tool == "run_script" && result.is_object()) {
        // What the script printed, then what it returned.
        std::istringstream output(result.value("output", std::string()));
        std::string        line;
        int                shown = 0;
        while (std::getline(output, line))
            if (shown++ < 40)
                lines.push_back(line);
        if (shown > 40)
            // TRN %1% is how many more lines of a script's output there are.
            lines.push_back(format(_L_PLURAL("(%1% more line)", "(%1% more lines)", shown - 40), shown - 40));
        if (result.contains("result") && !result["result"].is_null())
            // TRN %1% is the value a Python script returned, as JSON.
            lines.push_back(format(_u8L("Result: %1%"), result["result"].dump()));
    }
    const json summary = Mcp::describe_result(tool, result);
    if (summary.is_object()) {
        const std::string kind = summary.value("kind", "");
        if (kind == "objects") {
            const int n = summary.value("count", 0);
            lines.push_back(format(_L_PLURAL("%1% object", "%1% objects", n), n));
        } else if (kind == "plates") {
            const int ok = summary.value("ok", 0), other = summary.value("other", 0);
            lines.push_back(format(_L_PLURAL("%1% plate sliced", "%1% plates sliced", ok), ok));
            if (other > 0)
                lines.push_back(format(_L_PLURAL("%1% plate did not slice", "%1% plates did not slice", other), other));
        } else if (kind == "replay" && !summary.value("passed", true)) {
            // TRN %1% is the number of the step of the nested replay that failed.
            view["warning_text"] = format(_u8L("The nested replay failed at step %1%."), summary.value("failed_step", 0));
        }
    }
    view["result_lines"] = std::move(lines);

    json files = json::array();
    for (const std::string& path : Mcp::result_files(result))
        files.push_back({{"path", path}, {"name", Mcp::path_to_utf8(Mcp::path_from_utf8(path).filename())}});
    if (tool == "screenshot" && !files.empty())
        if (const std::string url = thumbnail(files[0]["path"].get<std::string>()); !url.empty())
            view["thumbnail"] = url;
    view["files"] = std::move(files);
    return view;
}

json ActionReplayDialog::finished_view(const json& result) const
{
    json view{{"command", "run_finished"}, {"single", m_single.has_value()}, {"out_dir", Mcp::path_to_utf8(out_dir())}};
    if (result.contains("error")) {
        view.update({{"passed", false}, {"outcome_kind", "fail"}, {"outcome_text", _u8L("The replay could not run.")}, {"error_detail", value_text(result["error"])}});
        return view;
    }
    const bool  passed  = result.value("passed", false);
    const json  steps   = result.value("steps", json::array());
    bool        stopped = false, nested_failed = false;
    for (const json& step : steps) {
        stopped = stopped || step.value("cancelled", false);
        const json summary = Mcp::describe_result(step.value("tool", ""), step.value("result", json()));
        nested_failed = nested_failed || (summary.is_object() && summary.value("kind", "") == "replay" && !summary.value("passed", true));
    }
    const int failed = result.value("failed_step", 0);
    const int total  = int(m_total);
    view.update({{"passed", passed}, {"stopped", stopped}});
    if (failed > 0)
        view["failed_step"] = failed;
    if (stopped)
        view.update({{"outcome_kind", "warn"},
                     // TRN %1% is the step the run stopped at, %2% how many steps the replay has.
                     {"outcome_text", m_single ? _u8L("Stopped.") : format(_u8L("Stopped at step %1% of %2%"), failed, total)}});
    else if (!passed)
        view.update({{"outcome_kind", "fail"},
                     // TRN %1% is the step that failed, %2% how many steps the replay has.
                     {"outcome_text", m_single ? _u8L("The step failed.") : format(_u8L("Failed at step %1% of %2%"), failed, total)}});
    else if (nested_failed)
        view.update({{"outcome_kind", "warn"}, {"outcome_text", _u8L("Finished, but a nested replay failed.")}});
    else if (m_single)
        view.update({{"outcome_kind", "pass"}, {"outcome_text", _u8L("The step passed.")}});
    else {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_started).count();
        // TRN %1% is how many steps ran, %2% how long the run took in seconds, such as 14.0.
        view.update({{"outcome_kind", "pass"},
                     {"outcome_text", format(_L_PLURAL("Passed, %1% step in %2% s", "Passed, %1% steps in %2% s", total), total, seconds_text(ms))}});
    }
    return view;
}

void ActionReplayDialog::on_close(wxCloseEvent& event)
{
    if (event.CanVeto()) {
        if (running()) {
            MessageDialog dialog(this, _L("A replay is running. Stop it and close this window?"), _L("Action Replay"), wxYES_NO | wxICON_QUESTION);
            dialog.SetName(Mcp::kHiddenFromAutomation);
            if (dialog.ShowModal() != wxID_YES) {
                event.Veto();
                return;
            }
        }
        // Asked before the run is stopped, so declining keeps the run going as well as the text.
        if (!confirm_unsaved()) {
            event.Veto();
            return;
        }
    }
    if (running()) {
        m_run->stop();
        m_run->detach();
    }
    Destroy();
}

}} // namespace Slic3r::GUI
