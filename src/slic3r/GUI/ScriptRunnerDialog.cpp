#include "ScriptRunnerDialog.hpp"

#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "NativeCommands.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Mcp/McpPaths.hpp"
#include "Mcp/McpScript.hpp"
#include "Mcp/McpScriptRun.hpp"
#include "Mcp/McpScriptSummary.hpp"
#include "Mcp/McpService.hpp"
#include "Mcp/McpTypes.hpp"
#include "Widgets/WebViewHostDialog.hpp"
#include "libslic3r/Utils.hpp"

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
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/utils.h>

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

// The file name a new script is first offered under: the script's name, with the characters file
// systems refuse replaced.
std::string file_name_for(const std::string& text)
{
    std::string err;
    const auto  script = Mcp::parse_script(text, err);
    std::string name   = script ? sanitize_filename(script->name) : std::string();
    boost::trim_if(name, boost::is_any_of(" ."));
    return (name.empty() ? std::string("script") : name) + ".json";
}

wxString script_wildcard() { return format_wxstr("%1% (*.json)|*.json", _L("Script files")); }

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

// The step's main line and, for a command with a param, the second line, from describe_step()'s `what`.
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
        return {command != nullptr ? _u8L(command->title.c_str()) : key, what.contains("param") ? value_text(what["param"]) : std::string()};
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
    if (kind == "script") {
        const int n = what.value("steps", 0);
        // TRN A step that runs another script; %1% is how many steps that script has.
        return {format(_L_PLURAL("A script of %1% step", "A script of %1% steps", n), n), {}};
    }
    if (kind == "value")
        return {what.value("text", ""), {}};
    return {};
}

// One check of a step's expect in a sentence; the pointer and operand are the script's data.
std::string check_text(const std::string& pointer, const std::string& op, const json& operand)
{
    const std::string value = value_text(operand);
    // TRN In these messages %1% names the field of a step's result that the script checks (a JSON
    // pointer such as /plates/0/outcome) and %2% the value the script compares it with.
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
    // A matcher this version doesn't know, which the run reports: the script's own words.
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

// A failed expectation in a sentence; the pointer and values are the script's data.
std::string expect_text(const json& r)
{
    const std::string pointer  = r.value("pointer", "");
    const std::string op       = r.value("op", "");
    const std::string actual   = value_text(r.value("actual", json()));
    const std::string expected = value_text(r.value("expected", json()));
    const bool        exists   = r.value("exists", false);
    // TRN In these messages %1% names the field of a step's result that the script checked (a JSON
    // pointer such as /plates/0/outcome), %2% the value it had and %3% the value the script expected.
    if (op == "exists")
        return r.value("expected", json(true)) == true ? format(_u8L("The step finished, but %1% was missing."), pointer) :
                                                         format(_u8L("The step finished, but %1% was there, which the script did not expect."), pointer);
    if (!exists)
        return format(_u8L("The step finished, but %1% was missing."), pointer);
    if (op == "eq")
        return format(_u8L("The step finished, but %1% was %2%, not %3%."), pointer, actual, expected);
    if (op == "ne")
        return format(_u8L("The step finished, but %1% was %2%, which the script did not expect."), pointer, actual);
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

// What the editor suggests for each tool: its name, title and arguments with their types and allowed
// values. The argument descriptions are written for agents, in English, and stay out.
json tool_catalog(const Mcp::ToolRegistry& registry)
{
    json tools = json::array();
    for (const Mcp::ToolDef& def : registry.tools()) {
        json       args       = json::object();
        const auto properties = def.input_schema.find("properties");
        if (properties != def.input_schema.end())
            for (const auto& [name, schema] : properties->items()) {
                json arg = json::object();
                for (const char* key : {"type", "enum"})
                    if (schema.contains(key))
                        arg[key] = schema[key];
                args[name] = std::move(arg);
            }
        tools.push_back({{"name", def.name}, {"title", _u8L(def.title.c_str())}, {"args", std::move(args)}});
    }
    return tools;
}

} // namespace

ScriptRunnerDialog::ScriptRunnerDialog(wxWindow* parent) : WebViewHostDialog(parent)
{
    // Scripts drive the main window through the MCP tools; the dialog keeps out of their sight.
    SetName(Mcp::kHiddenFromAutomation);
    // The script runner needs the MCP service's tools and jobs, never its network endpoint.
    if (service() == nullptr)
        wxGetApp().start_mcp_service(false);

    m_ready_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        Mcp::McpService* s = service();
        if (s == nullptr || s->ready())
            m_ready_timer.Stop();
        send_service_state();
    });
    Bind(wxEVT_CLOSE_WINDOW, &ScriptRunnerDialog::on_close, this);
    create_webview("web/dialog/ScriptRunner/index.html", _L("Run Script"), wxSize(760, 720), wxSize(560, 480));
}

ScriptRunnerDialog::~ScriptRunnerDialog()
{
    m_alive->store(false);
    m_ready_timer.Stop();
    // A parent's destruction (a language change, quitting) ends the run with the window.
    if (m_run) {
        m_run->stop();
        m_run->detach();
    }
}

bool ScriptRunnerDialog::running() const { return m_run && m_run->running(); }

nlohmann::json ScriptRunnerDialog::localized_strings() const
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
        {"open_script", _u8L("Open script…")},
        {"save", _u8L("Save")},
        // TRN Saves a new script: the button opens a file dialog.
        {"save_new", _u8L("Save…")},
        {"save_as", _u8L("Save as…")},
        // TRN Shown for a moment after a script is saved.
        {"saved", _u8L("Saved")},
        {"edited", _u8L("(edited)")},
        // TRN The name a new script starts with, in the editor of the Run Script dialog.
        {"untitled_script", _u8L("Untitled script")},
        {"steps", _u8L("Steps")},
        {"steps_hint", _u8L("Steps appear here as you write them.")},
        {"script_source", _u8L("Script source")},
        {"source_placeholder", _u8L("Write a script here, or open one.")},
        // TRN The first comment line a new script starts with. MCP is the protocol AI agents use to drive OrcaSlicer.
        {"draft_about", _u8L("A script runs OrcaSlicer's tools in order, the way an AI agent does through MCP.")},
        // TRN A comment line a new script starts with. "tool", "args" and "expect" are the script's keys and stay in English.
        {"draft_steps", _u8L("Each step names a \"tool\" and its \"args\"; \"expect\" checks the result before the next step.")},
        // TRN %s is the placeholder ${out_dir}, written as is in the script.
        {"draft_out_dir", _u8L("%s is this run's output folder.")},
        // TRN The first %s is a key such as Ctrl+Space, the second one such as Ctrl+Enter.
        {"draft_keys", _u8L("%s shows suggestions, %s runs the script.")},
        {"run", _u8L("Run")},
        {"run_again", _u8L("Run again")},
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
        {"hands_off", _u8L("Don't use OrcaSlicer until the script finishes.")},
        {"starting", _u8L("OrcaSlicer is still starting…")},
    };
}

void ScriptRunnerDialog::on_script_message(const nlohmann::json& payload)
{
    // Out of the web view's message callback: window work on its stack (file dialogs, prompts,
    // closing) is a crash class on GTK and macOS.
    wxGetApp().CallAfter([this, alive = m_alive, payload] {
        if (alive->load())
            handle(payload);
    });
}

void ScriptRunnerDialog::handle(const nlohmann::json& payload)
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

void ScriptRunnerDialog::send_service_state()
{
    Mcp::McpService* s     = service();
    const bool       ready = s != nullptr && s->ready();
    if (!ready && s != nullptr && !m_ready_timer.IsRunning())
        m_ready_timer.Start(500);
    json msg{{"command", "service"}, {"ready", ready}};
    if (ready)
        msg["tools"] = tool_catalog(s->registry());
    call_web_handler(msg);
}

void ScriptRunnerDialog::open_file()
{
    if (!confirm_unsaved())
        return;
    wxFileDialog dialog(this, _L("Open a script"), wxEmptyString, wxEmptyString, script_wildcard(), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    const int result = dialog.ShowModal();
    if (!m_alive->load())
        return;
    restore_z_order();
    if (result != wxID_OK)
        return;
    const fs::path          path = into_path(dialog.GetPath());
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

// Writes the source to its file, or to one the person picks when it has none or `pick` asks for another.
// False when nothing was written: the person cancelled, or the file can't be written.
bool ScriptRunnerDialog::save(bool pick)
{
    fs::path path = m_file;
    if (pick || path.empty()) {
        const wxString name = path.empty() ? from_u8(file_name_for(m_text)) : from_path(path.filename());
        wxFileDialog   dialog(this, _L("Save the script"), path.empty() ? wxString() : from_path(path.parent_path()), name,
                              script_wildcard(), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        const int      result = dialog.ShowModal();
        if (!m_alive->load())
            return false;
        restore_z_order();
        if (result != wxID_OK)
            return false;
        path = into_path(dialog.GetPath());
        // GTK's file dialog doesn't add the filter's extension, so it never asked about the file adding it names.
        if (!boost::iequals(path.extension().string(), ".json")) {
            path += ".json";
            boost::system::error_code ec;
            if (fs::exists(path, ec)) {
                MessageDialog confirm(this, wxString::Format(_L("The file %s already exists.\nDo you want to replace it?"), from_path(path)),
                                      _L("Confirm Save As"), wxYES_NO | wxICON_WARNING);
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
    // Saved somewhere new, the script is that file's: ${script_dir} and the output folder follow it.
    const bool moved = path != m_file;
    m_file           = path;
    m_edited         = false;
    call_web_handler({{"command", "saved"}, {"file_name", Mcp::path_to_utf8(path.filename())}, {"moved", moved}, {"text", m_text}});
    return true;
}

// Asks whether to save an edited source before it is closed or replaced. False to keep it: the person
// cancelled, or chose to save and nothing was saved.
bool ScriptRunnerDialog::confirm_unsaved()
{
    if (!m_edited)
        return true;
    const wxString question = m_file.empty() ? _L("Save this script?\nIf you don't, your changes are lost.") :
                                               // TRN %1% is a script's file name.
                                               format_wxstr(_L("Save the changes to %1%?\nIf you don't, your changes are lost."), from_path(m_file.filename()));
    MessageDialog dialog(this, question, _L("Run Script"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
    dialog.SetButtonLabel(wxID_YES, _L("Save"));
    dialog.SetButtonLabel(wxID_NO, _L("Don't save"));
    const int answer = dialog.ShowModal();
    if (!m_alive->load())
        return false;
    return answer == wxID_YES ? save(false) : answer == wxID_NO;
}

// A file's runs write next to it, into <name>.out; a new script's into one folder of the session's
// output, named when first needed.
fs::path ScriptRunnerDialog::out_dir() const
{
    if (!m_file.empty()) {
        fs::path dir = m_file.parent_path() / m_file.stem();
        dir += ".out";
        return dir;
    }
    if (m_draft_out_dir.empty())
        m_draft_out_dir = Mcp::output_dir() / ("script-" + timestamp());
    return m_draft_out_dir;
}

Mcp::ScriptContext ScriptRunnerDialog::context() const
{
    Mcp::ScriptContext ctx;
    ctx.out_dir    = out_dir();
    ctx.script_dir = m_file.empty() ? ctx.out_dir : m_file.parent_path();
    ctx.jobs       = m_jobs;
    return ctx;
}

json ScriptRunnerDialog::describe(const Mcp::Script& script) const
{
    Mcp::McpService* s = service();
    // ${jobs.NAME} stays as written: a full run starts from no saved jobs and a single step uses the
    // ones saved when it runs, so a job id resolved now would be one neither of them uses.
    Mcp::ScriptContext ctx = context();
    ctx.jobs               = json::object();
    json               steps = json::array();
    for (std::size_t i = 0; i < script.steps.size(); ++i) {
        const Mcp::ScriptStep&      step = script.steps[i];
        const Mcp::StepDescription  d    = Mcp::describe_step(step, script, ctx);
        const Mcp::ToolDef*         def  = s != nullptr ? s->registry().find(step.tool) : nullptr;
        json                        flags = json::array();
        for (Mcp::StepFlag f : d.flags)
            flags.push_back(Mcp::flag_id(f));
        json view{{"index", i + 1},
                  {"tool", step.tool},
                  {"title", def != nullptr ? _u8L(def->title.c_str()) : step.tool},
                  {"flags", std::move(flags)},
                  {"checks_result", d.checks_result},
                  {"optional", step.optional},
                  {"args", Mcp::substitute_lenient(step.args, script, ctx)},
                  {"expect", step.expect},
                  {"checks", checks_view(step.expect)}};
        const auto [what, param] = what_text(d.what);
        if (!what.empty())
            view["what_text"] = what;
        if (!param.empty())
            view["param_text"] = param;
        steps.push_back(std::move(view));
    }
    return steps;
}

void ScriptRunnerDialog::parse(const nlohmann::json& payload)
{
    json reply{{"command", "parsed"}, {"seq", payload.value("seq", 0)}, {"ok", false}};
    try {
        Mcp::McpService* s = service();
        std::string      err;
        const auto       script = Mcp::parse_script(m_text, err);
        if (s == nullptr)
            reply["error"] = _u8L("Scripts can't run while OrcaSlicer is starting or closing.");
        else if (!script)
            reply["error"] = err;
        else {
            for (std::size_t i = 0; i < script->steps.size() && !reply.contains("error"); ++i)
                if (s->registry().find(script->steps[i].tool) == nullptr)
                    // TRN %1% is a step number, %2% the name of the tool the script asks for.
                    reply["error"] = format(_u8L("Step %1% uses a tool this version of OrcaSlicer doesn't have: %2%"), i + 1,
                                            script->steps[i].tool);
            if (!reply.contains("error")) {
                const int n = int(script->steps.size());
                reply.update({{"ok", true},
                              {"name", script->name},
                              {"steps", describe(*script)},
                              {"count_text", format(_L_PLURAL("%1% step", "%1% steps", n), n)}});
            }
        }
    } catch (const std::exception& e) {
        reply["error"] = e.what();
    }
    call_web_handler(reply);
}

void ScriptRunnerDialog::refuse(const std::string& message, const std::string& detail)
{
    call_web_handler({{"command", "run_refused"}, {"message", message}, {"detail", detail}});
}

void ScriptRunnerDialog::run(const nlohmann::json& payload)
{
    if (running())
        return;
    Mcp::McpService* s = service();
    if (s == nullptr || !s->ready()) {
        refuse(_u8L("Scripts can't run while OrcaSlicer is starting or closing."));
        return;
    }
    std::string err;
    auto        script = Mcp::parse_script(m_text, err);
    if (!script) {
        refuse(_u8L("The script could not run."), err);
        return;
    }
    std::optional<std::size_t> only;
    if (payload.contains("step") && payload["step"].is_number_unsigned()) {
        only = payload["step"].get<std::size_t>();
        if (*only < 1 || *only > script->steps.size()) {
            refuse(_u8L("The script could not run."));
            return;
        }
    }
    // A full run starts from no saved jobs; the ones earlier runs saved stay until a run starts.
    Mcp::ScriptContext ctx = context();
    if (!only)
        ctx.jobs = json::object();

    // The runner asks about unsaved changes only before replacements it can answer for (steps that
    // check them themselves), or ones the script may answer the app's own question for.
    bool checked = false, app_asked = false, answers = false;
    for (std::size_t i = only ? *only - 1 : 0; i < (only ? *only : script->steps.size()); ++i) {
        const Mcp::StepDescription d = Mcp::describe_step(script->steps[i], *script, ctx);
        checked                      = checked || d.replace == "checked";
        app_asked                    = app_asked || d.replace == "app_asked";
        answers                      = answers || d.answers_dialogs;
    }
    bool    discard = false;
    Plater* plater  = wxGetApp().plater();
    if (plater != nullptr && plater->is_project_dirty() && (checked || (app_asked && answers))) {
        MessageDialog dialog(this, _L("This script replaces the current project, which has unsaved changes. Save them before running the script?"),
                             _L("Run Script"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
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

    // Steps act on the main window, and keys a script sends without a target go where the focus is.
    if (MainFrame* frame = wxGetApp().mainframe) {
        frame->Raise();
        if (plater != nullptr)
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->force_set_focus();
    }

    Mcp::ScriptRun::Events events;
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
    };
    m_single  = only;
    m_total   = script->steps.size();
    m_started = std::chrono::steady_clock::now();
    m_run     = s->start_script_run(std::move(*script), ctx, only, discard, std::move(events));
    if (!m_run) {
        refuse(_u8L("Scripts can't run while OrcaSlicer is starting or closing."));
        return;
    }
    // Reaches the page before any report of this run: those come through the service's dispatcher
    // and then call_web_handler(), two CallAfter()s on the app's queue, after this one.
    json started{{"command", "run_started"}, {"total", m_total}};
    if (only)
        started["step"] = *only;
    call_web_handler(started);
}

json ScriptRunnerDialog::step_view(const json& entry) const
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
            view["failure_text"]   = _u8L("The step's result was not what the script expected.");
            const std::string text = expect_text(reason);
            view["failure_detail"] = text.empty() ? entry.value("expect_failure", "") : text;
        } else if (kind == "expect_invalid") {
            view["failure_text"]   = _u8L("The script's check of this step's result isn't valid.");
            view["failure_detail"] = entry.value("expect_failure", "");
        } else if (kind == "placeholder")
            // TRN %1% is a placeholder such as ${vars.model} that the script uses.
            view["failure_text"] = format(_u8L("The script refers to %1%, which has no value."), reason.value("text", ""));
        else if (kind == "missing_job") {
            if (reason.contains("saved_by") && reason["saved_by"].is_number())
                // TRN %1% is the number of the step that saves the job this step needs.
                view["failure_text"] = format(_u8L("This step needs the job saved by step %1%. Run that step first."), reason["saved_by"].get<int>());
            else
                view["failure_text"] = _u8L("This step needs a job that no step of this script saves.");
        } else {
            // A tool's own refusal is the only account of what went wrong; it is agent-facing English.
            view["failure_text"]   = _u8L("This step failed.");
            view["failure_detail"] = entry.contains("error") ? value_text(entry["error"]) : entry.value("expect_failure", "");
        }
    }

    const json result = entry.value("result", json());
    json       lines  = json::array();
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
        } else if (kind == "script" && !summary.value("passed", true)) {
            // TRN %1% is the number of the step of the nested script that failed.
            view["warning_text"] = format(_u8L("The nested script failed at step %1%."), summary.value("failed_step", 0));
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

json ScriptRunnerDialog::finished_view(const json& result) const
{
    json view{{"command", "run_finished"}, {"single", m_single.has_value()}, {"out_dir", Mcp::path_to_utf8(out_dir())}};
    if (result.contains("error")) {
        view.update({{"passed", false}, {"outcome_kind", "fail"}, {"outcome_text", _u8L("The script could not run.")}, {"error_detail", value_text(result["error"])}});
        return view;
    }
    const bool  passed  = result.value("passed", false);
    const json  steps   = result.value("steps", json::array());
    bool        stopped = false, nested_failed = false;
    for (const json& step : steps) {
        stopped = stopped || step.value("cancelled", false);
        const json summary = Mcp::describe_result(step.value("tool", ""), step.value("result", json()));
        nested_failed = nested_failed || (summary.is_object() && summary.value("kind", "") == "script" && !summary.value("passed", true));
    }
    const int failed = result.value("failed_step", 0);
    const int total  = int(m_total);
    view.update({{"passed", passed}, {"stopped", stopped}});
    if (failed > 0)
        view["failed_step"] = failed;
    if (stopped)
        view.update({{"outcome_kind", "warn"},
                     // TRN %1% is the step the run stopped at, %2% how many steps the script has.
                     {"outcome_text", m_single ? _u8L("Stopped.") : format(_u8L("Stopped at step %1% of %2%"), failed, total)}});
    else if (!passed)
        view.update({{"outcome_kind", "fail"},
                     // TRN %1% is the step that failed, %2% how many steps the script has.
                     {"outcome_text", m_single ? _u8L("The step failed.") : format(_u8L("Failed at step %1% of %2%"), failed, total)}});
    else if (nested_failed)
        view.update({{"outcome_kind", "warn"}, {"outcome_text", _u8L("Finished, but a nested script failed.")}});
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

void ScriptRunnerDialog::on_close(wxCloseEvent& event)
{
    if (event.CanVeto()) {
        if (running()) {
            MessageDialog dialog(this, _L("A script is running. Stop it and close this window?"), _L("Run Script"), wxYES_NO | wxICON_QUESTION);
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
