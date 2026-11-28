#pragma once

#include "CurveEditorPanel.hpp"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <wx/defs.h>
#include <wx/gdicmn.h>
#include <wx/recguard.h>
#include <wx/string.h>
#include "GUI_Utils.hpp"
#include "libslic3r/CurveModel.hpp"

class wxFlexGridSizer;
class wxGrid;
class wxPanel;
class wxStaticText;
class wxTextCtrl;
class wxScrolledWindow;
class wxSpinButton;
class Button;
class TextInput;

namespace Slic3r::GUI {

// Reusable presentation and interaction for a wx-independent CurveModel.
class CurveEditorDialog : public DPIDialog
{
public:
    using Rows = CurveModel::Rows;
    ~CurveEditorDialog() override;
    const std::vector<std::string>& get_parameters() const { return m_model->parameters(); }
    bool is_modified() const { return m_model->is_modified(); }

protected:
    // Use a stable, non-localized geometry key for each dialog; an empty key disables persistence.
    CurveEditorDialog(wxWindow* parent, const wxString& title, const wxString& help,
                      const CurveEditorAppearance& appearance, std::unique_ptr<CurveModel> model,
                      std::string geometry_key = {});
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override;
#ifdef __WXMSW__
    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

private:
    void load_points(const Rows& rows);
    Rows read_rows() const;
    bool read_points(std::vector<double>& x, std::vector<double>& y);
    void finish_edit();
    void update_preview();
    void refresh_chart_data(const std::vector<double>& x, const std::vector<double>& y);
    void move_point(int row, double x, double y);
    void fit_chart();
    void apply_chart_range();
    void step_chart_range(int field, int direction);
    void sync_chart_range();
    void update_range_tooltips();
    void import_csv();
    void export_csv();
    void paste_points();
    void layout_content();
    int grid_height() const;
    void layout_actions();
    void update_grid_metrics();
    void update_grid_colours();

    CurveEditorAppearance m_appearance;
    std::unique_ptr<CurveModel> m_model;
    std::string m_geometry_key;
    wxScrolledWindow* m_content;
    CurveEditorPanel* m_chart;
    wxGrid* m_grid;
    std::array<int, 2> m_column_widths{};
    wxPanel* m_table_panel;
    wxPanel* m_actions_panel;
    Button* m_add_point;
    Button* m_remove_point;
    Button* m_export_csv;
    Button* m_ok;
    wxStaticText* m_status;
    wxFlexGridSizer* m_ranges;
    TextInput* m_range_fields[4];
    wxSpinButton* m_range_arrows[4];
    wxStaticText* m_range_status;
    wxString m_status_text;
    std::vector<double> m_points_x;
    std::vector<double> m_points_y;
    // Redraw theme-dependent widget icons and styles after a system appearance change.
    std::vector<std::function<void()>> m_theme_rescales;
    wxRecursionGuardFlag m_layout_depth = 0;
    wxRecursionGuardFlag m_grid_resize_depth = 0;
#ifdef __WXMSW__
    // The rect of the last WM_DPICHANGED, its position relative to the window's position then,
    // and the window's size then: a border drag that goes on after the DPI change voids the rect.
    struct { wxRect rect; wxSize size; } m_dpi_suggested;
#endif
};

} // namespace Slic3r::GUI
