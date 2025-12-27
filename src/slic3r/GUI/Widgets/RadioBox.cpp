#include "RadioBox.hpp"

#include "../wxExtensions.hpp"

namespace Slic3r {
namespace GUI {
RadioBox::RadioBox(wxWindow *parent)
    : wxBitmapToggleButton(parent, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_on(this, "radio_on", 18)
    , m_off(this, "radio_off", 18)
    , m_ban(this, "radio_ban", 18)
{
    // SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
    if (parent) SetBackgroundColour(parent->GetBackgroundColour());
    // Bind(wxEVT_TOGGLEBUTTON, [this](auto& e) { update(); e.Skip(); });
    SetSize(m_on.GetBmpSize());
    SetMinSize(m_on.GetBmpSize());
    update();
}

void RadioBox::SetValue(bool value)
{
    wxBitmapToggleButton::SetValue(value);
    update();
}

bool RadioBox::GetValue()
{
    return wxBitmapToggleButton::GetValue();
}


void RadioBox::Rescale()
{
    m_on.msw_rescale();
    m_off.msw_rescale();
    SetSize(m_on.GetBmpSize());
    update();
}

void RadioBox::update() {
    if (IsEnabled())
    {
        SetBitmap((GetValue() ? m_on : m_off).bmp());
    } else
    {
        SetBitmap(m_ban.bmp());
    }

}

}
}

