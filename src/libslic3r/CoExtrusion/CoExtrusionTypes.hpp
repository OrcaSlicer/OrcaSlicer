#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::CoExtrusion {

using SurfaceColorId = std::uint32_t;

enum class CAxisRotationMode {
    ShortestPath,
    PositiveOnly,
    LimitedRange,
};

enum class LargeRotationStrategy {
    SlowDown,
    Preposition,
    IndependentRotation,
};

enum class DelayModel {
    Disabled,
    FixedTime,
    TransportVolume,
};

enum class TopBottomStrategy {
    HoldLast,
    PrimaryColor,
    TangentFollow,
    DisableControl,
};

struct ColorSector {
    SurfaceColorId color_id { 0 };
    std::string    preview_color;
    // +X = 0, positive toward +Y. In the physical top view with +Y toward
    // the observer, red/blue/green centers are clockwise 0/120/240 degrees.
    double         center_angle_deg { 0.0 };
    double         angular_width_deg { 0.0 };
};

struct DelayCalibrationEstimate {
    double response_delay_s { 0.0 };
    double transport_volume_mm3 { 0.0 };
};

// Serialized into a single ConfigOptionStrings entry per filament so that the
// outer ConfigOption vector can continue to represent multiple filaments.
// Format: v1;<color-id>,<#RRGGBB[AA]>,<center-deg>,<width-deg>;...
class Profile
{
public:
    static constexpr std::string_view VERSION = "v1";

    std::vector<ColorSector> sectors;

    static std::optional<Profile> parse(std::string_view serialized, std::string *error = nullptr);
    std::string serialize() const;
    std::vector<std::string> validate() const;

    const ColorSector* find_sector(SurfaceColorId color_id) const;
    bool empty() const { return sectors.empty(); }
};

double normalize_degrees(double angle_deg);
double shortest_angular_distance(double from_deg, double to_deg);
double unwrap_near(double normalized_angle_deg, double reference_angle_deg);

// Solve the same physical-angle convention consumed by
// SurfaceDirectionResolver:
// physical = sector center + calibration + machine zero + direction * command.
double calibration_offset_from_observation(
    double measured_physical_azimuth_deg,
    double sector_center_angle_deg,
    double commanded_axis_angle_deg,
    int axis_direction,
    double axis_zero_offset_deg) noexcept;

std::optional<DelayCalibrationEstimate> estimate_delay_from_boundary_shift(
    double boundary_shift_mm,
    double path_speed_mm_s,
    double line_width_mm,
    double layer_height_mm) noexcept;

std::optional<CAxisRotationMode> parse_c_axis_rotation_mode(std::string_view value);
std::optional<char> parse_c_axis_letter(std::string_view value);
std::optional<LargeRotationStrategy> parse_large_rotation_strategy(std::string_view value);
std::optional<TopBottomStrategy> parse_top_bottom_strategy(std::string_view value);
std::optional<DelayModel> parse_delay_model(std::string_view value);

} // namespace Slic3r::CoExtrusion
