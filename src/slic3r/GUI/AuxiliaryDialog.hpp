#ifndef slic3r_GUI_AuxiliaryDialog_hpp_
#define slic3r_GUI_AuxiliaryDialog_hpp_

#include <wx/wx.h>
#include <wx/intl.h>
#include <wx/collpane.h>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"

class AuxiliaryList;

namespace Slic3r { 
namespace GUI {

class AuxiliaryDialog : public DPIDialog
{
public:
    AuxiliaryDialog(wxWindow * parent);

    AuxiliaryList * aux_list() { return m_aux_list; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    AuxiliaryList * m_aux_list;
};

} // namespace GUI
} // namespace Slic3r

#endif
