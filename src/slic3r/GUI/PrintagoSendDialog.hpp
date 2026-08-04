#ifndef slic3r_GUI_PrintagoSendDialog_hpp_
#define slic3r_GUI_PrintagoSendDialog_hpp_

#include <memory>
#include <string>

#include <wx/dialog.h>
#include <wx/webview.h>

#include <nlohmann/json.hpp>

#include "PrintagoBridge.hpp"

namespace Slic3r { namespace GUI {

// Description of the exported project 3MF, handed to the embedded page as project:info and used as
// the source for the native upload.
struct PrintagoProject
{
    std::string        file_path;             // absolute path to the temp 3MF to upload
    std::string        file_name;             // basename shown to the user, e.g. "model.3mf"
    std::string        project_name;          // e.g. "My Model"
    unsigned long long file_size_bytes = 0;   // size of the exported 3MF
    int                plate_count     = 0;   // number of plates in the project
};

// Modal dialog that hosts Printago's "Save to Printago" / "Save & Queue to Printago" embedded page in
// a wxWebView. It implements the host side of the Printago embedded-pages protocol (v1) via a small
// JSON message bridge (see PrintagoBridge and designs/orca-embedded-pages-protocol.md). The page
// authenticates itself, maps materials, and queues builds; the host answers page:ready with host:init
// + project:info and performs the native 3MF upload on upload:begin.
class PrintagoSendDialog : public wxDialog
{
public:
    PrintagoSendDialog(wxWindow* parent, const PrintagoProject& project, const wxString& url);
    ~PrintagoSendDialog() override;

private:
    void on_script_message(wxWebViewEvent& evt);
    void on_new_window(wxWebViewEvent& evt);

    void handle_page_ready(); // (re)send host:init + project:info + theme (stateless re-init rule)

    PrintagoProject                 m_project;
    wxString                        m_url;
    wxWebView*                      m_browser      = nullptr;
    bool                            m_load_retried = false; // reload the page once if the load errors
    std::unique_ptr<PrintagoBridge> m_bridge; // host<->page delivery + the upload leg
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PrintagoSendDialog_hpp_
