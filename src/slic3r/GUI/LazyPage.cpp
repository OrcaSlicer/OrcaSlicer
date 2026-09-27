#include "LazyPage.hpp"

#include "GUI_App.hpp"

namespace Slic3r { namespace GUI {

void apply_dark_ui_to_lazy_panel(wxWindow* panel)
{
#ifdef _MSW_DARK_MODE
    wxGetApp().UpdateDarkUIWin(panel);
#endif
}

}} // namespace Slic3r::GUI
