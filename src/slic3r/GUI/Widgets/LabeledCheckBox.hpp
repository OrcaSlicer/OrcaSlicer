#ifndef slic3r_GUI_LabeledCheckBox_hpp_
#define slic3r_GUI_LabeledCheckBox_hpp_

#include "CheckBox.hpp"
#include "Label.hpp"
#include "StaticBox.hpp"

#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/tooltip.h>
#include <wx/window.h>
#include <wx/windowid.h>

// CheckBox with a clickable label
class LabeledCheckBox : public wxPanel
{
public:
    LabeledCheckBox(wxWindow* parent, const wxString& label = wxEmptyString, wxWindowID id = wxID_ANY);

    void SetValue(bool value);
    bool GetValue() const { return m_check->GetValue(); }
    bool IsChecked() const { return m_check->GetValue(); }
    void SetHalfChecked(bool half = true);

    void Wrap(int width);
    void AutoWrap(bool enable = true);

    Label*          GetLabelCtrl() { return m_text; }
    const Label*    GetLabelCtrl() const { return m_text; }

    void Rescale();

    void SetLabel(const wxString& label) override;
    bool SetFont(const wxFont& font) override;
    bool SetForegroundColour(const wxColour& colour) override; // label color
    bool SetBackgroundColour(const wxColour& colour) override;

    bool Enable(bool enable = true) override;
    bool IsEnabled() const { return m_check->IsEnabled(); }

    bool HasFocus() const override { return m_check->HasFocus(); }

protected:
    void DoSetToolTip(wxToolTip* tip) override;
    void DoSetToolTipText(const wxString& tip) override;

private:
    void OnToggled();                  // sends the events
    void OnLabelClick(wxMouseEvent& e);
    void OnLabelHover(wxMouseEvent& e);
    void OnSize(wxSizeEvent& e);
    void ShowFocus(bool focused);
    void ShowHover();                  // hover icon on the check while the mouse is on the label
    void UpdateLabelColor();
    void UpdateMetrics();              // borders and wrap
    void Relayout();

    CheckBox*  m_check    {nullptr};
    StaticBox* m_text_box {nullptr};
    Label*     m_text     {nullptr};

    int  m_wrap_width {-1};            // control width to wrap for, -1: no wrap
    bool m_auto_wrap  {false};
    bool m_hovered    {false};         // mouse is on the label
};

#endif // !slic3r_GUI_LabeledCheckBox_hpp_
