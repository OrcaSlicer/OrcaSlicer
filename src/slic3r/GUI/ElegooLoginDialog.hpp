#pragma once

#include <wx/string.h>

#include "GUI_Utils.hpp"

class wxWebView;
class wxWebViewEvent;
class wxWindow;

namespace Slic3r { namespace GUI {

// Elegoo account sign-in. Hosts the Elegoo account site's slicer login page, which reports the
// tokens back over the WebView bridge ("report.userInfo"), and offers importing an ElegooSlicer
// sign-in instead.
class ElegooLoginDialog : public DPIDialog
{
public:
    explicit ElegooLoginDialog(wxWindow* parent);

    // Shows the dialog; true when an account is signed in afterwards.
    static bool sign_in(wxWindow* parent);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void on_script_message(wxWebViewEvent& evt);
    void respond(const wxString& id, const wxString& method, bool ok, const wxString& message);
    void load_page();

    wxWebView* m_browser{nullptr};
};

}} // namespace Slic3r::GUI
