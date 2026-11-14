#ifndef slic3r_GUI_RenamePrinterModelDialog_hpp_
#define slic3r_GUI_RenamePrinterModelDialog_hpp_

// ORCA #12105: dialog to bulk-rename a user-defined printer_model across all user printer
// presets that share it. Also the supported way to clean up auto-generated "<model> - Copy" names
// produced by the legacy-preset migration.

#include "GUI_Utils.hpp"
#include "Widgets/TextInput.hpp"

#include <wx/wx.h>
#include <string>
#include <vector>

class wxComboBox;

namespace Slic3r { namespace GUI {

class RenamePrinterModelDialog : public DPIDialog
{
public:
    RenamePrinterModelDialog(wxWindow* parent, const std::vector<std::string>& models);
    ~RenamePrinterModelDialog();

    std::string get_selected_model() const;
    std::string get_new_name() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override {}

private:
    // Validate the new name (mirrors SavePresetDialog::Item::update): shows an inline orange warning
    // and enables the OK button only when the name is valid. Reuses the existing save-dialog msgids.
    void update_valid();

    std::vector<std::string> m_models;      // existing user printer_model names (for dup detection)
    wxComboBox*   m_model_combo  {nullptr};
    ::TextInput*  m_name_input   {nullptr};
    wxStaticText* m_valid_label  {nullptr};
    wxWindow*     m_ok_btn       {nullptr};
};

}} // namespace Slic3r::GUI

#endif
