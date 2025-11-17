#include "BaseTransparentDPIFrame.hpp"

#include <thread>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/dcmemory.h>
#include "GUI_App.hpp"
#include "Tab.hpp"
#include "PartPlate.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/TextInput.hpp"
#include "Notebook.hpp"
#include <chrono>
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "CapsuleButton.hpp"
using namespace Slic3r;
using namespace Slic3r::GUI;

namespace Slic3r { namespace GUI {
#define ANIMATION_REFRESH_INTERVAL 20
BaseTransparentDPIFrame::BaseTransparentDPIFrame(
    wxWindow *parent, int win_width, wxPoint dialog_pos, int ok_button_width, wxString win_text, wxString ok_text, wxString cancel_text, DisappearanceMode disappearance_mode)
    : DPIFrame(static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, "", wxDefaultPosition, wxDefaultSize, !wxCAPTION | !wxCLOSE_BOX | wxBORDER_NONE)
    , m_timed_disappearance_mode(disappearance_mode)
{
    // SetBackgroundStyle(wxBackgroundStyle::wxBG_STYLE_TRANSPARENT);
    SetTransparent(m_init_transparent);
    SetBackgroundColour(wxColour(23, 25, 22, 128));
    //Adaptive Frame Width
    wxClientDC dc(parent);
    wxSize msg_sz = dc.GetMultiLineTextExtent(ok_text);
    auto   ratio = msg_sz.GetX() / (float) win_width;
    if (ratio > 0.75f) {
        win_width += msg_sz.GetX() / 2.0f;
    }

    SetMinSize(wxSize(FromDIP(win_width), -1));
    SetMaxSize(wxSize(FromDIP(win_width), -1));
    SetPosition(dialog_pos);

    m_sizer_main           = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *text_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_sizer->AddSpacer(FromDIP(20));
    auto image_sizer  = new wxBoxSizer(wxVERTICAL);
    auto imgsize      = FromDIP(25);
    auto completedimg = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("completed", this, 25), wxDefaultPosition, wxSize(imgsize, imgsize), 0);
    image_sizer->Add(completedimg, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(0));
    image_sizer->AddStretchSpacer();
    text_sizer->Add(image_sizer);
    text_sizer->AddSpacer(FromDIP(5));
    m_finish_text = new Label(this, win_text, LB_AUTO_WRAP);
    m_finish_text->SetMinSize(wxSize(FromDIP(win_width - 64), -1));
    m_finish_text->SetMaxSize(wxSize(FromDIP(win_width - 64), -1));
    m_finish_text->SetForegroundColour(wxColour(255, 255, 255, 255));
    text_sizer->Add(m_finish_text, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxALL, 0);
    text_sizer->AddSpacer(FromDIP(20));
    m_sizer_main->Add(text_sizer, FromDIP(0), wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxTOP, FromDIP(15));

    wxBoxSizer *bSizer_button = new wxBoxSizer(wxHORIZONTAL);
    bSizer_button->SetMinSize(wxSize(FromDIP(100), -1));
    /* m_checkbox = new wxCheckBox(this, wxID_ANY, _L("Don't show again"), wxDefaultPosition, wxDefaultSize, 0);
     bSizer_button->Add(m_checkbox, 0, wxALIGN_LEFT);*/
    bSizer_button->AddStretchSpacer(1);
    m_button_ok = new Button(this, ok_text);
    m_button_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_button_ok->SetSize(wxSize(FromDIP(60), FromDIP(30)));
    m_button_ok->SetMinSize(wxSize(FromDIP(90), FromDIP(30)));
    bSizer_button->Add(m_button_ok, 0, wxALIGN_RIGHT | wxLEFT | wxTOP, FromDIP(10));

    m_button_ok->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent &e) { deal_ok(); });

    m_button_cancel = new Button(this, cancel_text);
    m_button_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_button_cancel->SetSize(wxSize(FromDIP(65), FromDIP(30)));
    m_button_cancel->SetMinSize(wxSize(FromDIP(65), FromDIP(30)));
    bSizer_button->Add(m_button_cancel, 0, wxALIGN_RIGHT | wxLEFT | wxTOP, FromDIP(10));

    m_button_cancel->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent &e) { deal_cancel(); });

    m_sizer_main->Add(bSizer_button, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));

    Bind(wxEVT_CLOSE_WINDOW, [this](auto &e) {
        on_hide();
    });
    SetSizer(m_sizer_main);
    Layout();
    Fit();

    if (m_timed_disappearance_mode != DisappearanceMode::None) {
        init_timer();
        Bind(wxEVT_TIMER, &BaseTransparentDPIFrame::on_timer, this);
        Bind(wxEVT_ENTER_WINDOW, [this](auto &e) {
            if (m_enter_window_valid) {
                clear_timer_count();
                m_display_stage = 0;
                m_refresh_timer->Stop();
                SetTransparent(m_init_transparent);
            }
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](auto &e) {
            auto x    = e.GetX();
            auto y    = e.GetY();
            auto size = this->GetClientSize();
            if (x >= 0 && y >= 0 && x <= size.x && y <= size.y) { return; }
            if (m_enter_window_valid) {
                m_refresh_timer->Start(ANIMATION_REFRESH_INTERVAL);
            }
        });
    }
}

BaseTransparentDPIFrame::~BaseTransparentDPIFrame() {

}

bool BaseTransparentDPIFrame::Show(bool show)
{
    if (show) {
        m_finish_text->SetForegroundColour(wxColour(255, 255, 255, 255));
        if (m_refresh_timer) {
            m_refresh_timer->Start(ANIMATION_REFRESH_INTERVAL);
        }
    } else {
        if (m_refresh_timer) {
            m_refresh_timer->Stop();
        }
    }
    Layout();
    return DPIFrame::Show(show);
}

void BaseTransparentDPIFrame::on_full_screen(IntEvent &e) {
#ifdef __APPLE__
    SetWindowStyleFlag(GetWindowStyleFlag() | wxSTAY_ON_TOP);
#endif
}

void BaseTransparentDPIFrame::on_dpi_changed(const wxRect &suggested_rect)
{
    m_button_ok->Rescale();
    m_button_cancel->Rescale();
}

void BaseTransparentDPIFrame::on_show() {
    Show();
    Raise();
}

void BaseTransparentDPIFrame::on_hide()
{
    if (m_refresh_timer) {
        m_refresh_timer->Stop();
    }
    Hide();
    if (wxGetApp().mainframe != nullptr) {
        wxGetApp().mainframe->Show();
        wxGetApp().mainframe->Raise();
    }
}


void BaseTransparentDPIFrame::clear_timer_count() {
    m_timer_count = 0;
}

void BaseTransparentDPIFrame::init_timer()
{
    m_refresh_timer = new wxTimer();
    m_refresh_timer->SetOwner(this);
}

void BaseTransparentDPIFrame::calc_step_transparent() {
    m_max_size         = GetSize();
    m_step_size.x      = GetSize().x / m_time_gradual_and_scale;
    m_step_size.y      = GetSize().y / m_time_gradual_and_scale;
    m_step_transparent = m_init_transparent / m_time_gradual_and_scale;
}

void BaseTransparentDPIFrame::on_close() {
    Destroy();
}

void BaseTransparentDPIFrame::on_timer(wxTimerEvent &event)
{
    if (m_timed_disappearance_mode == DisappearanceMode::TimedDisappearance && m_display_stage == 0) {
        auto cur_time = ANIMATION_REFRESH_INTERVAL * m_timer_count;
        if (cur_time > m_disappearance_second) {
            start_gradual_disappearance();
            m_display_stage++;
        }
    }
    if (m_display_stage == 1) {
        if (m_move_to_target_gradual_disappearance) {
            begin_move_to_target_and_gradual_disappearance();
        }
        else {
            begin_gradual_disappearance();
        }
    }
    m_timer_count++;
}

void BaseTransparentDPIFrame::call_start_gradual_disappearance()//for ok or cancel button
{
    if (m_enter_window_valid) {
        m_enter_window_valid = false;
        m_display_stage      = 1;
        m_refresh_timer->Start(ANIMATION_REFRESH_INTERVAL);
        start_gradual_disappearance();
    }
}

void BaseTransparentDPIFrame::restart() {
    m_display_stage = 0;
    m_enter_window_valid = true;
    SetTransparent(m_init_transparent);
    if (m_refresh_timer) {
        clear_timer_count();
        m_refresh_timer->Start(ANIMATION_REFRESH_INTERVAL);
    }
}
void BaseTransparentDPIFrame::start_gradual_disappearance()
{
    clear_timer_count();
    //hide_all();
    calc_step_transparent();
}
void BaseTransparentDPIFrame::set_target_pos_and_gradual_disappearance(wxPoint pos)
{
    m_move_to_target_gradual_disappearance = true;
    m_target_pos            = pos;
    m_start_pos             = GetScreenPosition();
    m_step_pos.x            = (m_target_pos.x - m_start_pos.x) / m_time_move;
    m_step_pos.y            = (m_target_pos.y - m_start_pos.y) / m_time_move;
}

void BaseTransparentDPIFrame::begin_gradual_disappearance()
{
    if (m_timer_count <=  m_time_gradual_and_scale - 1) {
        auto transparent = m_init_transparent - m_timer_count * m_step_transparent;
        SetTransparent(transparent < 0 ? 0 : transparent);
    } else {
        on_hide();
        return;
    }
    m_timer_count++;
}

void BaseTransparentDPIFrame::begin_move_to_target_and_gradual_disappearance()
{
    if (m_timer_count <= m_time_move) {
        if (m_timer_count <= m_time_move - 1) {
            auto pos = wxPoint(m_start_pos.x + m_timer_count * m_step_pos.x, m_start_pos.y + m_timer_count * m_step_pos.y);
            SetPosition(pos);
        } else {
            SetPosition(m_target_pos);
        }
        Refresh();
    } else {
        SetPosition(m_target_pos);
        if (m_timer_count <= m_time_move + m_time_gradual_and_scale - 1) {
            auto size = wxSize(m_max_size.x - m_timer_count * m_step_size.x, m_max_size.y - m_timer_count * m_step_size.y);
            SetSize(size);
            SetTransparent(m_init_transparent - m_timer_count * m_step_transparent);
        } else {
            on_hide();
            return;
        }
    }
    m_timer_count++;
}

void BaseTransparentDPIFrame::show_sizer(wxSizer *sizer, bool show)
{
    wxSizerItemList items = sizer->GetChildren();
    for (wxSizerItemList::iterator it = items.begin(); it != items.end(); ++it) {
        wxSizerItem *item   = *it;
        if (wxWindow *window = item->GetWindow()) {
            window->Show(show);
        }
        if (wxSizer *son_sizer = item->GetSizer()) {
            show_sizer(son_sizer, show);
        }
    }
}

void BaseTransparentDPIFrame::hide_all() {
    show_sizer(m_sizer_main, false);
}

void BaseTransparentDPIFrame::deal_ok() {}

void BaseTransparentDPIFrame::deal_cancel(){}

}} // namespace Slic3r::GUI