#include "ProgressStatusBar.hpp"

#include <wx/timer.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/statusbr.h>
#include <wx/frame.h>

#include "GUI_App.hpp"

#include "I18N.hpp"

#include <iostream>

namespace Slic3r {

ProgressStatusBar::ProgressStatusBar(wxWindow *parent, int id)
    : self{new wxStatusBar(parent, id == -1 ? wxID_ANY : id)}
    , m_prog{new wxGauge(self,
                         wxGA_HORIZONTAL,
                         100,
                         wxDefaultPosition,
                         wxDefaultSize)}
    , m_cancelbutton{new wxButton(self,
                                  -1,
                                  _(L("Cancel")),
                                  wxDefaultPosition,
                                  wxDefaultSize)}
    , m_timer{new wxTimer(self)}
{
    update_dark_ui();
    m_prog->Hide();
    m_cancelbutton->Hide();

    self->SetFieldsCount(3);
    int w[] = {-1, 150, 155};
    self->SetStatusWidths(3, w);

    wxSize s = m_cancelbutton->GetTextExtent(m_cancelbutton->GetLabel());
    self->SetMinHeight(int(2 * self->GetBorderY() + 1.2 * s.GetHeight()));

    self->Bind(wxEVT_TIMER, [this](const wxTimerEvent&) {
        if (m_prog->IsShown()) m_timer->Stop();
        if(is_busy()) m_prog->Pulse();
    });

    self->Bind(wxEVT_SIZE, [this](wxSizeEvent& event){
        wxRect rect;
        self->GetFieldRect(1, rect);
        auto offset = 0;
        m_cancelbutton->Move(rect.GetX() + offset, rect.GetY() + offset);
        m_cancelbutton->SetSize(rect.GetWidth() - offset, rect.GetHeight());

        self->GetFieldRect(2, rect);
        m_prog->Move(rect.GetX() + offset, rect.GetY() + offset);
        m_prog->SetSize(rect.GetWidth() - offset, rect.GetHeight());

        event.Skip();
    });

    m_cancelbutton->Bind(wxEVT_BUTTON, [this](const wxCommandEvent&) {
        if (m_cancel_cb) 
            m_cancel_cb();
        m_cancelbutton->Hide();
    });
}

ProgressStatusBar::~ProgressStatusBar() {
    if(m_timer && m_timer->IsRunning()) m_timer->Stop();
}

void ProgressStatusBar::update_dark_ui()
{
    GUI::wxGetApp().UpdateDarkUI(self);
    GUI::wxGetApp().UpdateDarkUI(m_prog);
    GUI::wxGetApp().UpdateDarkUI(m_cancelbutton);
}

int ProgressStatusBar::get_progress() const
{
    return m_prog ? m_prog->GetValue() : 0;
}

void ProgressStatusBar::set_progress(int val)
{
    if(!m_prog) return;
    
    if(!m_prog->IsShown()) show_progress(true);
    if(val < 0) return;

    if(val == m_prog->GetRange()) {
        m_prog->SetValue(0);
        show_progress(false);
    }
    else {
        m_prog->SetValue(val);
    }
}

int ProgressStatusBar::get_range() const
{
    return m_prog ? m_prog->GetRange() : 0;
}

void ProgressStatusBar::set_range(int val)
{
    if(m_prog && val != m_prog->GetRange()) {
        m_prog->SetRange(val);
    }
}

void ProgressStatusBar::show_progress(bool show)
{
    if(m_prog) {
        m_prog->Show(show);
        m_prog->Pulse();
    }
}

void ProgressStatusBar::start_busy(int rate)
{
    if(!m_prog) return;
    
    m_busy = true;
    show_progress(true);
    if (!m_timer->IsRunning()) {
        m_timer->Start(rate);
    }
}

void ProgressStatusBar::stop_busy()
{
    if(!m_timer || !m_prog) return;
    
    m_timer->Stop();
    show_progress(false);
    m_prog->SetValue(0);
    m_busy = false;
}

void ProgressStatusBar::set_cancel_callback(ProgressStatusBar::CancelFn ccb) {
    m_cancel_cb = ccb;
    if(m_cancelbutton) {
        if(ccb) m_cancelbutton->Show();
        else m_cancelbutton->Hide();
    }
}

void ProgressStatusBar::run(int rate)
{
    if(m_timer && !m_timer->IsRunning()) {
        m_timer->Start(rate);
    }
}

void ProgressStatusBar::embed(wxFrame *frame)
{
    if(frame) frame->SetStatusBar(self);
}

void ProgressStatusBar::set_status_text(const wxString& txt)
{
	if(self) self->SetStatusText(txt);
}

void ProgressStatusBar::set_status_text(const std::string& txt)
{ 
    this->set_status_text(txt.c_str());
}

void ProgressStatusBar::set_status_text(const char *txt)
{ 
    this->set_status_text(wxString::FromUTF8(txt));
}

wxString ProgressStatusBar::get_status_text() const
{
    return self->GetStatusText();
}

void ProgressStatusBar::set_font(const wxFont &font)
{
    self->SetFont(font);
}

void ProgressStatusBar::show_cancel_button()
{
    if(m_cancelbutton) m_cancelbutton->Show();
}

void ProgressStatusBar::hide_cancel_button()
{
    if(m_cancelbutton) m_cancelbutton->Hide();
}

}

