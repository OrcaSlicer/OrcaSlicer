#ifndef slic3r_GUI_Widgets_WebViewHostDialog_hpp_
#define slic3r_GUI_Widgets_WebViewHostDialog_hpp_

#include <nlohmann/json.hpp>
#include <slic3r/GUI/GUI_Utils.hpp>

#include <exception>
#include <string>

#include <wx/string.h>
#include <wx/webview.h>

namespace Slic3r { namespace GUI {

// Shared shell for local HTML dialogs that communicate through window.wx.postMessage().
class WebViewHostDialog : public Slic3r::GUI::DPIDialog
{
public:
    WebViewHostDialog(wxWindow* parent,
                      wxWindowID id         = wxID_ANY,
                      const wxString& title = wxT(""),
                      const wxPoint& pos    = wxDefaultPosition,
                      const wxSize& size    = wxDefaultSize,
                      // wxRESIZE_BORDER is required for a resizable frame on MSW/GTK; macOS derives
                      // one from wxMAXIMIZE_BOX alone, which is why these dialogs used to resize only there.
                      long style            = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER);
    ~WebViewHostDialog() override = default;

    bool create_webview(const std::string& resource_path,
                        const wxString& title,
                        const wxSize& dialog_size = wxSize(820, 660),
                        const wxSize& min_size    = wxSize(640, 640));

    void load_url(const wxString& url);
    bool run_script(const wxString& script);
    void call_web_handler(const nlohmann::json& payload, const wxString& handler = wxT("HandleStudio"));

    // Wraps `markup` (an HTML fragment, usually a <style> block) in a document-start user
    // script that inserts it once — guarded by element id `dom_id`, at `position` (an
    // insertAdjacentHTML target such as "afterbegin"/"beforeend") — retrying via a
    // MutationObserver until a root node exists. On WebView2 a document-start script can run
    // before <html> exists (document.head and document.documentElement both null), so a bare
    // insert would throw and silently never apply. `prelude` is emitted once before the
    // injector (extra var/flag declarations); `on_inject` runs inside inject() after each
    // successful insert. Both default to empty.
    static std::string document_start_injector(const std::string& markup,
                                               const char*        dom_id,
                                               const char*        position,
                                               const std::string& prelude   = {},
                                               const std::string& on_inject = {});

protected:
    wxWebView* browser() const { return m_browser; }

    wxString build_resource_url(const std::string& resource_path) const;
    bool handle_common_script_command(const nlohmann::json& payload, int close_return_code = wxID_CANCEL);

    void on_dpi_changed(const wxRect& suggested_rect) override;

    virtual void on_script_message(const nlohmann::json& payload) = 0;
    virtual void on_script_message_parse_error(const wxString& payload, const std::exception& error);
    virtual bool append_language_to_url() const { return true; }

    // Registers all document-start user scripts: the shared host theme contract first,
    // then subclass scripts from add_user_scripts(). Called ONCE, at creation. Live
    // re-theme goes through apply_theme_live() (RunScript), not a re-registration —
    // calling this again would append duplicate scripts.
    void register_theme_user_scripts();

    // Subclasses override to add page-specific document-start user scripts (e.g. the
    // plugin bridge / unstyled-content defaults). Called AFTER the theme contract is
    // added, by register_theme_user_scripts(). Default: none.
    virtual void add_user_scripts() {}

    // Pushes the current app theme into the already-loaded document without a reload
    // (updates the injected :root variables and the data-orca-theme attribute).
    void apply_theme_live();

private:
    void on_script_message_event(wxWebViewEvent& event);
    void on_webview_recreated(wxCommandEvent& event);

    wxWebView* m_browser{nullptr};
};

}} // namespace Slic3r::GUI

#endif
