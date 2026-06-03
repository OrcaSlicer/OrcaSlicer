#pragma once
#include <wx/panel.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include "libslic3r/MixedFilament.hpp"

namespace Slic3r { namespace GUI {

class MixedMixPreview;       // Task 15
class MixedGradientSelector; // Task 16

// ---------------------------------------------------------------------------
// MixedFilamentConfigPanel
//
// Inline per-row editor for a single MixedFilament entry.  Composes
// MixedMixPreview, MixedGradientSelector and MixedGradientWeightsDialog.
//
// Extracted from FullSpectrum Plater.cpp:4588-6857.
// ---------------------------------------------------------------------------
class MixedFilamentConfigPanel : public wxPanel
{
public:
    using OnChangeFn = std::function<void(const MixedFilament &)>;

    MixedFilamentConfigPanel(wxWindow *parent,
                             size_t mixed_id,
                             const MixedFilament &mf,
                             size_t num_physical,
                             const std::vector<std::string> &physical_colors,
                             const std::vector<double> &nozzle_diameters,
                             const std::vector<wxColour> &palette,
                             const MixedFilamentPreviewSettings &preview_settings,
                             bool bias_mode_enabled,
                             OnChangeFn on_change = {});

    // Get the updated mixed filament data.
    MixedFilament get_mixed_filament() const { return m_mf; }
    bool has_changes() const { return m_has_changes; }

    static int effective_local_z_preview_mix_b_percent(const MixedFilament &mf,
                                                       const MixedFilamentPreviewSettings &preview_settings);

private:
    void build_ui();
    void update_preview();
    void update_local_z_breakdown();
    void update_component_picker_visuals();

    // Static helpers — verbatim from FullSpectrum Plater.cpp:4943-6042.
    static std::vector<unsigned int> decode_gradient_ids(const std::string &s);
    static std::string               encode_gradient_ids(const std::vector<unsigned int> &ids);
    static std::vector<unsigned int> decode_manual_pattern_ids(const std::string &pattern,
                                                               unsigned int       a,
                                                               unsigned int       b,
                                                               size_t             num_physical,
                                                               size_t             wall_loops = 0);
    static std::vector<int>          decode_gradient_weights(const std::string &s, size_t n);
    static std::vector<int>          normalize_gradient_weights(const std::vector<int> &w, size_t n);
    static std::string               encode_gradient_weights(const std::vector<int> &w);
    static std::vector<unsigned int> build_weighted_pair_sequence(unsigned int a, unsigned int b, int percent_b, bool limit_cycle = false);
    static std::vector<unsigned int> build_weighted_multi_sequence(const std::vector<unsigned int> &ids,
                                                                   const std::vector<int> &weights,
                                                                   size_t max_cycle_limit = 0);
    static std::string               summarize_sequence(const std::vector<unsigned int> &seq);
    static std::string               summarize_local_z_breakdown(const MixedFilament &mf,
                                                                 const std::vector<int> &weights,
                                                                 const MixedFilamentPreviewSettings &preview_settings);
    static std::string               blend_from_sequence(const std::vector<std::string> &colors,
                                                         const std::vector<unsigned int> &seq,
                                                         const std::string &fallback);
    static std::vector<double>       build_local_z_preview_pass_heights(double nominal_layer_height,
                                                                        double lower_bound,
                                                                        double upper_bound,
                                                                        double preferred_a_height,
                                                                        double preferred_b_height,
                                                                        int mix_b_percent,
                                                                        int max_sublayers_limit);

    size_t                          m_mixed_id;
    MixedFilament                   m_mf;
    size_t                          m_num_physical;
    std::vector<std::string>        m_physical_colors;
    std::vector<double>             m_nozzle_diameters;
    std::vector<wxColour>           m_palette;
    MixedFilamentPreviewSettings    m_preview_settings;
    bool                            m_bias_mode_enabled = false;
    bool                            m_has_changes       = false;

    wxChoice                       *m_choice_a = nullptr;
    wxChoice                       *m_choice_b = nullptr;
    wxChoice                       *m_choice_c = nullptr;
    wxChoice                       *m_choice_d = nullptr;
    wxPanel                        *m_picker_a_container = nullptr;
    wxPanel                        *m_picker_b_container = nullptr;
    wxPanel                        *m_picker_c_container = nullptr;
    wxPanel                        *m_picker_d_container = nullptr;
    wxPanel                        *m_picker_a_swatch = nullptr;
    wxPanel                        *m_picker_b_swatch = nullptr;
    wxPanel                        *m_picker_c_swatch = nullptr;
    wxPanel                        *m_picker_d_swatch = nullptr;
    wxStaticText                   *m_picker_a_label = nullptr;
    wxStaticText                   *m_picker_b_label = nullptr;
    wxStaticText                   *m_picker_c_label = nullptr;
    wxStaticText                   *m_picker_d_label = nullptr;
    wxPanel                        *m_surface_offset_target_container = nullptr;
    wxPanel                        *m_surface_offset_target_swatch    = nullptr;
    wxStaticText                   *m_surface_offset_target_label     = nullptr;
    MixedGradientSelector          *m_blend_selector = nullptr;
    wxStaticText                   *m_blend_label    = nullptr;
    wxTextCtrl                     *m_pattern_ctrl   = nullptr;
    wxCheckBox                     *m_local_z_limit_checkbox = nullptr;
    wxSpinCtrl                     *m_local_z_limit_spin     = nullptr;
    wxSpinCtrlDouble               *m_surface_offset_spin    = nullptr;
    std::vector<wxButton *>         m_pattern_quick_buttons;
    MixedMixPreview                *m_mix_preview    = nullptr;
    wxStaticText                   *m_breakdown_label = nullptr;
    wxPanel                        *m_swatch          = nullptr;
    std::shared_ptr<std::vector<int>> m_selected_weight_state;
    OnChangeFn                       m_on_change;
};

} } // namespace Slic3r::GUI
