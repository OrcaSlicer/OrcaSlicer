#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

// The extrusion of the second of two moves that both give E1.
float second_move_extrusion(GCodeReader &reader)
{
    float extrusion = -1.f;
    reader.parse_buffer("G1 X1 E1\nG1 X2 E1\n", [&extrusion](GCodeReader &reader, const GCodeReader::GCodeLine &line) {
        extrusion = line.dist_E(reader);
    });
    return extrusion;
}

} // namespace

TEST_CASE("A G-code reader measures extrusion as relative or absolute as its config says", "[GCodeReader]")
{
    const bool  relative = GENERATE(false, true);
    const float expected = relative ? 1.f : 0.f;

    GCodeConfig config;
    config.use_relative_e_distances.value = relative;
    GCodeReader applied;
    applied.apply_config(config);
    CHECK(applied.config().use_relative_e_distances.value == relative);
    CHECK_THAT(second_move_extrusion(applied), Catch::Matchers::WithinAbs(expected, 1e-6));

    DynamicPrintConfig dynamic;
    dynamic.set_key_value("use_relative_e_distances", new ConfigOptionBool(relative));
    GCodeReader applied_dynamic;
    applied_dynamic.apply_config(dynamic);
    CHECK_THAT(second_move_extrusion(applied_dynamic), Catch::Matchers::WithinAbs(expected, 1e-6));

    GCodeReader copy = applied;
    CHECK(copy.config().use_relative_e_distances.value == relative);
    CHECK_THAT(second_move_extrusion(copy), Catch::Matchers::WithinAbs(expected, 1e-6));
}

TEST_CASE("A G-code reader without a config uses the default one", "[GCodeReader]")
{
    const bool  relative = GCodeConfig().use_relative_e_distances.value;
    GCodeReader reader;
    CHECK(reader.config().use_relative_e_distances.value == relative);
    CHECK_THAT(second_move_extrusion(reader), Catch::Matchers::WithinAbs(relative ? 1.f : 0.f, 1e-6));
}
