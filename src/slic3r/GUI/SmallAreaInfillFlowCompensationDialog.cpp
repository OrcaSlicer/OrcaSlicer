#include "SmallAreaInfillFlowCompensationDialog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include "I18N.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/PchipInterpolatorHelper.hpp"

namespace Slic3r::GUI {
namespace {

std::pair<wxString, wxString> split_point(const std::string& parameter)
{
    const wxString text = wxString::FromUTF8(parameter);
    const int comma = text.Find(',');
    // Keep malformed input visible so the user can repair it without losing rows.
    wxString length = comma == wxNOT_FOUND ? text : text.Left(comma);
    wxString factor = comma == wxNOT_FOUND ? wxString() : text.Mid(comma + 1);
    return {length.Trim().Trim(false), factor.Trim().Trim(false)};
}
CurveEditorDialog::Rows decode_rows(const std::vector<std::string>& parameters)
{
    CurveEditorDialog::Rows rows;
    for (const auto& parameter : parameters) rows.push_back(split_point(parameter));
    return rows;
}

} // namespace

SmallAreaInfillFlowCompensationDialog::SmallAreaInfillFlowCompensationDialog(
    wxWindow* parent, const std::vector<std::string>& parameters)
    : CurveEditorDialog(parent, _L("Flow Compensation Model"),
        _L("Drag points on the graph or edit their extrusion length and flow correction factor in the table. "
           "Lengths and factors must strictly increase. The first length must be 0 and the last factor must be 1."),
        {_L("Extrusion length") + " (mm)", _L("Flow correction factor"), 1.0, "1.0", 0.0, 1.0}),
      m_initial_parameters(parameters)
{
    initialize(decode_rows(parameters));
}

wxString SmallAreaInfillFlowCompensationDialog::validate_points(
    const std::vector<double>& x, const std::vector<double>& y, int& row) const
{
    if (x.size() == 1) return _L("The model must contain at least two points.");
    for (size_t i = 0; i < x.size(); ++i) {
        row = int(i);
        if (i == 0 && x[i] != 0.0) return _L("The first extrusion length must be 0.");
        if (i > 0 && x[i] <= x[i - 1]) return _L("Extrusion lengths must strictly increase.");
        if (i > 0 && y[i] <= y[i - 1]) return _L("Flow correction factors must strictly increase.");
        if (i == x.size() - 1 && y[i] != 1.0) return _L("The last flow correction factor must be 1.");
    }
    return {};
}

CurveEditorPanel::Interpolator SmallAreaInfillFlowCompensationDialog::make_interpolator(
    const std::vector<double>& x, const std::vector<double>& y) const
{
    return [model = PchipInterpolatorHelper(x, y)](double value) { return model.interpolate(value); };
}

CurveEditorView SmallAreaInfillFlowCompensationDialog::fitted_view(
    const std::vector<double>& x, const std::vector<double>& y) const
{
    double maximum = x.empty() ? 1.0 : x.back();
    if (maximum <= std::numeric_limits<double>::max() / 1.1) maximum *= 1.1;
    return {0.0, maximum, y.empty() ? 0.0 : std::min(0.0, y.front()), 1.0};
}

CurveEditorView SmallAreaInfillFlowCompensationDialog::drag_bounds(
    int row, const std::vector<double>& x, const std::vector<double>& y, const CurveEditorView& view) const
{
    CurveEditorView bounds = view;
    const int last = int(x.size()) - 1;
    const double infinity = std::numeric_limits<double>::infinity();
    if (row == 0) bounds.min_x = bounds.max_x = 0.0;
    else bounds.min_x = std::max(view.min_x, std::nextafter(x[row - 1], infinity));
    if (row != last && row != 0) bounds.max_x = std::min(view.max_x, std::nextafter(x[row + 1], -infinity));
    if (row == last) bounds.min_y = bounds.max_y = 1.0;
    else bounds.max_y = std::min(view.max_y, std::nextafter(y[row + 1], -infinity));
    if (row > 0 && row != last) bounds.min_y = std::max(view.min_y, std::nextafter(y[row - 1], infinity));
    return bounds;
}

CurveEditorDialog::Rows SmallAreaInfillFlowCompensationDialog::default_rows() const
{
    const auto* defaults = static_cast<const ConfigOptionStrings*>(
        print_config_def.get("small_area_infill_flow_compensation_model")->default_value.get());
    return decode_rows(defaults->values);
}

CurveEditorDialog::Rows SmallAreaInfillFlowCompensationDialog::seed_rows() const
{
    return {{"0", "0"}, {"10", "1"}};
}

wxString SmallAreaInfillFlowCompensationDialog::empty_message() const
{
    return _L("The model is empty. No flow compensation will be applied.");
}

wxString SmallAreaInfillFlowCompensationDialog::validate_view(const CurveEditorView& view) const
{
    return view.min_x < 0.0 ? _L("Extrusion length cannot be negative.") : wxString();
}

void SmallAreaInfillFlowCompensationDialog::accept_rows(const Rows& rows)
{
    std::vector<std::string> output;
    for (size_t row = 0; row < rows.size(); ++row) {
        if (row < m_initial_parameters.size() && rows[row] == split_point(m_initial_parameters[row]))
            output.push_back(m_initial_parameters[row]);
        else {
            wxString length = rows[row].first;
            wxString factor = rows[row].second;
            length.Trim().Trim(false).Replace(",", ".");
            factor.Trim().Trim(false).Replace(",", ".");
            output.push_back((row == 0 ? "" : "\n") + length.ToStdString() + "," + factor.ToStdString());
        }
    }
    // Keep the existing serialized GUI path: semicolons separate ConfigOptionStrings entries.
    m_output_data.clear();
    for (const auto& point : output) m_output_data += point + ";";
    m_modified = output != m_initial_parameters;
}

} // namespace Slic3r::GUI
