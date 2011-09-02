#ifndef slic3r_GUI_PrinterCloudAuthDialog_hpp_
#define slic3r_GUI_PrinterCloudAuthDialog_hpp_

#include <wx/wx.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/dialog.h>
#include "wx/webview.h"

#if wxUSE_WEBVIEW_IE
#include "wx/msw/webview_ie.h"
#endif
#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "GUI_Utils.hpp"
#include "PrintHost.hpp"

namespace Slic3r { namespace GUI {

class PrinterCloudAuthDialog : public wxDialog
{
protected:
    wxWebView* m_browser;
    wxString   m_TargetUrl;

    wxString    m_javascript;
    wxString    m_response_js;
    std::string m_apikey;

public:
    PrinterCloudAuthDialog(wxWindow* parent, PrintHost* host);
    ~PrinterCloudAuthDialog();

    std::string GetApiKey() { return m_apikey; };

    void OnNavigationRequest(wxWebViewEvent& evt);
    void OnNavigationComplete(wxWebViewEvent& evt);
    void OnDocumentLoaded(wxWebViewEvent& evt);
    void OnNewWindow(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    
};

}} // namespace Slic3r::GUI

#endif