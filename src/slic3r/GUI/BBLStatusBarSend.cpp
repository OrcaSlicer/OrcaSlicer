#include "BBLStatusBarSend.hpp"

#include <wx/timer.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/statusbr.h>
#include <wx/frame.h>
#include "wx/evtloop.h"
#include <wx/gdicmn.h>
#include "GUI_App.hpp"

#include "I18N.hpp"

#include <iostream>
#include <regex>

namespace Slic3r {

wxDEFINE_EVENT(EVT_SHOW_ERROR_INFO_SEND, wxCommandEvent);
wxDEFINE_EVENT(EVT_SHOW_ERROR_FAIL_SEND, wxCommandEvent);

BBLStatusBarSend::BBLStatusBarSend(wxWindow *parent, int id)
 : m_self{new wxPanel(parent, id == -1 ? wxID_ANY : id)}
    , m_sizer(new wxBoxSizer(wxHORIZONTAL))
{
    m_self->SetBackgroundColour(wxColour(255,255,255));

    wxBoxSizer *m_sizer_body = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *m_sizer_bottom = new wxBoxSizer(wxHORIZONTAL);

    m_status_text = new wxStaticText(m_self, wxID_ANY, wxEmptyString);
    m_status_text->SetForegroundColour(wxColour(107, 107, 107));
    m_status_text->SetFont(::Label::Body_13);
    m_status_text->SetMaxSize(wxSize(m_self->FromDIP(360), m_self->FromDIP(40)));

    m_prog = new wxGauge(m_self, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, m_self->FromDIP(6)), wxGA_HORIZONTAL);
    m_prog->SetMinSize(wxSize(m_self->FromDIP(300),m_self->FromDIP(6)));
    m_prog->SetValue(0);

    //StateColor btn_bd_white(std::pair<wxColour, int>(*wxWHITE, StateColor::Disabled), std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));

    m_cancelbutton = new Button(m_self, _L("Cancel"));
    m_cancelbutton->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_cancelbutton->Bind(wxEVT_BUTTON,
        [this](wxCommandEvent &evt) {
        cancel();
    });

    m_stext_percent = new wxStaticText(m_self, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0);
    m_stext_percent->SetForegroundColour(wxColour(107, 107, 107));
    m_stext_percent->SetFont(::Label::Body_13);
    m_stext_percent->Wrap(-1);

    m_sizer_status_text = new wxBoxSizer(wxHORIZONTAL);
    m_link_show_error = new Label(m_self, _L("Check the reason"));
    m_link_show_error->SetForegroundColour(wxColour("#6b6b6b"));
    m_link_show_error->SetFont(::Label::Head_13);

    m_bitmap_show_error_close = create_scaled_bitmap("link_more_error_close", nullptr, 7);
    m_bitmap_show_error_open = create_scaled_bitmap("link_more_error_open", nullptr, 7);
    m_static_bitmap_show_error = new wxStaticBitmap(m_self, wxID_ANY, m_bitmap_show_error_open, wxDefaultPosition, wxSize(m_self->FromDIP(7), m_self->FromDIP(7)));

    m_link_show_error->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {this->m_self->SetCursor(wxCURSOR_HAND); });
    m_link_show_error->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {this->m_self->SetCursor(wxCURSOR_ARROW); });
    m_link_show_error->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        if (!m_show_error_info_state) { m_show_error_info_state = true; m_static_bitmap_show_error->SetBitmap(m_bitmap_show_error_close); }
        else { m_show_error_info_state = false; m_static_bitmap_show_error->SetBitmap(m_bitmap_show_error_open); }
        wxCommandEvent* evt = new wxCommandEvent(EVT_SHOW_ERROR_INFO_SEND);
        wxQueueEvent(this->m_self->GetParent(), evt);
    });


    m_link_show_error->Hide();
    m_static_bitmap_show_error->Hide();


    m_static_bitmap_show_error->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {this->m_self->SetCursor(wxCURSOR_HAND); });
    m_static_bitmap_show_error->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {this->m_self->SetCursor(wxCURSOR_ARROW); });
    m_static_bitmap_show_error->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        if (!m_show_error_info_state) {m_show_error_info_state = true;m_static_bitmap_show_error->SetBitmap(m_bitmap_show_error_close);}
        else {m_show_error_info_state = false;m_static_bitmap_show_error->SetBitmap(m_bitmap_show_error_open);}
        wxCommandEvent* evt = new wxCommandEvent(EVT_SHOW_ERROR_INFO_SEND);
        wxQueueEvent(this->m_self->GetParent(), evt);
    });


    m_sizer_status_text->Add(m_link_show_error, 0, wxLEFT | wxALIGN_CENTER, 0);
    m_sizer_status_text->Add(m_static_bitmap_show_error, 0, wxLEFT | wxTOP| wxALIGN_CENTER, m_self->FromDIP(2));

    m_sizer_bottom->Add(m_prog, 1, wxALIGN_CENTER, 0);
    m_sizer_bottom->Add(m_stext_percent, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, 10);
    m_sizer_bottom->Add(m_sizer_status_text, 0, wxALIGN_CENTER, 10);
    m_sizer_bottom->Add(0, 0, 1, wxEXPAND, 0);
    m_sizer_bottom->Add(m_cancelbutton, 0, wxALIGN_CENTER, 0);

    m_sizer_body->Add(0, 0, 1, wxEXPAND, 0);
    m_sizer_body->Add(m_status_text, 0, wxEXPAND, 0);
    m_sizer_body->Add(m_sizer_bottom, 0, wxEXPAND, 0);
    m_sizer_body->Add(0, 0, 1, wxEXPAND, 0);

    m_sizer->Add(m_sizer_body, 1, wxALIGN_CENTER, 0);

    m_self->SetSizer(m_sizer);
    m_self->Layout();
    m_sizer->Fit(m_self);
}

void BBLStatusBarSend::set_prog_block()
{
}

int BBLStatusBarSend::get_progress() const
{
    return m_prog->GetValue();
}

void BBLStatusBarSend::set_progress(int val)
{
    if(val < 0) return;

    //add the logic for arrange/orient jobs, which don't call stop_busy
    if (!m_prog->IsShown()) {
        m_sizer->Show(m_prog);
        m_sizer->Show(m_cancelbutton);
    }
    m_prog->SetValue(val);
    set_percent_text(wxString::Format("%d%%", val));

    m_sizer->Layout();
}

int BBLStatusBarSend::get_range() const
{
    return m_prog->GetRange();
}

void BBLStatusBarSend::set_range(int val)
{
    if(val != m_prog->GetRange()) {
        m_prog->SetRange(val);
    }
}

void BBLStatusBarSend::clear_percent()
{
    //set_percent_text(wxEmptyString);
    m_cancelbutton->Hide();
}

void BBLStatusBarSend::show_error_info(wxString msg, int code, wxString description, wxString extra)
{
    set_status_text(msg);
    m_prog->Hide();
    m_stext_percent->Hide();
    m_link_show_error->Show();
    m_static_bitmap_show_error->Show();

    m_cancelbutton->Show();
    m_self->Layout();
    m_sizer->Layout();

    wxCommandEvent* evt = new wxCommandEvent(EVT_SHOW_ERROR_FAIL_SEND);
    wxQueueEvent(this->m_self->GetParent(), evt);
}

void BBLStatusBarSend::show_progress(bool show)
{
    if (show) {
        m_sizer->Show(m_prog);
        m_sizer->Layout();
    }
    else {
        m_sizer->Hide(m_prog);
        m_sizer->Layout();
    }
}

void BBLStatusBarSend::start_busy(int rate)
{
    m_busy = true;
    show_progress(true);
    show_cancel_button();
}

void BBLStatusBarSend::stop_busy()
{
    show_progress(false);
    hide_cancel_button();
    m_prog->SetValue(0);
    m_sizer->Layout();
    m_busy = false;
}

void BBLStatusBarSend::set_cancel_callback_fina(BBLStatusBarSend::CancelFn ccb)
{
    m_cancel_cb_fina = ccb;
     if (ccb) {
        m_sizer->Show(m_cancelbutton);
    } else {
        m_sizer->Hide(m_cancelbutton);
    }
}

void BBLStatusBarSend::set_cancel_callback(BBLStatusBarSend::CancelFn ccb) {
    /*  m_cancel_cb = ccb;
      if (ccb) {
          m_sizer->Show(m_cancelbutton);
      }
      else {
          m_sizer->Hide(m_cancelbutton);
      }
      m_sizer->Layout();*/
}

wxPanel* BBLStatusBarSend::get_panel()
{
    return m_self;
}

bool BBLStatusBarSend::is_english_text(wxString str)
{
    std::regex reg("^[0-9a-zA-Z]+$");
    std::smatch matchResult;

    std::string pattern_Special = "{}[]<>~!@#$%^&*(),.?/ :";
    for (auto i = 0; i < str.Length(); i++) {
        std::string regex_str = wxString(str[i]).ToStdString();
        if (std::regex_match(regex_str, matchResult, reg)) {
            continue;
        }
        else {
            int result = pattern_Special.find(regex_str.c_str());
            if (result < 0 || result > pattern_Special.length()) {
                return false;
            }
        }
    }
    return true;
}

bool BBLStatusBarSend::format_text(wxStaticText* dc, int width, const wxString& text, wxString& multiline_text)
{
    bool multiline = false;
    multiline_text = text;
    if (width > 0 && dc->GetTextExtent(text).x > width) {
        size_t start = 0;
        while (true) {
            size_t idx = size_t(-1);
            for (size_t i = start; i < multiline_text.Len(); i++) {
                if (multiline_text[i] == ' ') {
                    if (dc->GetTextExtent(multiline_text.SubString(start, i)).x < width)
                        idx = i;
                    else {
                        if (idx == size_t(-1)) idx = i;
                        break;
                    }
                }
            }
            if (idx == size_t(-1)) break;
            multiline = true;
            multiline_text[idx] = '\n';
            start = idx + 1;
            if (dc->GetTextExtent(multiline_text.Mid(start)).x < width) break;
        }
    }
    return multiline;
    //return dc->GetTextExtent(multiline_text);
}


void BBLStatusBarSend::set_status_text(const wxString& txt)
{
    //auto txtss = "Sending the printing task has timed out.\nPlease try again!";
    //auto txtss = "The printing project is being uploaded... 25%%";
    //m_status_text->SetLabelText(txtss);
    //wxString str;
    //format_text(m_status_text, m_self->FromDIP(300), txt, str);

    if (m_status_text->GetTextExtent(txt).x > m_self->FromDIP(360)) {
        m_status_text->SetSize(m_self->FromDIP(360), m_self->FromDIP(40));
    }
    m_status_text->SetLabelText(txt);
    m_status_text->Wrap(m_self->FromDIP(360));
    m_status_text->Layout();
    m_self->Layout();
    //if (is_english_text(str)) m_status_text->Wrap(m_self->FromDIP(280));
}

void BBLStatusBarSend::set_percent_text(const wxString &txt)
{
    m_stext_percent->SetLabelText(txt);
}

void BBLStatusBarSend::set_status_text(const std::string& txt)
{
    this->set_status_text(txt.c_str());
}

void BBLStatusBarSend::set_status_text(const char *txt)
{
    this->set_status_text(wxString::FromUTF8(txt));
    get_panel()->GetParent()->Layout();
    get_panel()->GetParent()->Update();
}

void BBLStatusBarSend::msw_rescale() {
    //set_prog_block();
    m_cancelbutton->SetMinSize(wxSize(m_self->FromDIP(56), m_self->FromDIP(24)));
}

wxString BBLStatusBarSend::get_status_text() const
{
    return m_status_text->GetLabelText();
}

bool BBLStatusBarSend::update_status(wxString &msg, bool &was_cancel, int percent, bool yield)
{
    set_status_text(msg);
    if (percent >= 0)
        this->set_progress(percent);

    if (yield)
        wxEventLoopBase::GetActive()->YieldFor(wxEVT_CATEGORY_UI | wxEVT_CATEGORY_USER_INPUT);
    was_cancel = m_was_cancelled;
    return true;
}

void BBLStatusBarSend::reset()
{
    m_link_show_error->Hide();
    m_static_bitmap_show_error->Hide();
    m_prog->Show();
    m_stext_percent->Show();
    m_cancelbutton->Enable();
    m_cancelbutton->Show();
    m_was_cancelled = false;

    set_status_text("");
    set_progress(0);
    set_percent_text(wxString::Format("%d%%", 0));
}

void BBLStatusBarSend::set_font(const wxFont &font)
{
    m_self->SetFont(font);
}

void BBLStatusBarSend::show_cancel_button()
{
    m_sizer->Show(m_cancelbutton);
    m_sizer->Layout();
}

void BBLStatusBarSend::hide_cancel_button()
{
    m_sizer->Hide(m_cancelbutton);
    m_sizer->Layout();
}

void BBLStatusBarSend::change_button_label(wxString name)
{
    m_cancelbutton->SetLabel(name);
}

void BBLStatusBarSend::disable_cancel_button()
{
    m_cancelbutton->Disable();
}

void BBLStatusBarSend::enable_cancel_button()
{
    m_cancelbutton->Enable();
}

void BBLStatusBarSend::cancel()
{
    m_was_cancelled = true;
    if (m_cancel_cb_fina) m_cancel_cb_fina();
}

}
