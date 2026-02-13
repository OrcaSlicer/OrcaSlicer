#include "BBLStatusBar.hpp"

#include <wx/timer.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/statusbr.h>
#include <wx/frame.h>
#include <wx/statline.h>

#include "GUI_App.hpp"

#include "I18N.hpp"

#include <iostream>

namespace Slic3r {

BBLStatusBar::BBLStatusBar(wxWindow *parent, int id)
    : m_self{new wxPanel(parent, id == -1 ? wxID_ANY : id)}
    , m_prog{new wxGauge(m_self,
                         wxGA_HORIZONTAL,
                         100,
                         wxDefaultPosition,
                         wxSize(120, -1))}
    , m_cancelbutton{new wxButton(m_self,
                                  -1,
                                  _(L("Cancel")),
                                  wxDefaultPosition,
                                  wxDefaultSize)}
    , m_sizer(new wxBoxSizer(wxHORIZONTAL))
    , m_slice_info_sizer(new wxBoxSizer(wxHORIZONTAL))
    , m_object_info_sizer(new wxBoxSizer(wxHORIZONTAL))
{
    m_status_text = new wxStaticText(m_self, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_status_text->SetForegroundColour(*wxBLACK);

    m_object_info = new wxStaticText(m_self, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_object_info->SetForegroundColour(*wxBLACK);

    m_slice_info = new wxStaticText(m_self, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_slice_info->SetForegroundColour(*wxBLACK);

    wxStaticLine* seperator_1 = new wxStaticLine(m_self, wxID_ANY, wxDefaultPosition, wxSize(3, -1), wxLI_VERTICAL);
    wxStaticLine* seperator_2 = new wxStaticLine(m_self, wxID_ANY, wxDefaultPosition, wxSize(3, -1), wxLI_VERTICAL);

    m_object_info_sizer->Add(m_object_info, 1, wxEXPAND | wxALL, 0);
    m_object_info_sizer->Add(seperator_1, 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

    m_slice_info_sizer->Add(m_slice_info, 1, wxEXPAND | wxALL, 0);
    m_slice_info_sizer->Add(seperator_2, 0, wxEXPAND | wxLEFT | wxRIGHT, 5);

    m_cancelbutton->Bind(wxEVT_BUTTON, [this](const wxCommandEvent&) {
        if (m_cancel_cb) 
            m_cancel_cb();
        m_cancelbutton->Hide();
    });

    m_sizer->Add(m_object_info_sizer, 1, wxEXPAND | wxALL | wxALIGN_LEFT, 5);
    m_sizer->Add(m_slice_info_sizer, 1, wxEXPAND | wxALL | wxALIGN_LEFT, 5);
    m_sizer->Add(m_status_text, 1, wxEXPAND | wxALL | wxALIGN_LEFT, 5);
    m_sizer->Add(m_prog, 0, wxEXPAND | wxLEFT | wxALL, 5);
    m_sizer->Add(m_cancelbutton, 0, wxEXPAND | wxALL, 5);
    m_sizer->SetSizeHints(m_self);
    m_self->SetSizer(m_sizer);

    m_sizer->Hide(m_object_info_sizer);
    m_sizer->Hide(m_slice_info_sizer);
    m_sizer->Hide(m_prog);
    m_sizer->Hide(m_cancelbutton);
    m_sizer->Layout();
}

int BBLStatusBar::get_progress() const
{
    return m_prog->GetValue();
}

void BBLStatusBar::set_progress(int val)
{
    if(val < 0)
        return;

    bool need_layout = false;
    //add the logic for arrange/orient jobs, which don't call stop_busy
    if(val == m_prog->GetRange()) {
        m_prog->SetValue(0);
        m_sizer->Hide(m_prog);
        need_layout = true;
    }
    else
    {
        if (m_sizer->IsShown(m_object_info_sizer)) {
            m_sizer->Hide(m_object_info_sizer);
            need_layout = true;
        }

        if (m_sizer->IsShown(m_slice_info_sizer)) {
            m_sizer->Hide(m_slice_info_sizer);
            need_layout = true;
        }

        if (!m_sizer->IsShown(m_prog)) {
            m_sizer->Show(m_prog);
            m_sizer->Show(m_cancelbutton);
            need_layout = true;
        }
        m_prog->SetValue(val);
    }

    if (need_layout) {
        m_sizer->Layout();
    }
}

int BBLStatusBar::get_range() const
{
    return m_prog->GetRange();
}

void BBLStatusBar::set_range(int val)
{
    if(val != m_prog->GetRange()) {
        m_prog->SetRange(val);
    }
}

void BBLStatusBar::clear_percent()
{

}

void BBLStatusBar::show_error_info(wxString msg, int code, wxString description, wxString extra)
{

}

void BBLStatusBar::show_progress(bool show)
{
    if (show) {
        m_sizer->Hide(m_object_info);
        m_sizer->Hide(m_slice_info);

        m_sizer->Show(m_prog);
        m_sizer->Layout();
    }
    else {
        m_sizer->Hide(m_prog);
        m_sizer->Layout();
    }
}

void BBLStatusBar::start_busy(int rate)
{
    m_busy = true;
    show_progress(true);
    show_cancel_button();
}

void BBLStatusBar::stop_busy()
{
    show_progress(false);
    hide_cancel_button();
    m_prog->SetValue(0);
    m_sizer->Show(m_slice_info_sizer);
    m_sizer->Layout();
    m_busy = false;
}

void BBLStatusBar::set_cancel_callback(BBLStatusBar::CancelFn ccb) {
    m_cancel_cb = ccb;
    if (ccb) {
        m_sizer->Show(m_cancelbutton);
    }
    else {
        m_sizer->Hide(m_cancelbutton);
    }
}

wxPanel* BBLStatusBar::get_panel()
{
    return m_self;
}

void BBLStatusBar::set_status_text(const wxString& txt)
{
    m_status_text->SetLabelText(txt);
}

void BBLStatusBar::set_status_text(const std::string& txt)
{ 
    this->set_status_text(txt.c_str());
}

void BBLStatusBar::set_status_text(const char *txt)
{ 
    this->set_status_text(wxString::FromUTF8(txt));
}

wxString BBLStatusBar::get_status_text() const
{
    return m_status_text->GetLabelText();
}

void BBLStatusBar::set_object_info(const wxString& txt)
{
    if (txt == "") {
        m_object_info->SetLabelText("");
        m_sizer->Hide(m_object_info_sizer);
    }
    else {
        if (!m_sizer->IsShown(m_object_info_sizer)) {
            m_sizer->Show(m_object_info_sizer);
        }
        m_object_info->SetLabelText(txt);
    }
    m_sizer->Layout();
}

void BBLStatusBar::set_slice_info(const wxString& txt)
{
    if (!txt.empty()) {
        if (!m_sizer->IsShown(m_slice_info_sizer)) {
            m_sizer->Show(m_slice_info_sizer);
        }
        m_slice_info->SetLabelText(txt);
        m_sizer->Layout();
    }
}

void BBLStatusBar::show_slice_info(bool show)
{
    if (show) {
        m_sizer->Show(m_slice_info_sizer);
        m_sizer->Layout();
    }
    else {
        m_sizer->Hide(m_slice_info_sizer);
        m_sizer->Layout();
    }
}

bool BBLStatusBar::is_slice_info_shown()
{
    return m_sizer->IsShown(m_slice_info_sizer);
}

void BBLStatusBar::set_font(const wxFont &font)
{
    m_self->SetFont(font);
}

void BBLStatusBar::show_cancel_button()
{
    m_sizer->Show(m_cancelbutton);
    m_sizer->Layout();
}

void BBLStatusBar::hide_cancel_button()
{
    m_sizer->Hide(m_cancelbutton);
    m_sizer->Layout();
}

}
