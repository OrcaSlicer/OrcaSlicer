#include "SmallAreaInfillFlowCompensationModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "libslic3r/Config.hpp"
#include "libslic3r/CurveModel.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace {

constexpr double minimum_span = 0.001;

bool nearly_equal(double a, double b)
{
    // Keep the endpoint tolerance of existing compensation models.
    return std::nextafter(a, std::numeric_limits<double>::lowest()) <= b &&
           std::nextafter(a, std::numeric_limits<double>::max()) >= b;
}

} // namespace

SmallAreaInfillFlowCompensationModel::SmallAreaInfillFlowCompensationModel(
    std::vector<std::string> parameters, double bed_diagonal)
    : CurveModel(std::move(parameters)),
      m_maximum_x(std::isfinite(bed_diagonal) && bed_diagonal > 0.0 ? minimum_span : 1000.0)
{
    std::vector<double> lengths{bed_diagonal};
    for (const auto& row : rows()) {
        double length;
        if (read_number(row.first, length)) lengths.push_back(length);
    }
    expand_view_limits(lengths);
}

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

CurveView SmallAreaInfillFlowCompensationModel::fitted_view(
    const std::vector<double>& x, const std::vector<double>& y) const
{
    const auto limits = view_limits();
    double maximum = x.empty() ? 1.0 : x.back();
    maximum = std::clamp(maximum, limits.minimum_span, limits.bounds.max_x);
    maximum = std::min(limits.bounds.max_x, maximum * 1.1);
    return {0.0, maximum,
            y.empty() ? 0.0 : std::clamp(y.front(), limits.bounds.min_y, 0.0),
            y.empty() ? 1.0 : std::clamp(y.back(), 1.0, limits.bounds.max_y)};
}

CurveViewLimits SmallAreaInfillFlowCompensationModel::view_limits() const
{
    // Limit zooming to useful lengths and factors, without restricting saved model points.
    return {{0.0, m_maximum_x, -1.0, 2.0}, 1.0, 0.01, minimum_span};
}

bool SmallAreaInfillFlowCompensationModel::expand_view_limits(const std::vector<double>& x)
{
    const double previous = m_maximum_x;
    for (double length : x) {
        if (!std::isfinite(length) || length <= 0.0) continue;
        // The bed diagonal is a navigation reference, not a limit on saved points.
        // Start with the same 10% headroom used by Reset View, without overflowing.
        double maximum = length <= std::numeric_limits<double>::max() / 1.1 ? length * 1.1 : length;
        // Round down to two significant digits, while keeping the point reachable.
        const double step = std::pow(10.0, std::floor(std::log10(maximum)) - 1.0);
        if (step > 0.0) maximum = std::max(length, std::floor(maximum / step) * step);
        m_maximum_x = std::max(m_maximum_x, maximum);
    }
    return previous != m_maximum_x;
}

const char* SmallAreaInfillFlowCompensationModel::x_view_limit_message() const
{
    return L("The extrusion length limit depends on the current printer's bed size and model points.");
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
    if (row == 1) {
        // Keep the first interval outside the zero endpoint tolerance and its
        // slope finite, including legacy models with negative initial factors.
        const double gap = std::max(std::numeric_limits<double>::min(),
            (bounds.max_y - y.front()) / std::numeric_limits<double>::max());
        bounds.min_x = std::max(bounds.min_x, x.front() + std::nextafter(gap, infinity));
    }
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

} // namespace Slic3r
