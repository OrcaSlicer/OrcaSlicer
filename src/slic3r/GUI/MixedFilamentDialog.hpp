#pragma once

#include "GUI_Utils.hpp"

#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/spinctrl.h>
#include <wx/textctrl.h>

namespace Slic3r {
namespace GUI {

// Minimal Color Mixing dialog (M4). Does not port MixedFilamentConfigPanel.
// Sets mixed_filament_definitions + optional object extruder + enable_prime_tower.
class MixedFilamentDialog : public DPIDialog
{
public:
    explicit MixedFilamentDialog(wxWindow *parent = nullptr);
    ~MixedFilamentDialog() override = default;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    void load_from_config();
    void apply_to_project();

    wxSpinCtrl *m_spin_a{nullptr};
    wxSpinCtrl *m_spin_b{nullptr};
    wxSpinCtrl *m_spin_ratio_a{nullptr};
    wxSpinCtrl *m_spin_ratio_b{nullptr};
    wxTextCtrl *m_pattern{nullptr};
    wxCheckBox *m_enabled{nullptr};
    wxCheckBox *m_apply_object{nullptr};
    wxCheckBox *m_enable_tower{nullptr};
    wxCheckBox *m_local_z{nullptr};
    wxCheckBox *m_full_domain{nullptr};
    wxCheckBox *m_gradient{nullptr};
    wxTextCtrl *m_offset_a{nullptr};
    wxTextCtrl *m_offset_b{nullptr};
};

} // namespace GUI
} // namespace Slic3r
