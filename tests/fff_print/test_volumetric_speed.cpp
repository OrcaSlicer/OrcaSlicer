#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

constexpr double filament_limit = 20.; // mm³/s
constexpr double outer_limit    = 8.;  // mm³/s
// Far above both limits, so every feature prints at the limit that applies to it.
constexpr double requested_speed = 400.; // mm/s

const std::vector<ExtrusionRole> outer_roles = {erExternalPerimeter, erTopSolidInfill, erBottomSurface};
const std::vector<ExtrusionRole> inner_roles = {erPerimeter, erSolidInfill, erInternalInfill};

// Only the volumetric limits may lower a feature's speed.
DynamicPrintConfig volumetric_config(double max_outer_volumetric_speed)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"filament_max_volumetric_speed", filament_limit},
        {"filament_max_outer_volumetric_speed", max_outer_volumetric_speed},
        {"outer_wall_speed", requested_speed},
        {"inner_wall_speed", requested_speed},
        {"sparse_infill_speed", requested_speed},
        {"internal_solid_infill_speed", requested_speed},
        {"top_surface_speed", requested_speed},
        {"initial_layer_speed", requested_speed},
        {"initial_layer_infill_speed", requested_speed},
        {"slow_down_for_layer_cooling", "0"},
        {"slow_down_layers", "0"},
    });
    return config;
}

// mm³/s per role.
std::map<ExtrusionRole, double> peak_volumetric_flow(const std::string& gcode, const DynamicPrintConfig& config)
{
    const double       filament_area = 0.25 * PI * sqr(config.opt_float("filament_diameter", 0));
    const std::string& role_tag      = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);

    std::map<ExtrusionRole, double> peaks;
    ExtrusionRole                   role = erNone;
    GCodeReader                     parser;
    parser.apply_config(config);
    parser.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (line.comment().find(role_tag) == 0)
            role = ExtrusionEntity::string_to_role(line.comment().substr(role_tag.size()));
        // Short moves carry too few E digits to read their flow.
        if (line.extruding(self) && line.dist_XY(self) > 1.) {
            const double flow = line.dist_E(self) / line.dist_XY(self) * filament_area * line.new_F(self) / MM_PER_MIN;
            peaks[role]       = std::max(peaks[role], flow);
        }
    });
    return peaks;
}

void check_peaks(const std::map<ExtrusionRole, double>& peaks, const std::vector<ExtrusionRole>& roles, double expected)
{
    for (ExtrusionRole role : roles) {
        INFO(ExtrusionEntity::role_to_string(role));
        const auto peak = peaks.find(role);
        REQUIRE(peak != peaks.end());
        CHECK_THAT(peak->second, Catch::Matchers::WithinRel(expected, 0.02));
    }
}

} // namespace

TEST_CASE("The outer volumetric limit caps outer walls and top and bottom surfaces only", "[VolumetricSpeed]")
{
    const DynamicPrintConfig config = volumetric_config(outer_limit);
    const auto               peaks  = peak_volumetric_flow(slice({cube(20.)}, config), config);

    check_peaks(peaks, outer_roles, outer_limit);
    check_peaks(peaks, inner_roles, filament_limit);
}

// Validation rejects an outer limit above the filament limit, but emission must not rely on it.
TEST_CASE("Outer features keep the filament limit when the outer limit is off or above it", "[VolumetricSpeed]")
{
    const double max_outer_volumetric_speed = GENERATE(0., 1.5 * filament_limit);
    INFO("filament_max_outer_volumetric_speed: " << max_outer_volumetric_speed);

    const DynamicPrintConfig config = volumetric_config(max_outer_volumetric_speed);
    const auto               peaks  = peak_volumetric_flow(slice({cube(20.)}, config), config);

    check_peaks(peaks, outer_roles, filament_limit);
    check_peaks(peaks, inner_roles, filament_limit);
}

// Start and change-filament G-code calibrate at this flow.
TEST_CASE("The outer wall volumetric speed placeholder reports the outer limit", "[VolumetricSpeed]")
{
    const double max_outer_volumetric_speed = GENERATE(0., outer_limit);
    INFO("filament_max_outer_volumetric_speed: " << max_outer_volumetric_speed);

    DynamicPrintConfig config = volumetric_config(max_outer_volumetric_speed);
    const std::string  marker = "; outer_wall_volumetric_speed = ";
    config.set_deserialize_strict("machine_start_gcode", marker + "{outer_wall_volumetric_speed}\n");
    const std::string gcode = slice({cube(20.)}, config);

    const size_t pos = gcode.find(marker);
    REQUIRE(pos != std::string::npos);
    const double expected = max_outer_volumetric_speed > 0. ? max_outer_volumetric_speed : filament_limit;
    CHECK_THAT(std::stod(gcode.substr(pos + marker.size())), Catch::Matchers::WithinRel(expected, 0.001));
}
