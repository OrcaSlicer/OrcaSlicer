#pragma once

#include "CurveEditorDialog.hpp"
#include <string>

namespace Slic3r::GUI {

class SmallAreaInfillFlowCompensationDialog : public CurveEditorDialog
{
public:
    SmallAreaInfillFlowCompensationDialog(wxWindow* parent, const std::vector<std::string>& parameters);
    const std::string& get_parameters() const { return m_output_data; }
    bool is_modified() const { return m_modified; }

private:
    wxString validate_points(const std::vector<double>& x, const std::vector<double>& y, int& row) const override;
    CurveEditorPanel::Interpolator make_interpolator(const std::vector<double>& x, const std::vector<double>& y) const override;
    CurveEditorView fitted_view(const std::vector<double>& x, const std::vector<double>& y) const override;
    CurveEditorView drag_bounds(int row, const std::vector<double>& x, const std::vector<double>& y,
                               const CurveEditorView& view) const override;
    Rows default_rows() const override;
    Rows seed_rows() const override;
    void accept_rows(const Rows& rows) override;
    wxString empty_message() const override;
    wxString validate_view(const CurveEditorView& view) const override;

    std::vector<std::string> m_initial_parameters;
    std::string m_output_data;
    bool m_modified = false;
};

} // namespace Slic3r::GUI
