#ifndef slic3r_MixedFilamentDialog_hpp_
#define slic3r_MixedFilamentDialog_hpp_

#include <string>
#include <vector>
#include <wx/bitmap.h>
#include <wx/panel.h>
#include <wx/checkbox.h>
#include <wx/stattext.h>

#include "GUI_Utils.hpp"

class Button;
class ComboBox;

namespace Slic3r {
namespace GUI {

struct MixedFilamentResult {
    std::vector<unsigned int> components;   // 1-based physical filament indices
    std::vector<int>          ratios;       // percentages, sum = 100
    bool gradient_enabled  = false;
    int  gradient_direction = 0;            // 0 = A→B, 1 = B→A  (only for 2-color)
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
    void on_add_material();
    void on_recommendation_clicked(unsigned int comp_a, unsigned int comp_b);
    void update_preview();
    void update_ok_button_state();
    void update_gradient_direction_items();
    void update_component_count_ui();
    void update_type_constraints();

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

    // UI controls
    wxPanel*                    m_preview_canvas{nullptr};
    wxPanel*                    m_summary_panel{nullptr};
    std::vector<ComboBox*>      m_combo_filaments;
    wxBoxSizer*                 m_material_rows_sizer{nullptr};
    wxPanel*                    m_ratio_bar{nullptr};
    wxPanel*                    m_triangle_panel{nullptr};
    wxStaticText*               m_label_ratio_a{nullptr};
    wxStaticText*               m_label_ratio_b{nullptr};
    wxCheckBox*                 m_chk_gradient{nullptr};
    ComboBox*                   m_combo_gradient_dir{nullptr};
    wxBoxSizer*                 m_gradient_sizer{nullptr};
    Button*                     m_btn_add_material{nullptr};
    Button*                     m_btn_ok{nullptr};
    Button*                     m_btn_cancel{nullptr};

    wxBoxSizer*                 m_ratio_sizer{nullptr};
    wxBoxSizer*                 m_triangle_sizer{nullptr};
    wxBoxSizer*                 m_right_sizer{nullptr};

    // Cached preview bitmaps (loaded once at construction)
    wxBitmap                    m_preview_bmp_two;
    wxBitmap                    m_preview_bmp_three;

    // Drag state
    bool   m_dragging{false};
    // Triangle picker drag point (barycentric weights)
    double m_tri_wx{0.333}, m_tri_wy{0.333}, m_tri_wz{0.334};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_MixedFilamentDialog_hpp_
