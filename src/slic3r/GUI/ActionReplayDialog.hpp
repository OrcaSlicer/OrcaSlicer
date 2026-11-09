#pragma once

#include "CodeEditorWindow.hpp"
#include "Widgets/WebViewHostDialog.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <wx/event.h>
#include <wx/filename.h>
#include <wx/timer.h>

class wxCloseEvent;
class wxWebViewEvent;
class wxWindow;

namespace Slic3r { namespace GUI {

namespace Mcp {
class PersonRun;
struct Replay;
struct ReplayContext;
} // namespace Mcp

// The Action Replay window: a person opens or writes a replay file (an orca_replay, the JSON list of tool calls an
// agent session exports), saves it, reads its steps and replays it, whole or one step at a time, watching each step's
// outcome. The page (resources/web/dialog/ActionReplay) shows; this host parses, describes, words and runs, through the
// MCP service's replay runner without opening its endpoint. A Python script opened or dropped here is offered to the Run
// Script panel instead. Modeless, one at a time (MainFrame::show_action_replay), and invisible to the MCP GUI tools, so
// a replay can't act on it.
class ActionReplayDialog : public WebViewHostDialog, public CodeEditorWindow
{
public:
    explicit ActionReplayDialog(wxWindow* parent);
    ~ActionReplayDialog() override;

    bool running() const;

    // Reads a replay file into the editor. Callers ask about unsaved changes first.
    void load_file(const boost::filesystem::path& path);

    bool                         owns_focus(const wxWindow* focus) const override;
    void                         forward_action(const std::string& action) override;
    wxWebView*                   web_view() const override { return browser(); }
    const std::set<std::string>& editor_keys() const override { return m_editor_keys; }

private:
    void           on_script_message(const nlohmann::json& payload) override;
    nlohmann::json localized_strings() const override;

    void handle(const nlohmann::json& payload);
    void send_service_state();
    void open_file();
    void on_navigating(wxWebViewEvent& event);
    void dropped(const boost::filesystem::path& path);
    // A Python script was picked or dropped: offers to open it in the Run Script panel.
    void offer_run_script(const boost::filesystem::path& path);
    bool save(bool pick);
    bool confirm_unsaved();
    void parse(const nlohmann::json& payload);
    void run(const nlohmann::json& payload);
    void refuse(const std::string& message, const std::string& detail = {});
    void on_close(wxCloseEvent& event);

    // The context a run of the current source gets: its folders and, for a single step, the jobs
    // earlier runs saved.
    Mcp::ReplayContext context() const;
    boost::filesystem::path out_dir() const;
    nlohmann::json          describe(const Mcp::Replay& replay) const;
    nlohmann::json          step_view(const nlohmann::json& entry) const;
    nlohmann::json          finished_view(const nlohmann::json& result) const;

    // False once destroyed: page commands are handled later, from the event loop, and must not reach
    // a dialog that is gone by then.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    wxTimer                            m_ready_timer;
    // The page's file, which its own loads navigate to; a navigation to any other file is a drop.
    wxFileName                         m_page;
    // The chords the page's editor binds, as it reported them.
    std::set<std::string>              m_editor_keys;

    // The source: its text as the page last reported it, whether that differs from what was opened or
    // saved last (for a new replay, from the draft it started as), the file it was opened from or saved
    // to (empty for a new replay), and the output folder a new replay's runs write to.
    std::string                     m_text;
    bool                            m_edited = false;
    boost::filesystem::path         m_file;
    mutable boost::filesystem::path m_draft_out_dir;
    nlohmann::json                  m_jobs = nlohmann::json::object(); // jobs earlier runs saved

    std::shared_ptr<Mcp::PersonRun>       m_run;
    std::optional<std::size_t>            m_single;
    std::size_t                           m_total = 0;
    std::chrono::steady_clock::time_point m_started;
};

}} // namespace Slic3r::GUI
