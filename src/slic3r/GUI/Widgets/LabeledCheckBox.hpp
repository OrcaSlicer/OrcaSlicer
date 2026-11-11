#ifndef slic3r_GUI_LabeledCheckBox_hpp_
#define slic3r_GUI_LabeledCheckBox_hpp_

#include "Button.hpp"
#include "Label.hpp"

#include <wx/panel.h>

// Check box with a clickable label.
//
// The label is a Label with LB_AUTO_WRAP, it wraps by itself to the width the control gets. The
// control asks for the width of the whole text on one line, so the label only wraps where the
// control is allowed to be narrower than that:
//   cb->SetMaxSize(wxSize(width, -1));               // wraps when longer than width
//   cb->SetMinSize(wxSize(width, -1)) + wxEXPAND     // follows the parent, down to width
//
// Like wxCheckBox, a change made by the user sends wxEVT_CHECKBOX (and wxEVT_TOGGLEBUTTON, to be a
// drop-in replacement for CheckBox as well) while SetValue() does not send anything.
class LabeledCheckBox : public wxPanel
{
public:
    LabeledCheckBox(wxWindow* parent, const wxString& label = wxEmptyString, wxWindowID id = wxID_ANY);

    void SetValue(bool value);
    bool GetValue() const { return m_value; }
    bool IsChecked() const { return m_value; }

    // Only changes the look, the next SetValue() or click of the user resets it
    void SetHalfChecked(bool half = true);
    bool IsHalfChecked() const { return m_half_checked; }

    // Direct access to the inner controls, for anything that is not wrapped by this class (binding
    // events, measuring the text...). Prefer SetLabel() / SetFont() / SetForegroundColour() of this
    // class to the ones of the label, they keep the layout and the disabled look in sync.
    // Call Rescale() after changing the label in a way that changes its size.
    Label*        GetLabelCtrl() { return m_text; }
    const Label*  GetLabelCtrl() const { return m_text; }
    Button*       GetCheckCtrl() { return m_check; }
    const Button* GetCheckCtrl() const { return m_check; }

    void Rescale();

    void SetLabel(const wxString& label) override;
    bool SetFont(const wxFont& font) override;
    bool SetForegroundColour(const wxColour& colour) override; // color of the label
    bool SetBackgroundColour(const wxColour& colour) override;
    void SetMaxSize(const wxSize& size) override; // wraps the label right away when it is longer

    // The panel itself stays enabled, a natively disabled label ignores its color on Windows.
    // wxWindow::IsEnabled() is not virtual, this one hides it.
    bool Enable(bool enable = true) override;
    bool IsEnabled() const { return m_enabled; }

    bool HasFocus() const override;
    bool Layout() override;

protected:
    wxSize DoGetBestClientSize() const override;
    void   DoSetToolTip(wxToolTip* tip) override;

private:
    void Toggle();        // change made by the user, sends the events
    void UpdateState();   // icon and label color for the current state
    void UpdateMetrics(); // borders and width for the current font and DPI
    void Relayout();      // text, font or size limits changed
    void CheckBestSize(); // asks the parent for a new layout when the best size changed

    void OnClick(wxMouseEvent& e);
    void OnHover(wxMouseEvent& e);
    void OnFocus(wxFocusEvent& e);
    void OnLabelSize(wxSizeEvent& e);
    void OnPaint(wxPaintEvent& e);

    Button* m_check {nullptr};
    Label*  m_text  {nullptr};

    bool m_value        {false};
    bool m_half_checked {false};
    bool m_enabled      {true};
    bool m_hovered      {false};
    bool m_focused      {false};

    int    m_held_key   {WXK_NONE}; // key being held down on the check
    int    m_text_width {0};        // width the label needs to stay on one line
    wxSize m_best_size;             // last best size the parent was told about
};

#endif // !slic3r_GUI_LabeledCheckBox_hpp_
