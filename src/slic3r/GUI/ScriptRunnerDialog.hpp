#pragma once

#include "Widgets/WebViewHostDialog.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <wx/event.h>
#include <wx/timer.h>

class wxCloseEvent;
class wxWindow;

namespace Slic3r { namespace GUI {

namespace Mcp {
class ScriptRun;
struct Script;
struct ScriptContext;
} // namespace Mcp

// The Run Script dialog: a person opens or pastes an MCP script (orca_script), reads its steps and
// runs it, whole or one step at a time, watching each step's outcome. The page
// (resources/web/dialog/ScriptRunner) shows; this host parses, describes, words and runs, through the
// MCP service's script runner without opening its endpoint. Modeless, one at a time
// (MainFrame::show_script_runner), and invisible to the MCP GUI tools, so a script can't act on it.
class ScriptRunnerDialog : public WebViewHostDialog
{
public:
    explicit ScriptRunnerDialog(wxWindow* parent);
    ~ScriptRunnerDialog() override;

    bool running() const;

private:
    void           on_script_message(const nlohmann::json& payload) override;
    nlohmann::json localized_strings() const override;

    void handle(const nlohmann::json& payload);
    void send_service_state();
    void open_file();
    void paste();
    void parse(const nlohmann::json& payload);
    void run(const nlohmann::json& payload);
    void refuse(const std::string& message, const std::string& detail = {});
    void on_close(wxCloseEvent& event);

    // The context a run of the current source gets: its folders and, for a single step, the jobs
    // earlier runs saved.
    Mcp::ScriptContext context() const;
    boost::filesystem::path out_dir() const;
    nlohmann::json          describe(const Mcp::Script& script) const;
    nlohmann::json          step_view(const nlohmann::json& entry) const;
    nlohmann::json          finished_view(const nlohmann::json& result) const;

    // False once destroyed: page commands are handled later, from the event loop, and must not reach
    // a dialog that is gone by then.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    wxTimer                            m_ready_timer;

    // The source: an opened file (empty for pasted text) and the output folder its runs write to.
    boost::filesystem::path         m_file;
    mutable boost::filesystem::path m_out_dir;
    nlohmann::json                  m_jobs = nlohmann::json::object(); // jobs earlier runs saved

    std::shared_ptr<Mcp::ScriptRun>       m_run;
    std::optional<std::size_t>            m_single;
    std::size_t                           m_total = 0;
    std::chrono::steady_clock::time_point m_started;
};

}} // namespace Slic3r::GUI
