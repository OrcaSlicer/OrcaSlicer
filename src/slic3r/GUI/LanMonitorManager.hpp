#pragma once

#include "slic3r/Utils/LanMonitorSnapshot.hpp"

#include <map>
#include <memory>
#include <string>
#include <wx/event.h>
#include <wx/string.h>
#include <wx/timer.h>

namespace Slic3r {
class MachineObject;
namespace GUI {

class GUI_App;

// All access and worker notifications run on the GUI thread. SDK state lives in child processes.
class LanMonitorManager : public wxEvtHandler
{
public:
    explicit LanMonitorManager(GUI_App& app);
    ~LanMonitorManager() override;
    void start();
    void stop();
    void sync();
    void retry(const std::string& id);
    const LanMonitorStatus* status(const std::string& id) const;

private:
    struct Session;
    void launch(const std::shared_ptr<Session>& session);
    GUI_App& m_app;
    wxTimer m_timer;
    std::map<std::string, std::shared_ptr<Session>> m_sessions;
};

wxString lan_monitor_status_text(const LanMonitorStatus* status);
wxString lan_monitor_progress_text(const LanMonitorStatus* status);

} // namespace GUI
} // namespace Slic3r
