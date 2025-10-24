#include "GUI_App.hpp"
#include "CapsuleButton.hpp"
#include <wx/dcbuffer.h>
#include "wx/graphics.h"
#include "Widgets/Label.hpp"

namespace Slic3r { namespace GUI {

static const wxColour BgNormalColor  = wxColour("#FFFFFF");
static const wxColour BgSelectColor  = wxColour("#EBF9F0");

static const wxColour TextNormalColor = wxColour("#000000");
static const wxColour TextSelectColor = wxColour("#009688");

static const wxColour BorderNormalColor   = wxColour("#CECECE");
static const wxColour BorderSelectColor = wxColour("#009688");

CapsuleButton::CapsuleButton(wxWindow *parent, wxWindowID id, const wxString &label, bool selected) : wxPanel(parent, id)
{
    SetBackgroundColour(*wxWHITE);
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    m_hovered  = false;
    m_selected = selected;

    auto sizer = new wxBoxSizer(wxHORIZONTAL);

    tag_on_bmp = create_scaled_bitmap("capsule_tag_on", nullptr, FromDIP(16));
    tag_off_bmp = create_scaled_bitmap("capsule_tag_off", nullptr, FromDIP(16));

    m_btn = new wxBitmapButton(this, wxID_ANY, selected?tag_on_bmp:tag_off_bmp, wxDefaultPosition, wxDefaultSize, wxNO_BORDER);
    m_btn->SetBackgroundColour(*wxWHITE);

    m_label = new Label(this, label);

    sizer->AddSpacer(FromDIP(8));
    sizer->Add(m_btn, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(6));
    sizer->AddSpacer(FromDIP(8));
    sizer->Add(m_label, 0, wxALIGN_CENTER);
    sizer->AddSpacer(FromDIP(8));

    SetSizer(sizer);
    Layout();
    Fit();

    auto forward_click_to_parent = [this](auto &event) {
        wxCommandEvent click_event(wxEVT_BUTTON, GetId());
        click_event.SetEventObject(this);
        this->ProcessEvent(click_event);
    };

    m_btn->Bind(wxEVT_LEFT_DOWN, forward_click_to_parent);
    m_label->Bind(wxEVT_LEFT_DOWN, forward_click_to_parent);
    this->Bind(wxEVT_LEFT_DOWN, forward_click_to_parent);

    Bind(wxEVT_PAINT, &CapsuleButton::OnPaint, this);
    Bind(wxEVT_ENTER_WINDOW, &CapsuleButton::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &CapsuleButton::OnLeaveWindow, this);

    UpdateStatus();
}
void CapsuleButton::OnPaint(wxPaintEvent &event)
{
    wxAutoBufferedPaintDC dc(this);
    wxGraphicsContext    *gc = wxGraphicsContext::Create(dc);

    if (gc) {
        dc.Clear();
        wxRect rect = GetClientRect();
        gc->SetBrush(wxTransparentColour);
        gc->DrawRoundedRectangle(0, 0, rect.width, rect.height, 0);
        wxColour bg_color     = m_selected ? BgSelectColor : BgNormalColor;
        wxColour border_color = m_hovered || m_selected ? BorderSelectColor : BorderNormalColor;
        bg_color = StateColor::darkModeColorFor(bg_color);
        border_color = StateColor::darkModeColorFor(border_color);
        gc->SetBrush(wxBrush(bg_color));
        gc->SetPen(wxPen(border_color, 1));
        gc->DrawRoundedRectangle(1, 1, rect.width - 2, rect.height - 2, 5);
        delete gc;
    }
}
void CapsuleButton::Select(bool selected)
{
    m_selected = selected;
    UpdateStatus();
    Refresh();
}

void CapsuleButton::OnEnterWindow(wxMouseEvent &event)
{
    if (!m_hovered) {
        m_hovered = true;
        UpdateStatus();
        Refresh();
    }
    event.Skip();
}

void CapsuleButton::OnLeaveWindow(wxMouseEvent &event)
{
    if (m_hovered) {
        wxPoint pos = this->ScreenToClient(wxGetMousePosition());
        if (this->GetClientRect().Contains(pos)) return;
        m_hovered = false;
        UpdateStatus();
        Refresh();
    }
    event.Skip();
}

void CapsuleButton::UpdateStatus()
{
    if (m_selected) {
        m_btn->SetBitmap(tag_on_bmp);
        m_label->SetForegroundColour(TextSelectColor);
        m_label->SetBackgroundColour(BgSelectColor);
        m_btn->SetBackgroundColour(BgSelectColor);
    } else {
        m_btn->SetBitmap(tag_off_bmp);
        m_label->SetForegroundColour(TextNormalColor);
        m_label->SetBackgroundColour(BgNormalColor);
        m_btn->SetBackgroundColour(BgNormalColor);
    }

    GUI::wxGetApp().UpdateDarkUIWin(this);
}
}} // namespace Slic3r::GUI
