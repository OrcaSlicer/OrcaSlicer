// Modify the flow of extrusion lines inversely proportional to the length of
// the extrusion line. When infill lines get shorter the flow rate will auto-
// matically be reduced to mitigate the effect of small infill areas being
// over-extruded.

// Based on original work by Alexander Þór licensed under the GPLv3:
// https://github.com/Alexander-T-Moss/Small-Area-Flow-Comp

#include "SmallAreaInfillFlowCompensator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

int hex_digit(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    const char lower = char(character | 0x20);
    return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

// Reads an unsigned hexadecimal number such as "0x1.8p-1" from the start of the text, rounded as
// std::strtod rounds it. Returns false when the text does not start with one.
bool read_hex_prefix(std::string_view text, double& value)
{
    if (text.size() < 3 || text[0] != '0' || (text[1] | 0x20) != 'x') return false;
    uint64_t mantissa = 0;
    int exponent = 0;
    bool digits = false, point = false;
    size_t position = 2;
    for (; position < text.size(); ++position) {
        const int digit = hex_digit(text[position]);
        if (digit < 0) {
            if (text[position] != '.' || point) break;
            point = true;
        } else if (mantissa >> 60) {
            // No room left: the mantissa already holds more bits than a double, so a digit
            // beyond them only tells whether the value lies above what was read.
            mantissa |= uint64_t(digit != 0);
            if (!point) exponent += 4;
        } else {
            mantissa = mantissa << 4 | uint64_t(digit);
            if (point) exponent -= 4;
        }
        digits = digits || digit >= 0;
    }
    if (!digits) return false;
    if (position < text.size() && (text[position] | 0x20) == 'p') {
        ++position;
        const bool negative = position < text.size() && text[position] == '-';
        if (negative || (position < text.size() && text[position] == '+')) ++position;
        // An exponent without digits is not a part of the number.
        int power = 0;
        for (; position < text.size() && text[position] >= '0' && text[position] <= '9'; ++position)
            power = std::min(power * 10 + (text[position] - '0'), 1000000);
        exponent += negative ? -power : power;
    }
    value = std::ldexp(double(mantissa), exponent);
    return true;
}

// Reads the longest numeric prefix after leading whitespace and a sign, decimal or hexadecimal, as
// std::stod read the points before the curve editor. Unlike std::stod it looks up no locale, refuses
// infinities and NaN, and reads a value too small for a double as the editor does: rounded, not refused.
bool read_number_prefix(std::string_view text, double& value)
{
    const size_t start = text.find_first_not_of(whitespace);
    if (start == std::string_view::npos) return false;
    text.remove_prefix(start);
    const bool negative = text.front() == '-';
    if (negative || text.front() == '+') text.remove_prefix(1);
    if (!read_hex_prefix(text, value)) {
        // The sign is read already: a second one starts no number.
        if (text.empty() || text.front() == '-') return false;
        size_t length = 0;
        value = string_to_double_decimal_point(text, &length);
        if (length == 0) return false;
    }
    if (negative) value = -value;
    return std::isfinite(value);
}

} // namespace

SmallAreaInfillFlowCompensator::SmallAreaInfillFlowCompensator(const Slic3r::GCodeConfig& config)
{
    try {
        // Read the points as the parser that preceded the curve editor did, so that every model which
        // sliced then gives the same points now. In its order: an entry with a blank first field is
        // skipped, a first field without a number is an error, an entry with nothing after the first
        // comma is skipped, and a second field without a number is an error. Text after a number or
        // after the second field is ignored. The editor splits an entry into the same two fields and
        // reads them strictly; its table rows number the entries that are not blank.
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
            const std::string_view first = entry.substr(0, comma);
            if (first.find_first_not_of(whitespace) == std::string_view::npos) continue;
            double length, factor;
            bool valid = read_number_prefix(first, length);
            if (valid) {
                if (comma == std::string_view::npos || comma + 1 == entry.size()) continue;
                valid = read_number_prefix(entry.substr(comma + 1, entry.find(',', comma + 1) - comma - 1), factor);
            }
            if (!valid) {
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
