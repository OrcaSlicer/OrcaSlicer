#include "LabeledStaticBox.hpp"
#include "libslic3r/Utils.hpp"
#include "../GUI.hpp"
#include "../GUI_Utils.hpp"
#include "Label.hpp"
#include <utility>
#include <wx/string.h>
#include <wx/gdicmn.h>
#include <wx/event.h>
#include <wx/dcclient.h>
#include "slic3r/GUI/Widgets/StateHandler.hpp"
#include <wx/dc.h>
#include <wx/peninfobase.h>
#include <algorithm>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <wx/dcgraph.h>

LabeledStaticBox::LabeledStaticBox()
    : state_handler(this)
{
    m_radius       = 3;
    m_border_width = 1;
    text_color = StateColor(
        std::make_pair(0x363636, (int) StateColor::Normal),
        std::make_pair(0x6B6B6B, (int) StateColor::Disabled)
    );
    background_color = StateColor(
        std::make_pair(0xFFFFFF, (int) StateColor::Normal),
        std::make_pair(0xF0F0F1, (int) StateColor::Disabled)
    );
    border_color = StateColor(
        std::make_pair(0xDBDBDB, (int) StateColor::Normal),
        std::make_pair(0xDBDBDB, (int) StateColor::Disabled)
    );
}

LabeledStaticBox::LabeledStaticBox(
    wxWindow*       parent,
    const wxString& label,
    const wxPoint&  pos,
    const wxSize&   size,
    long            style
)
    : LabeledStaticBox()
{
    Create(parent, label, pos, size, style);
}

bool LabeledStaticBox::Create(
    wxWindow*       parent,
    const wxString& label,
    const wxPoint&  pos,
    const wxSize&   size,
    long            style
)
{
    if (style & wxBORDER_NONE)
        m_border_width = 0;
    wxStaticBox::Create(parent, wxID_ANY, label, pos, size, style);
    wxStaticBox::SetFont(Label::Head_14);
#ifdef __WXOSX__
    Slic3r::GUI::staticbox_remove_margin(this);
#endif

    m_label = label;
    m_pos   = this->GetPosition();

    Bind(wxEVT_PAINT,([this](wxPaintEvent e) {
        wxPaintDC dc(this);
        PickDC(dc);
    }));

    state_handler.attach({&text_color, &background_color, &border_color});
    state_handler.update_binds();
    #ifndef __WXOSX__
        SetBackgroundStyle(wxBG_STYLE_PAINT);
    #endif
    SetBackgroundColour(background_color.colorForStates(state_handler.states()));
    SetForegroundColour(      text_color.colorForStates(state_handler.states()));
    SetBorderColor(         border_color.colorForStates(state_handler.states()));
    SetCanFocus(false);
    DisableFocusFromKeyboard();
    return true;
}

wxFont LabeledStaticBox::scaled_font() const
{
    wxFont font = GetFont();
#ifdef __WXMSW__
    // Custom DC drawing does not receive the native control's per-monitor
    // font scaling on Windows.
    font = font.Scaled(GetDPIScaleFactor());
#endif
    return font;
}

void LabeledStaticBox::SetCornerRadius(int radius)
{
    this->m_radius = radius;
    Refresh();
}

void LabeledStaticBox::SetBorderWidth(int width)
{
    this->m_border_width = width;
    Refresh();
}

void LabeledStaticBox::SetBorderColor(StateColor const &color)
{
    border_color = color;
    state_handler.update_binds();
    Refresh();
}

bool LabeledStaticBox::Enable(bool enable)
{
    bool result = this->wxStaticBox::Enable(enable);
    if (result) {
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
        this->SetForegroundColour(      text_color.colorForStates(state_handler.states()));
        this->SetBorderColor(         border_color.colorForStates(state_handler.states()));
    }
    return result;
}

void LabeledStaticBox::PickDC(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize size = GetSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    wxMemoryDC memdc(&dc);
    if (!memdc.IsOk()) {
        DrawBorderAndLabel(dc);
        return;
    }
    wxBitmap bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.SetBackground(wxBrush(GetBackgroundColour()));
    memdc.Clear();
    {
        wxGCDC dc2(memdc);
        DrawBorderAndLabel(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    DrawBorderAndLabel(dc);
#endif
}

void LabeledStaticBox::DrawBorderAndLabel(wxDC& dc)
{
    // fill full background
    dc.SetBackground(wxBrush(background_color.colorForStates(0)));
    dc.Clear();

    const wxFont font = scaled_font();
    dc.SetFont(font);
    const wxSize label_size = dc.GetTextExtent(m_label.IsEmpty() ? "Orca" : m_label);
    const int label_width = label_size.GetWidth();
    const int label_height = label_size.GetHeight();

    wxSize wSz = GetSize();
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(border_color.colorForStates(state_handler.states()), m_border_width, wxPENSTYLE_SOLID));
    dc.DrawRoundedRectangle( // Border
        std::max(0, m_pos.x),
        std::max(0, m_pos.y) + label_height * .5,
        wSz.GetWidth(),
        wSz.GetHeight() - label_height * .5,
        FromDIP(m_radius)
    );

    if (!m_label.IsEmpty()) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(background_color.colorForStates(0)));
        dc.DrawRectangle(wxRect(FromDIP(7), 0, label_width + FromDIP(7), label_height)); // text background
        // NEEDFIX if text lenght > client size 
        dc.SetTextForeground(text_color.colorForStates(state_handler.states()));
        dc.DrawText(m_label, FromDIP(wxPoint(10, 0)));
    }
}

void LabeledStaticBox::GetBordersForSizer(int* borderTop, int* borderOther) const {
    wxStaticBox::GetBordersForSizer(borderTop, borderOther);
    int text_width, text_height, descent, external_leading;
    const wxFont font = scaled_font();
    GetTextExtent(m_label.IsEmpty() ? "Orca" : m_label, &text_width, &text_height, &descent, &external_leading, &font);
    *borderTop = std::max(*borderTop, text_height - external_leading);
#ifdef __WXOSX__
    *borderOther = 5; // Make sure macOS uses the same border padding as other platforms
#endif
}
