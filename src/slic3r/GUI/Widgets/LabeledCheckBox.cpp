#include "LabeledCheckBox.hpp"

#include <wx/anybutton.h>
#include <wx/colour.h>
#include <wx/dcclient.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/tglbtn.h> // wxEVT_TOGGLEBUTTON
#include <wx/tooltip.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include "slic3r/GUI/Widgets/Button.hpp"
#include <slic3r/GUI/Widgets/Label.hpp>
#include <slic3r/GUI/Widgets/StateColor.hpp>

#include <algorithm>
#include <initializer_list>

static const int LABEL_PADDING = 5; // space between the text and its focus border

// Color of the parent. Inside a scrolled window the color of the window placed directly in it is
// used instead, the windows between don't always have the color they are painted with.
static wxColour GetParentBackground(wxWindow* parent)
{
    wxWindow* child = parent;
    for (wxWindow* win = parent; win; child = win, win = win->GetParent())
        if (dynamic_cast<wxScrollHelper*>(win))
            return child->GetBackgroundColour();
    return parent->GetBackgroundColour();
}

LabeledCheckBox::LabeledCheckBox(wxWindow* parent, const wxString& label, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
{
    // The check takes the keyboard focus
    m_check = new Button(this, wxEmptyString, "check_off", wxBORDER_NONE, 18);
    m_check->SetPaddingSize(wxSize(0, 0));
    m_check->SetCornerRadius(0);

    m_text = new Label(this, Label::Body_14, label, LB_AUTO_WRAP);
    m_text->Show(!label.empty());

    // Borders are set by UpdateMetrics(). The label takes all the remaining width, this is the
    // width it wraps itself to.
    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_check, 0, wxTOP);
    sizer->Add(m_text, 1, wxALL);
    SetSizer(sizer);

    wxPanel::SetLabel(label);
    wxPanel::SetFont(Label::Body_14);
    SetBackgroundColour(GetParentBackground(parent));
    SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    UpdateMetrics();
    SetSize(GetBestSize());
    m_best_size = GetBestSize();

    // A click anywhere toggles: on the check, on the label and on the empty area left when the
    // control is stretched. Button doesn't see the click, so it doesn't send its wxEVT_BUTTON.
    for (wxWindow* win : std::initializer_list<wxWindow*>{this, m_check, m_text}) {
        win->Bind(wxEVT_LEFT_DOWN,    &LabeledCheckBox::OnClick, this);
        win->Bind(wxEVT_LEFT_DCLICK,  &LabeledCheckBox::OnClick, this); // second click of a fast double click
        win->Bind(wxEVT_ENTER_WINDOW, &LabeledCheckBox::OnHover, this);
        win->Bind(wxEVT_LEAVE_WINDOW, &LabeledCheckBox::OnHover, this);
    }
    // Button turns every Space / Enter key down into a click, drop the repeated ones of a held key
    // (wxKeyEvent::IsAutoRepeat() is not implemented on Linux)
    m_check->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        e.Skip(e.GetKeyCode() != m_held_key);
        m_held_key = e.GetKeyCode();
    });
    m_check->Bind(wxEVT_KEY_UP, [this](wxKeyEvent& e) {
        m_held_key = WXK_NONE;
        e.Skip();
    });
    m_check->Bind(wxEVT_SET_FOCUS,  &LabeledCheckBox::OnFocus, this);
    m_check->Bind(wxEVT_KILL_FOCUS, &LabeledCheckBox::OnFocus, this);
    m_text->Bind(wxEVT_SIZE, &LabeledCheckBox::OnLabelSize, this);
    Bind(wxEVT_PAINT, &LabeledCheckBox::OnPaint, this);
}

void LabeledCheckBox::SetValue(bool value)
{
    m_value        = value;
    m_half_checked = false;
    UpdateState();
}

void LabeledCheckBox::SetHalfChecked(bool half)
{
    m_half_checked = half;
    UpdateState();
}

void LabeledCheckBox::Toggle()
{
    SetValue(!m_value);

    // A handler may destroy the control
    const wxWeakRef<wxWindow> self(this);
    const wxWindowID id    = GetId();
    const bool       value = m_value;
    for (wxEventType type : {wxEVT_CHECKBOX, wxEVT_TOGGLEBUTTON}) {
        wxCommandEvent evt(type, id);
        evt.SetEventObject(this);
        evt.SetInt(value);
        ProcessWindowEvent(evt);
        if (!self)
            return;
    }
}

void LabeledCheckBox::UpdateState()
{
    wxString icon = m_half_checked ? "check_half" : m_value ? "check_on" : "check_off";
    if (!m_enabled)
        icon += "_disabled";
    else if (m_hovered || m_focused)
        icon += "_focused";
    m_check->SetIcon(icon);

    // Only the color of the label changes when disabled, see Enable()
    if (m_text->SetForegroundColour(m_enabled ? GetForegroundColour() : StateColor::darkModeColorFor(wxColour("#6B6A6A"))))
        m_text->Refresh();
}

void LabeledCheckBox::UpdateMetrics()
{
    const int padding = FromDIP(LABEL_PADDING);
    GetSizer()->GetItem(m_text)->SetBorder(padding);
    // Keeps the check on the first line of the text, whether it is wrapped or not
    const int offset = m_text->IsShown() ? padding + (m_text->GetCharHeight() - m_check->GetMinSize().y) / 2 : 0;
    GetSizer()->GetItem(m_check)->SetBorder(std::max(offset, 0));

    // Back to one line, the layout wraps the label again if there is no room for it
    m_text->Wrap(-1);
    // Label wraps by the extent of the text: it stays on one line when it is wider than this
    m_text_width = wxClientDC(m_text).GetMultiLineTextExtent(GetLabel()).x + 1;
}

void LabeledCheckBox::Relayout()
{
    UpdateMetrics();
    SetSize(GetBestSize());
    Layout();
    m_best_size = GetBestSize();
    SendSizeEventToParent();
    Refresh();
    // GTK reports the best size of the label for the new font only once it updated its style
    CallAfter(&LabeledCheckBox::CheckBestSize);
}

void LabeledCheckBox::CheckBestSize()
{
    const wxSize best = GetBestSize();
    if (best == m_best_size)
        return;
    m_best_size = best;
    SendSizeEventToParent();
    Refresh();
}

bool LabeledCheckBox::Layout()
{
    // The label wraps itself to the width it gets here. Keep that width inside our size limits:
    // a control squeezed below its minimal size is clipped like any other one instead of wrapped,
    // and a layout made before the parent has its final size can't leave a wrapped label behind.
    const wxSize size  = GetClientSize();
    int          width = std::max(size.x, GetEffectiveMinSize().x);
    if (GetMaxSize().x > 0)
        width = std::min(width, GetMaxSize().x);
    GetSizer()->SetDimension(0, 0, width, size.y);
    return true;
}

wxSize LabeledCheckBox::DoGetBestClientSize() const
{
    // Height of the label as it is wrapped now, but never the width of the wrapped text: the
    // parent would not give more and the label could not unwrap again. The best size of the
    // native label alone is not enough, it can be measured differently than the text extent.
    wxSize size = GetSizer()->GetMinSize();
    if (m_text->IsShown())
        size.x = std::max(size.x, m_check->GetMinSize().x + 2 * FromDIP(LABEL_PADDING) + m_text_width);
    return size;
}

void LabeledCheckBox::Rescale()
{
    m_check->Rescale();
    Relayout();
}

void LabeledCheckBox::SetLabel(const wxString& label)
{
    wxPanel::SetLabel(label);
    m_text->SetLabel(label);
    m_text->Show(!label.empty());
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
    UpdateState();
    return changed;
}

bool LabeledCheckBox::SetBackgroundColour(const wxColour& colour)
{
    m_check->SetBackgroundColor(colour); // the one the button paints with
    m_check->SetBackgroundColour(colour);
    m_text->SetBackgroundColour(colour);
    return wxPanel::SetBackgroundColour(colour);
}

void LabeledCheckBox::SetMaxSize(const wxSize& size)
{
    wxPanel::SetMaxSize(size);
    Relayout(); // wraps the label right away, a following Fit() of the parent gets the final height
}

bool LabeledCheckBox::Enable(bool enable)
{
    if (m_enabled == enable)
        return false;
    m_enabled = enable;
    m_check->Enable(enable);
    UpdateState();
    Refresh();
    return true;
}

bool LabeledCheckBox::HasFocus() const { return m_check->HasFocus(); }

void LabeledCheckBox::DoSetToolTip(wxToolTip* tip)
{
    // Children cover the panel, they need the tooltip too
    const wxString text = tip ? tip->GetTip() : wxString();
    m_check->SetToolTip(text);
    m_text->SetToolTip(text);
    wxPanel::DoSetToolTip(tip);
}

void LabeledCheckBox::OnClick(wxMouseEvent& e)
{
    if (!m_enabled)
        return e.Skip();
    m_check->SetFocus();
    Toggle();
}

void LabeledCheckBox::OnHover(wxMouseEvent& e)
{
    e.Skip();
    // Entering a child leaves the panel and the other way around, only the position tells the truth
    const bool hovered = GetScreenRect().Contains(wxGetMousePosition());
    if (hovered != m_hovered) {
        m_hovered = hovered;
        UpdateState();
    }
}

void LabeledCheckBox::OnFocus(wxFocusEvent& e)
{
    e.Skip();
    if (IsBeingDeleted())
        return;
    m_focused  = e.GetEventType() == wxEVT_SET_FOCUS;
    m_held_key = WXK_NONE;
    UpdateState();
    Refresh();
}

void LabeledCheckBox::OnLabelSize(wxSizeEvent& e)
{
    e.Skip();
    // The label wraps itself to its new width, which changes the height we need. Let the parent
    // know once all the sizing in progress is done.
    CallAfter(&LabeledCheckBox::CheckBestSize);
    Refresh(); // focus border
}

void LabeledCheckBox::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    if (!m_focused || !m_enabled || !m_text->IsShown())
        return;

    // Focus border around the label
    wxRect rect = m_text->GetRect();
    rect.Inflate(FromDIP(LABEL_PADDING));
    rect.Intersect(GetClientRect());
    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#009688"))));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(rect);
}
