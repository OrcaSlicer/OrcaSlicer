#ifndef slic3r_GUI_RADIOBOX_hpp_
#define slic3r_GUI_RADIOBOX_hpp_

#include "../wxExtensions.hpp"

#include <wx/tglbtn.h>

class wxWindow;

namespace Slic3r {
namespace GUI {

class RadioBox : public wxBitmapToggleButton
{
public:
    RadioBox(wxWindow *parent);

public:
    void SetValue(bool value) override;
    void Rescale();
    bool Disable() {
        return wxBitmapToggleButton::Disable();
    }

protected:
#if wxUSE_ACCESSIBILITY
    // ORCA: a radio button rendered purely as bitmap swaps has no accessible
    // role/state/name of its own by default.
    wxAccessible* CreateAccessible() override;
#endif

private:
    void update();

private:
    ScalableBitmap m_on;
    ScalableBitmap m_off;
    ScalableBitmap m_ban;
};

}}



#endif // !slic3r_GUI_CheckBox_hpp_
