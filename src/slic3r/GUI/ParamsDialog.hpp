#ifndef slic3r_GUI_ParamsDialog_hpp_
#define slic3r_GUI_ParamsDialog_hpp_

#include <wx/wx.h>
#include <wx/intl.h>
#include <wx/collpane.h>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"

namespace Slic3r { 
namespace GUI {

wxDECLARE_EVENT(EVT_MODIFY_FILAMENT, SimpleEvent);

class Filamentinformation : public wxObject
{
public:
    std::string filament_id;
    std::string filament_name;
};

class ParamsPanel;

class ParamsDialog : public DPIDialog
{
public:
    ParamsDialog(wxWindow * parent);

    ParamsPanel * panel() { return m_panel; }

    void Popup();

    void set_editing_filament_id(std::string id) { m_editing_filament_id = id; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    std::string       m_editing_filament_id;
    ParamsPanel * m_panel;
    wxWindowDisabler *m_winDisabler = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif
