#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentMatch.hpp"

#include <wx/checkbox.h>
#include <wx/clrpicker.h>
#include <wx/dialog.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/textctrl.h>

#include <vector>

class wxButton;
class wxListBox;
class wxStaticText;

namespace Slic3r {
namespace GUI {

// Compact multi-row Color Mixing dialog (M6). Does not port MixedFilamentConfigPanel.
// Sets mixed_filament_definitions + optional volume/object extruder + enable_prime_tower.
class MixedFilamentDialog : public DPIDialog
{
public:
    explicit MixedFilamentDialog(wxWindow *parent = nullptr);
    ~MixedFilamentDialog() override = default;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    void load_from_config();
    bool apply_to_project();

    void refresh_list();
    void refresh_list_labels();
    void load_selected_row_into_editors();
    void store_editors_into_selected_row();
    void on_list_select(wxCommandEvent &evt);
    void on_add_row(wxCommandEvent &evt);
    void on_remove_row(wxCommandEvent &evt);
    void on_create_mix_from_color(wxCommandEvent &evt);
    void on_target_colour_changed(wxColourPickerEvent &evt);
    void on_hex_target_changed();
    void on_candidate_select(wxCommandEvent &evt);
    void on_min_mix_slider(wxCommandEvent &evt);
    void refresh_predicted_swatch();
    // preserve_selection: Min Mix slider uses true. Hex/picker reset to ranked [0] only when
    // decoded RGB changed; same RGB skips rebuild (no Clear).
    void refresh_candidates(bool preserve_selection = false);
    bool parse_target_color(ColorRGB &out) const;
    wxString candidate_label(const MixMatchResult &r, const std::vector<std::string> *slot_names) const;

    wxString row_label(size_t idx, size_t num_physical) const;
    int      virtual_id_for_row(size_t idx, size_t num_physical) const;
    size_t   physical_filament_count() const;
    size_t   enabled_mix_count() const;
    std::vector<ColorRGB> live_physical_colors() const;

    std::vector<MixedFilament>  m_rows;
    std::vector<MixMatchResult> m_candidates;
    ColorRGB                    m_candidates_target;
    bool                        m_candidates_target_valid{false};
    int                         m_selected_row{-1};
    bool                        m_suppress_events{false};

    wxListBox  *m_list{nullptr};
    wxButton   *m_btn_add{nullptr};
    wxButton   *m_btn_remove{nullptr};
    wxSpinCtrl *m_spin_a{nullptr};
    wxSpinCtrl *m_spin_b{nullptr};
    wxSpinCtrl *m_spin_c{nullptr};
    wxSpinCtrl *m_spin_ratio_a{nullptr};
    wxSpinCtrl *m_spin_ratio_b{nullptr};
    wxSpinCtrl *m_spin_ratio_c{nullptr};
    wxTextCtrl *m_pattern{nullptr};
    wxCheckBox *m_enabled{nullptr};
    wxCheckBox *m_apply_object{nullptr};
    wxCheckBox *m_enable_tower{nullptr};
    wxCheckBox *m_local_z{nullptr};
    wxCheckBox *m_full_domain{nullptr};
    wxCheckBox *m_gradient{nullptr};
    wxTextCtrl *m_offset_a{nullptr};
    wxTextCtrl *m_offset_b{nullptr};

    wxColourPickerCtrl *m_clr_target{nullptr};
    wxTextCtrl         *m_hex_target{nullptr};
    wxButton           *m_btn_create_mix{nullptr};
    wxColourPickerCtrl *m_clr_predicted{nullptr};
    wxListBox          *m_candidate_list{nullptr};
    wxSlider           *m_min_mix_slider{nullptr};
    wxStaticText       *m_min_mix_label{nullptr};
};

} // namespace GUI
} // namespace Slic3r
