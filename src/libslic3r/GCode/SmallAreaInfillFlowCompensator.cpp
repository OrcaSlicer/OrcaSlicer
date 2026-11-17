// Modify the flow of extrusion lines inversely proportional to the length of
// the extrusion line. When infill lines get shorter the flow rate will auto-
// matically be reduced to mitigate the effect of small infill areas being
// over-extruded.

// Based on original work by Alexander Þór licensed under the GPLv3:
// https://github.com/Alexander-T-Moss/Small-Area-Flow-Comp

#include "SmallAreaInfillFlowCompensator.hpp"

#include <cmath>
#include <exception>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <boost/log/trivial.hpp>

#include "../PrintConfig.hpp"
#include "libslic3r/CurveModel.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/format.hpp"

#include "SmallAreaInfillFlowCompensationModel.hpp"
#include "libslic3r/Config.hpp"

namespace Slic3r {

namespace {

constexpr std::string_view whitespace = " \t\n\v\f\r";

// Reads the longest numeric prefix after leading whitespace and a '+', as std::stod read the points
// before the curve editor, but without a locale lookup and refusing values that are not finite.
bool read_number_prefix(std::string_view text, double& value)
{
    const size_t start = text.find_first_not_of(whitespace);
    if (start == std::string_view::npos) return false;
    text.remove_prefix(start);
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (!text.empty() && text.front() == '-') return false;
    }
    size_t length = 0;
    value = string_to_double_decimal_point(text, &length);
    return length > 0 && std::isfinite(value);
}

} // namespace

SmallAreaInfillFlowCompensator::SmallAreaInfillFlowCompensator(const Slic3r::GCodeConfig& config)
{
    try {
        // Read the points as the parser that preceded the curve editor did, so that every model which
        // sliced then gives the same points now: an entry without a first or a second field is skipped,
        // and text after a number or after the second field is ignored. The editor reads the same
        // entries strictly; its table rows number the entries that are not blank.
        const std::vector<std::string>& values = config.small_area_infill_flow_compensation_model.values;
        const SmallAreaInfillFlowCompensationModel model;
        const char* error = nullptr;
        std::vector<int> point_rows;
        int row = -1;
        for (const std::string& value : values) {
            const std::string_view entry = value;
            if (entry.find_first_not_of(whitespace) == std::string_view::npos) continue;
            ++row;
            const size_t comma = entry.find(',');
            if (comma == std::string_view::npos || comma + 1 == entry.size() ||
                entry.substr(0, comma).find_first_not_of(whitespace) == std::string_view::npos) continue;
            double length, factor;
            if (!read_number_prefix(entry.substr(0, comma), length) ||
                !read_number_prefix(entry.substr(comma + 1, entry.find(',', comma + 1) - comma - 1), factor)) {
                error = L("Enter a finite number in each cell.");
                break;
            }
            eLengths.push_back(length);
            flowComps.push_back(factor);
            point_rows.push_back(row);
        }
        // A blank model applies no compensation, as the editor says of it.
        if (row < 0) return;

        int error_row = error ? row : -1;
        if (!error && eLengths.empty()) error = L("The model must contain at least two points.");
        if (!error) {
            error = model.validate_points(eLengths, flowComps, error_row);
            if (error && error_row >= 0) error_row = point_rows[error_row];
        }
        if (error) {
            std::string message = _u8L(error);
            if (error_row >= 0) {
                const CurveModel::Row point = CurveModel::decode_rows(values)[error_row];
                message = Slic3r::format(_u8L("Table row %d: %s"), error_row + 1, message) +
                    " [" + point.first + "," + point.second + "]";
            }
            // TRN %s is the reason the small area flow compensation model is invalid.
            throw Slic3r::InvalidArgument(Slic3r::format(_u8L("Flow Compensation Model: %s"), message));
        }

        // Use the same factory as the graph, including any feature-specific override.
        flowModel = model.make_interpolator(eLengths, flowComps);

    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Error parsing small area infill compensation model: " << e.what();
        throw;
    }
}

SmallAreaInfillFlowCompensator::~SmallAreaInfillFlowCompensator() = default;

double SmallAreaInfillFlowCompensator::flow_comp_model(const double line_length)
{
    if (!flowModel)
        return 1.0;

    if (line_length == 0 || line_length > max_modified_length()) {
        return 1.0;
    }

    return flowModel(line_length);
}

double SmallAreaInfillFlowCompensator::modify_flow(const double line_length, const double dE, const ExtrusionRole role)
{
    if (flowModel &&
        (role == ExtrusionRole::erSolidInfill || role == ExtrusionRole::erTopSolidInfill || role == ExtrusionRole::erBottomSurface)) {
        return dE * flow_comp_model(line_length);
    }

    return dE;
}

} // namespace Slic3r
