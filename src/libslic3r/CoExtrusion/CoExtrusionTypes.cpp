#include "CoExtrusionTypes.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace Slic3r::CoExtrusion {

namespace {

std::string_view trim(std::string_view value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string_view> split(std::string_view value, char separator)
{
    std::vector<std::string_view> result;
    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t end = value.find(separator, begin);
        result.emplace_back(value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return result;
}

bool parse_uint32(std::string_view text, SurfaceColorId &value)
{
    text = trim(text);
    const char *begin = text.data();
    const char *end   = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc() && ptr == end;
}

bool parse_double(std::string_view text, double &value)
{
    text = trim(text);
    if (text.empty())
        return false;

    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> value;
    return stream && stream.peek() == std::char_traits<char>::eof() && std::isfinite(value);
}

bool is_hex_color(std::string_view color)
{
    if (color.size() != 7 && color.size() != 9)
        return false;
    if (color.front() != '#')
        return false;
    return std::all_of(color.begin() + 1, color.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    });
}

void set_error(std::string *error, std::string message)
{
    if (error != nullptr)
        *error = std::move(message);
}

} // namespace

std::optional<Profile> Profile::parse(std::string_view serialized, std::string *error)
{
    serialized = trim(serialized);
    if (serialized.empty())
        return Profile{};

    const std::vector<std::string_view> records = split(serialized, ';');
    if (records.empty() || trim(records.front()) != VERSION) {
        set_error(error, "Unsupported co-extrusion profile version");
        return std::nullopt;
    }

    Profile profile;
    profile.sectors.reserve(records.size() - 1);
    for (size_t index = 1; index < records.size(); ++index) {
        const std::string_view record = trim(records[index]);
        if (record.empty())
            continue;

        const std::vector<std::string_view> fields = split(record, ',');
        if (fields.size() != 4) {
            set_error(error, "Each co-extrusion sector must contain color id, preview color, center angle and angular width");
            return std::nullopt;
        }

        ColorSector sector;
        if (!parse_uint32(fields[0], sector.color_id)) {
            set_error(error, "Invalid co-extrusion sector color id");
            return std::nullopt;
        }

        const std::string_view preview_color = trim(fields[1]);
        if (!is_hex_color(preview_color)) {
            set_error(error, "Invalid co-extrusion preview color");
            return std::nullopt;
        }
        sector.preview_color.assign(preview_color);

        if (!parse_double(fields[2], sector.center_angle_deg) || !parse_double(fields[3], sector.angular_width_deg)) {
            set_error(error, "Invalid co-extrusion sector angle");
            return std::nullopt;
        }
        sector.center_angle_deg = normalize_degrees(sector.center_angle_deg);
        profile.sectors.emplace_back(std::move(sector));
    }

    const std::vector<std::string> errors = profile.validate();
    if (!errors.empty()) {
        set_error(error, errors.front());
        return std::nullopt;
    }
    return profile;
}

std::string Profile::serialize() const
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << VERSION << std::setprecision(12);
    for (const ColorSector &sector : sectors) {
        stream << ';' << sector.color_id << ',' << sector.preview_color << ','
               << normalize_degrees(sector.center_angle_deg) << ',' << sector.angular_width_deg;
    }
    return stream.str();
}

std::vector<std::string> Profile::validate() const
{
    std::vector<std::string> errors;
    std::unordered_set<SurfaceColorId> color_ids;
    for (const ColorSector &sector : sectors) {
        if (!color_ids.emplace(sector.color_id).second)
            errors.emplace_back("Co-extrusion sector color ids must be unique");
        if (!is_hex_color(sector.preview_color))
            errors.emplace_back("Co-extrusion preview colors must use #RRGGBB or #RRGGBBAA format");
        if (!std::isfinite(sector.center_angle_deg))
            errors.emplace_back("Co-extrusion sector center angles must be finite");
        if (!std::isfinite(sector.angular_width_deg) || sector.angular_width_deg <= 0.0 || sector.angular_width_deg > 360.0)
            errors.emplace_back("Co-extrusion sector angular widths must be in the range (0, 360]");
    }
    return errors;
}

const ColorSector* Profile::find_sector(SurfaceColorId color_id) const
{
    const auto it = std::find_if(sectors.begin(), sectors.end(), [color_id](const ColorSector &sector) {
        return sector.color_id == color_id;
    });
    return it == sectors.end() ? nullptr : &*it;
}

double normalize_degrees(double angle_deg)
{
    double normalized = std::fmod(angle_deg, 360.0);
    if (normalized < 0.0)
        normalized += 360.0;
    return normalized == 360.0 ? 0.0 : normalized;
}

double shortest_angular_distance(double from_deg, double to_deg)
{
    double delta = normalize_degrees(to_deg) - normalize_degrees(from_deg);
    if (delta > 180.0)
        delta -= 360.0;
    else if (delta <= -180.0)
        delta += 360.0;
    return delta;
}

double unwrap_near(double normalized_angle_deg, double reference_angle_deg)
{
    return reference_angle_deg + shortest_angular_distance(reference_angle_deg, normalized_angle_deg);
}

double calibration_offset_from_observation(
    double measured_physical_azimuth_deg,
    double sector_center_angle_deg,
    double commanded_axis_angle_deg,
    int axis_direction,
    double axis_zero_offset_deg) noexcept
{
    const int direction = axis_direction == -1 ? -1 : 1;
    double offset = normalize_degrees(
        measured_physical_azimuth_deg - sector_center_angle_deg - axis_zero_offset_deg -
        double(direction) * commanded_axis_angle_deg);
    if (offset > 180.0)
        offset -= 360.0;
    return offset;
}

std::optional<DelayCalibrationEstimate> estimate_delay_from_boundary_shift(
    double boundary_shift_mm,
    double path_speed_mm_s,
    double line_width_mm,
    double layer_height_mm) noexcept
{
    if (!std::isfinite(boundary_shift_mm) || boundary_shift_mm < 0.0 ||
        !std::isfinite(path_speed_mm_s) || path_speed_mm_s <= 0.0 ||
        !std::isfinite(line_width_mm) || line_width_mm <= 0.0 ||
        !std::isfinite(layer_height_mm) || layer_height_mm <= 0.0)
        return std::nullopt;
    return DelayCalibrationEstimate {
        boundary_shift_mm / path_speed_mm_s,
        boundary_shift_mm * line_width_mm * layer_height_mm
    };
}

std::optional<CAxisRotationMode> parse_c_axis_rotation_mode(std::string_view value)
{
    value = trim(value);
    if (value == "shortest_path")
        return CAxisRotationMode::ShortestPath;
    if (value == "positive_only")
        return CAxisRotationMode::PositiveOnly;
    if (value == "limited_range")
        return CAxisRotationMode::LimitedRange;
    return std::nullopt;
}

std::optional<char> parse_c_axis_letter(std::string_view value)
{
    if (value.size() != 1)
        return std::nullopt;
    char axis = value.front();
    if (axis >= 'a' && axis <= 'z')
        axis = char(axis - 'a' + 'A');
    switch (axis) {
    case 'A':
    case 'B':
    case 'C':
    case 'U':
    case 'V':
    case 'W':
        return axis;
    default:
        return std::nullopt;
    }
}

std::optional<LargeRotationStrategy> parse_large_rotation_strategy(std::string_view value)
{
    value = trim(value);
    if (value == "slow_down")
        return LargeRotationStrategy::SlowDown;
    if (value == "preposition")
        return LargeRotationStrategy::Preposition;
    if (value == "independent_rotation")
        return LargeRotationStrategy::IndependentRotation;
    return std::nullopt;
}

std::optional<TopBottomStrategy> parse_top_bottom_strategy(std::string_view value)
{
    value = trim(value);
    if (value == "hold_last")
        return TopBottomStrategy::HoldLast;
    if (value == "primary_color")
        return TopBottomStrategy::PrimaryColor;
    if (value == "tangent_follow")
        return TopBottomStrategy::TangentFollow;
    if (value == "disable_control")
        return TopBottomStrategy::DisableControl;
    return std::nullopt;
}

std::optional<DelayModel> parse_delay_model(std::string_view value)
{
    value = trim(value);
    if (value == "disabled")
        return DelayModel::Disabled;
    if (value == "fixed_time")
        return DelayModel::FixedTime;
    if (value == "transport_volume")
        return DelayModel::TransportVolume;
    return std::nullopt;
}

} // namespace Slic3r::CoExtrusion
