#ifndef slic3r_WebViewDialog_hpp_
#define slic3r_WebViewDialog_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include <wx/setup.h>
#include <wx/string.h>
#include <wx/event.h>
#include <string>
#include <functional>
#include <wx/dialog.h>
#include <wx/accel.h>
#include <wx/webview.h>

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
#include <wx/sizer.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>
#include "Lazy.hpp"


namespace Slic3r {

class NetworkAgent;

namespace GUI {


class WebViewPanel : public wxPanel, public LazyInstance<WebViewPanel>
{
public:
    WebViewPanel(wxWindow *parent);
    virtual ~WebViewPanel();

    void load_url(wxString& url);

    void UpdateState();
    void OnIdle(wxIdleEvent& evt);
    void OnUrl(wxCommandEvent& evt);
    void OnBack(wxCommandEvent& evt);
    void OnForward(wxCommandEvent& evt);
    void OnStop(wxCommandEvent& evt);
    void OnReload(wxCommandEvent& evt);
    void OnNavigationRequest(wxWebViewEvent& evt);
    void OnNavigationComplete(wxWebViewEvent& evt);
    void OnDocumentLoaded(wxWebViewEvent& evt);
    void OnTitleChanged(wxWebViewEvent &evt);
    void OnNewWindow(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void OnScriptResponseMessage(wxCommandEvent& evt);
    void OnViewSourceRequest(wxCommandEvent& evt);
    void OnViewTextRequest(wxCommandEvent& evt);
    void OnToolsClicked(wxCommandEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnCut(wxCommandEvent& evt);
    void OnCopy(wxCommandEvent& evt);
    void OnPaste(wxCommandEvent& evt);
    void OnUndo(wxCommandEvent& evt);
    void OnRedo(wxCommandEvent& evt);
    void OnMode(wxCommandEvent& evt);
    void RunScript(const wxString& javascript);
    void OnRunScriptString(wxCommandEvent& evt);
    void OnRunScriptInteger(wxCommandEvent& evt);
    void OnRunScriptDouble(wxCommandEvent& evt);
    void OnRunScriptBool(wxCommandEvent& evt);
    void OnRunScriptObject(wxCommandEvent& evt);
    void OnRunScriptArray(wxCommandEvent& evt);
    void OnRunScriptDOM(wxCommandEvent& evt);
    void OnRunScriptUndefined(wxCommandEvent& evt);
    void OnRunScriptNull(wxCommandEvent& evt);
    void OnRunScriptDate(wxCommandEvent& evt);
    void OnRunScriptMessage(wxCommandEvent& evt);
    void OnRunScriptCustom(wxCommandEvent& evt);
    void OnAddUserScript(wxCommandEvent& evt);
    void OnSetCustomUserAgent(wxCommandEvent& evt);
    void OnClearSelection(wxCommandEvent& evt);
    void OnDeleteSelection(wxCommandEvent& evt);
    void OnSelectAll(wxCommandEvent& evt);
    void OnLoadScheme(wxCommandEvent& evt);
    void OnUseMemoryFS(wxCommandEvent& evt);
    void OnEnableContextMenu(wxCommandEvent& evt);
    void OnEnableDevTools(wxCommandEvent& evt);
    void OnClose(wxCloseEvent& evt);

    wxTimer * m_LoginUpdateTimer{nullptr};
    void OnFreshLoginStatus(wxTimerEvent &event);

public:
    void SendRecentList(int images);
    void SetLoginPanelVisibility(bool bshow);
    void SendDesignStaffpick(bool on);
    void OpenModelDetail(std::string id, NetworkAgent *agent);
    void SendLoginInfo();
    void ShowNetpluginTip();
    void SendCloudProvidersInfo();

    void get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback);
    int  get_model_mall_detail_url(std::string *url, std::string id);

    void update_mode();

    // Shows/hides the native OrcaCloud pane beside this homepage nav column. `url` is the
    // cloud Start URL ({web}/auth/handoff?next=…&user_id=…), which the page uses to decide
    // whether it already has a session; empty falls back to the plain cloud page.
    // `page_url` is a full {web} page URL: an already-open pane is navigated straight there,
    // without re-running the handoff, which is what an explicit "open this page" wants. Empty
    // keeps whatever page the pane holds.
    void set_cloud_mode(bool on, const wxString& url = wxString(), const wxString& page_url = wxString());

    // True while the OrcaCloud pane exists and is visible.
    bool cloud_mode_active() const { return m_cloud_browser && m_cloud_browser->IsShown(); }

    // Loads `url` in the cloud pane — the webview that owns the session handoff. Raw LoadURL (no
    // wxURI re-serialisation) so the Complete URL's #fragment stays intact; never logged.
    void load_cloud_url(const wxString& url);

    // Clears the cloud pane's own session on an explicit desktop sign-out. If the pane has not
    // been created yet, the sign-out is remembered and applied when it is first opened.
    void cloud_signed_out(const wxString& signout_url);

    bool Show(bool show = true) override;
private:
    void create_browser();
    void create_cloud_browser();
    void reset_browser();
    // Consumes the cloud pane's webview events so they stay off the homepage handlers.
    void OnCloudWebViewEvent(wxWebViewEvent& evt);
    // True when `url` has the same scheme/host/port as the cloud origin ({web}).
    bool is_cloud_origin(const wxString& url) const;
    // True when a navigation to `url` must leave the pane: an http(s) URL on another origin than
    // the (known) cloud origin. False for non-web schemes and while the origin is unknown.
    bool should_divert_cloud_url(const wxString& url) const;
    // Parses a request_web_session message and defers it to GUI_App.
    void on_cloud_script_message(const std::string& body);
    // Loads `url`, first clearing a lazily-required sign-out (see cloud_signed_out).
    void load_cloud_after_signout(const wxString& url);
    // First load of a freshly created pane: deferred so CreateWebView's queued "wx" handler add
    // runs first and the page sees window.wx at load time (guide §3).
    void schedule_cloud_load(const wxString& url);
    // Clears the pane's stored web data (its cookie-backed session) on an explicit sign-out.
    void clear_cloud_storage();
    // Re-applies the content sizer's proportions and min sizes for the cloud state.
    void apply_cloud_layout(bool cloud_on);

    wxWebView* m_browser;
    wxWebView* m_cloud_browser{nullptr};
    wxBoxSizer* m_content_sizer{nullptr};
    wxString m_home_url;
    bool m_reset_on_show{false};
    // {web} origin (scheme://host[:port]) of the loaded pane. Set from every load, including the
    // plain cloud fallback, so navigation policy can keep {web} pages in the pane and send other
    // sites to the system browser.
    wxString m_cloud_base_url;
    // Start URL the pane was last seeded with. An open whose URL matches this is an ordinary
    // re-open (nothing changed), so it only shows the pane instead of reloading it (guide §10).
    wxString m_cloud_seed_url;
    // Sign-out clear the pane must apply once it exists / next loads.
    wxString m_cloud_signout_url;
    wxString m_cloud_after_signout_url;
    bool     m_cloud_pending_signout{false};
    wxButton *  m_button_stop;
    wxTextCtrl *m_url;
#if !BBL_RELEASE_TO_PUBLIC
    // Created only by the internal-build toolbar in the constructor.
    wxBoxSizer *bSizer_toolbar;
    wxButton *  m_button_back;
    wxButton *  m_button_forward;
    wxButton *  m_button_reload;
    wxButton *  m_button_tools;
#endif //BBL_RELEASE_TO_PUBLIC

    wxMenu* m_tools_menu;
    wxMenuItem* m_tools_handle_navigation;
    wxMenuItem* m_tools_handle_new_window;
    wxMenuItem* m_edit_cut;
    wxMenuItem* m_edit_copy;
    wxMenuItem* m_edit_paste;
    wxMenuItem* m_edit_undo;
    wxMenuItem* m_edit_redo;
    wxMenuItem* m_edit_mode;
    wxMenuItem* m_script_string;
    wxMenuItem* m_script_integer;
    wxMenuItem* m_script_double;
    wxMenuItem* m_script_bool;
    wxMenuItem* m_script_object;
    wxMenuItem* m_script_array;
    wxMenuItem* m_script_dom;
    wxMenuItem* m_script_undefined;
    wxMenuItem* m_script_null;
    wxMenuItem* m_script_date;
    wxMenuItem* m_script_message;
    wxMenuItem* m_script_custom;
    wxMenuItem* m_selection_clear;
    wxMenuItem* m_selection_delete;
    wxMenuItem* m_context_menu;
    wxMenuItem* m_dev_tools;

    wxInfoBar *m_info;

    long m_zoomFactor;

    // Last executed JavaScript snippet, for convenience.
    wxString m_javascript;
    wxString m_response_js;

    DECLARE_EVENT_TABLE()
};

class SourceViewDialog : public wxDialog
{
public:
    SourceViewDialog(wxWindow* parent, wxString source);
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
