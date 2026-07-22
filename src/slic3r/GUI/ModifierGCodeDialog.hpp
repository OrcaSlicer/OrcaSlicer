#ifndef slic3r_ModifierGCodeDialog_hpp_
#define slic3r_ModifierGCodeDialog_hpp_

#include <string>

#include "GUI_Utils.hpp"

class wxTextCtrl;
class wxCheckBox;

namespace Slic3r {
namespace GUI {

// Which feature types a modifier's enter/exit G-code applies to. Mirrors the
// modifier_gcode_on_walls/infill/support/skirt_brim PrintRegionConfig options.
struct ModifierGCodeFeatureToggles
{
    bool walls          = true;
    bool infill         = true;
    bool support        = true;
    bool skirt_brim     = true;
    // Orca: mirrors modifier_group_together. Unlike the four toggles above, this defaults to
    // false — it changes infill print order (all of it deferred to one block per layer) and
    // should be an explicit opt-in; walls are unaffected.
    bool group_together = false;
};

// Dialog for editing a modifier volume's "enter"/"exit" custom G-code, fired whenever
// G-code emission starts/stops printing extrusions that belong to that modifier's region.
class ModifierGCodeDialog : public DPIDialog
{
    wxTextCtrl *m_enter_gcode_editor{ nullptr };
    wxTextCtrl *m_exit_gcode_editor{ nullptr };
    wxCheckBox *m_walls_checkbox{ nullptr };
    wxCheckBox *m_infill_checkbox{ nullptr };
    wxCheckBox *m_support_checkbox{ nullptr };
    wxCheckBox *m_skirt_brim_checkbox{ nullptr };
    wxCheckBox *m_group_together_checkbox{ nullptr };

public:
    ModifierGCodeDialog(wxWindow *parent, const std::string &enter_gcode, const std::string &exit_gcode,
                         const ModifierGCodeFeatureToggles &feature_toggles);

    std::string get_enter_gcode() const;
    std::string get_exit_gcode() const;
    ModifierGCodeFeatureToggles get_feature_toggles() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
};

} // namespace GUI
} // namespace Slic3r

#endif
