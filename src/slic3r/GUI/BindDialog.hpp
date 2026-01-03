#ifndef slic3r_BindDialog_hpp_
#define slic3r_BindDialog_hpp_

#include "I18N.hpp"

#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/dialog.h>
#include <curl/curl.h>
#include <wx/webrequest.h>
#include "wxExtensions.hpp"
#include "Widgets/StepCtrl.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ProgressBar.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/HyperLink.hpp"
#include "Jobs/BindJob.hpp"
#include "BBLStatusBar.hpp"
#include "BBLStatusBarBind.hpp"
#include "Jobs/Worker.hpp"
#include "GUI_Utils.hpp"
#include "Widgets/TextInput.hpp"
#include "Jobs/PrintJob.hpp"
#include "Jobs/SendJob.hpp"
#include "DeviceManager.hpp"

#define BIND_DIALOG_GREY200 wxColour(248, 248, 248)
#define BIND_DIALOG_GREY800 wxColour(50, 58, 61)
#define BIND_DIALOG_GREY900 wxColour(38, 46, 48)
#define BIND_DIALOG_BUTTON_SIZE wxSize(FromDIP(68), FromDIP(24))
#define BIND_DIALOG_BUTTON_PANEL_SIZE wxSize(FromDIP(450), FromDIP(30))
#define PING_CODE_LENGTH 6

namespace Slic3r { namespace GUI {
class Plater;
struct MemoryStruct
{
    char * memory;
    size_t read_pos;
    size_t size;
};

class PingCodeBindDialog : public DPIDialog
{
private:

    Label* m_status_text;
    wxStaticText* m_text_input_title;
    HyperLink* m_link_show_ping_code_wiki; // ORCA
    TextInput* m_text_input_single_code[PING_CODE_LENGTH];
    Button* m_button_bind;
    Button* m_button_cancel;
    Button* m_button_close;
    wxSimplebook* m_simplebook;
    wxPanel* request_bind_panel;
    wxPanel* binding_panel;

    wxScrolledWindow* m_sw_bind_failed_info;
    Label* m_bind_failed_info;
    Label* m_st_txt_error_code{ nullptr };
    Label* m_st_txt_error_desc{ nullptr };
    Label* m_st_txt_extra_info{ nullptr };
    HyperLink*      m_link_network_state{ nullptr };
    wxString        m_result_info;
    wxString        m_result_extra;
    wxString        m_ping_code_wiki;
    bool            m_show_error_info_state = true;

    int             m_result_code;
    std::shared_ptr<BBLStatusBarBind> m_status_bar;

public:
    PingCodeBindDialog(Plater* plater = nullptr);
    ~PingCodeBindDialog();

    void     on_key_input(wxKeyEvent& evt);
    void     on_text_changed(wxCommandEvent& event);
    void     on_key_backspace(wxKeyEvent& event);
    void     on_cancel(wxCommandEvent& event);
    void     on_bind_printer(wxCommandEvent& event);
    void     on_dpi_changed(const wxRect& suggested_rect) override;
};

class BindMachineDialog : public DPIDialog
{
private:
    wxWindow*      m_panel_agreement;
    wxStaticText * m_printer_name;
    wxStaticText * m_user_name;
    StaticBox *   m_panel_left;
    StaticBox *   m_panel_right;
    wxStaticText *m_status_text;
    wxStaticText* m_link_show_error;
    Button *      m_button_bind;
    Button *      m_button_cancel;
    wxSimplebook *m_simplebook;
    wxStaticBitmap *m_avatar;
    wxStaticBitmap *m_printer_img;
    wxStaticBitmap *m_static_bitmap_show_error;
    wxBitmap      m_bitmap_show_error_close;
    wxBitmap      m_bitmap_show_error_open;
    wxScrolledWindow* m_sw_bind_failed_info;
    Label*          m_bind_failed_info;
    Label*          m_st_txt_error_code{ nullptr };
    Label*          m_st_txt_error_desc{ nullptr };
    Label*          m_st_txt_extra_info{ nullptr };
    HyperLink*      m_link_network_state{ nullptr };
    wxString        m_result_info;
    wxString        m_result_extra;
    bool            m_show_error_info_state = true;
    bool            m_allow_privacy{false};
    bool            m_allow_notice{false};
    int             m_result_code;
    std::shared_ptr<int>     m_tocken;

    MachineObject *                   m_machine_info{nullptr};
    std::shared_ptr<BBLStatusBarBind> m_status_bar;
    std::unique_ptr<Worker>           m_worker;

public:
    BindMachineDialog(Plater *plater = nullptr);
    ~BindMachineDialog();

    void     show_bind_failed_info(bool show, int code = 0, wxString description = wxEmptyString, wxString extra = wxEmptyString);
    void     on_cancel(wxCommandEvent& event);
    void     on_bind_fail(wxCommandEvent &event);
    void     on_update_message(wxCommandEvent &event);
    void     on_bind_success(wxCommandEvent &event);
    void     on_bind_printer(wxCommandEvent &event);
    void     on_dpi_changed(const wxRect &suggested_rect) override;
    void     update_machine_info(MachineObject *info);
    void     on_show(wxShowEvent &event);
    void     on_close(wxCloseEvent& event);
    void     on_destroy();
    wxString get_print_error(wxString str);
};

class UnBindMachineDialog : public DPIDialog
{
protected:
    wxStaticText *  m_printer_name;
    wxStaticText *  m_user_name;
    wxStaticText *m_status_text;
    Button *      m_button_unbind;
    Button *      m_button_cancel;
    MachineObject *m_machine_info{nullptr};
    wxStaticBitmap *m_avatar;
    wxStaticBitmap *m_printer_img;
    std::shared_ptr<int>     m_tocken;

public:
    UnBindMachineDialog(Plater *plater = nullptr);
    ~UnBindMachineDialog();

    void on_cancel(wxCommandEvent &event);
    void on_unbind_printer(wxCommandEvent &event);
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void update_machine_info(MachineObject *info) { m_machine_info = info; };
    void on_show(wxShowEvent &event);
};

}} // namespace Slic3r::GUI

#endif /* slic3r_BindDialog_hpp_ */
