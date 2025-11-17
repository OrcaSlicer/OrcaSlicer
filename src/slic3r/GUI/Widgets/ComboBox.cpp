#include "ComboBox.hpp"
#include "Label.hpp"

#include <wx/dcgraph.h>

BEGIN_EVENT_TABLE(ComboBox, TextInput)

EVT_LEFT_DOWN(ComboBox::mouseDown)
EVT_LEFT_DCLICK(ComboBox::mouseDown)
//EVT_MOUSEWHEEL(ComboBox::mouseWheelMoved)
EVT_KEY_DOWN(ComboBox::keyDown)

// catch paint events
END_EVENT_TABLE()

/*
 * Called by the system of by wxWidgets when the panel needs
 * to be redrawn. You can also trigger this call by
 * calling Refresh()/Update().
 */

static wxWindow *GetScrollParent(wxWindow *pWindow)
{
    wxWindow *pWin = pWindow;
    while (pWin->GetParent()) {
        auto pWin2 = pWin->GetParent();
        if (auto top = dynamic_cast<wxScrollHelper *>(pWin2))
            return dynamic_cast<wxWindow *>(pWin);
        pWin = pWin2;
    }
    return nullptr;
}

ComboBox::ComboBox(wxWindow *parent,
                   wxWindowID      id,
                   const wxString &value,
                   const wxPoint & pos,
                   const wxSize &  size,
                   int             n,
                   const wxString  choices[],
                   long            style)
    : drop(items)
{
    if ((style & wxALIGN_MASK) == 0 && (style & wxCB_READONLY))
        style |= wxALIGN_RIGHT;
    text_off = style & CB_NO_TEXT;
    TextInput::Create(parent, "", value, (style & CB_NO_DROP_ICON) ? "" : "drop_down", pos, size,
                      style | wxTE_PROCESS_ENTER);
    drop.Create(this, style & DD_STYLE_MASK);

    if (style & wxCB_READONLY) {
        GetTextCtrl()->Hide();
        TextInput::SetFont(Label::Body_14);
        TextInput::SetBorderColor(StateColor(std::make_pair(0xDBDBDB, (int) StateColor::Disabled),
            std::make_pair(0x009688, (int) StateColor::Hovered),
            std::make_pair(0xDBDBDB, (int) StateColor::Normal)));
        TextInput::SetBackgroundColor(StateColor(std::make_pair(0xF0F0F1, (int) StateColor::Disabled),
            std::make_pair(0xE5F0EE, (int) StateColor::Focused), // ORCA updated background color for focused item
            std::make_pair(*wxWHITE, (int) StateColor::Normal)));
        TextInput::SetLabelColor(StateColor(
            std::make_pair(0x6B6B6B, (int) StateColor::Disabled), // ORCA: Use same color for disabled text on combo boxes
            std::make_pair(0x262E30, (int) StateColor::Normal)));
    }
    if (auto scroll = GetScrollParent(this))
        scroll->Bind(wxEVT_MOVE, &ComboBox::onMove, this);
    drop.Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        SetSelection(e.GetInt());
        e.SetEventObject(this);
        e.SetId(GetId());
        GetEventHandler()->ProcessEvent(e);
    });
    drop.Bind(EVT_DISMISS, [this](auto &) {
        drop_down = false;
        wxCommandEvent e(wxEVT_COMBOBOX_CLOSEUP);
        GetEventHandler()->ProcessEvent(e);
    });
    for (int i = 0; i < n; ++i) Append(choices[i]);
}

int ComboBox::GetSelection() const {
    return drop.GetSelection();
}

void ComboBox::SetSelection(int n)
{
    if (n == drop.selection)
        return;
    drop.SetSelection(n);
    SetLabel(drop.GetValue());
    if (drop.selection >= 0 && drop.iconSize.y > 0 && items[drop.selection].icon_textctrl.IsOk())
        SetIcon(items[drop.selection].icon_textctrl);
    else
        SetIcon("drop_down");

    if (drop.selection >= 0) {
        SetStaticTips(items[drop.selection].text_static_tips, wxNullBitmap);
    } else {
        SetStaticTips(wxEmptyString, wxNullBitmap);
    }

}

void ComboBox::SelectAndNotify(int n) {
    SetSelection(n);
    sendComboBoxEvent();
}

void ComboBox::Rescale()
{
    TextInput::Rescale();
    drop.Rescale();
}

wxString ComboBox::GetValue() const
{
    return drop.GetSelection() >= 0 ? drop.GetValue() : GetLabel();
}

void ComboBox::SetValue(const wxString &value)
{
    drop.SetValue(value);
    SetLabel(value);
    if (drop.selection >= 0 && drop.iconSize.y > 0 && items[drop.selection].icon_textctrl.IsOk())
        SetIcon(items[drop.selection].icon_textctrl);
    else
        SetIcon("drop_down");

    if (drop.selection >= 0) {
        SetStaticTips(items[drop.selection].text_static_tips, wxNullBitmap);
    } else {
        SetStaticTips(wxEmptyString, wxNullBitmap);
    }
}

void ComboBox::SetLabel(const wxString &value)
{
    if (GetTextCtrl()->IsShown() || text_off)
        GetTextCtrl()->SetValue(value);
    else {
        if (is_replace_text_to_image) {
            auto new_value = value;
            if (new_value.starts_with(replace_text)) {
                new_value.Replace(replace_text, "", false); // replace first text
                TextInput::SetIcon_1(image_for_text);
                TextInput::SetLabel(new_value);
                return;
            }
        }
        TextInput::SetIcon_1("");
        TextInput::SetLabel(value);
    }
}

wxString ComboBox::GetLabel() const
{
    if (GetTextCtrl()->IsShown() || text_off)
        return GetTextCtrl()->GetValue();
    else
        return TextInput::GetLabel();
}

int ComboBox::GetFlag(unsigned int n)
{
    if (n >= items.size())
        return -1;
    return items[n].flag;
}

void ComboBox::SetFlag(unsigned int n, int value) {
    if (n >= items.size()) return;
    items[n].flag = value;
    drop.Invalidate();
}

void ComboBox::SetTextLabel(const wxString& label)
{
    TextInput::SetLabel(label);
}

wxString ComboBox::GetTextLabel() const
{
    return TextInput::GetLabel();
}

bool ComboBox::SetFont(wxFont const& font)
{
    if (GetTextCtrl() && GetTextCtrl()->IsShown())
        return GetTextCtrl()->SetFont(font);
    else
        return TextInput::SetFont(font);
}

int ComboBox::Append(const wxString &item, const wxBitmap &bitmap, int style)
{
    if (&bitmap && bitmap.IsOk()) {
        return Append(item, bitmap, nullptr, style);
    }
    return Append(item, wxNullBitmap, nullptr, style);
}

int ComboBox::Append(const wxString &text,
                     const wxBitmap &bitmap,
                     void *          clientData,
                     int style)
{
    if (&bitmap && bitmap.IsOk()) {
        return Append(text, bitmap, wxString{}, clientData, style);
    }
    return Append(text, wxNullBitmap, wxString{}, clientData, style);
}

int ComboBox::Append(const wxString &text, const wxBitmap &bitmap, const wxString &group, void *clientData, int style)
{
    auto valid_bit_map = (&bitmap && bitmap.IsOk()) ? bitmap : wxNullBitmap;
    Item item{text, wxEmptyString, valid_bit_map, valid_bit_map, clientData, group};
    item.style = style;
    items.push_back(item);
    SetClientDataType(wxClientData_Void);
    drop.Invalidate();
    return items.size() - 1;
}

int ComboBox::SetItems(const std::vector<DropDown::Item>& the_items)
{
    items = the_items;
    drop.Invalidate();
    return items.size() - 1;
}

void ComboBox::DoClear()
{
    SetIcon("drop_down");
    items.clear();
    drop.Invalidate(true);
}

void ComboBox::DoDeleteOneItem(unsigned int pos)
{
    if (pos >= items.size()) return;
    items.erase(items.begin() + pos);
    drop.Invalidate(true);
}

unsigned int ComboBox::GetCount() const { return items.size(); }

void ComboBox::set_replace_text(wxString text, wxString image_name)
{
    replace_text = text;
    image_for_text = image_name;
    is_replace_text_to_image  = true;
}

wxString ComboBox::GetString(unsigned int n) const
{ return n < items.size() ? items[n].text : wxString{}; }

void ComboBox::SetString(unsigned int n, wxString const &value)
{
    if (n >= items.size()) return;
    items[n].text = value;
    drop.Invalidate();
    if (n == drop.GetSelection()) SetLabel(value);
}

wxString ComboBox::GetItemTooltip(unsigned int n) const
{
    if (n >= items.size()) return wxString();
    return items[n].tip;
}

void ComboBox::SetItemTooltip(unsigned int n, wxString const &value) {
    if (n >= items.size()) return;
    items[n].tip = value;
    if (n == drop.GetSelection()) drop.SetToolTip(value);
}

wxBitmap ComboBox::GetItemBitmap(unsigned int n) { return items[n].icon; }

void ComboBox::SetItemBitmap(unsigned int n, wxBitmap const &bitmap)
{
    if (n >= items.size()) return;
    items[n].icon = (&bitmap && bitmap.IsOk()) ? bitmap : wxNullBitmap;
    drop.Invalidate();
}

int ComboBox::DoInsertItems(const wxArrayStringsAdapter &items,
                            unsigned int                 pos,
                            void **                      clientData,
                            wxClientDataType             type)
{
    if (pos > this->items.size()) return -1;
    for (int i = 0; i < items.GetCount(); ++i) {
        Item item { items[i], wxEmptyString, wxNullBitmap, wxNullBitmap, clientData ? clientData[i] : NULL };
        this->items.insert(this->items.begin() + pos, item);
        ++pos;
    }
    drop.Invalidate(true);
    return pos - 1;
}

void *ComboBox::DoGetItemClientData(unsigned int n) const { return n < items.size() ? items[n].data : NULL; }

void ComboBox::DoSetItemClientData(unsigned int n, void *data)
{
    if (n < items.size())
        items[n].data = data;
}

void ComboBox::mouseDown(wxMouseEvent &event)
{
    if (!IsEnabled()) { return; } /*on mac, the event may triggered even disabled*/

    SetFocus();
    if (drop_down) {
        drop.Hide();
    } else if (drop.HasDismissLongTime()) {
        drop.autoPosition();
        drop_down = true;
        drop.Popup(&drop);
        wxCommandEvent e(wxEVT_COMBOBOX_DROPDOWN);
        GetEventHandler()->ProcessEvent(e);
    }
}

void ComboBox::mouseWheelMoved(wxMouseEvent &event)
{
    event.Skip();
    if (drop_down) return;
    auto delta = event.GetWheelRotation() < 0 ? 1 : -1;
    unsigned int n = GetSelection() + delta;
    if (n < GetCount()) {
        SetSelection((int) n);
        sendComboBoxEvent();
    }
}

void ComboBox::keyDown(wxKeyEvent& event)
{
    switch (event.GetKeyCode()) {
        case WXK_RETURN:
        case WXK_SPACE:
            if (drop_down) {
                drop.DismissAndNotify();
            } else if (drop.HasDismissLongTime()) {
                drop.autoPosition();
                drop_down = true;
                drop.Popup();
                wxCommandEvent e(wxEVT_COMBOBOX_DROPDOWN);
                GetEventHandler()->ProcessEvent(e);
            }
            break;
        case WXK_UP:
        case WXK_DOWN:
        case WXK_LEFT:
        case WXK_RIGHT:
            if ((event.GetKeyCode() == WXK_UP || event.GetKeyCode() == WXK_LEFT) && GetSelection() > 0) {
                SetSelection(GetSelection() - 1);
            } else if ((event.GetKeyCode() == WXK_DOWN || event.GetKeyCode() == WXK_RIGHT) && GetSelection() + 1 < items.size()) {
                SetSelection(GetSelection() + 1);
            } else {
                break;
            }
            sendComboBoxEvent();
            break;
        case WXK_TAB:
            HandleAsNavigationKey(event);
            break;
        default:
            event.Skip();
            break;
    }
}

void ComboBox::onMove(wxMoveEvent &event)
{
    event.Skip();
    drop.Hide();
}

void ComboBox::OnEdit()
{
    auto value = GetTextCtrl()->GetValue();
    SetValue(value);
}

#ifdef __WIN32__

WXLRESULT ComboBox::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    if (nMsg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }
    return TextInput::MSWWindowProc(nMsg, wParam, lParam);
}

#endif

void ComboBox::sendComboBoxEvent()
{
    wxCommandEvent event(wxEVT_COMBOBOX, GetId());
    event.SetEventObject(this);
    event.SetInt(drop.GetSelection());
    event.SetString(drop.GetValue());
    GetEventHandler()->ProcessEvent(event);
}
