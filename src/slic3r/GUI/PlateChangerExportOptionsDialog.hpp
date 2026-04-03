#ifndef slic3r_GUI_PlateChangerExportOptionsDialog_hpp_
#define slic3r_GUI_PlateChangerExportOptionsDialog_hpp_

#include "GUI_Utils.hpp"

namespace Slic3r { namespace GUI {

class PrintOption;

// Shown only for Export all (plate changer) when the edited printer preset has non-empty
// plate_change_gcode. Reuses PrintOption from SelectMachine.hpp (see .cpp).
class PlateChangerExportOptionsDialog : public DPIDialog
{
public:
    explicit PlateChangerExportOptionsDialog(wxWindow* parent);

    bool start_with_new_plate() const;
    bool end_with_new_plate() const;

private:
    void persist_plate_changer_prefs_to_appconfig();
    void on_ok(wxCommandEvent& evt);
    void on_cancel(wxCommandEvent& evt);
    void on_dpi_changed(const wxRect& suggested_rect) override;

    PrintOption* m_opt_start_with_new_plate{nullptr};
    PrintOption* m_opt_end_with_new_plate{nullptr};
};

}} // namespace Slic3r::GUI

#endif
