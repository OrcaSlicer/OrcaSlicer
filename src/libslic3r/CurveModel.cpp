#include "CurveModel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <istream>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <boost/algorithm/string/trim.hpp>
#include "I18N.hpp"
#include "GCode/PchipInterpolatorHelper.hpp"

namespace Slic3r {
namespace {

CurveModel::Row split_point(const std::string& parameter)
{
    const auto comma = parameter.find(',');
    return {boost::algorithm::trim_copy(parameter.substr(0, comma)),
            comma == std::string::npos ? std::string() : boost::algorithm::trim_copy(parameter.substr(comma + 1))};
}

} // namespace

CurveModel::CurveModel(std::vector<std::string> parameters)
    : m_initial_parameters(std::move(parameters)), m_parameters(m_initial_parameters),
      m_serialized_parameters(serialize_parameters(m_parameters))
{}

CurveModel::Interpolator CurveModel::make_interpolator(const std::vector<double>& x, const std::vector<double>& y) const
{
    if (x.empty() && y.empty()) return {};
    return [model = PchipInterpolatorHelper(x, y)](double value) { return model.interpolate(value); };
}

void CurveModel::accept_rows(const Rows& rows)
{
    m_parameters = encode_rows(rows, m_initial_parameters);
    m_serialized_parameters = serialize_parameters(m_parameters);
}

CurveModel::Rows CurveModel::decode_rows(const std::vector<std::string>& parameters)
{
    Rows rows;
    for (const auto& parameter : parameters)
        rows.push_back(split_point(parameter));
    return rows;
}

std::vector<std::string> CurveModel::encode_rows(const Rows& rows, const std::vector<std::string>& initial_parameters)
{
    std::vector<std::string> output;
    for (size_t row = 0; row < rows.size(); ++row) {
        if (row < initial_parameters.size() && rows[row] == split_point(initial_parameters[row]))
            output.push_back(initial_parameters[row]);
        else {
            std::string x = boost::algorithm::trim_copy(rows[row].first);
            std::string y = boost::algorithm::trim_copy(rows[row].second);
            std::replace(x.begin(), x.end(), ',', '.');
            std::replace(y.begin(), y.end(), ',', '.');
            output.push_back((row == 0 ? "" : "\n") + x + "," + y);
        }
    }
    return output;
}

std::string CurveModel::serialize_parameters(const std::vector<std::string>& parameters)
{
    std::string serialized;
    for (const auto& parameter : parameters)
        serialized += parameter + ";";
    return serialized;
}

bool CurveModel::read_number(std::string text, double& value)
{
    boost::algorithm::trim(text);
    std::replace(text.begin(), text.end(), ',', '.');
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    if (!(stream >> value) || !std::isfinite(value)) return false;
    stream >> std::ws;
    return stream.eof();
}

bool CurveModel::valid_view(const CurveView& view)
{
    return std::isfinite(view.min_x) && std::isfinite(view.max_x) &&
           std::isfinite(view.min_y) && std::isfinite(view.max_y) &&
           view.min_x < view.max_x && view.min_y < view.max_y &&
           std::isfinite(view.max_x - view.min_x) && std::isfinite(view.max_y - view.min_y);
}

const char* CurveModel::validate_view(const CurveView& view) const
{
    if (!valid_view(view)) return L("Enter finite bounds with minimum less than maximum.");
    const auto limits = view_limits();
    if (view.min_x < limits.bounds.min_x || view.max_x > limits.bounds.max_x ||
        view.min_y < limits.bounds.min_y || view.max_y > limits.bounds.max_y)
        return L("Visible range bounds are outside the allowed limits.");
    if (view.max_x < view.min_x + limits.minimum_span || view.max_y < view.min_y + limits.minimum_span)
        return L("The visible range is too small.");
    return nullptr;
}

const char* CurveModel::read_view(const std::array<std::string, 4>& bounds, CurveView& view) const
{
    double values[4];
    for (size_t i = 0; i < bounds.size(); ++i)
        if (!read_number(bounds[i], values[i]))
            return L("Enter finite bounds with minimum less than maximum.");
    const CurveView candidate = {values[0], values[1], values[2], values[3]};
    if (const char* error = validate_view(candidate)) return error;
    view = candidate;
    return nullptr;
}

CurveView CurveModel::panned_view(const CurveView& view, double x_offset, double y_offset) const
{
    if (validate_view(view) || !std::isfinite(x_offset) || !std::isfinite(y_offset)) return view;
    const auto limits = view_limits();
    const auto& bounds = limits.bounds;
    const double dx = std::clamp(x_offset, bounds.min_x - view.min_x, bounds.max_x - view.max_x);
    const double dy = std::clamp(y_offset, bounds.min_y - view.min_y, bounds.max_y - view.max_y);
    CurveView panned = {view.min_x + dx, view.max_x + dx, view.min_y + dy, view.max_y + dy};
    // Translation toward zero can expose roundoff in a minimum span accepted at larger coordinates.
    if (dx != 0.0) panned.max_x = std::max(panned.max_x, panned.min_x + limits.minimum_span);
    if (dy != 0.0) panned.max_y = std::max(panned.max_y, panned.min_y + limits.minimum_span);
    return validate_view(panned) ? view : panned;
}

CurveView CurveModel::zoomed_view(const CurveView& view, double factor, double anchor, bool vertical) const
{
    if (validate_view(view) || !std::isfinite(factor) || factor <= 0.0 ||
        !std::isfinite(anchor) || anchor < 0.0 || anchor > 1.0 || factor == 1.0) return view;
    const auto limits = view_limits();
    const double lower = vertical ? limits.bounds.min_y : limits.bounds.min_x;
    const double upper = vertical ? limits.bounds.max_y : limits.bounds.max_x;
    CurveView zoomed = view;
    double& minimum = vertical ? zoomed.min_y : zoomed.min_x;
    double& maximum = vertical ? zoomed.max_y : zoomed.max_x;
    const double span = maximum - minimum;
    const double new_span = std::clamp(span * factor, limits.minimum_span, upper - lower);
    if (new_span == span) return view;
    const double position = minimum + span * anchor;
    minimum = std::clamp(position - new_span * anchor, lower, upper - new_span);
    maximum = std::min(upper, minimum + new_span);
    return validate_view(zoomed) ? view : zoomed;
}

const char* CurveModel::read_points(const Rows& rows, std::vector<double>& x, std::vector<double>& y, int& row) const
{
    x.clear();
    y.clear();
    for (size_t i = 0; i < rows.size(); ++i) {
        double vx, vy;
        if (!read_number(rows[i].first, vx) || !read_number(rows[i].second, vy)) {
            row = int(i);
            return L("Enter a finite number in each cell.");
        }
        x.push_back(vx);
        y.push_back(vy);
    }
    return validate_points(x, y, row);
}

} // namespace Slic3r
