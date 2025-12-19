#ifndef slic3r_GUI_ReleaseNote_hpp_
#define slic3r_GUI_ReleaseNote_hpp_

#include <wx/wx.h>
#include <wx/intl.h>
#include <wx/collpane.h>
#include <wx/dataview.h>
#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/dataview.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/hyperlink.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/popupwin.h>
#include <wx/spinctrl.h>
#include <wx/artprov.h>
#include <wx/wrapsizer.h>
#include <wx/event.h>
#include <wx/hyperlink.h>
#include <wx/richtext/richtextctrl.h>

#include "AmsMappingPopup.hpp"
#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "DeviceManager.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/ScrolledWindow.hpp"
#include <wx/hashmap.h>
#include <wx/webview.h>

#include "Jobs/Worker.hpp"

namespace Slic3r { namespace GUI {

wxDECLARE_EVENT(EVT_SECONDARY_CHECK_CONFIRM, wxCommandEvent);
wxDECLARE_EVENT(EVT_SECONDARY_CHECK_CANCEL, wxCommandEvent);
wxDECLARE_EVENT(EVT_SECONDARY_CHECK_RETRY, wxCommandEvent);
wxDECLARE_EVENT(EVT_SECONDARY_CHECK_DONE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SECONDARY_CHECK_RESUME, wxCommandEvent);
wxDECLARE_EVENT(EVT_UPDATE_NOZZLE, wxCommandEvent);
wxDECLARE_EVENT(EVT_UPDATE_TEXT_MSG, wxCommandEvent);
wxDECLARE_EVENT(EVT_ERROR_DIALOG_BTN_CLICKED, wxCommandEvent);

class ReleaseNoteDialog : public DPIDialog
{
public:
    ReleaseNoteDialog(Plater *plater = nullptr);
    ~ReleaseNoteDialog();

    void on_dpi_changed(const wxRect &suggested_rect) override;
    void update_release_note(wxString release_note, std::string version);

    Label *    m_text_up_info{nullptr};
    wxScrolledWindow *m_vebview_release_note {nullptr};
};

class UpdatePluginDialog : public DPIDialog
{
public:
    UpdatePluginDialog(wxWindow* parent = nullptr);
    ~UpdatePluginDialog();

    void on_dpi_changed(const wxRect& suggested_rect) override;
    void update_info(std::string json_path);

    Label* m_text_up_info{ nullptr };
    Label* operation_tips{ nullptr };
    wxScrolledWindow* m_vebview_release_note{ nullptr };
};

class UpdateVersionDialog : public DPIDialog
{
public:
    UpdateVersionDialog(wxWindow *parent = nullptr);
    ~UpdateVersionDialog();

    wxWebView* CreateTipView(wxWindow* parent);
    void OnLoaded(wxWebViewEvent& event);
    void OnTitleChanged(wxWebViewEvent& event);
    void OnError(wxWebViewEvent& event);
    bool ShowReleaseNote(std::string content);
    void RunScript(std::string script);
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void update_version_info(wxString release_note, wxString version);
    std::vector<std::string> splitWithStl(std::string str, std::string pattern);

    wxStaticBitmap*   m_brand{nullptr};
    Label *           m_text_up_info{nullptr};
    wxWebView*        m_vebview_release_note{nullptr};
    wxSimplebook*     m_simplebook_release_note{nullptr};
    wxScrolledWindow* m_scrollwindows_release_note{nullptr};
    wxBoxSizer *      sizer_text_release_note{nullptr};
    Label *           m_staticText_release_note{nullptr};
    wxStaticBitmap*   m_bitmap_open_in_browser;
    Button*           m_button_skip_version;
    CheckBox*         m_cb_stable_only;
    Button*           m_button_download;
    Button*           m_button_cancel;
    std::string       url_line;
};

class SecondaryCheckDialog : public DPIFrame
{
private:
    wxWindow* event_parent { nullptr };
public:
    enum VisibleButtons { // ORCA VisibleButtons instead ButtonStyle 
        ONLY_CONFIRM        = 0,
        CONFIRM_AND_CANCEL  = 1,
        CONFIRM_AND_DONE    = 2,
        CONFIRM_AND_RETRY   = 3,
        CONFIRM_AND_RESUME  = 4,
        DONE_AND_RETRY      = 5,
        MAX_STYLE_NUM       = 6
    };
    SecondaryCheckDialog(
        wxWindow* parent,
        wxWindowID      id = wxID_ANY,
        const wxString& title = wxEmptyString,
        enum VisibleButtons btn_style = CONFIRM_AND_CANCEL, // ORCA VisibleButtons instead ButtonStyle 
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long            style = wxCLOSE_BOX | wxCAPTION,
        bool not_show_again_check = false
    );
    void update_text(wxString text);
    void on_show();
    void on_hide();
    void update_btn_label(wxString ok_btn_text, wxString cancel_btn_text);
    void update_title_style(wxString title, SecondaryCheckDialog::VisibleButtons style, wxWindow* parent = nullptr); // ORCA VisibleButtons instead ButtonStyle 
    void post_event(wxCommandEvent&& event);
    void rescale();
    ~SecondaryCheckDialog();
    void on_dpi_changed(const wxRect& suggested_rect);
    void msw_rescale();


    StateColor btn_bg_green;
    StateColor btn_bg_white;
    Label* m_staticText_release_note {nullptr};
    wxBoxSizer* m_sizer_main;
    wxScrolledWindow *m_vebview_release_note {nullptr};
    Button* m_button_ok { nullptr };
    Button* m_button_retry { nullptr };
    Button* m_button_cancel { nullptr };
    Button* m_button_fn { nullptr };
    Button* m_button_resume { nullptr };
    wxCheckBox* m_show_again_checkbox;
    VisibleButtons m_button_style; // ORCA VisibleButtons instead ButtonStyle 
    bool not_show_again = false;
    std::string show_again_config_text = "";
};

class PrintErrorDialog : public DPIFrame
{
private:
    wxWindow* event_parent{ nullptr };
public:
    enum PrintErrorButton : int {
        RESUME_PRINTING = 2,
        RESUME_PRINTING_DEFECTS = 3,
        RESUME_PRINTING_PROBELM_SOLVED = 4,
        STOP_PRINTING = 5,
        CHECK_ASSISTANT = 6,
        FILAMENT_EXTRUDED = 7,
        RETRY_FILAMENT_EXTRUDED = 8,
        CONTINUE = 9,
        LOAD_VIRTUAL_TRAY = 10,
        OK_BUTTON = 11,
        FILAMENT_LOAD_RESUME = 12,
        JUMP_TO_LIVEVIEW,

        NO_REMINDER_NEXT_TIME = 23,
        IGNORE_NO_REMINDER_NEXT_TIME = 25,
        //LOAD_FILAMENT = 26*/
        IGNORE_RESUME = 27,
        PROBLEM_SOLVED_RESUME = 28,
        TURN_OFF_FIRE_ALARM = 29,

        RETRY_PROBLEM_SOLVED = 34,
        STOP_DRYING = 35,
        REMOVE_CLOSE_BTN = 39, // special case, do not show close button

        ERROR_BUTTON_COUNT
    };
    PrintErrorDialog(
        wxWindow* parent,
        wxWindowID      id = wxID_ANY,
        const wxString& title = wxEmptyString,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long            style = wxCLOSE_BOX | wxCAPTION
    );
    void update_text_image(const wxString& text, const wxString& error_code,const wxString& image_url);
    void on_show();
    void on_hide();
    void update_title_style(wxString title, std::vector<int> style, wxWindow* parent = nullptr);
    void post_event(wxCommandEvent& event);
    void post_event(wxCommandEvent&& event);
    void rescale();
    ~PrintErrorDialog();
    void on_dpi_changed(const wxRect& suggested_rect);
    void msw_rescale();
    void init_button(PrintErrorButton style, wxString buton_text);
    void init_button_list();
    void on_webrequest_state(wxWebRequestEvent& evt);

    wxWebRequest web_request;
    wxStaticBitmap* m_error_prompt_pic_static;
    Label* m_staticText_release_note{ nullptr };
    Label* m_staticText_error_code{ nullptr };
    wxBoxSizer* m_sizer_main;
    wxBoxSizer* m_sizer_button;
    wxScrolledWindow* m_vebview_release_note{ nullptr };
    std::map<int, Button*> m_button_list;
    std::vector<int> m_used_button;
};

struct ConfirmBeforeSendInfo
{
public:
    enum InfoLevel {
        Normal = 0,
        Warning = 1
    };
    InfoLevel level;
    wxString text;
    wxString wiki_url;
    ConfirmBeforeSendInfo(const wxString& txt, const wxString& url = wxEmptyString, InfoLevel lev = Normal) : text(txt), wiki_url(url), level(lev){}
};

class ConfirmBeforeSendDialog : public DPIDialog
{
public:
    enum VisibleButtons { // ORCA VisibleButtons instead ButtonStyle 
        ONLY_CONFIRM = 0,
        CONFIRM_AND_CANCEL = 1,
        MAX_STYLE_NUM = 2
    };
    ConfirmBeforeSendDialog(
        wxWindow* parent,
        wxWindowID      id = wxID_ANY,
        const wxString& title = wxEmptyString,
        enum VisibleButtons btn_style = CONFIRM_AND_CANCEL, // ORCA VisibleButtons instead ButtonStyle 
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long            style = wxCLOSE_BOX | wxCAPTION,
        bool not_show_again_check = false
    );
    void update_text(wxString text);
    void update_text(std::vector<ConfirmBeforeSendInfo> texts, bool enable_warning_clr = true);
    void on_show();
    void on_hide();
    void update_btn_label(wxString ok_btn_text, wxString cancel_btn_text);
    void rescale();
    void on_dpi_changed(const wxRect& suggested_rect);
    void show_update_nozzle_button(bool show = false);
    void hide_button_ok();
    void edit_cancel_button_txt(const wxString& txt, bool switch_green = false);
    void disable_button_ok();
    void enable_button_ok();
    wxString format_text(wxString str, int warp);

    ~ConfirmBeforeSendDialog();

protected:
    wxBoxSizer* m_sizer_main;
    wxScrolledWindow* m_vebview_release_note{ nullptr };
    Label* m_staticText_release_note{ nullptr };
    Button* m_button_ok;
    Button* m_button_cancel;
    Button* m_button_update_nozzle;
    wxCheckBox* m_show_again_checkbox;
    bool not_show_again = false;
    std::string show_again_config_text = "";
};

class InputIpAddressDialog : public DPIDialog
{
public:
    wxString comfirm_before_check_text;
    wxString comfirm_before_enter_text;
    wxString comfirm_after_enter_text;
    wxString comfirm_last_enter_text;

    std::shared_ptr<InputIpAddressDialog> token_;
    boost::thread* m_thread{nullptr};

    std::string m_ip;
    wxWindow* m_step_icon_panel3{ nullptr };
    Label* m_tip0{ nullptr };
    Label* m_tip1{ nullptr };
    Label* m_tip2{ nullptr };
    Label* m_tip3{ nullptr };
    Label* m_tip4{ nullptr };
    InputIpAddressDialog(wxWindow* parent = nullptr);
    ~InputIpAddressDialog();

    MachineObject* m_obj{nullptr};
    wxPanel * ip_input_top_panel{ nullptr };
    wxPanel * ip_input_bot_panel{ nullptr };
    Button* m_button_ok{ nullptr };
    Button* m_button_manual_setup{ nullptr };
    Label* m_tips_ip{ nullptr };
    Label* m_tips_access_code{ nullptr };
    Label* m_tips_sn{nullptr};
    Label* m_tips_modelID{nullptr};
    Label* m_test_right_msg{ nullptr };
    Label* m_test_wrong_msg{ nullptr };
    TextInput* m_input_ip{ nullptr };
    TextInput* m_input_access_code{ nullptr };
    TextInput* m_input_printer_name{ nullptr };
    TextInput* m_input_sn{ nullptr };
    ComboBox*  m_input_modelID{ nullptr };
    wxStaticBitmap* m_img_help{ nullptr };
    wxStaticBitmap* m_img_step1{ nullptr };
    wxStaticBitmap* m_img_step2{ nullptr };
    wxStaticBitmap* m_img_step3{ nullptr };
    wxHyperlinkCtrl* m_trouble_shoot{ nullptr };
    wxTimer* closeTimer{ nullptr };
    int     closeCount{3};
    bool   m_show_access_code{ false };
    bool   m_need_input_sn{true};
    int    m_result;
    int    current_input_index {0};
    std::shared_ptr<BBLStatusBarSend> m_status_bar;
    std::unique_ptr<Worker> m_worker;
    std::map<std::string, std::string> m_models_map;// display_name -> model_id

    void switch_input_panel(int index);
    void on_cancel();
    void update_title(wxString title);
    void set_machine_obj(MachineObject* obj);
    void update_test_msg(wxString msg, bool connected);
    bool isIp(std::string ipstr);
    void check_ip_address_failed(int result);
    void on_check_ip_address_failed(wxCommandEvent& evt);
    void on_ok(wxMouseEvent& evt);
    void on_send_retry();
    void update_test_msg_event(wxCommandEvent &evt);
    void post_update_test_msg(std::weak_ptr<InputIpAddressDialog> w, wxString text, bool beconnect);
    void workerThreadFunc(std::string str_ip, std::string str_access_code, std::string sn, std::string model_id, std::string name);
    void OnTimer(wxTimerEvent& event);
    void on_text(wxCommandEvent& evt);
    void on_dpi_changed(const wxRect& suggested_rect) override;
};

class SendFailedConfirm : public DPIDialog
{
public:
    SendFailedConfirm(wxWindow *parent = nullptr);
    ~SendFailedConfirm(){};

    //void on_ok(wxMouseEvent &evt);
    void on_dpi_changed(const wxRect &suggested_rect) override;
};

wxDECLARE_EVENT(EVT_CLOSE_IPADDRESS_DLG, wxCommandEvent);
wxDECLARE_EVENT(EVT_CHECKBOX_CHANGE, wxCommandEvent);
wxDECLARE_EVENT(EVT_ENTER_IP_ADDRESS, wxCommandEvent);
wxDECLARE_EVENT(EVT_CHECK_IP_ADDRESS_FAILED, wxCommandEvent);
wxDECLARE_EVENT(EVT_CHECK_IP_ADDRESS_LAYOUT, wxCommandEvent);


}} // namespace Slic3r::GUI

#endif
