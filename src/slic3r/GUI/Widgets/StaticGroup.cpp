#include "StaticGroup.hpp"
#include "Label.hpp"

StaticGroup::StaticGroup(wxWindow *parent, wxWindowID id, const wxString &label)
    : LabeledStaticBox(parent, label)
{
    SetBackgroundColour(*wxWHITE);
    SetForegroundColour("#CECECE");
    borderColor_ = wxColour("#CECECE");
#ifdef __WXMSW__
    Bind(wxEVT_PAINT, &StaticGroup::OnPaint, this);
#endif
}

bool StaticGroup::Show(bool show)
{
    bool ret = wxStaticBox::Show(show);
    return ret;
}

void StaticGroup_layoutBadge(void * group, void * badge);


void StaticGroup::ShowBadge(bool show)
{
#ifdef __WXMSW__
    if (show && badge.name() != "badge") {
        badge = ScalableBitmap(this, "badge", 18);
        Refresh();
    } else if (!show && !badge.name().empty()) {
        badge = ScalableBitmap{};
        Refresh();
    }
#endif
#ifdef __WXOSX__
    if (show && badge == nullptr) {
        badge = new ScalableButton(this, wxID_ANY, "badge", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, false, 18);
        badge->SetSize(badge->GetBestSize());
        badge->SetBackgroundColour("#F7F7F7");
        StaticGroup_layoutBadge(GetHandle(), badge->GetHandle());
    }
    if (badge && badge->IsShown() != show)
        badge->Show(show);
#endif
}

void StaticGroup::DrawBorderAndLabel(wxDC& dc)
{
    LabeledStaticBox::DrawBorderAndLabel(dc);
    if (badge.bmp().IsOk()) {
        auto s = badge.bmp().GetScaledSize();
        dc.DrawBitmap(badge.bmp(), GetSize().x - s.x, std::max(0, m_pos.y) + m_label_height / 2);
    }
}
