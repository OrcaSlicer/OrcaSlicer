#ifndef slic3r_GUI_LabeledCheckBox_hpp_
#define slic3r_GUI_LabeledCheckBox_hpp_

#include "../wxExtensions.hpp"
#include "CheckBox.hpp"
#include "Label.hpp"
#include "Button.hpp"

#include <string>
#include <wx/wx.h>

class LabeledCheckBox : public wxPanel
{

public:
    LabeledCheckBox(wxWindow* parent, wxString label = wxEmptyString);

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

    void SetLabelColor(wxColour color);

    void SetLabel(const wxString& label) override;

    bool Enable(bool enable = true) override;

    virtual bool SetFont(const wxFont& font) override;

    wxFont GetFont(){return m_font;};

    bool Disable() {return LabeledCheckBox::Enable(false);};

    bool IsEnabled(){return m_enabled;};

    bool HasFocus() const override;

    void SetMaxSize(const wxSize& size) override;

private:

    void UpdateIcon();

    void UpdateLabelColor(bool enabled);
    void UpdateLabelBorder(bool focused);

    void UpdateAlignment();
    wxSizerItem* m_check_item = nullptr;
    wxSizerItem* m_text_item  = nullptr;

    void ApplyWrap(int width);

    void OnClick();

    wxWindow* GetScrollParent(wxWindow *pWindow);

    ScalableBitmap m_on;
    ScalableBitmap m_off;
    ScalableBitmap m_half;
    ScalableBitmap m_on_disabled;
    ScalableBitmap m_off_disabled;
    ScalableBitmap m_half_disabled;
    ScalableBitmap m_on_focused;
    ScalableBitmap m_off_focused;
    ScalableBitmap m_half_focused;
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
    wxColour        m_label_color;
    wxBoxSizer*     m_sizer;
    int             m_wrap;
    wxSize          m_max_size;
};

#endif // !slic3r_GUI_LabeledCheckBox_hpp_
