#ifndef slic3r_MixedFilamentDialog_hpp_
#define slic3r_MixedFilamentDialog_hpp_

#include <array>
#include <string>
#include <utility>
#include <vector>
#include <wx/bitmap.h>
#include <wx/panel.h>
#include <wx/tglbtn.h>
#include <wx/stattext.h>

#include "GUI_Utils.hpp"
#include "libslic3r/FilamentMixer.hpp"

class Button;
class CheckBox;
class ComboBox;
class wxMouseEvent;
class wxScrolledWindow;
class wxTextCtrl;
class wxWrapSizer;

namespace Slic3r {
namespace GUI {

class GradientCurveEditor;
class RatioLabelPanel;

struct MixedFilamentResult {
    std::vector<unsigned int> components;   // 1-based physical filament indices
    std::vector<int>          ratios;       // percentages, sum = 100
    bool gradient_enabled   = false;
    int  gradient_direction = 0;            // 0 = A→B, 1 = B→A  (only for 2-color)
    bool per_part_gradient  = false;        // valid only when gradient_enabled == true
    // Optional Photoshop-style custom curve overriding the linear A→B gradient.
    // Empty -> use linear (gradient_direction). Non-empty -> cubic Hermite over [0,1]^2
    // with optional per-anchor tangent overrides (see GradientAnchor).
    std::vector<GradientAnchor> gradient_curve;
};

class MixedFilamentDialog : public DPIDialog
{
public:
    MixedFilamentDialog(wxWindow* parent,
                        const std::vector<std::string>& physical_colors,
                        const std::vector<std::string>& physical_names,
                        const std::vector<std::string>& physical_types = {});

    MixedFilamentDialog(wxWindow* parent,
                        const MixedFilamentResult& existing,
                        const std::vector<std::string>& physical_colors,
                        const std::vector<std::string>& physical_names,
                        const std::vector<std::string>& physical_types = {});

    MixedFilamentResult get_result() const { return m_result; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build_ui();
    wxBoxSizer* create_preview_panel();
    wxBoxSizer* create_material_selection();
    wxBoxSizer* create_ratio_slider();
    wxBoxSizer* create_triangle_picker();
    wxBoxSizer* create_gradient_section();
    wxBoxSizer* create_recommendation_grid();
    wxBoxSizer* create_button_panel();

    void on_filament_changed();
    void on_ratio_changed(int new_ratio_a);
    void on_gradient_toggled();
    void on_gradient_direction_changed();
    void on_gradient_curve_changed();
    void on_per_part_gradient_toggled();
    void on_add_material();
    void on_remove_material();
    void on_recommendation_clicked(unsigned int comp_a, unsigned int comp_b);
    void on_recommendation_clicked_triple(unsigned int a, unsigned int b, unsigned int c);
    void apply_manual_ratio(size_t idx, int value);
    void apply_dragged_triangle_ratio(int r0, int r1, int r2);
    void reset_manual_ratio_state();
    void refresh_ratio_labels();
    void sync_triangle_weights_from_ratios();
    void start_ratio_editor(size_t idx, wxWindow* anchor, const wxRect& anchor_rect);
    void commit_ratio_editor(bool apply);
    void commit_ratio_editor_from_background(wxMouseEvent& e);
    void update_preview();
    void update_ok_button_state();
    void update_gradient_direction_items();
    void update_component_count_ui();
    // Picks dialog (width, height) based on current state so the gradient curve
    // editor and the recommendation list stay visible at the same time.
    wxSize compute_dialog_size() const;
    void rebuild_all_combos();
    void rebuild_recommendation_items();
    void refresh_curve_editor_colors();
    void paint_warning_panel(wxPaintEvent& evt);

    wxBitmap make_swatch_bitmap(size_t idx);

    // Helpers for component/ratio access
    size_t          num_components() const { return m_result.components.size(); }
    unsigned int    comp(size_t i) const { return (i < m_result.components.size()) ? m_result.components[i] : 1; }
    int             ratio(size_t i) const { return (i < m_result.ratios.size()) ? m_result.ratios[i] : 0; }
    wxColour        comp_colour(size_t i) const;

    MixedFilamentResult         m_result;
    bool                        m_edit_mode{false};
    std::vector<std::string>    m_physical_colors;
    std::vector<std::string>    m_physical_names;
    std::vector<std::string>    m_physical_types;
    wxString                    m_type_mismatch_msg;

    // Combo item index -> 1-based physical filament index (per combo)
    std::vector<std::vector<unsigned int>> m_combo_to_physical;

    // UI controls
    wxPanel*                    m_preview_canvas{nullptr};
    wxPanel*                    m_summary_panel{nullptr};
    std::vector<ComboBox*>      m_combo_filaments;
    wxBoxSizer*                 m_material_rows_sizer{nullptr};
    wxPanel*                    m_ratio_bar{nullptr};
    wxPanel*                    m_triangle_panel{nullptr};
    RatioLabelPanel*            m_label_ratio_a{nullptr};
    RatioLabelPanel*            m_label_ratio_b{nullptr};
    wxPanel*                    m_ratio_editor_panel{nullptr};
    wxTextCtrl*                 m_ratio_editor{nullptr};
    CheckBox*                   m_chk_gradient{nullptr};
    wxStaticText*               m_label_gradient{nullptr};
    ComboBox*                   m_combo_gradient_dir{nullptr};
    wxBoxSizer*                 m_gradient_sizer{nullptr};
    GradientCurveEditor*        m_curve_editor{nullptr};
    wxBoxSizer*                 m_curve_sizer{nullptr};
    CheckBox*                   m_chk_per_part_gradient{nullptr};
    wxStaticText*               m_label_per_part_gradient{nullptr};
    wxBoxSizer*                 m_per_part_gradient_sizer{nullptr};
    Button*                     m_btn_add_material{nullptr};
    Button*                     m_btn_remove_material{nullptr};
    Button*                     m_btn_ok{nullptr};
    Button*                     m_btn_cancel{nullptr};
    wxBoxSizer*                 m_warning_sizer{nullptr};
    wxPanel*                    m_warning_panel{nullptr};

    wxBoxSizer*                 m_ratio_sizer{nullptr};
    wxBoxSizer*                 m_triangle_sizer{nullptr};
    wxBoxSizer*                 m_right_sizer{nullptr};

    wxScrolledWindow*           m_recommendation_scroll{nullptr};
    wxWrapSizer*                m_recommendation_grid{nullptr};

    // Cached preview bitmaps (loaded once at construction)
    wxBitmap                    m_preview_bmp_two;
    wxBitmap                    m_preview_bmp_three;

    // Drag state
    bool   m_dragging{false};
    std::vector<size_t> m_ratio_manual_order;
    size_t m_ratio_editor_idx{0};
    bool   m_ratio_editor_committing{false};
    wxWindow* m_ratio_editor_anchor{nullptr};
    // Triangle picker drag point (barycentric weights)
    double m_tri_wx{0.333}, m_tri_wy{0.333}, m_tri_wz{0.334};

    // Cached triangle color bitmap (invalidated when colors or size change)
    wxBitmap m_tri_cache_bmp;
    wxColour m_tri_cache_c0, m_tri_cache_c1, m_tri_cache_c2;
    wxSize   m_tri_cache_size;
    std::array<RatioLabelPanel*, 3> m_triangle_ratio_labels{nullptr, nullptr, nullptr};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_MixedFilamentDialog_hpp_
