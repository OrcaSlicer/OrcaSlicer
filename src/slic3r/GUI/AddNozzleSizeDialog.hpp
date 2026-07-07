#ifndef slic3r_GUI_AddNozzleSizeDialog_hpp_
#define slic3r_GUI_AddNozzleSizeDialog_hpp_

// ORCA #12105: dialog to add nozzle sizes to a user printer. Reached from the nozzle-diameter
// dropdown's "--Add nozzle --" item (styled like "--Create printer --" in the Printer dropdown).
// Presents a checklist of the sizes the originating system model offers but the user hasn't created
// yet, plus a free-text "Custom size" field for diameters the system model does not ship.

#include "GUI_Utils.hpp"
#include "Widgets/CheckList.hpp"
#include "Widgets/TextInput.hpp"

#include <wx/wx.h>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class AddNozzleSizeDialog : public DPIDialog
{
public:
    // addable_sizes: variant strings (e.g. "0.6") the system model offers and the user lacks.
    AddNozzleSizeDialog(wxWindow* parent, const std::string& user_model, const std::vector<std::string>& addable_sizes);
    ~AddNozzleSizeDialog();

    // Checked sizes from the list (variant strings, as passed in).
    std::vector<std::string> get_checked_sizes() const;
    // Raw text of the custom-size field (caller parses/formats); empty if unused.
    std::string get_custom_text() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override {}

private:
    std::vector<std::string> m_sizes;
    CheckList*               m_check_list {nullptr};
    ::TextInput*             m_custom_input {nullptr};
};

}} // namespace Slic3r::GUI

#endif
