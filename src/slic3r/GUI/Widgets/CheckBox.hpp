#ifndef slic3r_GUI_CheckBox_hpp_
#define slic3r_GUI_CheckBox_hpp_

#include "../wxExtensions.hpp"

#include "Label.hpp"
#include "Button.hpp"

#include <string>
#include <wx/wx.h>
#include <wx/tglbtn.h> // to keep wxEVT_TOGGLEBUTTON

class CheckBox : public wxPanel
{
public:
    CheckBox(wxWindow* parent, wxString label = wxEmptyString);

public:
    void SetValue(bool value);

    void SetHalfChecked(bool value = true){
        m_half_checked = value;
        Refresh();
    };

    bool GetValue(){return m_value;};

    bool IsChecked() const {return m_value;};

    void Wrap(int width);

    void Rescale();

    void SetTooltip(wxString label);

    bool Enable(bool enable = true) override {
        m_enabled = enable;
        bool result = m_check->Enable(enable);
        if(m_has_text)
            m_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6A6A"))); // just change color otherwise it has unwanted effect
        UpdateIcon();
        Refresh();
        return result;
    };

    void SetFont(wxFont font){
        m_font = font;
        if(m_has_text)
            m_text->SetFont(font);
    };

    wxFont GetFont(){return m_font;};

    bool Disable() {return CheckBox::Enable(false);};

    bool IsEnabled(){return m_enabled;};

    bool HasFocus(){return m_check->HasFocus();}

private:

    void UpdateIcon();

    void OnClick();

    wxWindow* GetScrollParent(wxWindow *pWindow);

    ScalableBitmap m_on;
    ScalableBitmap m_off;
    ScalableBitmap m_half;
    ScalableBitmap m_on_hover;
    ScalableBitmap m_off_hover;
    ScalableBitmap m_half_hover;
    ScalableBitmap m_on_disabled;
    ScalableBitmap m_off_disabled;
    ScalableBitmap m_half_disabled;
    ScalableBitmap m_on_focused;
    ScalableBitmap m_off_focused;
    ScalableBitmap m_half_focused;
    ScalableBitmap m_on_hvrfcs;
    ScalableBitmap m_off_hvrfcs;
    ScalableBitmap m_half_hvrfcs;
    bool m_half_checked = false;
    bool m_value        = false;
    bool m_enabled      = true;
    bool m_hovered      = false;
    bool m_has_text     = false;
    wxString        m_label;
    Button* m_check = nullptr;
    wxStaticText*   m_text  = nullptr;
    StaticBox*      m_text_box  = nullptr;
    wxFont          m_font;
    wxBoxSizer*     m_sizer;
};

#endif // !slic3r_GUI_CheckBox_hpp_
