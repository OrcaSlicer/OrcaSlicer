#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/AdaptivePAProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"
#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// The speeds, in mm/s, the adaptive pressure advance comments report for the extrusions above the first layer.
static std::vector<double> adaptive_pa_speeds_above_first_layer(const std::string &gcode)
{
    const std::string token = "; APA Speed Used: ";
    std::vector<double> speeds;
    int layer = 0;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0)
            ++layer;
        else if (layer > 1 && line.rfind(token, 0) == 0)
            speeds.push_back(std::stod(line.substr(token.size())));
    }
    return speeds;
}

// The cooling buffer drops a G1 F that repeats the feedrate the preceding travel set, so the wall
// extrusion then inherits the travel feedrate without a G1 F line of its own.
TEST_CASE("Adaptive pressure advance reads the speed of an extrusion that inherits the travel feedrate", "[AdaptivePressureAdvance]")
{
    const int travel_speed = GENERATE(120, 150);
    DYNAMIC_SECTION("travel speed " << travel_speed) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "gcode_flavor",                      "marlin2" },
            { "gcode_comments",                    1 },
            { "enable_pressure_advance",           "1" },
            { "adaptive_pressure_advance",         "1" },
            { "adaptive_pressure_advance_model",   "0.08,1,4000\n0.04,20,4000\n0.08,1,100000\n0.04,20,100000" },
            { "pressure_advance",                  "0.08" },
            { "wall_loops",                        1 },
            { "sparse_infill_density",             0 },
            { "top_shell_layers",                  0 },
            { "bottom_shell_layers",               0 },
            { "skirt_loops",                       0 },
            { "brim_type",                         "no_brim" },
            { "initial_layer_speed",               30 },
            // the wall speed must not be capped by the volumetric limit
            { "filament_max_volumetric_speed",     "200" },
            { "outer_wall_speed",                  150 },
            { "inner_wall_speed",                  150 },
            { "travel_speed",                      travel_speed },
            { "initial_layer_travel_speed",        travel_speed },
            { "slow_down_for_layer_cooling",       "0" },
            { "slow_down_layer_time",              "0" },
            { "enable_overhang_speed",             "0" },
            { "wipe",                              "0" },
            { "retraction_length",                 "0" },
        });
        const std::string gcode = slice({ cube(20) }, config);

        const std::vector<double> speeds = adaptive_pa_speeds_above_first_layer(gcode);
        REQUIRE(!speeds.empty());
        // every wall above the first layer prints at the wall speed, whatever the travel speed
        for (const double speed : speeds)
            CHECK_THAT(speed, Catch::Matchers::WithinAbs(150.0, 0.01));
    }
}

// The pressure advance change tags inside gap fill, and the gap fill runs whose start they mark.
struct GapFillPAMarks
{
    int changes = 0;
    int runs    = 0;
};

// A gap fill run is a continuous stretch of gap fill extrusion. Classic gap fill splits each run into one path per width change, without a travel between them.
static GapFillPAMarks gap_fill_pa_marks(const std::string &gcode)
{
    const std::string_view gap_fill_role = "TYPE:Gap infill";
    GapFillPAMarks         marks;
    bool                   in_gap_fill     = false;
    bool                   in_gap_fill_run = false;
    GCodeReader            reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.rfind("TYPE:", 0) == 0) {
            in_gap_fill = comment == gap_fill_role;
            if (! in_gap_fill)
                in_gap_fill_run = false;
            return;
        }
        if (comment.rfind(" PA_CHANGE:", 0) == 0) {
            if (in_gap_fill)
                ++marks.changes;
            return;
        }
        if (! line.cmd_is("G0") && ! line.cmd_is("G1") && ! line.cmd_is("G2") && ! line.cmd_is("G3"))
            return;
        // An unretraction has positive E but lays down nothing and must not break a run.
        const bool deposits = line.extruding(self) && (line.dist_XY(self) > EPSILON || std::abs(line.dist_Z(self)) > EPSILON);
        if (in_gap_fill && deposits) {
            if (! in_gap_fill_run) {
                ++marks.runs;
                in_gap_fill_run = true;
            }
        } else if (line.dist_XY(self) > EPSILON || std::abs(line.dist_Z(self)) > EPSILON) {
            in_gap_fill_run = false;
        }
    });
    return marks;
}

// A plate whose width tapers along its length, so the classic gap fill between its two walls changes width along a single run and is split into one path per width change. A plate of constant width would leave a gap fill run made of a single path, which exercises nothing.
static TriangleMesh tapered_plate(double length, double narrow_width, double wide_width, double height)
{
    indexed_triangle_set plate = its_make_cube(length, 1., height);
    for (Vec3f &vertex : plate.vertices)
        vertex.y() *= float(vertex.x() > 0.f ? wide_width : narrow_width);
    return TriangleMesh(std::move(plate));
}

TEST_CASE("Adaptive pressure advance changes once at the start of each gap fill run", "[AdaptivePressureAdvance]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_flavor",                    "marlin2" },
        // the pressure advance change tag is kept only in commented G-code
        { "gcode_comments",                  1 },
        { "enable_pressure_advance",         "1" },
        { "adaptive_pressure_advance",       "1" },
        { "adaptive_pressure_advance_model", "0.08,1,4000\n0.04,20,4000\n0.08,1,100000\n0.04,20,100000" },
        { "pressure_advance",                "0.08" },
        // classic walls leave gap fill between them in a thin plate
        { "wall_generator",                  "classic" },
        { "wall_loops",                      2 },
        { "skirt_loops",                     0 },
        { "brim_type",                       "no_brim" },
        { "wipe",                            "0" },
        { "retraction_length",               "0" },
        { "z_hop",                           "0" },
    });
    // A 20mm plate tapering from 1.5mm to 2.1mm wide leaves classic gap fill between its two walls, split into one path per width change along a run.
    const TriangleMesh plate = tapered_plate(20., 1.5, 2.1, 5.);
    const GapFillPAMarks within_features_off = gap_fill_pa_marks(slice({ plate }, config));
    INFO("gap fill changes: " << within_features_off.changes << ", runs: " << within_features_off.runs);
    REQUIRE(within_features_off.runs > 0);
    // A width change inside a gap fill run keeps the pressure advance of the run.
    CHECK(within_features_off.changes == within_features_off.runs);

    // Within-features adaptation evaluates the pressure advance on every width path instead.
    config.set_deserialize_strict({ { "adaptive_pressure_advance_overhangs", "1" } });
    const GapFillPAMarks within_features_on = gap_fill_pa_marks(slice({ plate }, config));
    INFO("gap fill changes within features: " << within_features_on.changes << ", runs: " << within_features_on.runs);
    // Both slices have the same geometry, so within-features adaptation must now change PA inside the split runs too.
    REQUIRE(within_features_on.runs == within_features_off.runs);
    CHECK(within_features_on.changes > within_features_on.runs);
}

TEST_CASE("The adaptive pressure advance model validator ignores whitespace around the values", "[AdaptivePressureAdvance]")
{
    CHECK(AdaptivePAProcessor::validate_adaptive_pa_model("0.027, 8.1416, 2000.0\n0.017,\t20.354, 10000.0").empty());
    CHECK(AdaptivePAProcessor::validate_adaptive_pa_model("0.027,8.1416,2000.0\n0.017,20.354,10000.0").empty());
    // the values themselves are still checked, and whitespace inside a number is not a number
    CHECK_FALSE(AdaptivePAProcessor::validate_adaptive_pa_model("0.027, 0, 2000.0").empty());
    CHECK_FALSE(AdaptivePAProcessor::validate_adaptive_pa_model("0.0 27, 8.1416, 2000.0").empty());
    CHECK_FALSE(AdaptivePAProcessor::validate_adaptive_pa_model("0.027, 8.1416, x").empty());
}

TEST_CASE("The default adaptive pressure advance model holds no measurements", "[AdaptivePressureAdvance]")
{
    const std::string default_model = DynamicPrintConfig::full_print_config().opt_string("adaptive_pressure_advance_model", 0u);
    CHECK_FALSE(AdaptivePAProcessor::adaptive_pa_model_has_measurements(default_model));
    CHECK_FALSE(AdaptivePAProcessor::adaptive_pa_model_has_measurements(""));
    CHECK(AdaptivePAProcessor::adaptive_pa_model_has_measurements("0.04,3.96,3000"));
}

// The warnings Print::validate raises about the adaptive pressure advance models, one per filament,
// when a cube is printed with the given filament.
static std::vector<std::string> adaptive_pa_model_warnings(const std::vector<std::string> &models, int filament = 1)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        // the default config fails validation on relative extrusion without a G92 E0 at each layer
        { "use_relative_e_distances",  "0" },
    });
    for (const char *key : { "enable_pressure_advance", "adaptive_pressure_advance" })
        config.option<ConfigOptionBools>(key)->values.assign(models.size(), true);
    // the filament count is the number of filament diameters
    config.option<ConfigOptionFloats>("filament_diameter")->values.assign(models.size(), 1.75);
    // set as a value so that an empty model stays one filament column, as a preset holds it
    config.option<ConfigOptionStrings>("adaptive_pressure_advance_model")->values = models;
    for (const char *key : { "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                             "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id" })
        config.option<ConfigOptionInt>(key)->value = filament;
    Print print;
    Model model_instance;
    init_print({ cube(20) }, print, model_instance, config);
    std::vector<StringObjectException> warnings;
    const StringObjectException error = print.validate(&warnings);
    INFO(error.string);
    REQUIRE(error.string.empty());
    std::vector<std::string> texts;
    for (const StringObjectException &warning : warnings)
        if (warning.opt_key == "adaptive_pressure_advance_model")
            texts.push_back(warning.string);
    return texts;
}

static std::vector<std::string> adaptive_pa_model_warnings(const std::string &model)
{
    return adaptive_pa_model_warnings(std::vector<std::string>{ model });
}

TEST_CASE("Slicing warns when adaptive pressure advance is enabled without a model", "[AdaptivePressureAdvance]")
{
    const std::string default_model = DynamicPrintConfig::full_print_config().opt_string("adaptive_pressure_advance_model", 0u);
    const std::string model = GENERATE_COPY(default_model, std::string());
    const std::vector<std::string> warnings = adaptive_pa_model_warnings(model);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("no measurements") != std::string::npos);
}

TEST_CASE("Slicing accepts an adaptive pressure advance model typed with spaces after the commas", "[AdaptivePressureAdvance]")
{
    CHECK(adaptive_pa_model_warnings("0.027, 8.1416, 2000.0\n0.027, 20.354, 2000.0\n0.017, 8.1416, 10000.0\n0.017, 20.354, 10000.0").empty());
}

TEST_CASE("Slicing warns about an adaptive pressure advance model whose flow is not above its pressure advance", "[AdaptivePressureAdvance]")
{
    const std::vector<std::string> warnings = adaptive_pa_model_warnings("0.5,0.2,2000\n0.4,0.1,10000");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("invalid values") != std::string::npos);
}

TEST_CASE("Slicing names the filament and the line of an invalid adaptive pressure advance model", "[AdaptivePressureAdvance]")
{
    // a period where the comma belongs leaves the second line with two values
    const std::vector<std::string> warnings = adaptive_pa_model_warnings("0.07,4.71,500\n0.04.7.53,1000\n0.035,9.41,1000");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("filament 1") != std::string::npos);
    CHECK(warnings.front().find("Line 2") != std::string::npos);
}

TEST_CASE("Slicing names the second filament when its adaptive pressure advance model is invalid", "[AdaptivePressureAdvance]")
{
    const std::vector<std::string> warnings = adaptive_pa_model_warnings({ "0.07,4.71,500\n0.035,9.41,1000", "0.07,4.71,500\n0.04.7.53,1000" }, 2);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("filament 2") != std::string::npos);
    CHECK(warnings.front().find("Line 2") != std::string::npos);
}
