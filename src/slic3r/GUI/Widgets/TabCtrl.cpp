#include "TabCtrl.hpp"

#include <wx/dc.h>

wxDEFINE_EVENT( wxEVT_TAB_SEL_CHANGING, wxCommandEvent );
wxDEFINE_EVENT( wxEVT_TAB_SEL_CHANGED, wxCommandEvent );

BEGIN_EVENT_TABLE(TabCtrl, StaticBox)

EVT_KEY_DOWN(TabCtrl::keyDown)

END_EVENT_TABLE()

/*
 * Called by the system of by wxWidgets when the panel needs
 * to be redrawn. You can also trigger this call by
 * calling Refresh()/Update().
 */

#define TAB_LEADING_SPACE    FromDIP(5)
#define TAB_BUTTON_PADDING_X FromDIP(8)
#define TAB_BUTTON_PADDING_Y FromDIP(2)
#define TAB_BUTTON_PADDING TAB_BUTTON_PADDING_X, TAB_BUTTON_PADDING_Y

TabCtrl::TabCtrl(wxWindow *      parent,
                   wxWindowID      id,
                   const wxPoint & pos,
                   const wxSize &  size,
                   long            style)
    : StaticBox(parent, id, pos, size, style)
{
#if 0
    radius = 5;
#else
    radius = 1;
#endif
    border_width = 1;
    SetBorderColor(0xcecece);
    sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->AddSpacer(TAB_LEADING_SPACE);
    auto hsizer = new wxBoxSizer(wxVERTICAL);
    hsizer->Add(sizer, 0, wxEXPAND | wxBOTTOM, border_width * 4);
    SetSizer(hsizer);

    UpdateCompactWidth();

    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &TabCtrl::buttonClicked, this);
    //wxString reason;
    //IsTransparentBackgroundSupported(&reason);
}

TabCtrl::~TabCtrl()
{
    delete images;
}

int TabCtrl::GetSelection() const { return sel; }

void TabCtrl::SelectItem(int item)
{
    if (item == sel || !sendTabCtrlEvent(true))
        return;
    if (sel >= 0) {
        wxCommandEvent e(wxEVT_CHECKBOX);
        auto b = btns[sel];
        e.SetEventObject(b);
        b->GetEventHandler()->ProcessEvent(e);
    }
    sel = item;
    if (sel >= 0) {
        wxCommandEvent e(wxEVT_CHECKBOX);
        auto b = btns[sel];
        e.SetEventObject(b);
        b->GetEventHandler()->ProcessEvent(e);
    }
    sendTabCtrlEvent();
    relayout();
    Refresh();
}

void TabCtrl::Unselect()
{
    SelectItem(-1);
}

void TabCtrl::UpdateCompactWidth()
{
    wxClientDC dc(this);
    dc.SetFont(GetFont());
    m_compact_width = dc.GetTextExtent("MM...").x + TAB_BUTTON_PADDING_X * 2;
}

void TabCtrl::Rescale()
{
    UpdateCompactWidth();

    for (auto & b : btns)
        b->Rescale();
}

bool TabCtrl::SetFont(wxFont const& font)
{
    StaticBox::SetFont(font);
    bold = font.Bold();
    for (size_t i = 0; i < btns.size(); ++i)
        btns[i]->SetFont(i == sel ? bold : font);

    UpdateCompactWidth();

    return true;
}

int TabCtrl::AppendItem(const wxString &item,
                     int image, int selImage,
                     void * clientData)
{
    Button * btn = new Button();
    btn->Create(this, item, "", wxBORDER_NONE);
    btn->SetFont(GetFont());
    btn->SetTextColor(StateColor(
        std::make_pair(0x6B6B6C, (int) StateColor::NotChecked),
        std::make_pair(*wxLIGHT_GREY, (int) StateColor::Normal)));
    btn->SetBackgroundColor(StateColor());
    btn->SetCornerRadius(0);
    btn->SetPaddingSize({TAB_BUTTON_PADDING});
    btns.push_back(btn);
    sizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL);
    sizer->AddStretchSpacer(1);
    relayout();
    return btns.size() - 1;
}

bool TabCtrl::DeleteItem(int item)
{
    if (item < 0 || item >= btns.size()) {
        return false;
    }
    const bool selection_changed = sel >= item;

    if (selection_changed) {
        sendTabCtrlEvent(true);
    }

    Button* btn = btns[item];
    btn->Destroy();
    btns.erase(btns.begin() + item);
    sizer->Remove(item * 2);

    if (selection_changed) {
        sel--;  // `relayout()` uses `sel` so we need to update this before calling `relayout()`
    }
    relayout();
    if (selection_changed) {
        sendTabCtrlEvent();
    }

    return true;
}

void TabCtrl::DeleteAllItems()
{
    sizer->Clear(true);
    sizer->AddSpacer(TAB_LEADING_SPACE);
    btns.clear();
    if (sel >= 0) {
        sel = -1;
        sendTabCtrlEvent();
    }
}

unsigned int TabCtrl::GetCount() const { return btns.size(); }

wxString TabCtrl::GetItemText(unsigned int item) const
{
    return item < btns.size() ? btns[item]->GetLabel() : wxString{};
}

void TabCtrl::SetItemText(unsigned int item, wxString const &value)
{
    if (item >= btns.size()) return;
    btns[item]->SetLabel(value);
    btns[item]->SetToolTip(value); // ensure tooltip matches with tab
}

bool TabCtrl::GetItemBold(unsigned int item) const
{
    if (item >= btns.size()) return false;
    return btns[item]->GetFont() == bold;
}

void TabCtrl::SetItemBold(unsigned int item, bool bold)
{
    if (item >= btns.size()) return;
    btns[item]->SetFont(bold ? this->bold : GetFont());
    btns[item]->Rescale();
}

void* TabCtrl::GetItemData(unsigned int item) const
{
    if (item >= btns.size()) return nullptr;
    return btns[item]->GetClientData();
}

void TabCtrl::SetItemData(unsigned int item, void* clientData)
{
    if (item >= btns.size()) return;
    btns[item]->SetClientData(clientData);
}

void TabCtrl::AssignImageList(wxImageList* imageList)
{
    if (images == imageList) return;
    delete images;
    images = imageList;
}

void TabCtrl::SetItemTextColour(unsigned int item, const StateColor &col)
{
    if (item >= btns.size()) return;
    btns[item]->SetTextColor(col);
}

int TabCtrl::GetFirstVisibleItem() const
{
    return btns.size() == 0 ? -1 : 0;
}

int TabCtrl::GetNextVisible(int item) const
{
    return ++item < btns.size() ? item : -1;
}

bool TabCtrl::IsVisible(unsigned int item) const
{
    return true;
}

void TabCtrl::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (sizeFlags & wxSIZE_USE_EXISTING) return;
    relayout();
}

#ifdef __WIN32__

WXLRESULT TabCtrl::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    if (nMsg == WM_GETDLGCODE) { return DLGC_WANTARROWS; }
    return wxWindow::MSWWindowProc(nMsg, wParam, lParam);
}

#endif

void TabCtrl::relayout()
{
    const int count = (int)btns.size();
    if (count == 0) {
        Layout();
        return;
    }

    const int ctrl_width = GetSize().x;

    // reset size limits for all buttons first
    for (int i = 0; i < count; ++i)
        btns[i]->SetMaxSize(wxDefaultSize);

    int total_width = TAB_LEADING_SPACE;

    // calculate total width
    for (int i = 0; i < count; ++i)
        total_width += btns[i]->GetMinSize().x;

    int btns_width = total_width;

    // starting from the last button, clamp widths one by one until everything fits
    for (int i = count - 1; i >= 0 && total_width > ctrl_width; --i) {
        sizer->GetItem(i * 2 + 2)->SetMinSize({0, 0}); // reset all stretch spacer width
        int btn_width = btns[i]->GetMinSize().x;
        if (btn_width <= m_compact_width)
            continue; // already narrow enough

        total_width -= (btn_width - m_compact_width);
        btns[i]->SetMaxSize({m_compact_width, -1});
    }

    int b = ctrl_width - (TAB_LEADING_SPACE + btns_width + count * TAB_BUTTON_PADDING_X); // keep it left aligned with resizing last stretch spacer
    sizer->GetItem(sizer->GetItemCount() - 1)->SetMinSize({b > 0 ? b : 0, 0});

    Layout();
}

void TabCtrl::buttonClicked(wxCommandEvent &event)
{
    SetFocus();
    auto btn  = event.GetEventObject();
    auto iter = std::find(btns.begin(), btns.end(), btn);
    SelectItem(iter == btns.end() ? -1 : iter - btns.begin());
}

void TabCtrl::keyDown(wxKeyEvent &event)
{
    switch (event.GetKeyCode()) {
    case WXK_UP:
    case WXK_DOWN:
    case WXK_LEFT:
    case WXK_RIGHT:
        if ((event.GetKeyCode() == WXK_UP || event.GetKeyCode() == WXK_LEFT) && GetSelection() > 0) {
            SelectItem(GetSelection() - 1);
        } else if ((event.GetKeyCode() == WXK_DOWN || event.GetKeyCode() == WXK_RIGHT) && GetSelection() + 1 < btns.size()) {
            SelectItem(GetSelection() + 1);
        }
        break;
    }
}

void TabCtrl::doRender(wxDC& dc)
{
    wxSize size = GetSize();
    int states = state_handler.states();
    if (sel < 0) { return; }

    auto x1 = btns[sel]->GetPosition().x;
    auto x2 = x1 + btns[sel]->GetSize().x;
    const int BS2 = (1 + border_width) / 2;
#if 0
    const int BS = border_width / 2;
    x1 -= TAB_BUTTON_SPACE; x2 += TAB_BUTTON_SPACE;
    dc.SetPen(wxPen(border_color.colorForStates(states), border_width));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawLine(0, size.y - BS2, x1 - radius + BS2, size.y - BS2);
    dc.DrawArc(x1 - radius, size.y, x1, size.y - radius, x1 - radius, size.y - radius);
    dc.DrawLine(x1, size.y - radius, x1, radius);
    dc.DrawArc(x1 + radius, 0, x1, radius, x1 + radius, radius);
    dc.DrawLine(x1 + radius, BS, x2 - radius, BS);
    dc.DrawArc(x2, radius, x2 - radius, 0, x2 - radius, radius);
    dc.DrawLine(x2, radius, x2, size.y - radius);
    dc.DrawArc(x2, size.y - radius, x2 + radius, size.y, x2 + radius, size.y - radius);
    dc.DrawLine(x2 + radius - BS2, size.y - BS2, size.x, size.y - BS2);
#else
    dc.SetPen(wxPen(border_color.colorForStates(states), border_width));
    dc.DrawLine(0, size.y - BS2, size.x, size.y - BS2);
    wxColour c("#009688"); // ORCA selected tab underline stroke color
    dc.SetPen(wxPen(c, 1));
    dc.SetBrush(c);
    dc.DrawRoundedRectangle(x1 - radius, size.y - BS2 - border_width * 3, x2 + radius * 2 - x1, border_width * 3, radius);
#endif
}

bool TabCtrl::sendTabCtrlEvent(bool changing)
{
    wxCommandEvent event(changing ? wxEVT_TAB_SEL_CHANGING : wxEVT_TAB_SEL_CHANGED, GetId());
    event.SetEventObject(this);
    event.SetInt(sel);
    GetEventHandler()->ProcessEvent(event);
    return true;
}
