#include <catch2/catch_all.hpp>

#include "libslic3r/CoExtrusion/CoExtrusionTypes.hpp"
#include "libslic3r/CoExtrusion/CAxisMotionPlanner.hpp"
#include "libslic3r/CoExtrusion/DelayCompensator.hpp"
#include "libslic3r/CoExtrusion/SurfaceDirectionResolver.hpp"
#include "libslic3r/CoExtrusion/SurfaceColorAnnotation.hpp"
#include "libslic3r/CoExtrusion/SurfacePathMatcher.hpp"
#include "libslic3r/CoExtrusion/SurfaceProvenance.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace Slic3r;
using namespace Slic3r::CoExtrusion;

TEST_CASE("Co-extrusion object and part color fallback has deterministic precedence", "[CoExtrusion]")
{
    PrintRegionConfig defaults;
    CHECK(defaults.coextrusion_surface_color_id.value == -1);

    Model model;
    ModelObject *object = model.add_object("cube", "", make_cube(10, 10, 10));
    REQUIRE(object != nullptr);
    REQUIRE(object->volumes.size() == 1);
    ModelVolume *volume = object->volumes.front();

    CHECK_FALSE(configured_surface_color_id(*object, *volume).has_value());

    object->config.set_key_value("coextrusion_surface_color_id", new ConfigOptionInt(1));
    CHECK(configured_surface_color_id(*object, *volume) == std::optional<SurfaceColorId>(1));

    volume->config.set_key_value("coextrusion_surface_color_id", new ConfigOptionInt(4));
    CHECK(configured_surface_color_id(*object, *volume) == std::optional<SurfaceColorId>(4));

    volume->config.set_key_value("coextrusion_surface_color_id", new ConfigOptionInt(-1));
    CHECK_FALSE(configured_surface_color_id(*object, *volume).has_value());

    std::vector<SurfaceColorId> colors(volume->mesh().its.indices.size(), SurfaceColorAnnotation::UNASSIGNED);
    colors.front() = 3;
    CHECK(volume->coextrusion_surface_colors.set_triangle_colors(std::move(colors)));
    CHECK(volume->coextrusion_surface_colors.triangle_color(0) == std::optional<SurfaceColorId>(3));

    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder4);
    CHECK(selector.facet_state(0) == std::optional<EnforcerBlockerType>(EnforcerBlockerType::Extruder4));
}

TEST_CASE("Co-extrusion profiles preserve stable sector data", "[CoExtrusion]")
{
    const std::string serialized =
        "v1;1,#FF0000,0,120;2,#00FF00,120,120;3,#0000FF,240,120";
    const std::optional<Profile> profile = Profile::parse(serialized);

    REQUIRE(profile.has_value());
    REQUIRE(profile->sectors.size() == 3);
    CHECK(profile->find_sector(2) != nullptr);
    CHECK(profile->find_sector(2)->preview_color == "#00FF00");
    CHECK(profile->find_sector(2)->center_angle_deg == Catch::Approx(120.0));
    CHECK(profile->serialize() == serialized);
    CHECK(profile->validate().empty());
}

TEST_CASE("Co-extrusion rejects malformed profiles and unsafe axis letters", "[CoExtrusion]")
{
    CHECK_FALSE(Profile::parse("v2;1,#FF0000,0,120").has_value());
    CHECK_FALSE(Profile::parse("v1;1,#FF0000,0,0").has_value());
    CHECK(parse_c_axis_letter("c") == std::optional<char>('C'));
    CHECK(parse_c_axis_letter("A") == std::optional<char>('A'));
    CHECK_FALSE(parse_c_axis_letter("X").has_value());
    CHECK_FALSE(parse_c_axis_letter("F").has_value());
    CHECK_FALSE(parse_c_axis_letter("CC").has_value());
}

TEST_CASE("Co-extrusion calibration helpers follow the physical angle and delay conventions", "[CoExtrusion]")
{
    CHECK(calibration_offset_from_observation(30.0, 90.0, 20.0, 1, 5.0) == Catch::Approx(-85.0));
    CHECK(calibration_offset_from_observation(30.0, 90.0, 20.0, -1, 5.0) == Catch::Approx(-45.0));

    const std::optional<DelayCalibrationEstimate> estimate =
        estimate_delay_from_boundary_shift(8.0, 40.0, 0.5, 0.2);
    REQUIRE(estimate.has_value());
    CHECK(estimate->response_delay_s == Catch::Approx(0.2));
    CHECK(estimate->transport_volume_mm3 == Catch::Approx(0.8));
    CHECK_FALSE(estimate_delay_from_boundary_shift(8.0, 0.0, 0.5, 0.2).has_value());
}

TEST_CASE("Co-extrusion surface orientation targets the selected color-sector center", "[CoExtrusion]")
{
    DirectionResolverConfig config;
    config.angular_margin_deg = 2.0;
    SurfaceDirectionResolver resolver(config);
    const ColorSector sector { 1, "#FF0000", 0.0, 120.0 };

    const DirectionResolution resolution = resolver.resolve(
        Vec3d(1.0, 0.0, 0.0), Vec2d(1.0, 0.0), sector, 59.0);

    REQUIRE(resolution.acceptable_angles.has_value());
    REQUIRE(resolution.target_angle_deg.has_value());
    CHECK(resolution.acceptable_angles->half_width_deg == Catch::Approx(5.0));
    CHECK(*resolution.target_angle_deg == Catch::Approx(5.0));
}

TEST_CASE("Clockwise-positive C axis follows square outer-wall directions", "[CoExtrusion]")
{
    DirectionResolverConfig config;
    config.axis_direction = -1;
    SurfaceDirectionResolver resolver(config);
    const ColorSector red_sector { 1, "#FF0000", 0.0, 120.0 };
    const Vec2d tangent(1.0, 0.0);

    const DirectionResolution east = resolver.resolve(
        Vec3d(0.0, -1.0, 0.0), tangent, red_sector, 0.0);
    REQUIRE(east.target_angle_deg.has_value());
    const DirectionResolution north = resolver.resolve(
        Vec3d(1.0, 0.0, 0.0), tangent, red_sector, *east.target_angle_deg);
    REQUIRE(north.target_angle_deg.has_value());
    const DirectionResolution west = resolver.resolve(
        Vec3d(0.0, 1.0, 0.0), tangent, red_sector, *north.target_angle_deg);
    REQUIRE(west.target_angle_deg.has_value());
    const DirectionResolution south = resolver.resolve(
        Vec3d(-1.0, 0.0, 0.0), tangent, red_sector, *west.target_angle_deg);
    REQUIRE(south.target_angle_deg.has_value());

    CHECK(*east.target_angle_deg == Catch::Approx(85.0));
    CHECK(*north.target_angle_deg == Catch::Approx(5.0));
    CHECK(*west.target_angle_deg == Catch::Approx(-85.0));
    CHECK(std::abs(*south.target_angle_deg) == Catch::Approx(175.0));
}

TEST_CASE("Three-dimensional surface normals map onto the deposited filament cylinder", "[CoExtrusion]")
{
    DirectionResolverConfig config;
    config.top_bottom_strategy = TopBottomStrategy::TangentFollow;
    SurfaceDirectionResolver resolver(config);
    const ColorSector red_sector { 1, "#FF0000", 0.0, 120.0 };
    const Vec2d eastward_travel(1.0, 0.0);

    const DirectionResolution bottom = resolver.resolve(
        Vec3d(0.0, 0.0, -1.0), eastward_travel, red_sector, 0.0);
    const DirectionResolution top = resolver.resolve(
        Vec3d(0.0, 0.0, 1.0), eastward_travel, red_sector, 0.0);
    const DirectionResolution side = resolver.resolve(
        Vec3d(0.0, 1.0, 0.0), eastward_travel, red_sector, 0.0);
    const DirectionResolution upward_slope = resolver.resolve(
        Vec3d(0.0, 1.0, 1.0), eastward_travel, red_sector, 0.0);
    const DirectionResolution slope_with_axial_component = resolver.resolve(
        Vec3d(1.0, 1.0, 1.0), eastward_travel, red_sector, 0.0);

    REQUIRE(bottom.surface_azimuth_deg.has_value());
    REQUIRE(top.surface_azimuth_deg.has_value());
    REQUIRE(side.surface_azimuth_deg.has_value());
    REQUIRE(upward_slope.surface_azimuth_deg.has_value());
    REQUIRE(slope_with_axial_component.surface_azimuth_deg.has_value());
    CHECK(*bottom.surface_azimuth_deg == Catch::Approx(0.0));
    CHECK(*top.surface_azimuth_deg == Catch::Approx(180.0));
    CHECK(*side.surface_azimuth_deg == Catch::Approx(90.0));
    CHECK(*upward_slope.surface_azimuth_deg == Catch::Approx(135.0));
    CHECK(*slope_with_axial_component.surface_azimuth_deg == Catch::Approx(135.0));

    const DirectionResolution northward_bottom = resolver.resolve(
        Vec3d(0.0, 0.0, -1.0), Vec2d(0.0, 1.0), red_sector, 0.0);
    const DirectionResolution northward_top = resolver.resolve(
        Vec3d(0.0, 0.0, 1.0), Vec2d(0.0, 1.0), red_sector, 0.0);
    REQUIRE(northward_bottom.surface_azimuth_deg.has_value());
    REQUIRE(northward_top.surface_azimuth_deg.has_value());
    CHECK(*northward_bottom.surface_azimuth_deg == Catch::Approx(90.0));
    CHECK(*northward_top.surface_azimuth_deg == Catch::Approx(270.0));
}

TEST_CASE("Elliptical deposition weights visible color area without changing normal validity", "[CoExtrusion]")
{
    DirectionResolverConfig config;
    config.top_bottom_strategy = TopBottomStrategy::TangentFollow;
    config.bead_width_mm = 0.45;
    config.bead_height_mm = 0.20;
    const ColorSector blue { 2, "#0000FF", 120.0, 120.0 };
    const Vec3d tangent(1, 0, 0);

    SECTION("Sloped surface uses deposited aspect ratio") {
        const auto result = SurfaceDirectionResolver(config).resolve(
            Vec3d(0, 1, 1), tangent, blue, std::nullopt);
        REQUIRE(result.surface_azimuth_deg.has_value());
        REQUIRE(result.acceptable_angles.has_value());
        CHECK(*result.surface_azimuth_deg == Catch::Approx(156.037511).margin(1e-6));
        CHECK(result.acceptable_angles->center_deg == Catch::Approx(36.037511).margin(1e-6));
        CHECK(result.projected_normal_length == Catch::Approx(1.0));
    }

    SECTION("Side and top directions remain unchanged") {
        const auto side = SurfaceDirectionResolver(config).resolve(
            Vec3d(0, 1, 0), tangent, blue, std::nullopt);
        const auto top = SurfaceDirectionResolver(config).resolve(
            Vec3d(0, 0, 1), tangent, blue, std::nullopt);
        REQUIRE(side.surface_azimuth_deg.has_value());
        REQUIRE(top.surface_azimuth_deg.has_value());
        CHECK(*side.surface_azimuth_deg == Catch::Approx(90.0));
        CHECK(*top.surface_azimuth_deg == Catch::Approx(180.0));
    }

    SECTION("Circular and missing dimensions preserve the old slope angle") {
        for (double height : { 0.45, 0.0, -1.0 }) {
            config.bead_height_mm = height;
            const auto result = SurfaceDirectionResolver(config).resolve(
                Vec3d(0, 1, 1), tangent, blue, std::nullopt);
            REQUIRE(result.surface_azimuth_deg.has_value());
            CHECK(*result.surface_azimuth_deg == Catch::Approx(135.0));
        }
    }

    SECTION("Flattening does not change the radial normal threshold") {
        config.bead_height_mm = 0.01;
        const auto axial = SurfaceDirectionResolver(config).resolve(
            Vec3d(1, 0, 0.01), tangent, blue, 25.0);
        CHECK(axial.kind == DirectionIntentKind::HoldPrevious);
        const auto side = SurfaceDirectionResolver(config).resolve(
            Vec3d(0, 1, 0), tangent, blue, std::nullopt);
        CHECK(side.kind == DirectionIntentKind::OrientSurface);
    }
}

TEST_CASE("Clockwise machine coordinates align blue on walls and top surfaces", "[CoExtrusion]")
{
    DirectionResolverConfig config;
    config.top_bottom_strategy = TopBottomStrategy::TangentFollow;
    SurfaceDirectionResolver resolver(config);
    const ColorSector blue { 2, "#0000FF", 120.0, 120.0 };

    const auto wall = resolver.resolve(Vec3d(0, 1, 0), Vec3d(1, 0, 0), blue, std::nullopt);
    const auto top_forward = resolver.resolve(Vec3d(0, 0, 1), Vec3d(1, 0, 0), blue, std::nullopt);
    const auto top_reverse = resolver.resolve(Vec3d(0, 0, 1), Vec3d(-1, 0, 0), blue, std::nullopt);
    REQUIRE(wall.acceptable_angles.has_value());
    REQUIRE(top_forward.acceptable_angles.has_value());
    REQUIRE(top_reverse.acceptable_angles.has_value());
    CHECK(shortest_angular_distance(0, wall.acceptable_angles->center_deg) == Catch::Approx(-30.0));
    CHECK(shortest_angular_distance(0, top_forward.acceptable_angles->center_deg) == Catch::Approx(60.0));
    CHECK(shortest_angular_distance(0, top_reverse.acceptable_angles->center_deg) == Catch::Approx(-120.0));
}

TEST_CASE("Nonplanar motion changes the nozzle cross-section mapping", "[CoExtrusion]")
{
    DirectionResolverConfig config;
    config.top_bottom_strategy = TopBottomStrategy::TangentFollow;
    config.normal_xy_threshold = 0.0;
    SurfaceDirectionResolver resolver(config);
    const ColorSector red { 1, "#FF0000", 0.0, 120.0 };

    // A +X-facing normal has no radial component on a horizontal +X bead,
    // but faces the bottom/top radial side of an ascending/descending bead.
    const auto flat = resolver.resolve(Vec3d(1, 0, 0), Vec3d(1, 0, 0), red, 25.0);
    const auto rising = resolver.resolve(Vec3d(1, 0, 0), Vec3d(1, 0, 1), red, std::nullopt);
    const auto falling = resolver.resolve(Vec3d(1, 0, 0), Vec3d(1, 0, -1), red, std::nullopt);
    CHECK(flat.kind == DirectionIntentKind::HoldPrevious);
    CHECK(flat.target_angle_deg == std::optional<double>(25.0));
    REQUIRE(rising.surface_azimuth_deg.has_value());
    REQUIRE(falling.surface_azimuth_deg.has_value());
    CHECK(*rising.surface_azimuth_deg == Catch::Approx(0.0));
    CHECK(*falling.surface_azimuth_deg == Catch::Approx(180.0));

    const auto axial = resolver.resolve(Vec3d(1, 0, 1), Vec3d(1, 0, 1), red, 25.0);
    CHECK(axial.kind == DirectionIntentKind::HoldPrevious);
}

namespace {

std::vector<CAxisPathIntent> make_delay_intents()
{
    std::vector<CAxisPathIntent> intents(3);
    for (size_t index = 0; index < intents.size(); ++index) {
        intents[index].path_start_mm = double(index);
        intents[index].path_end_mm = double(index + 1);
        intents[index].control_source_path_mm = double(index);
    }
    intents[0].status = CAxisIntentStatus::NoSurfaceSource;
    intents[1].status = CAxisIntentStatus::UncoloredSurface;
    intents[2].status = CAxisIntentStatus::Resolved;
    return intents;
}

} // namespace

TEST_CASE("Fixed-time delay advances control intent without moving geometry", "[CoExtrusion]")
{
    const std::vector<CAxisPathIntent> intents = make_delay_intents();
    DelayCompensationConfig config;
    config.model = DelayModel::FixedTime;
    config.response_delay_s = 1.0;

    const std::vector<CAxisPathIntent> compensated =
        DelayCompensator::compensate(intents, { 1.0, 1.0, 1.0 }, config);

    REQUIRE(compensated.size() == intents.size());
    CHECK(compensated[0].path_start_mm == intents[0].path_start_mm);
    CHECK(compensated[0].path_end_mm == intents[0].path_end_mm);
    CHECK(compensated[0].status == intents[1].status);
    CHECK(compensated[1].status == intents[2].status);
    CHECK(compensated[2].status == intents[2].status);
    CHECK(compensated[0].compensation_advance_mm == Catch::Approx(1.0));
}

TEST_CASE("Transport-volume delay uses deposited volume as look-ahead metric", "[CoExtrusion]")
{
    const std::vector<CAxisPathIntent> intents = make_delay_intents();
    DelayCompensationConfig config;
    config.model = DelayModel::TransportVolume;
    config.transport_volume_mm3 = 0.4;
    config.effective_mm3_per_mm = 0.4;

    const std::vector<CAxisPathIntent> compensated =
        DelayCompensator::compensate(intents, {}, config);

    REQUIRE(compensated.size() == intents.size());
    CHECK(compensated[0].status == intents[1].status);
    CHECK(compensated[1].status == intents[2].status);
    CHECK(compensated[0].control_source_path_mm == Catch::Approx(1.0));
}

TEST_CASE("Delay compensation crosses finalized path boundaries", "[CoExtrusion]")
{
    std::vector<CAxisPathIntent> first = make_delay_intents();
    first.resize(2);
    std::vector<CAxisPathIntent> second = make_delay_intents();
    second.resize(2);
    first[1].target_color_id = SurfaceColorId(2);
    second[0].target_color_id = SurfaceColorId(3);
    second[0].status = CAxisIntentStatus::DirectionUnavailable;
    second[1].status = CAxisIntentStatus::Resolved;

    DelayCompensationConfig config;
    config.model = DelayModel::FixedTime;
    config.response_delay_s = 1.0;

    const std::vector<DelayCompensationPath> compensated = DelayCompensator::compensate_sequence(
        {
            { first, { 1.0, 1.0 }, 0.0 },
            { second, { 1.0, 1.0 }, 0.0 },
        },
        config);

    REQUIRE(compensated.size() == 2);
    REQUIRE(compensated[0].intents.size() == 2);
    CHECK(compensated[0].intents[1].status == CAxisIntentStatus::DirectionUnavailable);
    CHECK(compensated[0].intents[1].control_source_path_index == 1);
    CHECK(compensated[0].intents[1].control_source_path_mm == Catch::Approx(0.0));
    CHECK(compensated[0].intents[1].compensation_advance_mm == Catch::Approx(1.0));
    CHECK(compensated[0].intents[1].target_color_id == std::optional<SurfaceColorId>(2));
}

TEST_CASE("Transport-volume sequence uses each path deposition rate", "[CoExtrusion]")
{
    std::vector<CAxisPathIntent> first = make_delay_intents();
    first.resize(1);
    std::vector<CAxisPathIntent> second = make_delay_intents();
    second.resize(1);
    second[0].status = CAxisIntentStatus::Resolved;

    DelayCompensationConfig config;
    config.model = DelayModel::TransportVolume;
    config.transport_volume_mm3 = 0.4;

    const std::vector<DelayCompensationPath> compensated = DelayCompensator::compensate_sequence(
        {
            { first, {}, 0.2 },
            { second, {}, 0.4 },
        },
        config);

    REQUIRE(compensated.size() == 2);
    CHECK(compensated[0].intents[0].status == CAxisIntentStatus::Resolved);
    CHECK(compensated[0].intents[0].control_source_path_index == 1);
}

namespace {

CAxisPathIntent make_oriented_intent(double center_angle_deg)
{
    CAxisPathIntent intent;
    intent.path_start_mm = 0.0;
    intent.path_end_mm = 1.0;
    intent.status = CAxisIntentStatus::Resolved;
    DirectionResolution direction;
    direction.kind = DirectionIntentKind::OrientSurface;
    direction.acceptable_angles = AngularInterval { center_angle_deg, 0.0 };
    direction.target_angle_deg = center_angle_deg;
    intent.direction = direction;
    return intent;
}

} // namespace

TEST_CASE("C-axis planner slows XYZ when synchronized rotation exceeds limits", "[CoExtrusion]")
{
    CAxisMotionPlannerConfig config;
    config.large_rotation_strategy = LargeRotationStrategy::SlowDown;
    config.max_speed_deg_s = 30.0;
    config.max_acceleration_deg_s2 = 60.0;

    const CAxisMotionPlan plan = CAxisMotionPlanner(config).plan(
        { make_oriented_intent(90.0) }, { 0.1 }, 0.0);

    REQUIRE(plan.segments.size() == 1);
    CHECK(plan.segments.front().status == CAxisMotionStatus::SlowDownRequired);
    CHECK(plan.segments.front().target_angle_deg == std::optional<double>(90.0));
    CHECK(plan.segments.front().xyz_speed_scale < 1.0);
    CHECK(plan.final_angle_deg == std::optional<double>(90.0));
}

TEST_CASE("C-axis planner spreads an upcoming rotation over multiple segments", "[CoExtrusion]")
{
    CAxisMotionPlannerConfig config;
    config.max_speed_deg_s = 1000.0;
    config.max_acceleration_deg_s2 = 10000.0;
    config.transition_speed_deg_s = 30.0;
    config.transition_acceleration_deg_s2 = 60.0;
    config.spread_transitions = true;

    std::vector<CAxisPathIntent> intents {
        make_oriented_intent(0.0),
        make_oriented_intent(0.0),
        make_oriented_intent(0.0),
        make_oriented_intent(90.0),
        make_oriented_intent(90.0),
    };
    const CAxisMotionPlan plan = CAxisMotionPlanner(config).plan(
        intents, std::vector<double>(intents.size(), 0.01), 0.0);

    REQUIRE(plan.segments.size() == intents.size());
    REQUIRE(plan.segments.front().target_angle_deg.has_value());
    CHECK(*plan.segments.front().target_angle_deg > 0.0);
    CHECK(*plan.segments.front().target_angle_deg < 90.0);
    CHECK(plan.segments.front().transition_spread);
    CHECK(plan.segments.front().status == CAxisMotionStatus::SlowDownRequired);
    CHECK(plan.segments.front().xyz_speed_scale > 0.0);
    CHECK(plan.segments.front().xyz_speed_scale < 1.0);
    REQUIRE(plan.segments.back().target_angle_deg.has_value());
    CHECK(*plan.segments.back().target_angle_deg == Catch::Approx(90.0));
}

TEST_CASE("Positive-only C-axis mode unwraps to a forward angle", "[CoExtrusion]")
{
    CAxisMotionPlannerConfig config;
    config.rotation_mode = CAxisRotationMode::PositiveOnly;
    config.max_speed_deg_s = 1000.0;
    config.max_acceleration_deg_s2 = 10000.0;

    const CAxisMotionPlan plan = CAxisMotionPlanner(config).plan(
        { make_oriented_intent(350.0) }, { 1.0 }, 10.0);

    REQUIRE(plan.segments.size() == 1);
    REQUIRE(plan.segments.front().target_angle_deg.has_value());
    CHECK(*plan.segments.front().target_angle_deg >= 10.0);
    CHECK(*plan.segments.front().target_angle_deg == Catch::Approx(350.0));
}

TEST_CASE("Shortest-path C-axis mode remains continuous across configured limited-range bounds", "[CoExtrusion]")
{
    CAxisMotionPlannerConfig config;
    config.rotation_mode = CAxisRotationMode::ShortestPath;
    config.min_angle_deg = -360.0;
    config.max_angle_deg = 360.0;
    config.max_speed_deg_s = 1000.0;
    config.max_acceleration_deg_s2 = 10000.0;

    const CAxisMotionPlan plan = CAxisMotionPlanner(config).plan(
        { make_oriented_intent(353.0) }, { 1.0 }, -356.0);

    REQUIRE(plan.segments.size() == 1);
    CHECK(plan.segments.front().target_angle_deg == std::optional<double>(-367.0));
    CHECK(plan.segments.front().angular_delta_deg == Catch::Approx(-11.0));
}

TEST_CASE("Limited-range C-axis mode respects software limits", "[CoExtrusion]")
{
    CAxisMotionPlannerConfig config;
    config.rotation_mode = CAxisRotationMode::LimitedRange;
    config.min_angle_deg = -360.0;
    config.max_angle_deg = 360.0;

    const CAxisMotionPlan plan = CAxisMotionPlanner(config).plan(
        { make_oriented_intent(353.0) }, { 1.0 }, -356.0);

    REQUIRE(plan.segments.size() == 1);
    CHECK(plan.segments.front().target_angle_deg == std::optional<double>(-7.0));
}

TEST_CASE("Short curved-wall segments keep synchronized extrusion when slowed", "[CoExtrusion]")
{
    CAxisMotionPlannerConfig config;
    config.rotation_mode = CAxisRotationMode::LimitedRange;
    config.large_rotation_strategy = LargeRotationStrategy::SlowDown;
    config.max_speed_deg_s = 360.0;
    config.max_acceleration_deg_s2 = 1440.0;
    config.preposition_first_segment = true;
    std::vector<CAxisPathIntent> intents;
    for (int index = 0; index < 3; ++index) {
        auto intent = make_oriented_intent(6.0 * index);
        intent.target_color_id = 1;
        intent.path_start_mm = 0.55 * index;
        intent.path_end_mm = 0.55 * (index + 1);
        intents.emplace_back(std::move(intent));
    }
    const auto plan = CAxisMotionPlanner(config).plan(intents, {0.011, 0.011, 0.011}, 0.0);
    REQUIRE(plan.segments.size() == 3);
    for (size_t index = 1; index < plan.segments.size(); ++index) {
        CHECK(plan.segments[index].status == CAxisMotionStatus::SlowDownRequired);
        CHECK(plan.segments[index].emit_axis_command);
        CHECK(plan.segments[index].xyz_speed_scale > 0.0);
        CHECK(50.0 * plan.segments[index].xyz_speed_scale < 15.0);
    }
}

TEST_CASE("Half-turn limits force non-extruding unwind even when rotation is fast enough", "[CoExtrusion]")
{
    CAxisMotionPlannerConfig config;
    config.rotation_mode = CAxisRotationMode::LimitedRange;
    config.large_rotation_strategy = LargeRotationStrategy::SlowDown;
    config.spread_transitions = true;
    for (double sign : {-1.0, 1.0}) {
        const auto plan = CAxisMotionPlanner(config).plan(
            {make_oriented_intent(-175.0 * sign)}, {100.0}, 175.0 * sign);
        REQUIRE(plan.segments.size() == 1);
        CHECK(plan.segments.front().target_angle_deg == std::optional<double>(-175.0 * sign));
        CHECK(plan.segments.front().status == CAxisMotionStatus::IndependentRotationRequired);
        CHECK_FALSE(plan.segments.front().transition_spread);
    }
}

TEST_CASE("Limited C-axis writer retains physical coordinates and rejects out-of-range commands", "[CoExtrusion]")
{
    PrintConfig config;
    config.coextrusion_c_axis_enabled.value = true;
    config.coextrusion_c_axis_has_slip_ring.value = false;
    config.coextrusion_c_axis_min.value = -180.0;
    config.coextrusion_c_axis_max.value = 180.0;
    GCodeWriter writer;
    writer.apply_print_config(config);
    const std::string positive = writer.rotate_coextrusion_axis(180.0, 90.0);
    const std::string negative = writer.rotate_coextrusion_axis(-175.0, 90.0);
    CHECK(positive.find("C180") != std::string::npos);
    CHECK(negative.find("C-175") != std::string::npos);
    CHECK(negative.find("G92") == std::string::npos);
    CHECK_THROWS(writer.rotate_coextrusion_axis(181.0, 90.0));
}

TEST_CASE("Nonplanar co-extrusion paths query painted surfaces at each microsegment Z", "[CoExtrusion]")
{
    Model model;
    ModelObject *object = model.add_object("cube", "", make_cube(10, 10, 10));
    REQUIRE(object != nullptr);
    object->config.set_key_value("coextrusion_surface_color_id", new ConfigOptionInt(1));
    ObjectSurfaceProvenanceResolver resolver(*object);

    ExtrusionPath contoured(erExternalPerimeter, 0.04, 0.4f, 0.2f);
    contoured.z_contoured = true;
    contoured.polyline = Polyline3(Points3 {
        Point3(scale_(10.2), scale_(1.0), scale_(-4.0)),
        Point3(scale_(10.2), scale_(9.0), scale_(3.0))
    });

    const std::vector<MatchedSurfacePathSegment> contoured_segments =
        SurfacePathMatcher::match_nonplanar_surface_path(resolver, 6.0, contoured, 2.0, 0.5);
    REQUIRE(contoured_segments.size() == 4);
    REQUIRE(contoured_segments.front().query_z_mm.has_value());
    REQUIRE(contoured_segments.back().query_z_mm.has_value());
    CHECK(*contoured_segments.front().query_z_mm == Catch::Approx(2.875));
    CHECK(*contoured_segments.back().query_z_mm == Catch::Approx(8.125));
    CHECK(contoured_segments.front().z_delta_mm == Catch::Approx(1.75));
    CHECK(contoured_segments.back().z_delta_mm == Catch::Approx(1.75));
    CHECK(std::all_of(contoured_segments.begin(), contoured_segments.end(),
        [](const MatchedSurfacePathSegment &segment) { return segment.surface.has_value(); }));

    ExtrusionPath flat(erExternalPerimeter, 0.04, 0.4f, 0.2f);
    flat.polyline = Polyline3(Points3 {
        Point3(scale_(10.2), scale_(1.0), coord_t(0)),
        Point3(scale_(10.2), scale_(9.0), coord_t(0))
    });
    const ExtrusionPathSloped sloped(
        std::move(flat),
        ExtrusionPathSloped::Slope { 0.0, 1.0 },
        ExtrusionPathSloped::Slope { 1.0, 1.0 });

    const std::vector<MatchedSurfacePathSegment> sloped_segments =
        SurfacePathMatcher::match_nonplanar_surface_path(resolver, 6.0, sloped, 2.0, 0.5);
    REQUIRE(sloped_segments.size() == 4);
    REQUIRE(sloped_segments.front().query_z_mm.has_value());
    REQUIRE(sloped_segments.back().query_z_mm.has_value());
    CHECK(*sloped_segments.front().query_z_mm == Catch::Approx(5.825));
    CHECK(*sloped_segments.back().query_z_mm == Catch::Approx(5.975));
    CHECK(sloped_segments.front().z_delta_mm == Catch::Approx(0.05));
    CHECK(sloped_segments.back().z_delta_mm == Catch::Approx(0.05));
}
