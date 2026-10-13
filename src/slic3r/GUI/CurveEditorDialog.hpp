#pragma once

#include "CurveEditorPanel.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <wx/dialog.h>
#include <wx/grid.h>
#include <wx/recguard.h>
#include <wx/string.h>
#include "libslic3r/CurveModel.hpp"

class wxPanel;
class wxStaticText;
class wxTextCtrl;
class wxScrolledWindow;
class wxSpinButton;
class Button;
class TextInput;

namespace Slic3r::GUI {

// Reusable presentation and interaction for a wx-independent CurveModel.
class CurveEditorDialog : public wxDialog
{
public:
    using Rows = CurveModel::Rows;
    ~CurveEditorDialog() override;
    const std::string& get_parameters() const { return m_model->serialized_parameters(); }
    bool is_modified() const { return m_model->is_modified(); }

protected:
    // Escape first abandons only the active cell edit; other dialog changes remain intact.
    static bool cancel_cell_edit(wxGrid& grid) {
        if (!grid.IsCellEditControlEnabled()) return false;
        auto* editor = grid.GetCellEditor(grid.GetGridCursorRow(), grid.GetGridCursorCol());
        editor->Reset();
        editor->DecRef();
        grid.DisableCellEditControl();
        grid.SetFocus();
        return true;
    }
    // Use a stable, non-localized geometry key for each dialog; an empty key disables persistence.
    CurveEditorDialog(wxWindow* parent, const wxString& title, const wxString& help,
                      const CurveEditorAppearance& appearance, std::unique_ptr<CurveModel> model,
                      std::string geometry_key = {});

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
    void layout_actions();
    void update_grid_metrics();

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
    wxStaticText* m_status;
    TextInput* m_range_fields[4];
    wxSpinButton* m_range_arrows[4];
    wxStaticText* m_range_status;
    wxString m_status_text;
    std::vector<double> m_points_x;
    std::vector<double> m_points_y;
    wxRecursionGuardFlag m_layout_depth = 0;
    wxRecursionGuardFlag m_grid_resize_depth = 0;
};

} // namespace Slic3r::GUI
