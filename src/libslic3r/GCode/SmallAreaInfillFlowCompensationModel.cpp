#include "SmallAreaInfillFlowCompensationModel.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include "libslic3r/I18N.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "PchipInterpolatorHelper.hpp"

namespace Slic3r {
namespace {

bool nearly_equal(double a, double b)
{
    // Keep the endpoint tolerance of existing compensation models.
    return std::nextafter(a, std::numeric_limits<double>::lowest()) <= b &&
           std::nextafter(a, std::numeric_limits<double>::max()) >= b;
}

} // namespace

const char* SmallAreaInfillFlowCompensationModel::validate_points(
    const std::vector<double>& x, const std::vector<double>& y, int& row) const
{
    row = -1;
    if (x.size() != y.size()) return L("Enter a finite number in each cell.");
    if (x.size() == 1) return L("The model must contain at least two points.");
    for (size_t i = 0; i < x.size(); ++i) {
        row = int(i);
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) return L("Enter a finite number in each cell.");
        if (i == 0 && !nearly_equal(x[i], 0.0)) return L("The first extrusion length must be 0.");
        if (i > 0 && (nearly_equal(x[i], 0.0) || x[i] <= x[i - 1])) return L("Extrusion lengths must strictly increase.");
        if (i > 0 && y[i] <= y[i - 1]) return L("Flow correction factors must strictly increase.");
        if (i == x.size() - 1 && !nearly_equal(y[i], 1.0)) return L("The last flow correction factor must be 1.");
    }
    return nullptr;
}

CurveModel::Interpolator SmallAreaInfillFlowCompensationModel::make_interpolator(
    const std::vector<double>& x, const std::vector<double>& y) const
{
    if (x.empty() && y.empty()) return {};
    return [model = PchipInterpolatorHelper(x, y)](double value) { return model.interpolate(value); };
}

CurveView SmallAreaInfillFlowCompensationModel::fitted_view(
    const std::vector<double>& x, const std::vector<double>& y) const
{
    double maximum = x.empty() ? 1.0 : x.back();
    if (maximum <= std::numeric_limits<double>::max() / 1.1) maximum *= 1.1;
    return {0.0, maximum, y.empty() ? 0.0 : std::min(0.0, y.front()), y.empty() ? 1.0 : std::max(1.0, y.back())};
}

CurveView SmallAreaInfillFlowCompensationModel::drag_bounds(
    int row, const std::vector<double>& x, const std::vector<double>& y, const CurveView& view) const
{
    CurveView bounds = view;
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

CurveModel::Rows SmallAreaInfillFlowCompensationModel::default_rows() const
{
    const auto* defaults = static_cast<const ConfigOptionStrings*>(
        print_config_def.get("small_area_infill_flow_compensation_model")->default_value.get());
    return decode_rows(defaults->values);
}

CurveModel::Rows SmallAreaInfillFlowCompensationModel::seed_rows() const
{
    return {{"0", "0"}, {"10", "1"}};
}

const char* SmallAreaInfillFlowCompensationModel::empty_message() const
{
    return L("The model is empty. No flow compensation will be applied.");
}

const char* SmallAreaInfillFlowCompensationModel::validate_view(const CurveView& view) const
{
    return view.min_x < 0.0 ? L("Extrusion length cannot be negative.") : nullptr;
}

} // namespace Slic3r
