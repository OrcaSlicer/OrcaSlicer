#ifndef slic3r_GUI_PrintagoTabBridge_hpp_
#define slic3r_GUI_PrintagoTabBridge_hpp_

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <wx/webview.h>
#include <nlohmann/json.hpp>

#include "PrintagoBridge.hpp"

namespace Slic3r { namespace GUI {

class Plater;

// Host side of the "Edit in Orca Slicer" round trip for the persistent Printago app tab. Installs the
// printago bridge on the tab's webview, answers page:ready with host:init (no project:info), and
// handles part:edit (download the part's 3MF, open it as the active project, remember partId,
// reply part:edit-opened). The save-back leg is NOT here: per the spec it is a dialog webview at
// /orca/replace?partId=<id> (see GUI_App::printago_request_replace / PrintagoSendDialog).
// See designs/orca-embedded-pages-protocol.md, "App tab: Edit in Orca Slicer".
class PrintagoTabBridge
{
public:
    PrintagoTabBridge(wxWebView* webview, Plater* plater);
    ~PrintagoTabBridge();

    // Called by PrinterWebView when a message arrives on the "printago" channel.
    void on_message(const wxString& json_str);

    // partId of the active edit session if the current project's file is a downloaded part; empty
    // otherwise. Used to relabel "Save to Printago" -> "Replace in Printago" while editing and to
    // build the /orca/replace?partId= URL.
    std::string active_edit_part() const;

    // Process-wide accessor to the currently-installed tab bridge (at most one Printago tab exists).
    static PrintagoTabBridge* active() { return s_active; }

private:
    void        handle_page_ready();
    void        handle_part_edit(const nlohmann::json& payload);
    void        finish_edit_open(const std::string& part_id, const std::string& temp_path,
                                 const std::string& file_name, bool downloaded_ok);
    void        handle_file_open(const nlohmann::json& payload); // "Preview in Orca" (view-only gcode)
    // Download a signed URL (no auth headers) to temp_path on a worker thread, then invoke on_gui(ok)
    // on the GUI thread (guarded by m_alive). Shared by part:edit and file:open.
    void        download_to_temp(const std::string& url, const std::string& temp_path,
                                 std::function<void(bool ok)> on_gui);
    void        notify(const std::string& text) const;
    std::string current_project_path() const;

    struct EditPart
    {
        std::string temp_path;
        std::string file_name;
    };

    PrintagoBridge                    m_bridge;
    Plater*                           m_plater = nullptr;
    std::map<std::string, EditPart>   m_edit_parts; // partId -> downloaded temp 3MF
    std::shared_ptr<std::atomic_bool> m_alive;      // guards download worker-thread callbacks

    static PrintagoTabBridge* s_active;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PrintagoTabBridge_hpp_
