#include "CurveModel.hpp"

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>
#include <boost/algorithm/string/trim.hpp>
#include "I18N.hpp"

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
