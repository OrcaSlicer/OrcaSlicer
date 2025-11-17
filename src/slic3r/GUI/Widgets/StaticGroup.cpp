#include "StaticGroup.hpp"

StaticGroup::StaticGroup(wxWindow *parent, wxWindowID id, const wxString &label)
    : LabeledStaticBox(parent, label)
{
    SetBackgroundColour(*wxWHITE);
    SetForegroundColour("#CECECE");
}

void StaticGroup::ShowBadge(bool show)
{
    if (show)
        badge = ScalableBitmap(this, "badge", 18);
    else
        badge = ScalableBitmap{};
    Refresh();
}

void StaticGroup::DrawBorderAndLabel(wxDC& dc)
{
    LabeledStaticBox::DrawBorderAndLabel(dc);
    if (badge.bmp().IsOk()) {
        auto s = badge.bmp().GetScaledSize();
        dc.DrawBitmap(badge.bmp(), GetSize().x - s.x, std::max(0, m_pos.y) + m_label_height / 2);
    }
}
