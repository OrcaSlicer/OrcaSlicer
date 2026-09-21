#pragma once

#include "CurveEditorPanel.hpp"
#include <utility>
#include <wx/dialog.h>

class wxGrid;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r::GUI {

// Text rows preserve user precision, malformed input, and unchanged serialized values.
class CurveEditorDialog : public wxDialog
{
public:
    using Row = std::pair<wxString, wxString>;
    using Rows = std::vector<Row>;
    ~CurveEditorDialog() override;

protected:
    CurveEditorDialog(wxWindow* parent, const wxString& title, const wxString& help,
                      const CurveEditorAppearance& appearance);
    // Call at the end of the derived constructor, once its model is initialized.
    void initialize(const Rows& rows);
    // Numeric parsing is shared; the model supplies any ordering and endpoint rules.
    virtual wxString validate_points(const std::vector<double>& x, const std::vector<double>& y, int& row) const = 0;
    virtual CurveEditorPanel::Interpolator make_interpolator(const std::vector<double>& x, const std::vector<double>& y) const = 0;
    virtual CurveEditorView fitted_view(const std::vector<double>& x, const std::vector<double>& y) const = 0;
    // Return bounds within the viewport; equal bounds lock an axis for this point.
    virtual CurveEditorView drag_bounds(int row, const std::vector<double>& x, const std::vector<double>& y,
                                       const CurveEditorView& view) const = 0;
    virtual Rows default_rows() const = 0;
    virtual Rows seed_rows() const = 0;
    virtual void accept_rows(const Rows& rows) = 0;
    virtual wxString empty_message() const { return {}; }
    virtual wxString validate_view(const CurveEditorView&) const { return {}; }

private:
    void load_points(const Rows& rows);
    bool read_points(std::vector<double>& x, std::vector<double>& y);
    void finish_edit();
    void update_preview();
    void refresh_chart_data(const std::vector<double>& x, const std::vector<double>& y);
    void move_point(int row, double x, double y);
    void fit_chart();
    void apply_chart_range();
    void sync_chart_range();

    CurveEditorAppearance m_appearance;
    CurveEditorPanel* m_chart;
    wxGrid* m_grid;
    wxStaticText* m_status;
    wxTextCtrl* m_range_fields[4];
    wxStaticText* m_range_status;
};

} // namespace Slic3r::GUI
