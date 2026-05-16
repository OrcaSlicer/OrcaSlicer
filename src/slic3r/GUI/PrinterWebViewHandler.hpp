#ifndef slic3r_PrinterWebViewHandler_hpp_
#define slic3r_PrinterWebViewHandler_hpp_

#include <memory>
#include <wx/webview.h>
#include <wx/string.h>

class wxWebView;

namespace Slic3r {
namespace GUI {

class PrinterWebView;

class PrinterWebViewHandler {
public:
    explicit PrinterWebViewHandler(PrinterWebView& owner);
    virtual ~PrinterWebViewHandler();

    virtual void on_loaded(wxWebViewEvent &evt);
    virtual void on_script_message(wxWebViewEvent &evt);

protected:
    PrinterWebView& owner() const;
    wxWebView*      browser() const;

private:
    PrinterWebView& m_owner;
};

std::unique_ptr<PrinterWebViewHandler> create_printer_webview_handler(PrinterWebView& owner);

} // GUI
} // Slic3r

#endif /* slic3r_PrinterWebViewHandler_hpp_ */