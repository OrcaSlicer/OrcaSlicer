#include "TerminalDialog.hpp"
#include "ProcessRunner.hpp"
#include "I18N.hpp"

#include "slic3r/plugin/PythonInterpreter.hpp"

#include <wx/sizer.h>

#include <cctype>

namespace Slic3r { namespace GUI {

namespace {

bool consume_command_prefix(const std::string& command, const std::string& prefix, std::string& remainder)
{
    size_t pos = 0;
    while (pos < command.size() && std::isspace(static_cast<unsigned char>(command[pos])))
        ++pos;

    if (command.compare(pos, prefix.size(), prefix) != 0)
        return false;

    pos += prefix.size();
    if (pos < command.size() && !std::isspace(static_cast<unsigned char>(command[pos])))
        return false;

    while (pos < command.size() && std::isspace(static_cast<unsigned char>(command[pos])))
        ++pos;

    remainder = command.substr(pos);
    return true;
}

std::string quote_command_line_arg(const std::string& value)
{
    std::string quoted = "\"";
    for (const char ch : value) {
        if (ch == '"')
            quoted += "\\\"";
        else
            quoted += ch;
    }
    quoted += "\"";
    return quoted;
}

} // namespace

TerminalDialog::TerminalDialog(wxWindow* parent, wxWindowID id, const wxString& title,
                               const wxPoint& pos, const wxSize& size, long style)
    : WebViewHostDialog(parent, id, title, pos, size, style)
{
    m_runner = std::make_unique<ProcessRunner>();

    create_webview("web/dialog/TerminalDialog/index.html", _L("Terminal"),
                   wxSize(820, 600), wxSize(640, 480));
}

TerminalDialog::~TerminalDialog()
{
    if (m_runner && m_runner->is_running())
        m_runner->terminate();
}

void TerminalDialog::on_script_message(const nlohmann::json& payload)
{
    const std::string command = payload.value("command", "");

    if (command == "run_command") {
        std::string cmd = payload.value("cmd", "");
        if (!cmd.empty())
            resolve_and_run(cmd);
    }
    else if (command == "write_stdin") {
        std::string data = payload.value("data", "");
        if (!data.empty() && m_runner && m_runner->is_running())
            m_runner->write_stdin(data);
    }
    else {
        handle_common_script_command(payload);
    }
}

void TerminalDialog::resolve_and_run(const std::string& cmd)
{
    if (m_runner->is_running()) {
        nlohmann::json err;
        err["command"]    = "process_error";
        err["message"]    = "A command is already running.";
        call_web_handler(err);
        return;
    }

    std::string executable;
    std::string arg_string;

    auto send_process_error = [this](const std::string& message) {
        nlohmann::json err;
        err["command"] = "process_error";
        err["message"] = message;
        call_web_handler(err);
    };

    // Parse command: must start with "python" or "uv"
    if (consume_command_prefix(cmd, "python", arg_string)) {
        const std::string python_path = PythonInterpreter::bundled_python_executable();
        if (python_path.empty()) {
            send_process_error("Bundled Python executable not found.");
            return;
        }
        executable = python_path;
    }
    else if (consume_command_prefix(cmd, "uv", arg_string)) {
        const std::string uv_path = PythonInterpreter::bundled_uv_path();
        if (uv_path.empty()) {
            send_process_error("uv executable not found.");
            return;
        }
        executable = uv_path;
    }
    else {
        send_process_error("Only 'python' and 'uv' commands are supported.");
        return;
    }

    std::string command_line = quote_command_line_arg(executable);
    if (!arg_string.empty()) {
        command_line += " ";
        command_line += arg_string;
    }

    bool started = m_runner->run_command_line_async(
        command_line,
        [this](const std::vector<ProcessRunner::OutputLine>& lines) {
            nlohmann::json msg;
            msg["command"] = "output";
            nlohmann::json lines_arr = nlohmann::json::array();
            for (const auto& line : lines) {
                nlohmann::json l;
                l["text"]      = line.text;
                l["is_stderr"] = line.is_stderr;
                lines_arr.push_back(std::move(l));
            }
            msg["lines"] = std::move(lines_arr);
            call_web_handler(msg);
        },
        [this](int exit_code) {
            nlohmann::json msg;
            msg["command"]   = "process_done";
            msg["exit_code"] = exit_code;
            call_web_handler(msg);
        });

    if (!started) {
        nlohmann::json err;
        err["command"] = "process_error";
        err["message"] = "Failed to start process.";
        call_web_handler(err);
    }
}

}} // namespace Slic3r::GUI
