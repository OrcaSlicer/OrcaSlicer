#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_utils.hpp"

#include <vector>

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

TEST_CASE("A G-code reader follows relative positioning", "[GCodeReader]")
{
    std::vector<float> targets, distances;
    float height_after_lift = 0.f;
    GCodeReader reader;
    reader.parse_buffer("G1 Z10\nG91\nG1 Z2\nG90\nG1 X5\nG92 Z3\nG1 Z4\n",
        [&](GCodeReader &reader, const GCodeReader::GCodeLine &line) {
            if (line.cmd_is("G1") && line.has_z()) {
                targets.push_back(line.new_Z(reader));
                distances.push_back(line.dist_Z(reader));
            } else if (line.has_x())
                height_after_lift = reader.z();
        });
    // The lift is 2 mm up from 10 mm, and G92 then sets the height to 3 mm.
    CHECK_THAT(targets, Catch::Matchers::Approx(std::vector<float>{ 10.f, 12.f, 4.f }).margin(1e-4));
    CHECK_THAT(distances, Catch::Matchers::Approx(std::vector<float>{ 10.f, 2.f, 1.f }).margin(1e-4));
    CHECK_THAT(height_after_lift, Catch::Matchers::WithinAbs(12., 1e-4));
}

TEST_CASE("A G-code reader fails on a file it cannot open", "[GCodeReader]")
{
    const ScopedTemporaryFile missing(".gcode");
    GCodeReader reader;
    bool called = false;
    CHECK_FALSE(reader.parse_file(missing.string(), [&called](GCodeReader &, const GCodeReader::GCodeLine &) { called = true; }));
    CHECK_FALSE(reader.parse_file_raw(missing.string(), [&called](GCodeReader &, const char *, const char *) { called = true; }));
    CHECK_FALSE(called);
}

TEST_CASE("A G-code reader without a config uses the default one", "[GCodeReader]")
{
    const bool  relative = GCodeConfig().use_relative_e_distances.value;
    GCodeReader reader;
    CHECK(reader.config().use_relative_e_distances.value == relative);
    CHECK_THAT(second_move_extrusion(reader), Catch::Matchers::WithinAbs(relative ? 1.f : 0.f, 1e-6));
}
