#include "LabeledCheckBox.hpp"

#include "CheckBox.hpp"
#include "Label.hpp"
#include "StateColor.hpp"
#include "StaticBox.hpp"

#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/tglbtn.h> // wxEVT_TOGGLEBUTTON
#include <wx/tooltip.h>
#include <wx/weakref.h>
#include <wx/window.h>
#include <wx/windowid.h>

#include <algorithm>
#include <initializer_list>

static wxColour GetParentBackground(wxWindow* parent)
{
    wxWindow* child = parent;
    for (wxWindow* win = parent; win; child = win, win = win->GetParent())
        if (dynamic_cast<wxScrollHelper*>(win))
            return child->GetBackgroundColour();
    return parent->GetBackgroundColour();
}

LabeledCheckBox::LabeledCheckBox(wxWindow* parent, const wxString& label, wxWindowID id)
    : wxPanel(parent, id)
{
    m_check = new CheckBox(this);

    m_text_box = new StaticBox(this);
    m_text_box->SetCornerRadius(0);

    // Hidden while creation. wxGTK measures label itself and GTK reports size for a new font too late
    m_text_box->Hide();
    m_text = new Label(m_text_box, Label::Body_14, label);
    m_text_box->Show(!label.empty());
    UpdateFocusTarget();

    // Two sizers, as horizontal and vertical padding differ
    wxBoxSizer* text_sizer = new wxBoxSizer(wxHORIZONTAL);
    text_sizer->Add(m_text, 0, wxLEFT | wxRIGHT);
    wxBoxSizer* box_sizer = new wxBoxSizer(wxVERTICAL);
    box_sizer->Add(text_sizer, 0, wxTOP | wxBOTTOM);
    m_text_box->SetSizer(box_sizer);

    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_check, 0, wxTOP);
    sizer->Add(m_text_box);
    SetSizer(sizer);

    wxPanel::SetLabel(label);
    wxPanel::SetFont(Label::Body_14);
    SetBackgroundColour(GetParentBackground(parent));
    SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    UpdateMetrics();
    Fit();

    m_check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) {
        // Focus to frame once the click is over. Pressed button cancels its click on MSW. A click does not focus it at all on macOS
        CallAfter([this] { TakeFocus(); });
        OnToggled();
    });
    m_check->Bind(wxEVT_SET_FOCUS,    [this](wxFocusEvent& e) { ShowFocus(true);  e.Skip(); });
    m_check->Bind(wxEVT_KILL_FOCUS,   [this](wxFocusEvent& e) { ShowFocus(false); e.Skip(); });
    m_text_box->Bind(wxEVT_SET_FOCUS,  [this](wxFocusEvent& e) { ShowFocus(true);  e.Skip(); });
    m_text_box->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { ShowFocus(false); e.Skip(); });
    m_text_box->Bind(wxEVT_KEY_DOWN,   &LabeledCheckBox::OnKeyDown, this);
    for (wxWindow* win : std::initializer_list<wxWindow*>{m_text_box, m_text}) {
        win->Bind(wxEVT_LEFT_DOWN,    &LabeledCheckBox::OnLabelClick, this);
        win->Bind(wxEVT_LEFT_DCLICK,  &LabeledCheckBox::OnLabelClick, this); // 2nd click of a double click
        win->Bind(wxEVT_ENTER_WINDOW, &LabeledCheckBox::OnLabelHover, this);
        win->Bind(wxEVT_LEAVE_WINDOW, &LabeledCheckBox::OnLabelHover, this);
    }
    Bind(wxEVT_SIZE, &LabeledCheckBox::OnSize, this);
}

void LabeledCheckBox::SetValue(bool value)
{
    m_check->SetValue(value);
    ShowHover();
}

void LabeledCheckBox::SetHalfChecked(bool half)
{
    m_check->SetHalfChecked(half);
    ShowHover();
}

void LabeledCheckBox::Wrap(int width)
{
    m_auto_wrap  = false;
    m_wrap_width = width;
    Relayout();
}

void LabeledCheckBox::AutoWrap(bool enable)
{
    m_auto_wrap  = enable;
    m_wrap_width = -1; // wrapped again on the next resize
    Relayout();
}

void LabeledCheckBox::OnSize(wxSizeEvent& e)
{
    e.Skip();
    // Never wrap below the min width, and not for our own best width: it already fits
    const int    width = std::max(GetClientSize().x, GetMinSize().x);
    const wxSize best  = GetBestSize();
    if (!m_auto_wrap || width == m_wrap_width || width == best.x)
        return;

    m_wrap_width = width;
    UpdateMetrics();
    if (GetBestSize() != best) // Height changed, let the parent lay out again
        CallAfter([this]() { SendSizeEventToParent(); });
}

void LabeledCheckBox::OnToggled()
{
    m_check->SetHalfChecked(false); // also refreshes the bitmap
    ShowHover();

    // A handler may destroy us
    const wxWeakRef<wxWindow> self(this);
    const wxWindowID id    = GetId();
    const bool       value = GetValue();
    for (wxEventType type : {wxEVT_CHECKBOX, wxEVT_TOGGLEBUTTON}) {
        wxCommandEvent evt(type, id);
        evt.SetEventObject(this);
        evt.SetInt(value);
        ProcessWindowEvent(evt);
        if (!self)
            return;
    }
}

void LabeledCheckBox::OnLabelHover(wxMouseEvent& e)
{
    e.Skip();
    // Label and frame send enter / leave to each other, so check the position on leave.
    // Event position, as wxGetMousePosition() does not work on Wayland
    wxPoint pos = e.GetPosition();
    if (e.GetEventObject() == m_text)
        pos += m_text->GetPosition();
    m_hovered = e.Entering() || m_text_box->GetClientRect().Contains(pos);
    m_check->SetHalfChecked(m_check->IsHalfChecked()); // back to the normal icon
    ShowHover();
}

void LabeledCheckBox::ShowHover()
{
    if (!m_hovered)
        return;
    // CheckBox sets its icons on every change, so this is called after each of them.
    // Reset them first: on MSW setting the normal icon drops the hover one.
    m_check->SetHalfChecked(m_check->IsHalfChecked());
    m_check->SetBitmapLabel(m_check->GetBitmapCurrent());
}

void LabeledCheckBox::OnLabelClick(wxMouseEvent& e)
{
    if (!IsEnabled())
        return e.Skip();
    TakeFocus();
    m_check->SetValue(!GetValue());
    OnToggled();
}

void LabeledCheckBox::OnKeyDown(wxKeyEvent& e)
{
    if (e.GetKeyCode() == WXK_SPACE) {
        if (IsEnabled()) {
            m_check->SetValue(!GetValue());
            OnToggled();
        }
        return;
    }
    if (e.GetKeyCode() != WXK_TAB || !m_text_box->HandleAsNavigationKey(e))
        e.Skip();
}

// Frame takes keyboard focus: a native button cant take it while macOS full keyboard access is off ([NSView canBecomeKeyView])
void LabeledCheckBox::UpdateFocusTarget()
{
    if (m_text_box->IsShown())
        m_check->DisableFocusFromKeyboard(); // one tab stop. SetCanFocus() would work on GTK only
}

// With no label there is no frame to focus, only the check
void LabeledCheckBox::TakeFocus()
{
    if (m_text_box->IsShown())
        m_text_box->SetFocus();
    else
        m_check->SetFocus();
}

void LabeledCheckBox::ShowFocus(bool focused)
{
    if (!IsBeingDeleted())
        m_text_box->SetBorderColor(focused ? wxColour("#009688") : GetBackgroundColour());
}

void LabeledCheckBox::UpdateLabelColor()
{
    // Own state only: IsEnabled() is also false while an ancestor is disabled, e.g. the main frame under a modal dialog
    if (m_text->SetForegroundColour(m_check->IsThisEnabled() ? GetForegroundColour() : StateColor::darkModeColorFor(wxColour("#6B6A6A"))))
        m_text->Refresh();
}

void LabeledCheckBox::UpdateMetrics()
{
    const wxSize padding = FromDIP(wxSize(5, 2)); // around the text, same as RadioGroup
    wxSizer* text_sizer = m_text->GetContainingSizer();
    text_sizer->GetItem(m_text)->SetBorder(padding.x);
    m_text_box->GetSizer()->GetItem(text_sizer)->SetBorder(padding.y);
    // Keep the check on the first text line
    const int offset = m_text_box->IsShown() ? padding.y + (m_text->GetCharHeight() - m_check->GetMinSize().y) / 2 : 0;
    GetSizer()->GetItem(m_check)->SetBorder(std::max(offset, 0));

    // Label::Wrap() starts from the full text, -1 unwraps
    const int text_width = m_wrap_width - m_check->GetMinSize().x - 2 * padding.x;
    m_text->Wrap(m_wrap_width > 0 && text_width > 0 ? text_width : -1);
}

void LabeledCheckBox::Relayout()
{
    UpdateMetrics();
    Fit();
    SendSizeEventToParent();
}

void LabeledCheckBox::Rescale()
{
    m_check->Rescale();
    ShowHover();
    Relayout();
}

void LabeledCheckBox::SetLabel(const wxString& label)
{
    wxPanel::SetLabel(label);
    m_text->SetLabel(label);
    m_text_box->Show(!label.empty());
    UpdateFocusTarget();
    Relayout();
}

bool LabeledCheckBox::SetFont(const wxFont& font)
{
    const bool changed = wxPanel::SetFont(font);
    m_text->SetFont(font);
    Relayout();
    return changed;
}

bool LabeledCheckBox::SetForegroundColour(const wxColour& colour)
{
    const bool changed = wxPanel::SetForegroundColour(colour);
    UpdateLabelColor();
    return changed;
}

bool LabeledCheckBox::SetBackgroundColour(const wxColour& colour)
{
    const bool changed = wxPanel::SetBackgroundColour(colour);
    m_check->SetBackgroundColour(colour);
    m_text_box->SetBackgroundColour(colour);
    m_text->SetBackgroundColour(colour);
    ShowFocus(HasFocus()); // a hidden frame has the background color
    return changed;
}

bool LabeledCheckBox::Enable(bool enable)
{
    if (!m_check->Enable(enable))
        return false;
    m_text_box->Enable(enable); // it holds the focus, so it must not be a tab stop while disabled
    UpdateLabelColor();
    return true;
}

void LabeledCheckBox::DoSetToolTip(wxToolTip* tip)
{
    const wxString text = tip ? tip->GetTip() : wxString();
    for (wxWindow* win : std::initializer_list<wxWindow*>{m_check, m_text_box, m_text})
        win->SetToolTip(text);
    wxPanel::DoSetToolTip(tip);
}

// wx only changes the text of an existing tooltip here and does not call DoSetToolTip() again
void LabeledCheckBox::DoSetToolTipText(const wxString& tip)
{
    wxPanel::DoSetToolTipText(tip);
    for (wxWindow* win : std::initializer_list<wxWindow*>{m_check, m_text_box, m_text})
        win->SetToolTip(tip);
}
