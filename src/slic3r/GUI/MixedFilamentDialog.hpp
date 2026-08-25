#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <wx/wx.h>
#include <wx/statline.h>
#include <wx/dcbuffer.h>
#include <wx/wrapsizer.h>
#include <wx/clrpicker.h>

#include <set>
#include <vector>
#include <string>

// Forward declarations (in global namespace)
class ScalableButton;
class RadioGroup;
class Button;
class ComboBox;
class Label;
class StaticBox;

namespace Slic3r { namespace GUI {

// Forward declarations (only pointers are stored)
class MixedGradientSelector;
class MixedColorMatchPanel;
class MatchRangeSlider;

class MixedFilamentDialog : public DPIDialog
{
public:
    MixedFilamentDialog(wxWindow* parent, const std::vector<std::string>& filament_colours);
    MixedFilamentDialog(wxWindow* parent, const std::vector<std::string>& filament_colours,
                      const Slic3r::MixedFilament& existing);

    const Slic3r::MixedFilament& GetResult() const { return m_result; }
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build_ui();
    void rebuild_filament_rows();
    void on_mode_changed(int mode_index);
    void update_preview();
    void update_gradient_selector_colors();
    void build_swatch_grid();
    void rebuild_swatch_sizer();
    void sync_rows_to_result();
    void resize_gradient_ids(int target_count);
    void update_compatibility_warning();
    wxString get_ratio_warning_msg();
    void display_warning(const wxString& msg);
    void set_error(const wxString& msg);
    std::string compute_preview_color();
    wxBitmap make_color_bitmap(const wxColour& c, int size);
    int max_filaments_for_mode(int mode_index) const;
    void collect_result();
    void draw_strip(wxDC& dc, wxPanel* panel);

    static constexpr int MODE_RATIO    = 0;
    static constexpr int MODE_CYCLE    = 1;
    static constexpr int MODE_MATCH    = 2;
    static constexpr int MODE_GRADIENT = 3;

    // Segmented mode buttons
    class StaticBox*        m_mode_btn_container    = nullptr;
    std::vector<Button*>    m_mode_buttons;
    int                     m_mode_btn_selected     = MODE_RATIO;
    ScalableButton*         m_btn_add_filament    = nullptr;
    ScalableButton*         m_btn_remove_filament = nullptr;
    wxStaticText*           m_filament_card_title  = nullptr;
    wxPanel*                m_filament_rows_panel = nullptr;
    wxBoxSizer*             m_filament_rows_sizer = nullptr;
    wxPanel*                m_preview_panel       = nullptr;
    wxPanel*                m_preview_blend_panel = nullptr;
    MixedGradientSelector*  m_gradient_selector   = nullptr;
    wxPanel*                m_legend_panel         = nullptr;
    wxBoxSizer*             m_legend_sizer         = nullptr;
    std::vector<wxStaticText*> m_legend_labels;
    wxPanel*                m_tri_picker          = nullptr;
    wxSizerItem*            m_ratio_gradient_spacer = nullptr;
    wxSizerItem*            m_ratio_tri_spacer      = nullptr;
    wxPanel*                m_strip_panel         = nullptr;
    wxPanel*                m_cycle_strip_panel   = nullptr;
    wxPanel*                m_swatch_grid_panel   = nullptr;
    std::vector<int>         m_swatch_min_weights;
    wxPanel*                m_error_panel           = nullptr;
    Label*                   m_error_text            = nullptr;
    wxPanel*                m_warning_panel         = nullptr;
    Label*                   m_warning_text          = nullptr;
    Button*                 m_btn_cancel          = nullptr;
    Button*                 m_btn_confirm         = nullptr;
    wxTextCtrl*             m_pattern_ctrl        = nullptr;
    std::vector<class MixedFilamentBadge*>  m_pattern_quick_buttons;

    // Card containers (StaticBox for rounded-corner cards)
    class StaticBox*        m_filament_card         = nullptr;
    wxBoxSizer*             m_filament_card_sizer   = nullptr;
    class StaticBox*        m_ratio_card            = nullptr;
    wxBoxSizer*             m_ratio_card_sizer      = nullptr;
    class StaticBox*        m_cycle_card            = nullptr;
    wxBoxSizer*             m_cycle_card_sizer      = nullptr;
    class StaticBox*        m_swatch_card           = nullptr;
    wxBoxSizer*             m_swatch_card_sizer     = nullptr;
    class StaticBox*        m_gradient_effect_card   = nullptr;
    wxBoxSizer*             m_gradient_effect_card_sizer = nullptr;
    wxScrolledWindow*       m_scrolled_content      = nullptr;
    wxPanel*                m_cycle_blend_panel     = nullptr;
    wxPanel*                m_cycle_legend_panel    = nullptr;
    wxSizer*                m_cycle_legend_sizer    = nullptr;
    std::vector<wxStaticText*> m_cycle_legend_labels;

    // Match mode
    wxBoxSizer*             m_match_panel_sizer   = nullptr;
    MixedColorMatchPanel*   m_match_panel         = nullptr;

    // Match mode's own Mix Ratio card (separate from ratio card)
    class StaticBox*        m_match_ratio_card       = nullptr;
    wxBoxSizer*             m_match_ratio_card_sizer = nullptr;
    MixedGradientSelector*  m_match_gradient_selector = nullptr;
    wxPanel*                m_match_tri_picker       = nullptr;
    wxSizerItem*            m_match_gradient_spacer   = nullptr;
    wxSizerItem*            m_match_tri_spacer        = nullptr;
    wxPanel*                m_match_legend_panel     = nullptr;
    wxBoxSizer*             m_match_legend_sizer     = nullptr;
    std::vector<wxStaticText*> m_match_legend_labels;
    wxPanel*                m_match_strip_panel      = nullptr;
    wxPanel*                m_match_blend_panel      = nullptr;

    // Card 1 (Match mode: filament badges + target color)
    class StaticBox*        m_match_input_card      = nullptr;
    wxPanel*                m_match_badges_panel    = nullptr;
    wxWrapSizer*            m_match_badges_sizer    = nullptr;
    wxPanel*                m_match_target_picker   = nullptr;
    wxTextCtrl*             m_match_hex_input       = nullptr;
    wxPanel*                m_match_hex_wrapper     = nullptr;
    bool                    m_match_hex_error       = false;
    wxPanel*                m_match_target_swatch   = nullptr;

    // Range slider row (inside ratio card, match-mode only)
    wxPanel*                m_match_range_row       = nullptr;
    MatchRangeSlider*       m_match_range_slider    = nullptr;
    wxStaticText*           m_match_range_value     = nullptr;
    int                     m_match_min_pct         = 15;

    ScalableButton*         m_btn_swap_gradient_dir = nullptr;
    int                     m_gradient_direction   = 0;

    double m_tri_wx{1.0/3.0}, m_tri_wy{1.0/3.0}, m_tri_wz{1.0/3.0};
    bool   m_tri_dragging{false};

    // Match-specific tri-picker state (fully independent from ratio)
    std::vector<int>    m_match_tri_indices;
    std::vector<double> m_match_tri_weights;
    bool                m_match_tri_dragging{false};
    double m_match_tri_wx{1.0/3.0}, m_match_tri_wy{1.0/3.0}, m_match_tri_wz{1.0/3.0};

    void build_tri_picker(wxWindow* parent = nullptr);
    void build_match_tri_picker(wxWindow* parent);
    void set_combo_combined_icon(class ComboBox* cb, int filament_idx);
    void rebuild_legend();
    void update_legend_text();
    void rebuild_match_legend();
    void update_match_legend_labels();    void rebuild_cycle_legend();
    void validate_cycle_pattern();
    void update_ratio_or_tri_visibility();
    // Combo box helpers
    void populate_combo(ComboBox* cb, const std::set<int>& exclude_ids, int select_id,
                        std::vector<int>& out_filament_indices);
    void refresh_all_combos();
    void rebuild_all_combos_with_selections(const std::vector<int>& selections);
    int  get_filament_index(int row_idx) const;

    struct FilamentRow {
        wxPanel*         swatch           = nullptr;
        ComboBox*        combo            = nullptr;
        std::vector<int> filament_indices; // combo index → actual filament index
    };
    std::vector<FilamentRow> m_filament_rows;

    const std::vector<std::string>& m_filament_colours;
    Slic3r::MixedFilament            m_result;
    int                              m_current_mode = MODE_RATIO;
};

}} // namespace Slic3r::GUI
