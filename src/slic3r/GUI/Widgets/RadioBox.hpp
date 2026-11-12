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

#ifdef __WXMSW__
    bool MSWCommand(WXUINT param, WXWORD id) override;
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
