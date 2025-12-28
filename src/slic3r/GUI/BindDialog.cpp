#include "BindDialog.hpp"
#include "GUI_App.hpp"

#include <wx/wx.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include "wx/evtloop.h"
#include <wx/tokenzr.h>
#include <wx/richmsgdlg.h>
#include <wx/richtext/richtextctrl.h>
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"
#include "MainFrame.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/PlaterWorker.hpp"
#include "Widgets/WebView.hpp"

#include "DeviceCore/DevManager.h"

namespace Slic3r {
namespace GUI {

wxString get_fail_reason(int code)
{
    if (code == BAMBU_NETWORK_ERR_BIND_CREATE_SOCKET_FAILED)
        return _L("Failed to create socket");

    else if (code == BAMBU_NETWORK_ERR_BIND_SOCKET_CONNECT_FAILED)
        return _L("Failed to connect socket");

    else if (code == BAMBU_NETWORK_ERR_BIND_PUBLISH_LOGIN_REQUEST)
        return _L("Failed to publish login request");

    else if (code == BAMBU_NETWORK_ERR_BIND_GET_PRINTER_TICKET_TIMEOUT)
        return _L("Get ticket from device timeout");

    else if (code == BAMBU_NETWORK_ERR_BIND_GET_CLOUD_TICKET_TIMEOUT)
        return _L("Get ticket from server timeout");

    else if (code == BAMBU_NETWORK_ERR_BIND_POST_TICKET_TO_CLOUD_FAILED)
        return _L("Failed to post ticket to server");

    else if (code == BAMBU_NETWORK_ERR_BIND_PARSE_LOGIN_REPORT_FAILED)
        return _L("Failed to parse login report reason");

    else if (code == BAMBU_NETWORK_ERR_BIND_ECODE_LOGIN_REPORT_FAILED)
        return _L("Failed to parse login report reason");

    else if (code == BAMBU_NETWORK_ERR_BIND_RECEIVE_LOGIN_REPORT_TIMEOUT)
        return _L("Receive login report timeout");

    else
        return _L("Unknown Failure");
}

PingCodeBindDialog::PingCodeBindDialog(Plater* plater /*= nullptr*/)
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Bind with Pin Code"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
    wxBoxSizer* sizer_main = new wxBoxSizer(wxVERTICAL);

    SetBackgroundColour(*wxWHITE);
    wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));


    m_simplebook = new wxSimplebook(this);
    m_simplebook->SetSize(wxSize(FromDIP(460), FromDIP(240)));
    m_simplebook->SetMinSize(wxSize(FromDIP(460), FromDIP(240)));
    m_simplebook->SetMaxSize(wxSize(FromDIP(460), FromDIP(240)));


    request_bind_panel = new wxPanel(m_simplebook);
    binding_panel = new wxPanel(m_simplebook);

    request_bind_panel->SetSize(wxSize(FromDIP(460), FromDIP(240)));
    request_bind_panel->SetMinSize(wxSize(FromDIP(460), FromDIP(240)));
    request_bind_panel->SetMaxSize(wxSize(FromDIP(460), FromDIP(240)));

    binding_panel->SetSize(wxSize(FromDIP(460), FromDIP(240)));
    binding_panel->SetMinSize(wxSize(FromDIP(460), FromDIP(240)));
    binding_panel->SetMaxSize(wxSize(FromDIP(460), FromDIP(240)));


    request_bind_panel->SetBackgroundColour(*wxWHITE);
    binding_panel->SetBackgroundColour(*wxWHITE);

    m_status_text = new Label(request_bind_panel, _L("Please Find the Pin Code in Account page on printer screen,\n and type in the Pin Code below."));
    m_status_text->SetBackgroundColour(*wxWHITE);
    m_status_text->SetFont(Label::Body_14);
    m_status_text->SetMaxSize(wxSize(FromDIP(440), -1));
    m_status_text->Wrap(FromDIP(440));
    m_status_text->SetForegroundColour(wxColour(38, 46, 48));

    m_link_show_ping_code_wiki = new wxStaticText(request_bind_panel, wxID_ANY, _L("Can't find Pin Code?"));
    m_link_show_ping_code_wiki->SetFont(Label::Body_14);
    m_link_show_ping_code_wiki->SetBackgroundColour(*wxWHITE);
    m_link_show_ping_code_wiki->SetForegroundColour(wxColour(31, 142, 234));

    m_link_show_ping_code_wiki->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
    m_link_show_ping_code_wiki->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });

    m_link_show_ping_code_wiki->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        m_ping_code_wiki = "https://wiki.bambulab.com/en/bambu-studio/manual/pin-code";
        wxLaunchDefaultBrowser(m_ping_code_wiki);
    });

    m_text_input_title = new wxStaticText(request_bind_panel, wxID_ANY, _L("Pin Code"));
    m_text_input_title->SetFont(Label::Body_14);
    m_text_input_title->SetBackgroundColour(*wxWHITE);

    wxBoxSizer* ping_code_input = new wxBoxSizer(wxHORIZONTAL);


    for (int i = 0; i < PING_CODE_LENGTH; i++) {
        m_text_input_single_code[i] = new TextInput(request_bind_panel, wxEmptyString, "", "", wxDefaultPosition, wxSize(FromDIP(38), FromDIP(38)), wxTE_PROCESS_ENTER | wxTE_CENTER);
        wxTextAttr textAttr;
        textAttr.SetAlignment(wxTEXT_ALIGNMENT_CENTER);
        textAttr.SetTextColour(wxColour(34, 139, 34));
        m_text_input_single_code[i]->GetTextCtrl()->SetDefaultStyle(textAttr);
        m_text_input_single_code[i]->SetFont(Label::Body_16);
        m_text_input_single_code[i]->GetTextCtrl()->SetMaxLength(1);
        m_text_input_single_code[i]->GetTextCtrl()->Bind(wxEVT_TEXT, &PingCodeBindDialog::on_text_changed, this);
        m_text_input_single_code[i]->GetTextCtrl()->Bind(wxEVT_KEY_DOWN, &PingCodeBindDialog::on_key_backspace, this);
        m_text_input_single_code[i]->GetTextCtrl()->Bind(wxEVT_CHAR, &PingCodeBindDialog::on_key_input, this);
        ping_code_input->Add(m_text_input_single_code[i], 0, wxALL, FromDIP(5));
    }

    wxBoxSizer* m_sizer_button = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_button->Add(0, 0, 1, wxEXPAND, 5);
    m_button_bind = new Button(request_bind_panel, _L("Confirm"));
    m_button_bind->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_button_bind->Enable(false);

    m_button_cancel = new Button(request_bind_panel, _L("Cancel"));
    m_button_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    m_sizer_button->Add(m_button_bind, 0, wxALIGN_CENTER, 0);
    m_sizer_button->Add(0, 0, 0, wxLEFT, ButtonProps::ChoiceButtonGap());
    m_sizer_button->Add(m_button_cancel, 0, wxALIGN_CENTER, 0);



    m_simplebook->AddPage(request_bind_panel, wxEmptyString, true);
    m_simplebook->AddPage(binding_panel, wxEmptyString, false);


    auto sizer_request = new wxBoxSizer(wxVERTICAL);
    sizer_request->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer_request->Add(m_status_text, 0, wxLEFT, FromDIP(13));
    sizer_request->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer_request->Add(m_link_show_ping_code_wiki, 0, wxLEFT, FromDIP(13));
    sizer_request->Add(0, 0, 0, wxTOP, FromDIP(15));
    sizer_request->Add(m_text_input_title, 0, wxLEFT, FromDIP(13));
    sizer_request->Add(0, 0, 0, wxTOP, FromDIP(5));
    sizer_request->Add(ping_code_input, 0, wxLEFT, FromDIP(10));
    sizer_request->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer_request->Add(m_sizer_button, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, ButtonProps::ChoiceButtonGap());
    request_bind_panel->SetSizer(sizer_request);
    request_bind_panel->Layout();
    request_bind_panel->Fit();



    auto m_loading_txt = new Label(binding_panel, _L("Binding..."));
    m_loading_txt->SetBackgroundColour(*wxWHITE);
    m_loading_txt->SetFont(Label::Head_16);
    auto m_loading_tip_txt = new Label(binding_panel, _L("Please confirm on the printer screen"));
    m_loading_tip_txt->SetBackgroundColour(*wxWHITE);
    m_loading_tip_txt->SetFont(Label::Body_15);

    wxBoxSizer* m_sizer_binding_button = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_binding_button->Add(0, 0, 1, wxEXPAND, 5);

    m_button_close = new Button(binding_panel, _L("Close"));
    m_button_close->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    m_sizer_binding_button->Add(m_button_close, 0, wxALIGN_CENTER, 0);

    auto sizer_binding = new wxBoxSizer(wxVERTICAL);
    sizer_binding->Add(0, 0, 0, wxTOP, FromDIP(80));
    sizer_binding->Add(m_loading_txt, 0, wxALIGN_CENTER, 0);
    sizer_binding->Add(0, 0, 0, wxTOP, FromDIP(10));
    sizer_binding->Add(m_loading_tip_txt, 0, wxALIGN_CENTER, 0);
    sizer_binding->Add(0, 0, 0, wxTOP, FromDIP(30));
    sizer_binding->Add(m_sizer_binding_button, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, ButtonProps::ChoiceButtonGap());
    binding_panel->SetSizer(sizer_binding);
    binding_panel->Layout();
    binding_panel->Fit();



    sizer_main->Add(m_line_top, 0, wxEXPAND, 0);
    sizer_main->Add(m_simplebook, 0, wxEXPAND, 0);



    SetSizer(sizer_main);
    Layout();
    Fit();

    m_button_bind->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(PingCodeBindDialog::on_bind_printer), NULL, this);
    m_button_cancel->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(PingCodeBindDialog::on_cancel), NULL, this);
    m_button_close->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(PingCodeBindDialog::on_cancel), NULL, this);

    m_simplebook->SetSelection(0);

    wxGetApp().UpdateDlgDarkUI(this);
}

void PingCodeBindDialog::on_key_input(wxKeyEvent& evt)
{
    int keyCode = evt.GetKeyCode();

    if (keyCode == WXK_BACK  || (keyCode >= '0' && keyCode <= '9') || (keyCode >= 'a' && keyCode <= 'z') || (keyCode >= 'A' && keyCode <= 'Z'))
    {
        evt.Skip();
    }
    else
    {
        wxBell();
        return;
    }
}

void PingCodeBindDialog::on_text_changed(wxCommandEvent& event) {
    //switch focus to the text text input
    wxTextCtrl* text_input = static_cast<wxTextCtrl*>(event.GetEventObject());
    int idx = -1;
    for (int i = 0; i < PING_CODE_LENGTH; i++) {
        if (text_input == m_text_input_single_code[i]->GetTextCtrl()) {
            idx = i;
            break;
        }
    }

    if (idx != -1 && text_input->GetValue().Length() == 1) {
        if (idx < PING_CODE_LENGTH-1) {
            m_text_input_single_code[idx + 1]->SetFocus();
        }

        auto has_empty = false;
        for (int i = 0; i < PING_CODE_LENGTH; i++) {
            if (m_text_input_single_code[i]->GetTextCtrl()->GetValue().ToStdString().empty()) {
                has_empty = true;
            }
        }

        if (has_empty) {
            m_button_bind->Enable(false);
        }
        else {
            m_button_bind->Enable(true);
        }

        /*if (idx == PING_CODE_LENGTH - 1) {
            m_button_bind->Enable(true);
        }*/
    }

}

void PingCodeBindDialog::on_key_backspace(wxKeyEvent& event)
{
    wxTextCtrl* text_input = static_cast<wxTextCtrl*>(event.GetEventObject());
    int idx = -1;
    for (int i = 0; i < 6; i++) {
        if (text_input == m_text_input_single_code[i]->GetTextCtrl()) {
            idx = i;
            break;
        }
    }

    if (event.GetKeyCode() == WXK_BACK && idx >= 0) {
        CallAfter([this, idx]() {
            m_text_input_single_code[idx - 1]->SetFocus();
            m_button_bind->Enable(false);
        });
    }
    event.Skip();
}

void PingCodeBindDialog::on_bind_printer(wxCommandEvent& event)
{
    wxString ping_code;

    for (int i = 0; i < PING_CODE_LENGTH; i++) {
        ping_code += m_text_input_single_code[i]->GetTextCtrl()->GetValue().ToStdString();
    }

    NetworkAgent* agent = wxGetApp().getAgent();
    if (agent && agent->is_user_login() && ping_code.length() == PING_CODE_LENGTH) {
        auto result = agent->ping_bind(ping_code.ToStdString());

        if(result < 0){
            MessageDialog msg_wingow(nullptr, _L("Log in failed. Please check the Pin Code."), "", wxAPPLY | wxOK);
            msg_wingow.ShowModal();
            return;
        }
        m_simplebook->SetSelection(1);
    }
}

void PingCodeBindDialog::on_cancel(wxCommandEvent& event)
{
    EndModal(wxCLOSE);
}

void PingCodeBindDialog::on_dpi_changed(const wxRect& suggested_rect)
{

    Fit();
    Refresh();
}


PingCodeBindDialog::~PingCodeBindDialog() {
    m_button_bind->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(PingCodeBindDialog::on_bind_printer), NULL, this);
    m_button_cancel->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(PingCodeBindDialog::on_cancel), NULL, this);
    m_button_close->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(PingCodeBindDialog::on_cancel), NULL, this);
}

 BindMachineDialog::BindMachineDialog(Plater *plater /*= nullptr*/)
     : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Log in printer"), wxDefaultPosition, wxDefaultSize, wxCAPTION)
 {

#ifdef __WINDOWS__
     SetDoubleBuffered(true);
#endif //__WINDOWS__

     m_tocken.reset(new int(0));

     SetBackgroundColour(*wxWHITE);
     wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
     auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
     m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
     m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);
     m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(38));

     wxBoxSizer *m_sizer_body = new wxBoxSizer(wxHORIZONTAL);

     m_panel_left = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(201), FromDIP(212)), wxBORDER_NONE);
     m_panel_left->SetMinSize(wxSize(FromDIP(201), FromDIP(212)));
     m_panel_left->SetCornerRadius(FromDIP(8));
     m_panel_left->SetBackgroundColor(BIND_DIALOG_GREY200);
     wxBoxSizer *m_sizere_left_h = new wxBoxSizer(wxHORIZONTAL);
     wxBoxSizer *m_sizere_left_v= new wxBoxSizer(wxVERTICAL);

     m_printer_img = new wxStaticBitmap(m_panel_left, wxID_ANY, create_scaled_bitmap("printer_thumbnail", nullptr, FromDIP(100)), wxDefaultPosition, wxSize(FromDIP(120), FromDIP(120)), 0);
     m_printer_img->SetBackgroundColour(BIND_DIALOG_GREY200);
     m_printer_img->Hide();
     m_printer_name = new wxStaticText(m_panel_left, wxID_ANY, wxEmptyString);
     m_printer_name->SetForegroundColour(*wxBLACK);
     m_printer_name->SetBackgroundColour(BIND_DIALOG_GREY200);
     m_printer_name->SetFont(::Label::Head_14);
     m_sizere_left_v->Add(m_printer_img, 0, wxALIGN_CENTER, 0);
     m_sizere_left_v->Add(0, 0, 0, wxTOP, 5);
     m_sizere_left_v->Add(m_printer_name, 0, wxALIGN_CENTER, 0);
     m_sizere_left_h->Add(m_sizere_left_v, 1, wxALIGN_CENTER, 0);

     m_panel_left->SetSizer(m_sizere_left_h);
     m_panel_left->Layout();
     m_sizer_body->Add(m_panel_left, 0, wxEXPAND, 0);

     auto m_bind_icon = create_scaled_bitmap("bind_machine", nullptr, 14);
     m_sizer_body->Add(new wxStaticBitmap(this, wxID_ANY, m_bind_icon, wxDefaultPosition, wxSize(FromDIP(34), FromDIP(14)), 0), 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(20));

     m_panel_right = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(201), FromDIP(212)), wxBORDER_NONE);
     m_panel_right->SetMinSize(wxSize(FromDIP(201), FromDIP(212)));
     m_panel_right->SetCornerRadius(FromDIP(8));
     m_panel_right->SetBackgroundColor(BIND_DIALOG_GREY200);

     m_user_name = new wxStaticText(m_panel_right, wxID_ANY, wxEmptyString);
     m_user_name->SetBackgroundColour(BIND_DIALOG_GREY200);
     m_user_name->SetFont(::Label::Head_14);
     wxBoxSizer *m_sizer_right_h = new wxBoxSizer(wxHORIZONTAL);
     wxBoxSizer *m_sizer_right_v = new wxBoxSizer(wxVERTICAL);

     m_avatar = new wxStaticBitmap(m_panel_right, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(60), FromDIP(60)), 0);
     m_sizer_right_v->Add(m_avatar, 0, wxALIGN_CENTER, 0);
     m_sizer_right_v->Add(0, 0, 0, wxTOP, 7);
     m_sizer_right_v->Add(m_user_name, 0, wxALIGN_CENTER, 0);
     m_sizer_right_h->Add(m_sizer_right_v, 1, wxALIGN_CENTER, 0);

     m_panel_right->SetSizer(m_sizer_right_h);
     m_panel_right->Layout();
     m_sizer_body->Add(m_panel_right, 0, wxEXPAND, 0);

     m_sizer_main->Add(m_sizer_body, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(30));

     m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(20));


     auto m_sizer_status_text = new wxBoxSizer(wxHORIZONTAL);
     m_status_text = new wxStaticText(this, wxID_ANY, _L("Would you like to log in to this printer with the current account?"));
     m_status_text->SetForegroundColour(wxColour(107, 107, 107));
     m_status_text->SetFont(::Label::Body_13);
     m_status_text->Wrap(-1);


     m_link_show_error = new wxStaticText(this, wxID_ANY, _L("Check the reason"));
     m_link_show_error->SetForegroundColour(wxColour("#6b6b6b"));
     m_link_show_error->SetFont(::Label::Head_13);

     m_bitmap_show_error_close = create_scaled_bitmap("link_more_error_close",nullptr, 7);
     m_bitmap_show_error_open = create_scaled_bitmap("link_more_error_open",nullptr, 7);
     m_static_bitmap_show_error = new wxStaticBitmap(this, wxID_ANY, m_bitmap_show_error_open, wxDefaultPosition, wxSize(FromDIP(7), FromDIP(7)));

     m_link_show_error->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
     m_link_show_error->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });
     m_link_show_error->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
         if (!m_show_error_info_state) { m_show_error_info_state = true; m_static_bitmap_show_error->SetBitmap(m_bitmap_show_error_open); }
         else { m_show_error_info_state = false; m_static_bitmap_show_error->SetBitmap(m_bitmap_show_error_close); }
         show_bind_failed_info(true);}
     );
     m_static_bitmap_show_error->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
     m_static_bitmap_show_error->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });
     m_static_bitmap_show_error->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
         if (!m_show_error_info_state) { m_show_error_info_state = true; m_static_bitmap_show_error->SetBitmap(m_bitmap_show_error_open); }
         else { m_show_error_info_state = false; m_static_bitmap_show_error->SetBitmap(m_bitmap_show_error_close); }
         show_bind_failed_info(true);
     });

     m_link_show_error->Hide();
     m_static_bitmap_show_error->Hide();

     m_sizer_status_text->SetMinSize(wxSize(BIND_DIALOG_BUTTON_PANEL_SIZE.x, -1));
     m_sizer_status_text->Add(m_status_text, 0, wxALIGN_CENTER, 0);
     m_sizer_status_text->Add(m_link_show_error, 0, wxLEFT|wxALIGN_CENTER, FromDIP(8));
     m_sizer_status_text->Add(m_static_bitmap_show_error, 0, wxLEFT|wxALIGN_CENTER, FromDIP(2));


     //agreement
     m_panel_agreement = new wxWindow(this,wxID_ANY);
     m_panel_agreement->SetBackgroundColour(*wxWHITE);
     m_panel_agreement->SetMinSize(wxSize(FromDIP(450), -1));
     m_panel_agreement->SetMaxSize(wxSize(FromDIP(450), -1));


     wxWrapSizer* sizer_privacy_agreement =  new wxWrapSizer( wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS );
     wxWrapSizer* sizere_notice_agreement=  new wxWrapSizer( wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS );
     wxBoxSizer* sizer_privacy_body = new wxBoxSizer(wxHORIZONTAL);
     wxBoxSizer* sizere_notice_body = new wxBoxSizer(wxHORIZONTAL);

     auto m_checkbox_privacy = new CheckBox(m_panel_agreement, wxID_ANY);
     auto m_st_privacy_title = new Label(m_panel_agreement, _L("Read and accept"));
     m_st_privacy_title->SetFont(Label::Body_13);
     m_st_privacy_title->SetForegroundColour(wxColour(38, 46, 48));

     auto m_link_Terms_title = new Label(m_panel_agreement, _L("Terms and Conditions"));
     m_link_Terms_title->SetFont(Label::Head_13);
     m_link_Terms_title->SetMaxSize(wxSize(FromDIP(450), -1));
     m_link_Terms_title->Wrap(FromDIP(450));
     m_link_Terms_title->SetForegroundColour(wxColour("#009688"));
     m_link_Terms_title->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
         wxString txt = _L("Thank you for purchasing a Bambu Lab device. Before using your Bambu Lab device, please read the terms and conditions. "
                           "By clicking to agree to use your Bambu Lab device, you agree to abide by the Privacy Policy and Terms of Use (collectively, the \"Terms\"). "
                           "If you do not comply with or agree to the Bambu Lab Privacy Policy, please do not use Bambu Lab equipment and services.");
         ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Terms and Conditions"), ConfirmBeforeSendDialog::VisibleButtons::ONLY_CONFIRM); // ORCA VisibleButtons instead ButtonStyle 
         confirm_dlg.update_text(txt);
         confirm_dlg.CenterOnParent();
         confirm_dlg.on_show();
     });
     m_link_Terms_title->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
     m_link_Terms_title->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });

     auto m_st_and_title = new Label(m_panel_agreement, _L("and"));
     m_st_and_title->SetFont(Label::Body_13);
     m_st_and_title->SetForegroundColour(wxColour(38, 46, 48));

     auto m_link_privacy_title = new Label(m_panel_agreement, _L("Privacy Policy"));
     m_link_privacy_title->SetFont(Label::Head_13);
     m_link_privacy_title->SetMaxSize(wxSize(FromDIP(450), -1));
     m_link_privacy_title->Wrap(FromDIP(450));
     m_link_privacy_title->SetForegroundColour(wxColour("#009688"));
     m_link_privacy_title->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
         std::string url;
         std::string country_code = Slic3r::GUI::wxGetApp().app_config->get_country_code();

         if (country_code == "CN") {
             url = "https://www.bambulab.cn/policies/privacy";
         }
         else{
             url = "https://www.bambulab.com/policies/privacy";
         }
         wxLaunchDefaultBrowser(url);
     });
     m_link_privacy_title->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND);});
     m_link_privacy_title->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW);});

     sizere_notice_agreement->Add(0, 0, 0, wxTOP, FromDIP(4));
     sizer_privacy_agreement->Add(m_st_privacy_title, 0, wxALIGN_CENTER, 0);
     sizer_privacy_agreement->Add(0, 0, 0, wxLEFT, FromDIP(5));
     sizer_privacy_agreement->Add(m_link_Terms_title, 0, wxALIGN_CENTER, 0);
     sizer_privacy_agreement->Add(m_st_and_title, 0, wxALIGN_CENTER|wxLEFT|wxRIGHT, FromDIP(5));
     sizer_privacy_agreement->Add(m_link_privacy_title, 0, wxALIGN_CENTER, 0);

     sizer_privacy_body->Add(m_checkbox_privacy, 0, wxALL, 0);
     sizer_privacy_body->Add(0, 0, 0, wxLEFT, FromDIP(8));
     sizer_privacy_body->Add(sizer_privacy_agreement, 1, wxEXPAND, 0);


     wxString notice_title = _L("We ask for your help to improve everyone's printer");
     wxString notice_link_title = _L("Statement about User Experience Improvement Program");

     auto m_checkbox_notice = new CheckBox(m_panel_agreement, wxID_ANY);
     auto m_st_notice_title = new Label(m_panel_agreement, notice_title);
     m_st_notice_title->SetFont(Label::Body_13);
     m_st_notice_title->SetForegroundColour(wxColour(38, 46, 48));

     auto m_link_notice_title = new Label(m_panel_agreement, notice_link_title);
     m_link_notice_title->SetFont(Label::Head_13);
     m_link_notice_title->SetMaxSize(wxSize(FromDIP(450), -1));
     m_link_notice_title->Wrap(FromDIP(450));
     m_link_notice_title->SetForegroundColour(wxColour("#009688"));
     m_link_notice_title->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_HAND); });
     m_link_notice_title->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCURSOR_ARROW); });
     m_link_notice_title->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
         wxString txt = _L("In the 3D Printing community, we learn from each other's successes and failures to adjust "
                           "our own slicing parameters and settings. %s follows the same principle and uses machine "
                           "learning to improve its performance from the successes and failures of the vast number of "
                           "prints by our users. We are training %s to be smarter by feeding them the real-world data. "
                           "If you are willing, this service will access information from your error logs and usage "
                           "logs, which may include information described in Privacy Policy. We will not collect any "
                           "Personal Data by which an individual can be identified directly or indirectly, including "
                           "without limitation names, addresses, payment information, or phone numbers. By enabling "
                           "this service, you agree to these terms and the statement about Privacy Policy.");
         ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Statement on User Experience Improvement Plan"), ConfirmBeforeSendDialog::VisibleButtons::ONLY_CONFIRM); // ORCA VisibleButtons instead ButtonStyle 

         wxString model_id_text;

         if (m_machine_info) {
             model_id_text = m_machine_info->get_printer_type_display_str();
         }
         confirm_dlg.update_text(wxString::Format(txt, model_id_text, model_id_text));
         confirm_dlg.CenterOnParent();
         confirm_dlg.on_show();
     });

     sizere_notice_agreement->Add(0, 0, 0, wxTOP, FromDIP(4));
     sizere_notice_agreement->Add(m_st_notice_title, 0, 0, wxALIGN_CENTER, 0);
     sizere_notice_agreement->Add(0, 0, 0, wxLEFT, FromDIP(2));
     sizere_notice_agreement->Add(m_link_notice_title, 0, 0, wxALIGN_CENTER, 0);

     sizere_notice_body->Add(m_checkbox_notice, 0, wxALL, 0);
     sizere_notice_body->Add(0, 0, 0, wxLEFT, FromDIP(8));
     sizere_notice_body->Add(sizere_notice_agreement, 1, wxEXPAND, 0);

     wxBoxSizer* sizer_agreement = new wxBoxSizer(wxVERTICAL);
     sizer_agreement->Add(sizer_privacy_body, 1, wxEXPAND, 0);
     sizer_agreement->Add(sizere_notice_body, 1, wxEXPAND, 0);


     m_checkbox_privacy->Bind(wxEVT_TOGGLEBUTTON, [this, m_checkbox_privacy](auto& e) {
         m_allow_privacy = m_checkbox_privacy->GetValue();
         m_button_bind->Enable(m_allow_privacy);
         e.Skip();
     });
     m_checkbox_notice->Bind(wxEVT_TOGGLEBUTTON, [this, m_checkbox_notice](auto& e) {
         m_allow_notice = m_checkbox_notice->GetValue();
         e.Skip();
     });

     m_panel_agreement->SetSizer(sizer_agreement);
     m_panel_agreement->Layout();

     //show bind failed info
     m_sw_bind_failed_info = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(450), FromDIP(300)), wxVSCROLL);
     m_sw_bind_failed_info->SetBackgroundColour(*wxWHITE);
     m_sw_bind_failed_info->SetScrollRate(5, 5);
     m_sw_bind_failed_info->SetMinSize(wxSize(FromDIP(450), FromDIP(90)));
     m_sw_bind_failed_info->SetMaxSize(wxSize(FromDIP(450), FromDIP(90)));

     wxBoxSizer* m_sizer_bind_failed_info = new wxBoxSizer(wxVERTICAL);
     m_sw_bind_failed_info->SetSizer( m_sizer_bind_failed_info );

     m_link_network_state = new wxHyperlinkCtrl(m_sw_bind_failed_info, wxID_ANY,_L("Check the status of current system services"),"");
     m_link_network_state->SetFont(::Label::Body_12);
     m_link_network_state->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {wxGetApp().link_to_network_check(); });
     m_link_network_state->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {m_link_network_state->SetCursor(wxCURSOR_HAND); });
     m_link_network_state->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {m_link_network_state->SetCursor(wxCURSOR_ARROW); });



     wxBoxSizer* sizer_error_code = new wxBoxSizer(wxHORIZONTAL);
     wxBoxSizer* sizer_error_desc = new wxBoxSizer(wxHORIZONTAL);
     wxBoxSizer* sizer_extra_info = new wxBoxSizer(wxHORIZONTAL);

     auto st_title_error_code = new wxStaticText(m_sw_bind_failed_info, wxID_ANY, _L("Error code"));
     auto st_title_error_code_doc = new wxStaticText(m_sw_bind_failed_info, wxID_ANY, ": ");
     m_st_txt_error_code = new Label(m_sw_bind_failed_info, wxEmptyString);
     st_title_error_code->SetForegroundColour(0x909090);
     st_title_error_code_doc->SetForegroundColour(0x909090);
     m_st_txt_error_code->SetForegroundColour(0x909090);
     st_title_error_code->SetFont(::Label::Body_13);
     st_title_error_code_doc->SetFont(::Label::Body_13);
     m_st_txt_error_code->SetFont(::Label::Body_13);
     st_title_error_code->SetMinSize(wxSize(FromDIP(80), -1));
     st_title_error_code->SetMaxSize(wxSize(FromDIP(80), -1));
     m_st_txt_error_code->SetMinSize(wxSize(FromDIP(340), -1));
     m_st_txt_error_code->SetMaxSize(wxSize(FromDIP(340), -1));
     sizer_error_code->Add(st_title_error_code, 0, wxALL, 0);
     sizer_error_code->Add(st_title_error_code_doc, 0, wxALL, 0);
     sizer_error_code->Add(m_st_txt_error_code, 0, wxALL, 0);


     auto st_title_error_desc = new wxStaticText(m_sw_bind_failed_info, wxID_ANY, wxT("Error desc"));
     auto st_title_error_desc_doc = new wxStaticText(m_sw_bind_failed_info, wxID_ANY, ": ");
     m_st_txt_error_desc = new Label(m_sw_bind_failed_info, wxEmptyString);
     st_title_error_desc->SetForegroundColour(0x909090);
     st_title_error_desc_doc->SetForegroundColour(0x909090);
     m_st_txt_error_desc->SetForegroundColour(0x909090);
     st_title_error_desc->SetFont(::Label::Body_13);
     st_title_error_desc_doc->SetFont(::Label::Body_13);
     m_st_txt_error_desc->SetFont(::Label::Body_13);
     st_title_error_desc->SetMinSize(wxSize(FromDIP(80), -1));
     st_title_error_desc->SetMaxSize(wxSize(FromDIP(80), -1));
     m_st_txt_error_desc->SetMinSize(wxSize(FromDIP(340), -1));
     m_st_txt_error_desc->SetMaxSize(wxSize(FromDIP(340), -1));
     sizer_error_desc->Add(st_title_error_desc, 0, wxALL, 0);
     sizer_error_desc->Add(st_title_error_desc_doc, 0, wxALL, 0);
     sizer_error_desc->Add(m_st_txt_error_desc, 0, wxALL, 0);

     auto st_title_extra_info = new wxStaticText(m_sw_bind_failed_info, wxID_ANY, wxT("Extra info"));
     auto st_title_extra_info_doc = new wxStaticText(m_sw_bind_failed_info, wxID_ANY, ": ");
     m_st_txt_extra_info = new Label(m_sw_bind_failed_info, wxEmptyString);
     st_title_extra_info->SetForegroundColour(0x909090);
     st_title_extra_info_doc->SetForegroundColour(0x909090);
     m_st_txt_extra_info->SetForegroundColour(0x909090);
     st_title_extra_info->SetFont(::Label::Body_13);
     st_title_extra_info_doc->SetFont(::Label::Body_13);
     m_st_txt_extra_info->SetFont(::Label::Body_13);
     st_title_extra_info->SetMinSize(wxSize(FromDIP(80), -1));
     st_title_extra_info->SetMaxSize(wxSize(FromDIP(80), -1));
     m_st_txt_extra_info->SetMinSize(wxSize(FromDIP(340), -1));
     m_st_txt_extra_info->SetMaxSize(wxSize(FromDIP(340), -1));
     sizer_extra_info->Add(st_title_extra_info, 0, wxALL, 0);
     sizer_extra_info->Add(st_title_extra_info_doc, 0, wxALL, 0);
     sizer_extra_info->Add(m_st_txt_extra_info, 0, wxALL, 0);

     m_sizer_bind_failed_info->Add(m_link_network_state, 0, wxLEFT, 0);
     m_sizer_bind_failed_info->Add(sizer_error_code, 0, wxLEFT, 0);
     m_sizer_bind_failed_info->Add(0, 0, 0, wxTOP, FromDIP(3));
     m_sizer_bind_failed_info->Add(sizer_error_desc, 0, wxLEFT, 0);
     m_sizer_bind_failed_info->Add(0, 0, 0, wxTOP, FromDIP(3));
     m_sizer_bind_failed_info->Add(sizer_extra_info, 0, wxLEFT, 0);

     m_simplebook = new wxSimplebook(this, wxID_ANY, wxDefaultPosition,BIND_DIALOG_BUTTON_PANEL_SIZE, 0);
     m_simplebook->SetBackgroundColour(*wxWHITE);

     m_status_bar = std::make_shared<BBLStatusBarBind>(m_simplebook);

     m_worker = std::make_unique<PlaterWorker<BoostThreadWorker>>(this, m_status_bar, "bind_worker");

     auto        button_panel   = new wxPanel(m_simplebook, wxID_ANY, wxDefaultPosition, BIND_DIALOG_BUTTON_PANEL_SIZE);
     button_panel->SetBackgroundColour(*wxWHITE);
     wxBoxSizer *m_sizer_button = new wxBoxSizer(wxHORIZONTAL);
     m_button_bind = new Button(button_panel, _L("Confirm"));
     m_button_bind->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
     m_button_bind->Enable(false);

     m_button_cancel = new Button(button_panel, _L("Cancel"));
     m_button_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

     m_sizer_button->Add(m_button_bind, 0, wxALIGN_CENTER, 0);
     m_sizer_button->AddSpacer(ButtonProps::ChoiceButtonGap());
     m_sizer_button->Add(m_button_cancel, 0, wxALIGN_CENTER, 0);
     button_panel->SetSizer(m_sizer_button);
     button_panel->Layout();
     m_sizer_button->Fit(button_panel);

     m_simplebook->AddPage(m_status_bar->get_panel(), wxEmptyString, false);
     m_simplebook->AddPage(button_panel, wxEmptyString, false);

     //m_sizer_main->Add(m_sizer_button, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(30));

     show_bind_failed_info(false);


     m_sizer_main->Add(m_sizer_status_text, 0, wxALIGN_CENTER, FromDIP(40));
     m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(10));
     m_sizer_main->Add(m_panel_agreement, 0, wxALIGN_CENTER, 0);
     m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(10));
     m_sizer_main->Add(m_sw_bind_failed_info, 0, wxALIGN_CENTER, 0);
     m_sizer_main->Add(m_simplebook, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, ButtonProps::ChoiceButtonGap());

     SetSizer(m_sizer_main);
     Layout();
     Fit();
     Centre(wxBOTH);

     Bind(wxEVT_SHOW, &BindMachineDialog::on_show, this);
     Bind(wxEVT_CLOSE_WINDOW, &BindMachineDialog::on_close, this);

     m_button_bind->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(BindMachineDialog::on_bind_printer), NULL, this);
     m_button_cancel->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(BindMachineDialog::on_cancel), NULL, this);
     this->Connect(EVT_BIND_MACHINE_FAIL, wxCommandEventHandler(BindMachineDialog::on_bind_fail), NULL, this);
     this->Connect(EVT_BIND_MACHINE_SUCCESS, wxCommandEventHandler(BindMachineDialog::on_bind_success), NULL, this);
     this->Connect(EVT_BIND_UPDATE_MESSAGE, wxCommandEventHandler(BindMachineDialog::on_update_message), NULL, this);
     m_simplebook->SetSelection(1);

     wxGetApp().UpdateDlgDarkUI(this);
 }

 BindMachineDialog::~BindMachineDialog()
 {
     m_button_bind->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(BindMachineDialog::on_bind_printer), NULL, this);
     m_button_cancel->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(BindMachineDialog::on_cancel), NULL, this);
     this->Disconnect(EVT_BIND_MACHINE_FAIL, wxCommandEventHandler(BindMachineDialog::on_bind_fail), NULL, this);
     this->Disconnect(EVT_BIND_MACHINE_SUCCESS, wxCommandEventHandler(BindMachineDialog::on_bind_success), NULL, this);
     this->Disconnect(EVT_BIND_UPDATE_MESSAGE, wxCommandEventHandler(BindMachineDialog::on_update_message), NULL, this);
 }

 wxString BindMachineDialog::get_print_error(wxString str)
 {
     wxString extra;
     try {
         json j = json::parse(str.utf8_string());
         if (j.contains("err_code")) {
             int error_code = j["err_code"].get<int>();
             extra = wxGetApp().get_hms_query()->query_print_error_msg(m_machine_info, error_code);
         }
     }
     catch (...) {
         ;
     }

     if (extra.empty())
         extra = str;

     return extra;
 }

 void BindMachineDialog::show_bind_failed_info(bool show, int code, wxString description, wxString extra)
 {
     if (show) {
         if (!m_sw_bind_failed_info->IsShown()) {
             m_sw_bind_failed_info->Show(true);
             m_result_extra = get_print_error(m_result_extra);
             m_st_txt_error_code->SetLabelText(wxString::Format("%d", m_result_code));
             m_st_txt_error_desc->SetLabelText( wxGetApp().filter_string(m_result_info));
             m_st_txt_extra_info->SetLabelText( wxGetApp().filter_string(m_result_extra));

             m_st_txt_error_code->Wrap(FromDIP(330));
             m_st_txt_error_desc->Wrap(FromDIP(330));
             m_st_txt_extra_info->Wrap(FromDIP(330));
         }
         else {
             m_sw_bind_failed_info->Show(false);
         }
         Layout();
         Fit();
     }
     else {
         if (!m_sw_bind_failed_info->IsShown()) { return; }
         m_sw_bind_failed_info->Show(false);
         m_st_txt_error_code->SetLabelText(wxEmptyString);
         m_st_txt_error_desc->SetLabelText(wxEmptyString);
         m_st_txt_extra_info->SetLabelText(wxEmptyString);
         Layout();
         Fit();
     }
 }

 void BindMachineDialog::on_cancel(wxCommandEvent &event)
 {
     on_destroy();
     EndModal(wxID_CANCEL);
 }

 void BindMachineDialog::on_destroy()
 {
     m_worker.get()->cancel_all();
 }

 void BindMachineDialog::on_close(wxCloseEvent &event)
 {
     on_destroy();
     event.Skip();
 }

 void BindMachineDialog::on_bind_fail(wxCommandEvent &event)
 {
    m_simplebook->SetSelection(1);
    m_link_show_error->Show(true);
    m_static_bitmap_show_error->Show(true);

    m_result_code = event.GetInt();
    m_result_info = get_fail_reason(event.GetInt());
    m_result_extra = event.GetString();

    show_bind_failed_info(true, event.GetInt(), get_fail_reason(event.GetInt()), event.GetString());
 }

 void BindMachineDialog::on_update_message(wxCommandEvent &event)
 {
     m_status_text->SetLabelText(event.GetString());
 }

 void BindMachineDialog::on_bind_success(wxCommandEvent &event)
 {
     EndModal(wxID_OK);
     MessageDialog msg_wingow(nullptr, _L("Log in successful."), "", wxAPPLY | wxOK);
     msg_wingow.ShowModal();
     if(m_machine_info) wxGetApp().on_start_subscribe_again(m_machine_info->get_dev_id());
 }

 void BindMachineDialog::on_bind_printer(wxCommandEvent &event)
 {
     m_result_code = 0;
     m_result_extra = wxEmptyString;
     m_result_info = wxEmptyString;
     m_link_show_error->Hide();
     m_static_bitmap_show_error->Hide();
     show_bind_failed_info(false);

     //check isset info
     if (m_machine_info == nullptr || m_machine_info == NULL) return;

     //check dev_id
     if (m_machine_info->get_dev_id().empty()) return;

     // update ota version
     NetworkAgent* agent = wxGetApp().getAgent();
     if (agent)
         agent->track_update_property("dev_ota_version", m_machine_info->get_ota_version());

     m_simplebook->SetSelection(0);
     auto m_bind_job = std::make_unique<BindJob>(
        m_machine_info->get_dev_id(), m_machine_info->get_dev_ip(), m_machine_info->bind_sec_link, m_machine_info->bind_ssdp_version);

     if (m_machine_info && (m_machine_info->get_printer_series() == PrinterSeries::SERIES_X1)) {
         m_bind_job->set_improved(false);
     }
     else {
         m_bind_job->set_improved(m_allow_notice);
     }

     m_bind_job->set_event_handle(this);
     replace_job(*m_worker, std::move(m_bind_job));
 }

void BindMachineDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    m_button_bind->Rescale(); // ORCA
    m_button_cancel->Rescale(); // ORCA
}

void BindMachineDialog::update_machine_info(MachineObject* info)
{
    m_machine_info = info;
    if (m_machine_info && (m_machine_info->get_printer_series() == PrinterSeries::SERIES_X1)) {
        m_button_bind->Enable(true);
        m_panel_agreement->Hide();
    }
    else {
        m_button_bind->Enable(false);
        m_panel_agreement->Show();
    }
    Layout();
    Fit();
}

void BindMachineDialog::on_show(wxShowEvent &event)
{
    m_allow_privacy = false;
    m_allow_notice  = false;
    m_result_code   = 0;
    m_result_extra  = wxEmptyString;
    m_result_info   = wxEmptyString;

    if (event.IsShown()) {
        auto img = m_machine_info->get_printer_thumbnail_img_str();
        if (wxGetApp().dark_mode()) { img += "_dark"; }
        try {
            auto bitmap = create_scaled_bitmap(img, this, FromDIP(80));
            m_printer_img->SetBitmap(bitmap);
        }
        catch (...){}

        m_printer_img->Refresh();
        m_printer_img->Show();

        m_printer_name->SetLabelText(from_u8(m_machine_info->get_dev_name()));

        if (wxGetApp().is_user_login()) {
            wxString username_text = from_u8(wxGetApp().getAgent()->get_user_nickanme());
            m_user_name->SetLabelText(username_text);

            std::string avatar_url = wxGetApp().getAgent()->get_user_avatar();
            Slic3r::Http http = Slic3r::Http::get(avatar_url);
            std::string  suffix = avatar_url.substr(avatar_url.find_last_of(".") + 1);
            http.header("accept", "image/" + suffix)
                .on_complete([this, time = std::weak_ptr<int>(m_tocken)](std::string body, unsigned int status) {
                if (time.expired()) return;
                wxMemoryInputStream stream(body.data(), body.size());
                wxImage             avatar_image;
                if (avatar_image.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
                    if (avatar_image.IsOk() && m_avatar) {
                        avatar_image.Rescale(this->FromDIP(60), this->FromDIP(60));
                        CallAfter([this, avatar_image]() {
                            auto bitmap = new wxBitmap(avatar_image);
                            m_avatar->SetBitmap(*bitmap);
                            Layout();
                            });
                    }
                }
                    })
                .on_error([this](std::string body, std::string error, unsigned status) {
                        //BOOST_LOG_TRIVIAL(info) << "load oss picture failed, oss path: " << oss_path << " status:" << status << " error:" << error;
            }).perform();
        }
        Layout();
        event.Skip();
    }
}


UnBindMachineDialog::UnBindMachineDialog(Plater *plater /*= nullptr*/)
     : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Log out printer"), wxDefaultPosition, wxDefaultSize, wxCAPTION)
 {
    m_tocken.reset(new int(0));

     SetBackgroundColour(*wxWHITE);
     wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
     auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
     m_line_top->SetBackgroundColour(wxColour("#A6A9AA"));
     m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);
     m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(38));

     wxBoxSizer *m_sizer_body = new wxBoxSizer(wxHORIZONTAL);

     auto  m_panel_left = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(201), FromDIP(212)), wxBORDER_NONE);
     m_panel_left->SetMinSize(wxSize(FromDIP(201), FromDIP(212)));
     m_panel_left->SetCornerRadius(FromDIP(8));
     m_panel_left->SetBackgroundColor(BIND_DIALOG_GREY200);
     wxBoxSizer *m_sizere_left_h = new wxBoxSizer(wxHORIZONTAL);
     wxBoxSizer *m_sizere_left_v= new wxBoxSizer(wxVERTICAL);

     m_printer_img = new wxStaticBitmap(m_panel_left, wxID_ANY, create_scaled_bitmap("printer_thumbnail", nullptr, FromDIP(100)), wxDefaultPosition, wxSize(FromDIP(120), FromDIP(120)), 0);
     m_printer_img->SetBackgroundColour(BIND_DIALOG_GREY200);
     m_printer_img->Hide();
     m_printer_name     = new wxStaticText(m_panel_left, wxID_ANY, wxEmptyString);
     m_printer_name->SetFont(::Label::Head_14);
     m_printer_name->SetForegroundColour(*wxBLACK);
     m_printer_name->SetBackgroundColour(BIND_DIALOG_GREY200);
     m_sizere_left_v->Add(m_printer_img, 0, wxALIGN_CENTER, 0);
     m_sizere_left_v->Add(0, 0, 0, wxTOP, 5);
     m_sizere_left_v->Add(m_printer_name, 0, wxALIGN_CENTER, 0);
     m_sizere_left_h->Add(m_sizere_left_v, 1, wxALIGN_CENTER, 0);

     m_panel_left->SetSizer(m_sizere_left_h);
     m_panel_left->Layout();
     m_sizer_body->Add(m_panel_left, 0, wxEXPAND, 0);

     auto m_bind_icon = create_scaled_bitmap("unbind_machine", nullptr, 28);
     m_sizer_body->Add(new wxStaticBitmap(this, wxID_ANY, m_bind_icon, wxDefaultPosition, wxSize(FromDIP(36), FromDIP(28)), 0), 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(20));

     auto m_panel_right = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(201), FromDIP(212)), wxBORDER_NONE);
     m_panel_right->SetMinSize(wxSize(FromDIP(201), FromDIP(212)));
     m_panel_right->SetCornerRadius(FromDIP(8));
     m_panel_right->SetBackgroundColor(BIND_DIALOG_GREY200);
     m_user_name = new wxStaticText(m_panel_right, wxID_ANY, wxEmptyString);
     m_user_name->SetForegroundColour(*wxBLACK);
     m_user_name->SetBackgroundColour(BIND_DIALOG_GREY200);
     m_user_name->SetFont(::Label::Head_14);
     wxBoxSizer *m_sizer_right_h = new wxBoxSizer(wxHORIZONTAL);
     wxBoxSizer *m_sizer_right_v = new wxBoxSizer(wxVERTICAL);

     m_avatar = new wxStaticBitmap(m_panel_right, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(60), FromDIP(60)), 0);
     m_sizer_right_v->Add(m_avatar, 0, wxALIGN_CENTER, 0);
     m_sizer_right_v->Add(0, 0, 0, wxTOP, 7);
     m_sizer_right_v->Add(m_user_name, 0, wxALIGN_CENTER, 0);
     m_sizer_right_h->Add(m_sizer_right_v, 1, wxALIGN_CENTER, 0);

     m_panel_right->SetSizer(m_sizer_right_h);
     m_panel_right->Layout();
     m_sizer_body->Add(m_panel_right, 0, wxEXPAND, 0);

     m_sizer_main->Add(m_sizer_body, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(30));

     m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(20));

     m_status_text = new wxStaticText(this, wxID_ANY, _L("Would you like to log out the printer?"), wxDefaultPosition, wxSize(BIND_DIALOG_BUTTON_PANEL_SIZE.x, -1), wxST_ELLIPSIZE_END);
     m_status_text->SetForegroundColour(wxColour(107, 107, 107));
     m_status_text->SetFont(::Label::Body_13);



     wxBoxSizer *m_sizer_button = new wxBoxSizer(wxHORIZONTAL);

     m_sizer_button->Add(0, 0, 1, wxEXPAND, 5);
     m_button_unbind = new Button(this, _L("Confirm"));
     m_button_unbind->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);

     m_button_cancel = new Button(this, _L("Cancel"));
     m_button_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

     m_sizer_button->Add(m_button_unbind, 0, wxALIGN_CENTER, 0);
     m_sizer_button->Add(0, 0, 0, wxLEFT, ButtonProps::ChoiceButtonGap());
     m_sizer_button->Add(m_button_cancel, 0, wxALIGN_CENTER, 0);

     m_sizer_main->Add(m_status_text, 0, wxALIGN_CENTER, 0);
     m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(10));
     m_sizer_main->Add(m_sizer_button, 0, wxALIGN_RIGHT | wxRIGHT, ButtonProps::ChoiceButtonGap());
     m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(20));

     SetSizer(m_sizer_main);
     Layout();
     Fit();
     Centre(wxBOTH);

     Bind(wxEVT_SHOW, &UnBindMachineDialog::on_show, this);
     m_button_unbind->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(UnBindMachineDialog::on_unbind_printer), NULL, this);
     m_button_cancel->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(UnBindMachineDialog::on_cancel), NULL, this);


     wxGetApp().UpdateDlgDarkUI(this);
 }

 UnBindMachineDialog::~UnBindMachineDialog()
 {
     m_button_unbind->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(UnBindMachineDialog::on_unbind_printer), NULL, this);
     m_button_cancel->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(UnBindMachineDialog::on_cancel), NULL, this);
 }


void UnBindMachineDialog::on_cancel(wxCommandEvent &event)
{
    EndModal(wxID_CANCEL);
}

void UnBindMachineDialog::on_unbind_printer(wxCommandEvent &event)
{
    if (!wxGetApp().is_user_login()) {
        m_status_text->SetLabelText(_L("Please log in first."));
        return;
    }

    if (!m_machine_info) {
        m_status_text->SetLabelText(_L("There was a problem connecting to the printer. Please try again."));
        return;
    }

    m_machine_info->set_access_code("");
    int result = wxGetApp().request_user_unbind(m_machine_info->get_dev_id());
    if (result == 0) {
        DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (!dev) return;
        // clean local machine access code info
        MachineObject* obj = dev->get_local_machine(m_machine_info->get_dev_id());
        if (obj) {
            obj->set_access_code("");
        }
        dev->erase_user_machine(m_machine_info->get_dev_id());

        m_status_text->SetLabelText(_L("Log out successful."));
        m_button_cancel->SetLabel(_L("Close"));
        m_button_unbind->Hide();
        EndModal(wxID_OK);
    }
    else {
        m_status_text->SetLabelText(_L("Failed to log out."));
        EndModal(wxID_CANCEL);
        return;
    }
}

 void UnBindMachineDialog::on_dpi_changed(const wxRect &suggested_rect)
{
      m_button_unbind->Rescale(); // ORCA
      m_button_cancel->Rescale(); // ORCA
}

void UnBindMachineDialog::on_show(wxShowEvent &event)
{
    if (event.IsShown()) {
        auto img = m_machine_info->get_printer_thumbnail_img_str();
        if (wxGetApp().dark_mode()) { img += "_dark"; }
        try {
            auto bitmap = create_scaled_bitmap(img, this, FromDIP(80));
            m_printer_img->SetBitmap(bitmap);
        } catch (...) {}

        m_printer_img->Refresh();
        m_printer_img->Show();

        m_printer_name->SetLabelText(from_u8(m_machine_info->get_dev_name()));


        if (wxGetApp().is_user_login()) {
            wxString username_text = from_u8(wxGetApp().getAgent()->get_user_name());
            m_user_name->SetLabelText(username_text);

            std::string avatar_url = wxGetApp().getAgent()->get_user_avatar();
            Slic3r::Http http = Slic3r::Http::get(avatar_url);
            std::string  suffix = avatar_url.substr(avatar_url.find_last_of(".") + 1);
            http.header("accept", "image/" + suffix)
                .on_complete([this, time = std::weak_ptr<int>(m_tocken)](std::string body, unsigned int status) {
                if (time.expired()) return;
                wxMemoryInputStream stream(body.data(), body.size());
                wxImage             avatar_image;
                if (avatar_image.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
                    if (avatar_image.IsOk() && m_avatar) {
                        avatar_image.Rescale(this->FromDIP(60), this->FromDIP(60));
                        CallAfter([this, avatar_image]() {
                            auto bitmap = new wxBitmap(avatar_image);
                            m_avatar->SetBitmap(*bitmap);
                            Layout();
                            });
                    }
                }
                    })
                .on_error([this](std::string body, std::string error, unsigned status) {
                        //BOOST_LOG_TRIVIAL(info) << "load oss picture failed, oss path: " << oss_path << " status:" << status << " error:" << error;
                }).perform();

        }

        Layout();
        event.Skip();
    } 
}

}} // namespace Slic3r::GUI
