#include "BBLStatusBarBind.hpp"

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


namespace Slic3r {


BBLStatusBarBind::BBLStatusBarBind(wxWindow *parent, int id)
 : m_self{new wxPanel(parent, id == -1 ? wxID_ANY : id)} 
    , m_sizer(new wxBoxSizer(wxHORIZONTAL))
{
    m_self->SetBackgroundColour(wxColour(255,255,255));
    m_self->SetMinSize(wxSize(m_self->FromDIP(450), m_self->FromDIP(30)));

    m_prog = new wxGauge(m_self, wxID_ANY, 100, wxDefaultPosition, wxSize(m_self->FromDIP(400), m_self->FromDIP(6)), wxGA_HORIZONTAL);
    m_prog->SetValue(0);

    m_stext_percent = new wxStaticText(m_self, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0);
    m_stext_percent->SetForegroundColour(wxColour(107, 107, 107));
    m_stext_percent->SetFont(::Label::Body_13);
    m_stext_percent->Wrap(-1);

    m_sizer->Add(m_prog, 1, wxALIGN_CENTER, 0);
    m_sizer->Add(0, 0, 1, wxEXPAND, 0);
    m_sizer->Add(m_stext_percent, 1, wxALIGN_CENTER, 0);

    m_self->SetSizer(m_sizer);
    m_self->Layout();
    m_sizer->Fit(m_self);
}

void BBLStatusBarBind::set_prog_block()
{
}

int BBLStatusBarBind::get_progress() const
{
    return m_prog->GetValue();
}

void BBLStatusBarBind::set_progress(int val)
{
    set_prog_block();

    if(val < 0)
        return;

    if (!m_sizer->IsShown(m_prog)) {
        m_sizer->Show(m_prog);
        m_sizer->Show(m_cancelbutton);
    }
    m_prog->SetValue(val);
    set_percent_text(wxString::Format("%d%%", val));
    m_sizer->Layout();
}

int BBLStatusBarBind::get_range() const
{
    return m_prog->GetRange();
}

void BBLStatusBarBind::set_range(int val)
{
    if(val != m_prog->GetRange()) {
        m_prog->SetRange(val);
    }
}

void BBLStatusBarBind::clear_percent()
{

}

void BBLStatusBarBind::show_error_info(wxString msg, int code, wxString description, wxString extra)
{

}

void BBLStatusBarBind::show_progress(bool show)
{
    if (show) {
        m_sizer->Show(m_prog);
        m_sizer->Layout();
    }
    else {
        //m_sizer->Hide(m_prog);
        m_sizer->Layout();
    }
}

void BBLStatusBarBind::start_busy(int rate)
{
    m_busy = true;
    show_progress(true);
    show_cancel_button();
}

void BBLStatusBarBind::stop_busy()
{
    show_progress(false);
    hide_cancel_button();
    m_prog->SetValue(0);
    m_sizer->Layout();
    m_busy = false;
}

void BBLStatusBarBind::set_cancel_callback_fina(BBLStatusBarBind::CancelFn ccb) 
{ 
    m_cancel_cb_fina = ccb; 
     if (ccb) {
        m_sizer->Show(m_cancelbutton);
    } else {
        m_sizer->Hide(m_cancelbutton);
    }
}

void BBLStatusBarBind::set_cancel_callback(BBLStatusBarBind::CancelFn ccb) {
    /*  m_cancel_cb = ccb;
      if (ccb) {
          m_sizer->Show(m_cancelbutton);
      }
      else {
          m_sizer->Hide(m_cancelbutton);
      }
      m_sizer->Layout();*/
}

wxPanel* BBLStatusBarBind::get_panel()
{
    return m_self;
}

void BBLStatusBarBind::set_status_text(const wxString& txt)
{
    //auto txtss = "Sending the printing task has timed out.\nPlease try again!";
    //auto txtss = "The printing project is being uploaded... 25%%";
    //m_status_text->SetLabelText(txtss);
    //m_status_text->SetLabelText(txt);
}

void BBLStatusBarBind::set_percent_text(const wxString &txt)
{
    m_stext_percent->SetLabelText(txt);
}

void BBLStatusBarBind::set_status_text(const std::string& txt)
{ 
    this->set_status_text(txt.c_str());
}

void BBLStatusBarBind::set_status_text(const char *txt)
{ 
    this->set_status_text(wxString::FromUTF8(txt));
}

void BBLStatusBarBind::msw_rescale() { 
    set_prog_block();
    m_cancelbutton->SetMinSize(wxSize(m_self->FromDIP(56), m_self->FromDIP(24)));
}

wxString BBLStatusBarBind::get_status_text() const
{
    return m_status_text->GetLabelText();
}

bool BBLStatusBarBind::update_status(wxString &msg, bool &was_cancel, int percent, bool yield)
{
    set_status_text(msg);

    if (percent >= 0)
        this->set_progress(percent);

    if (yield)
        wxEventLoopBase::GetActive()->YieldFor(wxEVT_CATEGORY_UI | wxEVT_CATEGORY_USER_INPUT);
    was_cancel = m_was_cancelled;
    return true;
}

void BBLStatusBarBind::reset()
{
    set_status_text("");
    m_was_cancelled = false;
    set_progress(0);
}


void BBLStatusBarBind::set_font(const wxFont &font)
{
    m_self->SetFont(font);
}

void BBLStatusBarBind::show_cancel_button()
{
    m_sizer->Show(m_cancelbutton);
    m_sizer->Layout();
}

void BBLStatusBarBind::hide_cancel_button()
{
    m_sizer->Hide(m_cancelbutton);
    m_sizer->Layout();
}

}
