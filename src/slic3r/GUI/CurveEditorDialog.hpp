#pragma once

#include "CurveEditorPanel.hpp"
#include <memory>
#include <string>
#include <vector>
#include <wx/dialog.h>
#include <wx/string.h>
#include "libslic3r/CurveModel.hpp"
#include "Widgets/TextInput.hpp"

class wxGrid;
class wxStaticText;
class wxTextCtrl;

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
    CurveEditorDialog(wxWindow* parent, const wxString& title, const wxString& help,
                      const CurveEditorAppearance& appearance, std::unique_ptr<CurveModel> model);

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

    CurveEditorAppearance m_appearance;
    std::unique_ptr<CurveModel> m_model;
    CurveEditorPanel* m_chart;
    wxGrid* m_grid;
    wxStaticText* m_status;
    TextInput* m_range_fields[4];
    wxStaticText* m_range_status;
};

} // namespace Slic3r::GUI
