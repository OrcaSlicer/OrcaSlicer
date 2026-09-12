#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <string>

using namespace Slic3r;

TEST_CASE("The seam corner snapping angle defaults to the angle the seam placer snapped at before it was configurable", "[SeamPlacer]")
{
    DynamicPrintConfig    config = DynamicPrintConfig::full_print_config();
    const ConfigOptionInt *opt   = config.opt<ConfigOptionInt>("seam_angle_threshold");

    REQUIRE(opt != nullptr);
    REQUIRE(opt->value == 55);
    REQUIRE_THAT(SeamPlacer::sharp_angle_snapping_threshold,
                 Catch::Matchers::WithinAbs(55. * PI / 180., 1e-6));
}

TEST_CASE("The seam corner snapping angle survives serialization across its whole range", "[SeamPlacer]")
{
    const int          degrees    = GENERATE(1, 30, 55, 180);
    const std::string  serialized = std::to_string(degrees);
    DynamicPrintConfig config     = DynamicPrintConfig::full_print_config();

    config.set_deserialize_strict("seam_angle_threshold", serialized);

    REQUIRE(config.opt<ConfigOptionInt>("seam_angle_threshold")->value == degrees);
    REQUIRE(config.opt_serialize("seam_angle_threshold") == serialized);
    REQUIRE(config.validate().empty());
}

TEST_CASE("The seam corner snapping angle is stored in the print preset", "[SeamPlacer]")
{
    const std::vector<std::string> &keys = Preset::print_options();
    REQUIRE(std::find(keys.begin(), keys.end(), "seam_angle_threshold") != keys.end());
}

TEST_CASE("The seam corner snapping angle can be set per object", "[SeamPlacer]")
{
    // The seam placer reads it off PrintObjectConfig, so it has to live there rather than on the
    // print config alone.
    PrintObjectConfig object_config;
    REQUIRE(object_config.seam_angle_threshold.value == 55);
}
