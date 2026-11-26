#pragma once

#include <nlohmann/json.hpp>
#include <wx/timer.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class AgentActivityPane;

// The Agent Activity pane: a dock pane on the right of the 3D view (it can float, and floated it stays up on every tab)
// whose page (resources/web/dialog/AgentActivity) lists, live, what automation does in the app while the pane is open:
// the automation runtime's activity log (Mcp::ActivityLog), which records while the person has the pane open, with each
// call's title and target worded as the recording captions word them. It opens only when the person opens it, from View
// > Agent Activity or its Speed Dial action, and it only watches. One per app, owned by GUI_App, so it outlives the
// pane, which a main-window rebuild (a language change) destroys and the panel builds again. The pane and everything in
// it are hidden from the automation tools. UI thread.
class AgentActivityPanel
{
public:
    AgentActivityPanel();
    ~AgentActivityPanel();
    AgentActivityPanel(const AgentActivityPanel&)            = delete;
    AgentActivityPanel& operator=(const AgentActivityPanel&) = delete;

    // Hides the pane when it is on screen, else shows it, as View > Agent Activity and the Speed Dial do.
    void toggle();
    // Builds the pane on first use and shows it, selecting Prepare when the pane is docked and the current tab is
    // neither Prepare nor Preview.
    void show();
    // The pane's window is shown on screen.
    bool on_screen() const;

    // MainFrame::shutdown(), before the dock panes go: the pane goes.
    void main_window_closing();
    // The end of GUI_App::recreate_GUI(): the pane comes back if it was on screen, once the old main window is deleted.
    void main_window_rebuilt();

private:
    friend class AgentActivityPane;

    void pane_destroyed(AgentActivityPane* pane);
    // The pane is gone (or going with the main window): the page with it.
    void forget_pane();
    // The person has the pane open or not: the activity log records while they do.
    void set_wanted(bool wanted);
    void handle(const nlohmann::json& payload);
    // The log changed: the page gets the changes at most ten times a second, and only while the pane is on screen.
    void changed();
    void refresh();
    // A log entry as the page shows it: its title, target, source and effects in the UI's language, a screenshot's
    // thumbnail, and no arguments.
    nlohmann::json page_entry(const nlohmann::json& entry);

    std::shared_ptr<std::atomic<bool>>    m_alive;
    AgentActivityPane*                    m_pane   = nullptr;
    bool                                  m_wanted = false;
    // The main window was rebuilt and the pane is wanted back: built at the next idle.
    bool                                  m_rebuild_pending = false;
    // The page has loaded and holds the log as of this version (0: nothing yet).
    bool                                  m_page_ready = false;
    uint64_t                              m_version    = 0;
    wxTimer                               m_timer;
    std::chrono::steady_clock::time_point m_last_refresh{};
    // A finished entry's files that exist, from its result (Mcp::result_files()), which the page opens by their place in
    // the list; and a screenshot entry's thumbnail. Each made once.
    std::map<uint64_t, std::vector<std::string>> m_files;
    std::map<uint64_t, std::string>              m_thumbs;
};

}} // namespace Slic3r::GUI
