#ifndef slic3r_GUI_RADIOBOX_hpp_
#define slic3r_GUI_RADIOBOX_hpp_

#include "../wxExtensions.hpp"

#include <wx/tglbtn.h>

namespace Slic3r {
namespace GUI {

class RadioBox : public wxBitmapToggleButton
{
public:
    RadioBox(wxWindow *parent);

public:
    void SetValue(bool value) override;
	bool GetValue();
    void Rescale();
    // Orca: Makes a left click anywhere on `label` select this radio button, the way the label
    // of a native wxRadioButton does. The selection itself is left to the handler bound to the
    // box, which receives the forwarded click.
    void BindLabel(wxWindow *label);
    bool Disable() {
        return wxBitmapToggleButton::Disable();
    }
    bool Enable() {
        return wxBitmapToggleButton::Enable();
    }

private:
    void update();

private:
    ScalableBitmap m_on;
    ScalableBitmap m_off;
    ScalableBitmap m_ban;
};

}}



#endif // !slic3r_GUI_CheckBox_hpp_
