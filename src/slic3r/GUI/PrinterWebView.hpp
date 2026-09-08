#ifndef slic3r_PrinterWebView_hpp_
#define slic3r_PrinterWebView_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include <wx/webview.h>
#include <wx/string.h>

#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>
#include <memory>


namespace Slic3r {
namespace GUI {

class PrinterWebViewHandler;


class PrinterWebView : public wxPanel {
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    void load_url(wxString& url, wxString apikey = "");
    void UpdateState();
    void OnClose(wxCloseEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void OnNewWindow(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void reload();
    void update_mode();

    bool Show(bool show = true) override;

private:
    friend class PrinterWebViewHandler;

    void SendAPIKey();
    // Create/configure the underlying wxWebView (m_browser) and bind its events.
    void create_browser();
    // Tear down and recreate m_browser from scratch. Used to recover from a
    // wedged WebView2 backend after a recreate_GUI (language switch), where a
    // control created while the previous frame's active webview is still alive
    // silently ignores all navigations.
    void reset_browser();

    wxWebView* m_browser;
    long m_zoomFactor;
    wxString m_apikey;
    bool m_apikey_sent;
    wxString m_url_deferred;
    std::unique_ptr<PrinterWebViewHandler> m_handler;
    // When this view is constructed during a GUI rebuild, its WebView2 backend
    // may come up wedged. Recreate it the first time the view is actually shown
    // (by then the old frame is gone and creation is clean).
    bool m_reset_on_show{false};

    // DECLARE_EVENT_TABLE()
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
