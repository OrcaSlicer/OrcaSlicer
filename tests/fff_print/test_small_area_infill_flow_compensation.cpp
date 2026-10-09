#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "libslic3r/GCode/SmallAreaInfillFlowCompensationModel.hpp"
#include "libslic3r/GCode/SmallAreaInfillFlowCompensator.hpp"
#include "libslic3r/GCode/PchipInterpolatorHelper.hpp"
#include "libslic3r/CurveModel.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
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

TEST_CASE("Constructing a flow compensator without parsed points remains an error", "[SmallAreaInfillFlowCompensation][Regression]")
{
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values.clear();
    REQUIRE_THROWS_AS(SmallAreaInfillFlowCompensator(config), std::invalid_argument);
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

TEST_CASE("The G-code consumer rejects malformed points instead of silently using partial numbers", "[SmallAreaInfillFlowCompensation][Regression]")
{
    GCodeConfig config;
    const std::string point = GENERATE(as<std::string>{}, "5junk,0.5", "5,0.5junk", "5", " ", "5,0.5,extra");
    config.small_area_infill_flow_compensation_model.values = {"0,0", point, "10,1"};
    REQUIRE_THROWS_AS(SmallAreaInfillFlowCompensator(config), InvalidArgument);
    config.small_area_infill_flow_compensation_model.values = {"0", " "};
    REQUIRE_THROWS_AS(SmallAreaInfillFlowCompensator(config), InvalidArgument);
}

TEST_CASE("The G-code consumer rejects nonfinite flow model points", "[SmallAreaInfillFlowCompensation]")
{
    GCodeConfig config;
    const std::string coordinate = GENERATE(as<std::string>{}, "nan", "inf", "-inf");
    config.small_area_infill_flow_compensation_model.values = {"0,0", "\n5," + coordinate, "\n10,1"};
    REQUIRE_THROWS_AS(SmallAreaInfillFlowCompensator(config), InvalidArgument);
}

TEST_CASE("Inactive flow models leave exported extrusion unchanged", "[SmallAreaInfillFlowCompensation][Regression]")
{
    auto config = flow_config();
    const double baseline = exported_volume(config);
    REQUIRE(baseline > 0.0);
    const bool empty_enabled_model = GENERATE(false, true);
    config.option<ConfigOptionBool>("small_area_infill_flow_compensation")->value = empty_enabled_model;
    config.option<ConfigOptionStrings>("small_area_infill_flow_compensation_model")->values =
        empty_enabled_model ? std::vector<std::string>{} : std::vector<std::string>{"malformed model"};
    CHECK_THAT(exported_volume(config), WithinAbs(baseline, 1e-6));
}

TEST_CASE("Enabling an edited flow model reduces extrusion in small solid infill areas", "[SmallAreaInfillFlowCompensation]")
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
