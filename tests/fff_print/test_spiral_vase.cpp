#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/GCode/SpiralVase.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// The extrusion moves of `gcode`.
std::vector<GCodeReader::GCodeLine> extrusions(const PrintConfig &config, const std::string &gcode)
{
    std::vector<GCodeReader::GCodeLine> out;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&out](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self))
            out.push_back(line);
    });
    return out;
}

} // namespace

TEST_CASE("Spiral vase keeps the selected coordinate and extrusion precision", "[SpiralVase][Regression]")
{
    const bool high_precision = GENERATE(false, true);
    CAPTURE(high_precision);
    PrintConfig config;
    config.high_precision_gcode.value        = high_precision;
    config.use_relative_e_distances.value    = true;
    config.spiral_mode_smooth.value          = false;
    config.spiral_starting_flow_ratio.value  = 0.5;
    config.spiral_finishing_flow_ratio.value = 0.5;
    config.resolution.value                  = 0.001;

    SpiralVase vase(config);
    vase.process_layer("G1 X0 Y0 Z0.2\n", false);

    // The first spiral layer ramps Z up from 0.2 and the flow up from half. A third of the way along,
    // Z is 0.2 + 0.02 / 3 and E is 0.001234 * (0.5 + 0.5 / 3).
    vase.enable(true);
    const std::vector<GCodeReader::GCodeLine> first = extrusions(config,
        vase.process_layer("G1 Z0.22\nG1 X1 Y0 E0.001234 F3600\nG1 X1 Y2 E0.002468\n", false));
    REQUIRE(first.size() == 2);
    CHECK_THAT(first.front().precise_value(Z), Catch::Matchers::WithinAbs(high_precision ? 0.2067 : 0.207, 1e-9));
    CHECK_THAT(first.front().precise_value(E), Catch::Matchers::WithinAbs(high_precision ? 0.000823 : 0.00082, 1e-9));

    // The last layer is followed by a copy of its moves whose flow ramps down to half.
    // Halfway along, E is 0.001236 * (0.5 + 0.5 * 0.5).
    vase.enable(true);
    const std::vector<GCodeReader::GCodeLine> last = extrusions(config,
        vase.process_layer("G1 Z0.24\nG1 X1 Y0 E0.001236\nG1 X1 Y2 E0.002468\n", true));
    REQUIRE(last.size() == 4);
    CHECK_THAT(last[2].precise_value(E), Catch::Matchers::WithinAbs(high_precision ? 0.000927 : 0.00093, 1e-9));
}

TEST_CASE("Spiral vase mode warns about an object that overrides infill, walls or top shells", "[SpiralVase]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "spiral_mode",           1 },
        { "sparse_infill_density", "0%" },
        { "wall_loops",            1 },
        { "top_shell_layers",      0 },
        { "layer_change_gcode",    "G92 E0\n" }, // validate() relative-E reset
    });
    const auto [opt_key, value] = GENERATE(table<std::string, std::string>({
        { "",                      "" }, // no override: the vase-ready global config applies
        { "sparse_infill_density", "13%" },
        { "wall_loops",            "2" },
        { "top_shell_layers",      "3" },
    }));
    CAPTURE(opt_key);
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides(1);
    if (!opt_key.empty())
        overrides.front().emplace_back(opt_key, value);

    Print print;
    Model model;
    init_print({cube(20)}, print, model, config, &overrides);

    std::vector<StringObjectException> warnings;
    REQUIRE(print.validate(&warnings).string.empty());
    const PrintObject *object = print.objects().front();
    const auto warning = std::find_if(warnings.begin(), warnings.end(),
                                      [object](const StringObjectException &w) { return w.object == object; });
    if (opt_key.empty()) {
        CHECK(warning == warnings.end());
    } else {
        REQUIRE(warning != warnings.end());
        CHECK(warning->opt_key == opt_key);
    }
}

// SpiralVase rewrites every Z move of a layer it spirals, so a lift in the filament change G-code
// once drove the nozzle down into the print (#8387). The layer holding the change must stay flat.
TEST_CASE("A filament change in spiral vase mode prints its layer flat and the spiral resumes above it", "[SpiralVase][Regression]")
{
    const DynamicPrintConfig config = multifilament_config(2, {
        { "spiral_mode",           1 },
        { "sparse_infill_density", "0%" },
        { "wall_loops",            1 },
        { "top_shell_layers",      0 },
        { "skirt_loops",           0 },
        { "brim_type",             "no_brim" },
        { "layer_change_gcode",    "G92 E0\n" },
        // Lift and leave the print, as a change macro that purges elsewhere does.
        { "change_filament_gcode", "G1 Z{max_layer_z + 3} F600\nG1 X5 Y5 F3000\n" },
    });
    Print print;
    Model model;
    init_print({cube(20)}, print, model, config);
    CustomGCode::Info &ticks = model.plates_custom_gcodes[model.curr_plate_index];
    ticks.mode = CustomGCode::MultiAsSingle; // a one-filament plate, as the layer slider stores it
    ticks.gcodes.push_back({ 5.0, CustomGCode::ToolChange, 2, "", "" });
    print.apply(model, print.full_print_config());
    const std::string out = gcode(print);

    // The Z of each extrusion after the change, grouped by layer, the change layer first.
    std::vector<std::vector<double>> layers;
    bool changed = false;
    GCodeReader reader;
    reader.apply_config(print.config());
    reader.parse_buffer(out, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.cmd() == "T1") {
            changed = true;
            layers.emplace_back();
        } else if (changed && line.comment().substr(0, 2) == "Z:")
            layers.emplace_back();
        else if (changed && line.extruding(self) && line.dist_XY(self) > 0)
            layers.back().push_back(line.new_Z(self));
    });
    REQUIRE(changed);
    REQUIRE(layers.size() > 2);

    const std::vector<double> &change_layer = layers.front();
    REQUIRE_FALSE(change_layer.empty());
    CHECK_THAT(change_layer.front(), Catch::Matchers::WithinAbs(5.0, 1e-6));
    for (double z : change_layer)
        CHECK_THAT(z, Catch::Matchers::WithinAbs(change_layer.front(), 1e-6));

    const std::vector<double> &next_layer = layers[1];
    REQUIRE_FALSE(next_layer.empty());
    CHECK(next_layer.back() > next_layer.front()); // spiralling again

    double last_z = change_layer.front();
    for (const std::vector<double> &layer : layers)
        for (double z : layer) {
            CHECK(z >= last_z - 1e-6);
            last_z = z;
        }
}
