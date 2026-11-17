#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "libslic3r/GCode/SmallAreaInfillFlowCompensationModel.hpp"
#include "libslic3r/GCode/SmallAreaInfillFlowCompensator.hpp"
#include "libslic3r/GCode/PchipInterpolatorHelper.hpp"
#include "libslic3r/CurveModel.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_helpers.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using FlowModel = Slic3r::SmallAreaInfillFlowCompensationModel;

namespace {

DynamicPrintConfig flow_config()
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_height", new ConfigOptionFloat(0.4));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.4));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(100));
    config.set_key_value("small_area_infill_flow_compensation", new ConfigOptionBool(false));
    config.set_key_value("small_area_infill_flow_compensation_model", new ConfigOptionStrings{"0,0.2", "\n100,1"});
    return config;
}

double exported_volume(const DynamicPrintConfig& config)
{
    Print print;
    Model model;
    Test::init_print({Test::cube(4.0)}, print, model, config);
    Test::gcode(print);
    return print.print_statistics().total_extruded_volume;
}

} // namespace

TEST_CASE("Edited flow points drive the same interpolation as the graph preview", "[SmallAreaInfillFlowCompensation]")
{
    const FlowModel::Rows rows = {{"0", "0.2"}, {"10", "1"}};
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = FlowModel::encode_rows(rows, {});
    SmallAreaInfillFlowCompensator compensator(config);
    FlowModel model;
    const auto preview = model.make_interpolator({0, 10}, {0.2, 1});
    const double length = GENERATE(0.1, 2.0, 5.0, 10.0);
    const double extrusion = 3.0;
    const double expected_factor = 0.2 + (1.0 - 0.2) * length / 10.0;
    CHECK_THAT(preview(length), WithinAbs(expected_factor, 1e-12));
    CHECK_THAT(compensator.modify_flow(length, extrusion, erSolidInfill), WithinAbs(extrusion * expected_factor, 1e-12));
}

TEST_CASE("Nonlinear flow curves use identical PCHIP interpolation in the preview and G-code", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const FlowModel::Rows rows = {{"0", "0.2"}, {"2", "0.7"}, {"10", "1"}};
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = FlowModel::encode_rows(rows, {});
    SmallAreaInfillFlowCompensator compensator(config);
    FlowModel model;
    const std::vector<double> x = {0, 2, 10}, y = {0.2, 0.7, 1};
    const auto preview = model.make_interpolator(x, y);
    const PchipInterpolatorHelper expected(x, y);
    // At the first interval's midpoint a straight line gives 0.45; PCHIP must differ.
    REQUIRE(std::abs(preview(1.0) - 0.45) > 1e-3);
    const double length = GENERATE(0.1, 1.0, 2.0, 3.0, 5.0, 9.0, 10.0, 11.0);
    const double extrusion = 3.0;
    CHECK_THAT(preview(length), WithinAbs(expected.interpolate(length), 1e-12));
    CHECK_THAT(compensator.modify_flow(length, extrusion, erSolidInfill), WithinAbs(extrusion * preview(length), 1e-12));
}

TEST_CASE("A blank flow model applies no compensation", "[SmallAreaInfillFlowCompensation][Regression]")
{
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values =
        GENERATE(std::vector<std::string>{}, std::vector<std::string>{""}, std::vector<std::string>{"\n"},
                 std::vector<std::string>{" ", "\t\n", ""});
    SmallAreaInfillFlowCompensator compensator(config);
    const double length = GENERATE(0.0, 0.1, 5.0, 1000.0);
    CHECK_THAT(compensator.modify_flow(length, 3.0, erSolidInfill), WithinAbs(3.0, 1e-12));
}

TEST_CASE("A flow model whose entries hold fewer than two points is an error", "[SmallAreaInfillFlowCompensation][Regression]")
{
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values =
        GENERATE(std::vector<std::string>{"5"}, std::vector<std::string>{"0", " "}, std::vector<std::string>{"0,1"});
    REQUIRE_THROWS_AS(SmallAreaInfillFlowCompensator(config), InvalidArgument);
}

TEST_CASE("Legacy flow models with trailing and interleaved blanks remain usable during slicing", "[SmallAreaInfillFlowCompensation][Regression]")
{
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = {"", "0,0.2", " \n", "10,1", ""};
    SmallAreaInfillFlowCompensator compensator(config);
    CHECK_THAT(compensator.modify_flow(5, 3, erSolidInfill), WithinAbs(1.8, 1e-12));
}

TEST_CASE("Flow parsing errors identify the bad point and its entered values", "[SmallAreaInfillFlowCompensation][Regression]")
{
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = {"0,0", "", "5,bad", "10,1", ""};
    try {
        SmallAreaInfillFlowCompensator compensator(config);
        FAIL("Invalid point must be rejected.");
    } catch (const Slic3r::InvalidArgument& error) {
        const std::string message = error.what();
        CHECK(message.find("Table row 2") != std::string::npos);
        CHECK(message.find("5,bad") != std::string::npos);
    }
}

TEST_CASE("Flow parsing diagnostics translate the model name row and validation reason", "[SmallAreaInfillFlowCompensation][Regression]")
{
    struct RestoreTranslation {
        I18N::translate_fn_type previous = I18N::translate_fn;
        ~RestoreTranslation() { I18N::set_translate_callback(previous); }
    } restore;
    I18N::set_translate_callback([](const char* text) -> std::string {
        const std::string key(text);
        if (key == "Flow Compensation Model: %s") return "Translated model: %s";
        if (key == "Table row %d: %s") return "Translated row %d: %s";
        if (key == "Enter a finite number in each cell.") return "Translated validation reason";
        return key;
    });
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = {"0,0", "", "5,bad", "10,1"};
    try {
        SmallAreaInfillFlowCompensator compensator(config);
        FAIL("Invalid point must be rejected.");
    } catch (const InvalidArgument& error) {
        const std::string message = error.what();
        CHECK(message.find("Translated model") != std::string::npos);
        CHECK(message.find("Translated row 2") != std::string::npos);
        CHECK(message.find("Translated validation reason") != std::string::npos);
        CHECK(message.find("5,bad") != std::string::npos);
    }
}

TEST_CASE("Flow compensation only changes eligible infill roles within the model range", "[SmallAreaInfillFlowCompensation]")
{
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = {"0,0.2", "\n10,1"};
    SmallAreaInfillFlowCompensator compensator(config);
    const ExtrusionRole role = GENERATE(erSolidInfill, erTopSolidInfill, erBottomSurface,
                                        erPerimeter, erExternalPerimeter, erInternalInfill, erBridgeInfill, erSupportMaterial);
    const double extrusion = 3.0;
    const bool eligible = role == erSolidInfill || role == erTopSolidInfill || role == erBottomSurface;
    CHECK_THAT(compensator.modify_flow(5.0, extrusion, role), WithinAbs(eligible ? extrusion * 0.6 : extrusion, 1e-12));
    CHECK_THAT(compensator.modify_flow(0.0, extrusion, role), WithinAbs(extrusion, 1e-12));
    CHECK_THAT(compensator.modify_flow(11.0, extrusion, role), WithinAbs(extrusion, 1e-12));
}

TEST_CASE("Slicing reads flow model entries as the parser before the curve editor read them", "[SmallAreaInfillFlowCompensation][Regression]")
{
    // The points that parser took from the entry placed between "0,0" and "10,1". It split at
    // commas and, in this order: skipped an entry with a blank first field, failed on a first field
    // without a leading number, skipped an entry with nothing after the first comma, and failed on
    // a second field without a leading number.
    struct Entry { std::string text; std::vector<double> x, y; };
    const std::vector<double> ends_x = {0, 10}, ends_y = {0, 1};
    const auto entry = GENERATE_COPY(
        Entry{"5", ends_x, ends_y},
        Entry{"1,", ends_x, ends_y},
        Entry{" ,0.5", ends_x, ends_y},
        Entry{",", ends_x, ends_y},
        Entry{"\t,bad,more", ends_x, ends_y},
        Entry{"5.5e", ends_x, ends_y},
        Entry{"0x5", ends_x, ends_y},
        Entry{"5 mm,", ends_x, ends_y},
        Entry{"5,0.8,", {0, 5, 10}, {0, 0.8, 1}},
        Entry{"5,0.8,9", {0, 5, 10}, {0, 0.8, 1}},
        Entry{"\t+0x5,\n +0x.Cp0", {0, 5, 10}, {0, 0.75, 1}},
        Entry{"0.2,0.44,x", {0, 0.2, 10}, {0, 0.44, 1}},
        Entry{"5,0.8 mm", {0, 5, 10}, {0, 0.8, 1}},
        Entry{"5junk,0.8junk", {0, 5, 10}, {0, 0.8, 1}},
        Entry{"0.2,0.4444\n0.4,0.6145", {0, 0.2, 10}, {0, 0.4444, 1}},
        Entry{"\n 0.2 , 0.4444 ", {0, 0.2, 10}, {0, 0.4444, 1}},
        Entry{"+5.,\t+.8e0,,", {0, 5, 10}, {0, 0.8, 1}},
        Entry{"5e,8e-1e", {0, 5, 10}, {0, 0.8, 1}});
    CAPTURE(entry.text);
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = {"0,0", entry.text, "10,1"};
    SmallAreaInfillFlowCompensator compensator(config);
    // The same interpolation over the expected points: at each of them it gives their factor.
    const PchipInterpolatorHelper expected(entry.x, entry.y);
    CHECK_THAT(compensator.modify_flow(entry.x[1], 1.0, erSolidInfill), WithinAbs(entry.y[1], 1e-12));
    for (const double length : {0.1, 1.0, 3.0, 7.0})
        CHECK_THAT(compensator.modify_flow(length, 1.0, erSolidInfill), WithinAbs(expected.interpolate(length), 1e-12));
}

TEST_CASE("Slicing rejects flow model entries without a number in either of the first two fields", "[SmallAreaInfillFlowCompensation][Regression]")
{
    GCodeConfig config;
    const std::string point = GENERATE(as<std::string>{}, "bad,0.5", "5,bad", "5,,0.5", "5, ", "5,\n", "+-5,0.5", "5,mm0.5",
                                       "bad", "bad,", " bad ,", "x5", "-+5,0.5", "--5,0.5", "5,+-0.5", "+,0.5", "5,-",
                                       ".,0.5", "5,e1", "5,x0.5", "1e999,0.5", "5,-0x1p9999",
                                       "5,0x1.fffffffffffff8p1023");
    CAPTURE(point);
    config.small_area_infill_flow_compensation_model.values = {"0,0", point, "10,1"};
    REQUIRE_THROWS_AS(SmallAreaInfillFlowCompensator(config), InvalidArgument);
}

TEST_CASE("Slicing reads the number at the start of a field in each form std::stod read", "[SmallAreaInfillFlowCompensation][Regression]")
{
    struct Number { std::string text; double value; };
    const auto number = GENERATE(
        Number{"1e", 1}, Number{"1e+", 1}, Number{"1e5", 100000}, Number{"1E+2", 100}, Number{".5", 0.5}, Number{"5.", 5},
        Number{"5.e-1", 0.5}, Number{"7 mm", 7}, Number{"2.5.5", 2.5}, Number{"0x1p-1", 0.5}, Number{"0X1P-1", 0.5},
        Number{"0x.8", 0.5}, Number{"0x1.8p+1", 3}, Number{"0x10", 16}, Number{"0x1p", 1}, Number{"0x1p+", 1},
        Number{"0x2.p1", 4}, Number{"0xAp-1x", 5}, Number{"0x0.0000000000000000000008p84", 0.5},
        Number{"0x10000000000000000000p-76", 1});
    CAPTURE(number.text);
    GCodeConfig config;
    // As a length between 0 and 1e6: the factor 0.5 belongs to that length alone.
    config.small_area_infill_flow_compensation_model.values = {"0,0", number.text + ",0.5", "1e6,1"};
    CHECK_THAT(SmallAreaInfillFlowCompensator(config).modify_flow(number.value, 1.0, erSolidInfill), WithinAbs(0.5, 1e-12));
    // As a factor, negated to lie between -1e6 and 1.
    config.small_area_infill_flow_compensation_model.values = {"0,-1e6", "5,-" + number.text, "10,1"};
    CHECK_THAT(SmallAreaInfillFlowCompensator(config).modify_flow(5.0, 1.0, erSolidInfill), WithinAbs(-number.value, 1e-9));
}

TEST_CASE("Slicing rounds a hexadecimal number once, as std::strtod does", "[SmallAreaInfillFlowCompensation][Regression]")
{
    // Rounding to 53 bits and then scaling rounds twice: a tie that only the digits beyond 53 bits
    // break, or a subnormal value, comes out one step off.
    const std::string text = GENERATE(as<std::string>{}, "0x1.00000000000008p0", "0x1.000000000000080000001p0",
                                      "0x100000000000008000000001p-92", "0x00000000000000000000001.00000000000008p0",
                                      "0x0.000000000000000000000100000000000008000000001p88", "0x1.8p-1074", "0x1.00000000000001p-1075",
                                      "0x1p-1075", "0x1p-1076", "0x1.fffffffffffff7p-1");
    CAPTURE(text);
    GCodeConfig config;
    // At its own length the curve gives the factor exactly. Negated to lie between -2 and the last factor 1.
    config.small_area_infill_flow_compensation_model.values = {"0,-2", "5,-" + text, "10,1"};
    CHECK(SmallAreaInfillFlowCompensator(config).modify_flow(5.0, 1.0, erSolidInfill) == -std::strtod(text.c_str(), nullptr));
}

TEST_CASE("A hexadecimal subnormal number that rounds up to the smallest normal one is read exactly", "[SmallAreaInfillFlowCompensation][Regression]")
{
    // Not compared with std::strtod: the one of the Windows UCRT reads this text as 2^-1019.
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = {"0,-2", "5,-0x1.ffffffffffffffp-1023", "10,1"};
    CHECK(SmallAreaInfillFlowCompensator(config).modify_flow(5.0, 1.0, erSolidInfill) == -std::numeric_limits<double>::min());
}

TEST_CASE("A hexadecimal factor just above half of the smallest double is not read as 0", "[SmallAreaInfillFlowCompensation][Regression]")
{
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = {"0,0", "5,0x1.00000000000001p-1075", "10,1"};
    CHECK(SmallAreaInfillFlowCompensator(config).modify_flow(5.0, 1.0, erSolidInfill) == std::numeric_limits<double>::denorm_min());
    // Exactly half of it is a tie, rounded to the even 0, which repeats the first factor.
    config.small_area_infill_flow_compensation_model.values = {"0,0", "5,0x1p-1075", "10,1"};
    CHECK_THROWS_AS(SmallAreaInfillFlowCompensator(config), InvalidArgument);
}

TEST_CASE("The graph shows the points slicing reads from an entry with more than two fields", "[SmallAreaInfillFlowCompensation][Regression]")
{
    // A third field is not the fraction of a decimal comma: "0,0,5" is the point (0, 0), as it
    // sliced before the curve editor, and not (0, 0.5).
    struct Stored { std::vector<std::string> values; double length, factor; };
    const auto stored = GENERATE(Stored{{"0,0,5", "10,1"}, 5, 0.5},
                                 Stored{{"0,-1", "5,0,9", "10,1"}, 5, 0},
                                 Stored{{"0,0", "0.2,0.44,x", "10,1"}, 0.2, 0.44},
                                 Stored{{"0,0", "5,0.5", "10,1,0"}, 5, 0.5});
    CAPTURE(stored.values);
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = stored.values;
    SmallAreaInfillFlowCompensator compensator(config);
    CHECK_THAT(compensator.modify_flow(stored.length, 1.0, erSolidInfill), WithinAbs(stored.factor, 1e-12));
    const FlowModel model(stored.values);
    std::vector<double> x, y;
    int row = -1;
    REQUIRE(model.read_points(model.rows(), x, y, row) == nullptr);
    const auto preview = model.make_interpolator(x, y);
    for (const double length : {0.1, 1.0, 3.0, 5.0, 7.0, 10.0})
        CHECK_THAT(compensator.modify_flow(length, 1.0, erSolidInfill), WithinAbs(preview(length), 1e-12));
}

TEST_CASE("Slicing reports the table row of an entry whose length is not a number", "[SmallAreaInfillFlowCompensation][Regression]")
{
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = {"0,0", " ", "5", "bad,", "10,1"};
    try {
        SmallAreaInfillFlowCompensator compensator(config);
        FAIL("An entry without a length must be rejected.");
    } catch (const InvalidArgument& error) {
        // The skipped entry "5" is the second row of the table and the blank one is none.
        CHECK(std::string(error.what()).find("Table row 3") != std::string::npos);
    }
}

TEST_CASE("The default flow model compensates as before the curve editor in each stored spelling", "[SmallAreaInfillFlowCompensation][Regression]")
{
    // Profiles store the model as an array of entries, as one string of quoted entries with
    // escaped line breaks, or as one string with a line break after each semicolon.
    const std::string stored = GENERATE(as<std::string>{}, "",
        "0,0;\"\\n0.2,0.4444\";\"\\n0.4,0.6145\";\"\\n0.6,0.7059\";\"\\n0.8,0.7619\";\"\\n1.5,0.8571\";"
        "\"\\n2,0.8889\";\"\\n3,0.9231\";\"\\n5,0.9520\";\"\\n10,1\"",
        "0,0;\n0.2,0.4444;\n0.4,0.6145;\n0.6,0.7059;\n0.8,0.7619;\n1.5,0.8571;\n2,0.8889;\n3,0.9231;\n5,0.9520;\n10,1");
    GCodeConfig config;
    auto& model = config.small_area_infill_flow_compensation_model;
    model.values = {"0,0", "\n0.2,0.4444", "\n0.4,0.6145", "\n0.6,0.7059", "\n0.8,0.7619",
                    "\n1.5,0.8571", "\n2,0.8889", "\n3,0.9231", "\n5,0.9520", "\n10,1"};
    const auto* defaults = static_cast<const ConfigOptionStrings*>(
        print_config_def.get("small_area_infill_flow_compensation_model")->default_value.get());
    REQUIRE(model.values == defaults->values);
    if (!stored.empty()) REQUIRE(model.deserialize(stored));
    SmallAreaInfillFlowCompensator compensator(config);
    // PCHIP over the default points, computed apart from the slicer. A length of 0 and lengths
    // past the last point are left alone.
    const auto [length, factor] = GENERATE(table<double, double>({
        {0.0, 1.0}, {0.1, 0.24699636289666393}, {0.2, 0.4444}, {0.3, 0.5453402145411946},
        {0.5, 0.6663822828063748}, {1.0, 0.7972666843879391}, {1.5, 0.8571}, {2.5, 0.9090927793018251},
        {4.0, 0.9398994135110998}, {7.5, 0.9774238532110091}, {10.0, 1.0}, {10.5, 1.0}}));
    CAPTURE(stored, length);
    CHECK_THAT(compensator.modify_flow(length, 1.0, erSolidInfill), WithinAbs(factor, 1e-9));
}

TEST_CASE("The G-code consumer rejects nonfinite flow model points", "[SmallAreaInfillFlowCompensation]")
{
    GCodeConfig config;
    const std::string coordinate = GENERATE(as<std::string>{}, "nan", "inf", "-inf");
    config.small_area_infill_flow_compensation_model.values = {"0,0", "\n5," + coordinate, "\n10,1"};
    REQUIRE_THROWS_AS(SmallAreaInfillFlowCompensator(config), InvalidArgument);
}

TEST_CASE("G-code accepts saved flow models above the editor point limit", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const FlowModel model;
    const size_t count = model.maximum_point_count() + 5;
    CurveModel::Rows rows;
    for (size_t point = 0; point < count; ++point) {
        rows.emplace_back(CurveModel::format_number(double(point)),
                          CurveModel::format_number(0.2 + 0.8 * double(point) / double(count - 1)));
    }
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = CurveModel::encode_rows(rows, {});
    SmallAreaInfillFlowCompensator compensator(config);
    const double length = double(count - 1) / 2;
    const double extrusion = 3.0;
    CHECK_THAT(compensator.modify_flow(length, extrusion, erSolidInfill), WithinAbs(extrusion * 0.6, 1e-12));
}

TEST_CASE("Disabled flow compensation ignores a malformed stored model", "[SmallAreaInfillFlowCompensation][Regression][Slicing]")
{
    auto config = flow_config();
    const double baseline = exported_volume(config);
    REQUIRE(baseline > 0.0);
    config.option<ConfigOptionStrings>("small_area_infill_flow_compensation_model")->values =
        {"garbage", "0,0.5junk", "10,1"};
    CHECK_THAT(exported_volume(config), WithinAbs(baseline, 1e-9));
}

TEST_CASE("Enabled flow compensation with a blank model leaves exported extrusion unchanged", "[SmallAreaInfillFlowCompensation][Regression][Slicing]")
{
    auto config = flow_config();
    const double baseline = exported_volume(config);
    REQUIRE(baseline > 0.0);
    config.option<ConfigOptionBool>("small_area_infill_flow_compensation")->value = true;
    config.option<ConfigOptionStrings>("small_area_infill_flow_compensation_model")->values =
        GENERATE(std::vector<std::string>{}, std::vector<std::string>{""}, std::vector<std::string>{"\n"});
    CHECK_THAT(exported_volume(config), WithinAbs(baseline, 1e-6));
}

TEST_CASE("Enabling an edited flow model reduces extrusion in small solid infill areas", "[SmallAreaInfillFlowCompensation][Slicing]")
{
    auto config = flow_config();
    const double baseline = exported_volume(config);
    REQUIRE(baseline > 0.0);
    config.option<ConfigOptionBool>("small_area_infill_flow_compensation")->value = true;
    config.option<ConfigOptionStrings>("small_area_infill_flow_compensation_model")->values =
        FlowModel::encode_rows({{"0", "0.2"}, {"100", "1"}}, {});
    const double compensated = exported_volume(config);
    CHECK(compensated > 0.0);
    CHECK(compensated < baseline);
}
