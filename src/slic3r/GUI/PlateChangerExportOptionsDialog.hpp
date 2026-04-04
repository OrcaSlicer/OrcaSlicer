#ifndef slic3r_GUI_PlateChangerExportOptionsDialog_hpp_
#define slic3r_GUI_PlateChangerExportOptionsDialog_hpp_

#include "GUI_Utils.hpp"

#include <vector>

class wxFlexGridSizer;
class wxStaticText;
class wxSimplebook;
class wxPanel;
class Button;
class TextInput;

namespace Slic3r { namespace GUI {

class PrintOption;
class Plater;

// Shown only for Export all (plate changer) when the edited printer preset has non-empty
// plate_change_gcode. Reuses PrintOption from SelectMachine.hpp (see .cpp).
class PlateChangerExportOptionsDialog : public DPIDialog
{
public:
    explicit PlateChangerExportOptionsDialog(wxWindow* parent, Plater* plater);

    bool start_with_new_plate() const;
    bool end_with_new_plate() const;
    std::vector<bool> plate_changer_plate_included() const { return m_plate_changer_plate_included; }

private:
    void persist_plate_changer_prefs_to_appconfig();
    void rebuild_plate_stats_grid();
    void refresh_export_project_rename_labels();
    void on_export_project_rename_click(wxCommandEvent& evt);
    void on_export_project_rename_enter();
    void on_ok(wxCommandEvent& evt);
    void on_cancel(wxCommandEvent& evt);
    void on_dpi_changed(const wxRect& suggested_rect) override;

    Plater*          m_plater{nullptr};
    wxPanel*         m_plate_stats_panel{nullptr};
    wxFlexGridSizer* m_plate_stats_grid_sizer{nullptr};
    wxStaticText*    m_stext_export_plate_count{nullptr};

    wxString      m_current_project_name;
    bool          m_is_rename_mode{false};
    wxSimplebook* m_project_rename_book{nullptr};
    wxPanel*      m_project_rename_normal_panel{nullptr};
    wxStaticText* m_export_project_rename_text{nullptr};
    ::Button*     m_export_project_rename_btn{nullptr};
    ::TextInput*  m_export_project_rename_input{nullptr};

    PrintOption* m_opt_start_with_new_plate{nullptr};
    PrintOption* m_opt_end_with_new_plate{nullptr};

    std::vector<bool> m_plate_changer_plate_included;
};

}} // namespace Slic3r::GUI

#endif
