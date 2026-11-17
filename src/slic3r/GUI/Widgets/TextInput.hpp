#ifndef slic3r_GUI_TextInput_hpp_
#define slic3r_GUI_TextInput_hpp_

#include <wx/containr.h>
#include <wx/gdicmn.h>
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <wx/string.h>
#include <wx/event.h>
#include <wx/dc.h>
#include <wx/textctrl.h>
#include <vector>
#include "StaticBox.hpp"

class TextInput : public wxNavigationEnabled<StaticBox>
{

    wxSize labelSize;
    ScalableBitmap icon;
    ScalableBitmap icon_1;
    StateColor     label_color;
    StateColor     text_color;
    StateColor     unit_track_color;
    StateColor     unit_hover_color;
    wxTextCtrl * text_ctrl;

    wxString  static_tips;
    wxSize    static_tips_size;
    wxBitmap  static_tips_icon;

    std::vector<wxString> units;
    std::vector<wxRect>   unit_rects; // filled by render()
    int                   unit_selection{0};
    int                   unit_hover{-1}; // unit under the mouse that a click would select

    static const int TextInputWidth = 200;
    static const int TextInputHeight = 50;

public:
    TextInput();

    TextInput(wxWindow *     parent,
              wxString       text,
              wxString       label = "",
              wxString       icon  = "",
              const wxPoint &pos   = wxDefaultPosition,
              const wxSize & size  = wxDefaultSize,
              long           style = 0);

public:
    void Create(wxWindow *     parent,
              wxString       text,
              wxString       label = "",
              wxString       icon  = "",
              const wxPoint &pos   = wxDefaultPosition,
              const wxSize & size  = wxDefaultSize,
              long           style = 0);

    // Only meant to be used by inspector, not public API
    int GetCornerRadius() const { return static_cast<int>(radius); }

    void SetLabel(const wxString& label) override;

    // ORCA: alternative units drawn in place of the label as a segmented control, e.g. "mm" and "%".
    // A click on a unit that is not selected selects it and sends wxEVT_CHOICE. SetLabel() removes the units.
    void SetUnits(const std::vector<wxString>& units, int selection = 0);
    void SetUnitSelection(int selection);
    int  GetUnitSelection() const { return unit_selection; }

    void SetStaticTips(const wxString& tips, const wxBitmap& bitmap);

    void SetIcon(const wxBitmap & icon);
    void SetIcon(const wxString & icon);

    void SetIcon_1(const wxString &icon);
    void SetIcon_1(const wxBitmap &icon);

    void SetLabelColor(StateColor const &color);

    void SetTextColor(StateColor const &color);

    virtual void Rescale();

    virtual bool Enable(bool enable = true) override;

    virtual void SetMinSize(const wxSize& size) override;

    wxTextCtrl *GetTextCtrl() { return text_ctrl; }

    wxTextCtrl const *GetTextCtrl() const { return text_ctrl; }

protected:
    virtual void OnEdit() {}

    virtual void DoSetSize(
        int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO) override;

    void DoSetToolTipText(wxString const &tip) override;

private:
    void paintEvent(wxPaintEvent& evt);

    void render(wxDC& dc);

    void messureSize();

    int unitAt(const wxPoint& pos) const;

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_TextInput_hpp_
