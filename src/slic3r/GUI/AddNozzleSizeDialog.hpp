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
#include <set>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class AddNozzleSizeDialog : public DPIDialog
{
public:
    // addable_sizes: variant strings (e.g. "0.6") the system model offers and the user lacks.
    // existing_sizes: variant strings the printer already has (for custom-size duplicate detection).
    AddNozzleSizeDialog(wxWindow* parent, const std::string& user_model,
                        const std::vector<std::string>& addable_sizes, const std::vector<std::string>& existing_sizes);
    ~AddNozzleSizeDialog();

    // Checked sizes from the list (variant strings, as passed in).
    std::vector<std::string> get_checked_sizes() const;
    // Validated + normalized custom size as a printer_variant string ("0.7"), or empty if the field
    // is blank. Only meaningful after wxID_OK, which is only enabled when the field is valid.
    std::string get_custom_variant() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override {}

private:
    // Parse the (trimmed) custom-size text. Returns true + the diameter/variant on a valid entry.
    bool parse_custom(double& diameter_out, std::string& variant_out) const;
    // Validate the custom field: inline orange warning + OK gating (mirrors SavePresetDialog).
    void update_valid();

    std::vector<std::string> m_sizes;      // addable system sizes (checklist order)
    std::set<std::string>    m_existing;   // sizes the printer already has
    CheckList*               m_check_list  {nullptr};
    ::TextInput*             m_custom_input{nullptr};
    wxStaticText*            m_valid_label {nullptr};
    wxWindow*                m_ok_btn      {nullptr};
};

}} // namespace Slic3r::GUI

#endif
