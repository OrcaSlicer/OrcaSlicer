#ifndef slic3r_GUI_RenamePrinterModelDialog_hpp_
#define slic3r_GUI_RenamePrinterModelDialog_hpp_

// ORCA #12105 (Phase 5): dialog to bulk-rename a user-defined printer_model across all user printer
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
    void update_ok_state();

    wxComboBox* m_model_combo  {nullptr};
    TextInput*  m_name_input   {nullptr};
    wxWindow*   m_ok_btn       {nullptr};
};

}} // namespace Slic3r::GUI

#endif
