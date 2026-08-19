#include "LabeledStaticBox.hpp"
#include "libslic3r/Utils.hpp"
#include "../GUI.hpp"
#include "../GUI_Utils.hpp"
#include "Label.hpp"

LabeledStaticBox::LabeledStaticBox()
    : state_handler(this)
{
    m_radius       = 3;
    m_border_width = 1;
    SetFont(Label::Head_14);
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
#ifdef __WXOSX__
    Slic3r::GUI::staticbox_remove_margin(this);
#endif

    m_label = label;
    m_pos   = this->GetPosition();

    update_label_size();

    Bind(wxEVT_PAINT,([this](wxPaintEvent& e) {
        wxAutoBufferedPaintDC dc(this);
#ifdef __WXMSW__
        wxGCDC gdc(dc);
        DrawBorderAndLabel(gdc);
#else
        DrawBorderAndLabel(dc);
#endif
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

void LabeledStaticBox::calc_label_size(int& w, int& h) const
{
    int tW,tH,descent,externalLeading;
    // empty label sets m_label_height as 0 that causes extra spacing at top
    GetTextExtent(m_label.IsEmpty() ? "Orca" : m_label, &tW, &tH, &descent, &externalLeading);
    h = tH - externalLeading;
    w = tW;
}

void LabeledStaticBox::update_label_size()
{
    calc_label_size(m_label_width, m_label_height);
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

void LabeledStaticBox::DrawBorderAndLabel(wxDC& dc)
{
    // fill full background
    dc.SetBackground(wxBrush(background_color.colorForStates(0)));
    dc.Clear();

    update_label_size();

    wxSize wSz = GetSize();

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(border_color.colorForStates(state_handler.states()), m_border_width, wxPENSTYLE_SOLID));
    dc.DrawRoundedRectangle( // Border
        std::max(0, m_pos.x),
        std::max(0, m_pos.y) + m_label_height * .5,
        wSz.GetWidth(),
        wSz.GetHeight() - m_label_height * .5,
        m_radius
    );

    if (!m_label.IsEmpty()) {
        dc.SetFont(GetFont());
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(background_color.colorForStates(0)));
        dc.DrawRectangle(wxRect(FromDIP(7) ,0 , m_label_width + FromDIP(7), m_label_height)); // text background
        // NEEDFIX if text lenght > client size 
        dc.SetTextForeground(text_color.colorForStates(state_handler.states()));
        dc.DrawText(m_label, FromDIP(wxPoint(10, 0)));
    }
}

void LabeledStaticBox::GetBordersForSizer(int* borderTop, int* borderOther) const {
    wxStaticBox::GetBordersForSizer(borderTop, borderOther);
#ifdef __WXMSW__
    int lw, lh;
    calc_label_size(lw, lh);
    *borderTop = lh;
#endif
#ifdef __WXOSX__
    *borderOther = 5; // Make sure macOS uses the same border padding as other platforms
#endif
}