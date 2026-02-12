#ifndef slic3r_WebViewDialog_hpp_
#define slic3r_WebViewDialog_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include "wx/webview.h"

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
#include <wx/bmpbuttn.h>
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>


namespace Slic3r {

class NetworkAgent;

namespace GUI {


class WebViewPanel : public wxPanel
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
    void OnOnlineBack(wxCommandEvent& evt);
    void OnOnlineReload(wxCommandEvent& evt);
    void OnOpenInBrowser(wxCommandEvent& evt);
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
    void RunScriptLeft(const wxString &javascript);
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
    void ResetWholePage();

    // Login
    void SendLoginInfo();
    void ShowNetpluginTip();

    //MW
    void SetMakerworldModelID(std::string ModelID);
    void OpenMakerworldSearchPage(std::string KeyWord);
    void SetPrintHistoryTaskID(int TaskID);
    
    //DisconnectPage
    wxString MakeDisconnectUrl(std::string MenuName);

    //LeftMenu
    std::string m_contentname; // CurrentMenu
    bool        m_leftfirst;   // Left First Loaded
    void CheckMenuNewTag();
    void ShowMenuNewTag(std::string menuname, std::string show);
    void SetLeftMenuShow(std::string menuname, int show);
    void SetLeftMenuWidth(int nWidth);
    void        SwitchWebContent(std::string modelname, int refresh = 0);
    void        SwitchLeftMenu(std::string strMenu);

    //Recent File
    void SendRecentList(int images);

    //Online
    bool     m_onlinefirst;    // Online Page First Load
    wxString m_online_type;    // recommend & browse
    wxString m_online_LastUrl; // PageLastError Url
    std::string m_online_last_url; //last url

    void SendDesignStaffpick(bool on);
    void get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback);
    void get_user_mw_4u_config(std::function<void(std::string)> callback);
    void get_4u_staffpick(int seed, int limit, std::function<void(std::string)> callback);
    void OpenModelDetail(std::string id, NetworkAgent *agent);
    void UpdateMakerworldLoginStatus();
    void SetMakerworldPageLoginStatus(bool login, wxString ticket = "");
    void get_wiki_search_result(std::string keyword);
    void get_academy_list();

    //Makerlab
    bool     m_MakerLabFirst;
    wxString m_MakerLab_LastUrl;
    void SendMakerlabList();
    void get_makerlab_list(std::function<void(std::string)> callback);
    void     SetMakerlabUrl(std::string url);
    void     OpenOneMakerlab(std::string url);
    void     OpenMakerlab3mf(std::string Base64Buf, std::string FileName);
    bool     SaveBase64ToLocal(std::string Base64Buf, std::string FileName,std::string FileTail, wxString &download_path, wxString &download_file);
    void     SaveMakerlabStl(int SequenceID,std::string Base64Buf, std::string FileName);
    void     UpdateMakerlabStatus();

    //wiki
    bool m_WikiFirst;
    wxString m_Wiki_LastUrl;

    //Common UI
    void SetWebviewShow(wxString name, bool show);
    void SetOnlineToolbarVisible(bool visible);
    void UpdateOnlineToolbarState();
    std::string GetStudioLanguage();

    //PrintHistory
    std::string m_TaskInfo;
    bool        m_printhistoryfirst; // print history first load
    wxString    m_print_history_LastUrl;
    void ShowUserPrintTask(bool bShow, bool bForce=false);

    //
    bool GetJumpUrl(bool login, wxString ticket, wxString targeturl, wxString &finalurl);

    void update_mode();
private:
    std::string m_Region;

    wxBoxSizer *topsizer { nullptr };

    int         m_loginstatus;
    bool m_isPerformingBack = false;
    bool m_online_history_cleared { false };
    bool m_makerlab_history_cleared { false };
    wxBoxSizer* m_home_web { nullptr };
    wxWebView* m_browser { nullptr };
    wxWebView* m_browserLeft { nullptr };
    wxWebView * m_browserMW { nullptr };
    wxWebView  *m_browserPH { nullptr };               //PrintHistory
    wxWebView  *m_browserML { nullptr };               //MakerLab
    wxWebView  *m_browserWiki { nullptr };

    //Basic Browser
    wxBoxSizer *bSizer_toolbar { nullptr };
    wxButton *  m_button_back { nullptr };
    wxButton *  m_button_forward { nullptr };
    wxButton *  m_button_stop { nullptr };
    wxButton *  m_button_reload { nullptr };
    wxTextCtrl *m_url { nullptr };
    wxButton *  m_button_tools { nullptr };

    // Online page toolbar
    wxPanel *       m_online_toolbar_panel { nullptr };
    wxBoxSizer *    m_online_toolbar_sizer { nullptr };
    wxBitmapButton *m_online_back_btn { nullptr };
    wxBitmapButton *m_online_refresh_btn { nullptr };
    wxBitmapButton *m_online_open_browser_btn { nullptr };
    wxPanel *       m_online_container { nullptr };
    wxBoxSizer *    m_online_container_sizer { nullptr };
    int             m_online_toolbar_icon_px { 16 };

    wxMenu* m_tools_menu;
    wxMenuItem* m_tools_handle_navigation;
    wxMenuItem* m_tools_handle_new_window;
    wxMenuItem* m_edit_cut;
    wxMenuItem* m_edit_copy;
    wxMenuItem* m_edit_paste;
    wxMenuItem* m_edit_undo;
    wxMenuItem* m_edit_redo;
    wxMenuItem* m_edit_mode;
    wxMenuItem* m_scroll_line_up;
    wxMenuItem* m_scroll_line_down;
    wxMenuItem* m_scroll_page_up;
    wxMenuItem* m_scroll_page_down;
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
    wxStaticText* m_info_text;

    long m_zoomFactor;

    // Last executed JavaScript snippet, for convenience.
    wxString m_javascript;
    wxString m_response_js;

    bool m_has_pending_staff_pick { false };

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
