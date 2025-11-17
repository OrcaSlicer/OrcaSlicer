#include "SideMenuPopup.hpp"
#include "Label.hpp"

#include <wx/display.h>
#include <wx/dcgraph.h>
#include "../GUI_App.hpp"



wxBEGIN_EVENT_TABLE(SidePopup,PopupWindow)
EVT_PAINT(SidePopup::paintEvent)
wxEND_EVENT_TABLE()

SidePopup::SidePopup(wxWindow* parent)
    :PopupWindow(parent,
    wxBORDER_NONE |
    wxPU_CONTAINS_CONTROLS)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__
}

SidePopup::~SidePopup()
{
    ;
}

void SidePopup::OnDismiss()
{
    Slic3r::GUI::wxGetApp().set_side_menu_popup_status(false);
    PopupWindow::OnDismiss();
}

bool SidePopup::ProcessLeftDown(wxMouseEvent& event)
{
    return PopupWindow::ProcessLeftDown(event);
}
bool SidePopup::Show( bool show )
{
    return PopupWindow::Show(show);
}

void SidePopup::Popup(wxWindow* focus)
{
    Create();
    auto drect = wxDisplay(GetParent()).GetGeometry();
    int screenwidth = drect.x + drect.width;
    //int screenwidth = wxSystemSettings::GetMetric(wxSYS_SCREEN_X,NULL);

    int max_width = 0;

    for (auto btn : btn_list)
    {
        max_width = std::max(btn->GetMinSize().x, max_width);
    }
    if (focus) {
        wxPoint pos = focus->ClientToScreen(wxPoint(0, -6));

#ifdef __APPLE__
         pos.x = pos.x - FromDIP(20);
#endif // __APPLE__

        if (pos.x + max_width > screenwidth)
            Position({pos.x - (pos.x + max_width - screenwidth),pos.y}, {0, focus->GetSize().y + 12});
        else
            Position(pos, {0, focus->GetSize().y + 12});
    }
    Slic3r::GUI::wxGetApp().set_side_menu_popup_status(true);
    PopupWindow::Popup();
}

void SidePopup::Create()
{
    wxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    int max_width = 0;
    int height = 0;
    for (auto btn : btn_list)
    {
        max_width = std::max(btn->GetMinSize().x, max_width);
    }

    for (auto btn : btn_list)
    {
        wxSize size = btn->GetMinSize();
        height += size.y;
        size.x = max_width;
        btn->SetMinSize(size);
        btn->SetSize(size);
        sizer->Add(btn, 0, 0, 0);
    }

    SetSize(wxSize(max_width, height));

    SetSizer(sizer, true);

    Layout();
    Refresh();
}

void SidePopup::paintEvent(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    wxSize size = GetSize();
    dc.SetBrush(wxTransparentColour);
    dc.DrawRectangle(0, 0, size.x, size.y);
}

void SidePopup::append_button(SideButton* btn)
{
    btn_list.push_back(btn);
}
