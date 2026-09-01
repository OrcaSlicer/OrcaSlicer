#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/ExclusionVolumePathCheck.hpp"
#include "libslic3r/GCode/ExclusionVolumeTravelAvoidance.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/SupportLayer.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

DynamicPrintConfig exclusion_config(
    BedExcludeAreaMode mode = BedExcludeAreaMode::Shared,
    const std::string &shared = "0..10;40x40,60x40,60x60,40x60",
    const std::vector<std::string> &per_extruder = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const size_t extruder_count = std::max<size_t>(1, per_extruder.size());
    config.set_num_extruders(extruder_count);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(extruder_count, 0.4));
    config.set_key_value("extruder_offset", new ConfigOptionPoints(extruder_count, Vec2d::Zero()));
    config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable(extruder_count, 100.0));
    config.set_key_value("printable_height", new ConfigOptionFloat(100.0));
    config.set_key_value("printable_area", new ConfigOptionPoints{
        Vec2d(0.0, 0.0), Vec2d(100.0, 0.0), Vec2d(100.0, 100.0), Vec2d(0.0, 100.0),
    });
    config.set_key_value("bed_exclude_area_mode", new ConfigOptionEnum<BedExcludeAreaMode>(mode));
    config.set_deserialize_strict("bed_exclude_area", shared);
    if (!per_extruder.empty())
        config.set_key_value("extruder_bed_exclude_area", new ConfigOptionStrings(per_extruder));
    return config;
}

FullPrintConfig static_config(const DynamicPrintConfig &dynamic)
{
    FullPrintConfig config;
    // full_print_config() also contains GUI/profile-only keys which are not
    // members of the static slicing config used by these lower-level classes.
    config.apply(dynamic, true);
    return config;
}

Polyline travel(std::initializer_list<Vec2d> points)
{
    Points scaled_points;
    scaled_points.reserve(points.size());
    for (const Vec2d &point : points)
        scaled_points.emplace_back(Point::new_scale(point));
    return Polyline(std::move(scaled_points));
}

Vec2d to_mm(const Point &point)
{
    return point.cast<double>() * SCALING_FACTOR;
}

struct Rect
{
    double min_x;
    double min_y;
    double max_x;
    double max_y;
};

bool point_inside_open_rect(const Vec2d &point, const Rect &rect)
{
    return point.x() > rect.min_x && point.x() < rect.max_x &&
           point.y() > rect.min_y && point.y() < rect.max_y;
}

// Independent Liang-Barsky-style test oracle for the simple rectangular
// obstacles used here. This deliberately does not call the production path
// checker, so matching bugs in the checker and router cannot hide each other.
bool segment_enters_open_rect(const Vec2d &from, const Vec2d &to, const Rect &rect)
{
    if (point_inside_open_rect(from, rect) || point_inside_open_rect(to, rect))
        return true;

    double t_min = 0.0;
    double t_max = 1.0;
    const Vec2d delta = to - from;
    const auto clip_axis = [&](double origin, double direction, double min_value, double max_value,
                               double &lower, double &upper) {
        if (std::abs(direction) < 1e-12)
            return origin > min_value && origin < max_value;
        double first  = (min_value - origin) / direction;
        double second = (max_value - origin) / direction;
        if (first > second)
            std::swap(first, second);
        lower = std::max(lower, first);
        upper = std::min(upper, second);
        return lower < upper;
    };

    if (!clip_axis(from.x(), delta.x(), rect.min_x, rect.max_x, t_min, t_max))
        return false;
    if (!clip_axis(from.y(), delta.y(), rect.min_y, rect.max_y, t_min, t_max))
        return false;
    if (t_min >= t_max)
        return false;

    const double midpoint_t = 0.5 * (std::max(0.0, t_min) + std::min(1.0, t_max));
    return midpoint_t >= 0.0 && midpoint_t <= 1.0 &&
           point_inside_open_rect(from + midpoint_t * delta, rect);
}

void check_path_avoids_rect(const Polyline &path, const Rect &rect)
{
    REQUIRE(path.points.size() >= 2);
    for (size_t idx = 1; idx < path.points.size(); ++idx)
        CHECK_FALSE(segment_enters_open_rect(to_mm(path.points[idx - 1]), to_mm(path.points[idx]), rect));
}

ExclusionVolumePathChecker configured_checker(const FullPrintConfig &config)
{
    ExclusionVolumePathChecker checker;
    checker.configure(config);
    return checker;
}

void check_motion(
    ExclusionVolumePathChecker &checker,
    const Vec3d &from,
    const Vec3d &to,
    int extruder_id = 0,
    ExclusionVolumeMotionType type = ExclusionVolumeMotionType::Travel,
    bool start_xy_known = true,
    bool end_xy_known = true,
    bool start_z_known = true,
    bool end_z_known = true,
    unsigned int gcode_id = 17,
    size_t move_id = 23)
{
    checker.check_motion(
        from, to, start_xy_known, end_xy_known, start_z_known, end_z_known,
        extruder_id, type, 7, gcode_id, move_id);
}

struct ProcessorExclusionResult
{
    bool checked {false};
    bool conflict {false};
    bool travel_conflict {false};
    bool extrusion_conflict {false};
    int conflict_extruder_id {-1};
};

class ScopedBblPrinterMode
{
public:
    explicit ScopedBblPrinterMode(bool enabled)
        : m_previous(GCodeProcessor::s_IsBBLPrinter)
    {
        GCodeProcessor::s_IsBBLPrinter = enabled;
    }

    ~ScopedBblPrinterMode() { GCodeProcessor::s_IsBBLPrinter = m_previous; }

private:
    bool m_previous;
};

ProcessorExclusionResult run_processor(const FullPrintConfig &config, const std::string &gcode)
{
    ScopedTemporaryFile file(".gcode");
    {
        std::ofstream output(file.string());
        output << gcode;
    }

    // The processor exposes this mode as shared state. Restore it after each
    // invocation so these tests remain independent of Catch2 execution order.
    ScopedBblPrinterMode printer_mode(false);
    GCodeProcessor processor;
    processor.apply_config(config);
    processor.configure_exclusion_volume_path_check(config);
    processor.process_file(file.string());
    const GCodeProcessorResult &result = processor.get_result();
    return {
        result.exclusion_volume_path_checked,
        result.exclusion_volume_path_conflict,
        result.exclusion_volume_travel_conflict,
        result.exclusion_volume_extrusion_conflict,
        result.exclusion_volume_conflict_extruder_id,
    };
}

Polygon rectangle(double min_x, double min_y, double max_x, double max_y)
{
    Polygon polygon({
        Point::new_scale(min_x, min_y),
        Point::new_scale(max_x, min_y),
        Point::new_scale(max_x, max_y),
        Point::new_scale(min_x, max_y),
    });
    polygon.make_counter_clockwise();
    return polygon;
}

bool polygons_cover(const Polygons &polygons, double x, double y)
{
    const Point point = Point::new_scale(x, y);
    const ExPolygons expolygons = union_ex(polygons);
    return std::any_of(expolygons.begin(), expolygons.end(),
                       [&point](const ExPolygon &polygon) { return polygon.contains(point); });
}

struct BrimRun
{
    double total_length {0.0};
    bool intersects_test_region {false};
};

BrimRun generate_brim(const std::string &exclusion_definition)
{
    DynamicPrintConfig config = exclusion_config(BedExcludeAreaMode::Shared, exclusion_definition);
    config.set_deserialize_strict({
        {"skirt_loops", "0"},
        {"brim_type", "outer_only"},
        {"brim_width", "10"},
        {"initial_layer_line_width", "0.5"},
    });

    TriangleMesh mesh = make_cube(20.0, 20.0, 20.0);
    mesh.translate(40.0f, 40.0f, 0.0f);
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{std::move(mesh)}, print, model, config, nullptr, false);
    print.process();

    Polylines paths;
    for (const Print::SkirtBrimGroup &group : print.skirt_brim_groups())
        for (const Print::SkirtBrimGroup::Brim &brim : group.brims) {
            Polylines current = brim.brim.as_polylines();
            paths.insert(paths.end(), std::make_move_iterator(current.begin()), std::make_move_iterator(current.end()));
        }

    BrimRun result;
    for (const Polyline &path : paths)
        result.total_length += path.length();
    result.intersects_test_region =
        !intersection_pl(paths, Polygons{rectangle(32.0, 35.0, 38.0, 65.0)}).empty();
    return result;
}

DynamicPrintConfig support_config(int support_filament, int interface_filament)
{
    DynamicPrintConfig config = exclusion_config(
        BedExcludeAreaMode::PerExtruder, "",
        {"0..10;4x4,12x4,12x12,4x12", "0..10;22x4,30x4,30x12,22x12"});
    // PrintObject clamps support role ids against the number of logical
    // filaments, represented by filament_diameter in the static config.
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75});
    config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    config.set_key_value("support_filament", new ConfigOptionInt(support_filament));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(interface_filament));
    return config;
}

struct SupportPrintFixture
{
    Model model;
    Print print;

    explicit SupportPrintFixture(const DynamicPrintConfig &config)
    {
        ModelObject *object = model.add_object();
        ModelVolume *first = object->add_volume(make_cube(20.0, 20.0, 20.0));
        ModelVolume *second = object->add_volume(make_cube(20.0, 20.0, 20.0));
        first->config.set("extruder", 1);
        second->config.set("extruder", 2);
        // Pin the role assignments on the object as well as the process
        // config. This fixture intentionally exercises support exclusion
        // selection, not the separate preset-remapping subsystem.
        object->config.set("support_filament", config.opt_int("support_filament"));
        object->config.set("support_interface_filament", config.opt_int("support_interface_filament"));
        object->add_instance();
        object->ensure_on_bed();
        print.apply(model, config);
    }

    PrintObject &object() { return *print.objects_mutable().front(); }
};

} // namespace

TEST_CASE("Path checker ignores clear motions and motions outside the active Z slab", "[ExclusionVolume][GCode]")
{
    const FullPrintConfig config = static_config(exclusion_config());

    SECTION("clear XY") {
        auto checker = configured_checker(config);
        check_motion(checker, Vec3d(10.0, 10.0, 5.0), Vec3d(20.0, 20.0, 5.0));
        CHECK_FALSE(checker.result().has_any_conflict);
    }

    SECTION("above volume") {
        auto checker = configured_checker(config);
        check_motion(checker, Vec3d(20.0, 50.0, 20.0), Vec3d(80.0, 50.0, 20.0));
        CHECK_FALSE(checker.result().has_any_conflict);
    }

    SECTION("below volume") {
        auto checker = configured_checker(config);
        check_motion(checker, Vec3d(20.0, 50.0, -5.0), Vec3d(80.0, 50.0, -5.0));
        CHECK_FALSE(checker.result().has_any_conflict);
    }
}

TEST_CASE("Path checker detects crossing inside and boundary motions", "[ExclusionVolume][GCode]")
{
    const FullPrintConfig config = static_config(exclusion_config());
    const auto [from, to] = GENERATE(table<Vec3d, Vec3d>({
        {Vec3d(20.0, 50.0, 5.0), Vec3d(80.0, 50.0, 5.0)},
        {Vec3d(45.0, 45.0, 5.0), Vec3d(55.0, 55.0, 5.0)},
        {Vec3d(20.0, 40.0, 5.0), Vec3d(80.0, 40.0, 5.0)},
        {Vec3d(50.0, 20.0, -5.0), Vec3d(50.0, 80.0, 15.0)},
    }));

    auto checker = configured_checker(config);
    check_motion(checker, from, to);
    CHECK(checker.result().has_any_conflict);
    REQUIRE(checker.result().first_hit.has_value());
    CHECK(checker.result().first_hit->extruder_id == 0);
}

TEST_CASE("Path checker records motion classification and stable first-hit metadata", "[ExclusionVolume][GCode]")
{
    const auto [type, travel_hit, extrusion_hit, other_hit] = GENERATE(table<ExclusionVolumeMotionType, bool, bool, bool>({
        {ExclusionVolumeMotionType::Travel,  true,  false, false},
        {ExclusionVolumeMotionType::Extrude, false, true,  false},
        {ExclusionVolumeMotionType::Other,   false, false, true },
    }));

    auto checker = configured_checker(static_config(exclusion_config()));
    check_motion(checker, Vec3d(20.0, 50.0, 5.0), Vec3d(80.0, 50.0, 5.0), 0, type,
                 true, true, true, true, 101, 202);
    check_motion(checker, Vec3d(20.0, 50.0, 5.0), Vec3d(80.0, 50.0, 5.0), 0, type,
                 true, true, true, true, 303, 404);

    const ExclusionVolumePathCheckResult &result = checker.result();
    CHECK(result.has_travel_conflict == travel_hit);
    CHECK(result.has_extrusion_conflict == extrusion_hit);
    CHECK(result.has_other_motion_conflict == other_hit);
    REQUIRE(result.first_hit.has_value());
    CHECK(result.first_hit->gcode_id == 101);
    CHECK(result.first_hit->move_id == 202);
    CHECK(result.first_hit->source_move_type == 7);
}

TEST_CASE("Path checker handles unknown coordinates conservatively without inventing XY", "[ExclusionVolume][GCode]")
{
    const FullPrintConfig config = static_config(exclusion_config());

    SECTION("unknown XY start is ignored") {
        auto checker = configured_checker(config);
        check_motion(checker, Vec3d(0.0, 0.0, 5.0), Vec3d(80.0, 50.0, 5.0), 0,
                     ExclusionVolumeMotionType::Travel, false, true);
        CHECK_FALSE(checker.result().has_any_conflict);
    }

    SECTION("unknown Z checks every height and marks the result") {
        auto checker = configured_checker(config);
        check_motion(checker, Vec3d(20.0, 50.0, 100.0), Vec3d(80.0, 50.0, 100.0), 0,
                     ExclusionVolumeMotionType::Travel, true, true, false, false);
        REQUIRE(checker.result().first_hit.has_value());
        CHECK(checker.result().first_hit->used_unknown_z);
    }
}

TEST_CASE("Path checker selects the active nozzle and applies its XY offset", "[ExclusionVolume][GCode][MultiNozzle]")
{
    DynamicPrintConfig dynamic = exclusion_config(
        BedExcludeAreaMode::PerExtruder, "", {"", "0..10;40x40,60x40,60x60,40x60"});
    dynamic.set_key_value("extruder_offset", new ConfigOptionPoints{Vec2d::Zero(), Vec2d(20.0, 0.0)});
    const FullPrintConfig config = static_config(dynamic);

    SECTION("unrelated nozzle remains clear") {
        auto checker = configured_checker(config);
        check_motion(checker, Vec3d(20.0, 50.0, 5.0), Vec3d(80.0, 50.0, 5.0), 0);
        CHECK_FALSE(checker.result().has_any_conflict);
    }

    SECTION("active nozzle offset converts emitted coordinates into model space") {
        auto checker = configured_checker(config);
        check_motion(checker, Vec3d(0.0, 50.0, 5.0), Vec3d(50.0, 50.0, 5.0), 1);
        REQUIRE(checker.result().first_hit.has_value());
        CHECK(checker.result().first_hit->extruder_id == 1);
        CHECK_THAT(checker.result().first_hit->from.x(), WithinAbs(20.0, 1e-9));
    }

    SECTION("unknown active nozzle checks all physical nozzles") {
        auto checker = configured_checker(config);
        check_motion(checker, Vec3d(0.0, 50.0, 5.0), Vec3d(50.0, 50.0, 5.0), -1);
        REQUIRE(checker.result().first_hit.has_value());
        CHECK(checker.result().first_hit->extruder_id == 1);
    }
}

TEST_CASE("Travel router leaves clear and vertically separated paths unchanged", "[ExclusionVolume][TravelRouting]")
{
    ExclusionVolumeTravelAvoidance router;
    router.init(static_config(exclusion_config()), Vec3d::Zero());

    const Polyline clear = travel({Vec2d(10.0, 10.0), Vec2d(90.0, 10.0)});
    auto result = router.route(clear, 5.0, 5.0, 0);
    CHECK(result.status == ExclusionVolumeTravelAvoidance::Status::Unchanged);
    CHECK(result.detail == ExclusionVolumeTravelAvoidance::Detail::NoIntersection);
    CHECK(result.path.points == clear.points);

    const Polyline crossing = travel({Vec2d(20.0, 50.0), Vec2d(80.0, 50.0)});
    result = router.route(crossing, 20.0, 20.0, 0);
    CHECK(result.status == ExclusionVolumeTravelAvoidance::Status::Unchanged);
    CHECK(result.detail == ExclusionVolumeTravelAvoidance::Detail::NoActiveObstacles);
}

TEST_CASE("Travel router produces a safe in-bed detour around an active volume", "[ExclusionVolume][TravelRouting]")
{
    ExclusionVolumeTravelAvoidance router;
    router.init(static_config(exclusion_config()), Vec3d::Zero());
    const Polyline direct = travel({Vec2d(20.0, 50.0), Vec2d(80.0, 50.0)});

    const auto result = router.route(direct, 5.0, 5.0, 0);
    REQUIRE(result.status == ExclusionVolumeTravelAvoidance::Status::Rerouted);
    REQUIRE(result.path.points.size() > 2);
    CHECK(result.path.points.front() == direct.points.front());
    CHECK(result.path.points.back() == direct.points.back());
    check_path_avoids_rect(result.path, Rect{40.0, 40.0, 60.0, 60.0});
    for (const Point &point : result.path.points) {
        const Vec2d position = to_mm(point);
        CHECK(position.x() >= 0.0);
        CHECK(position.x() <= 100.0);
        CHECK(position.y() >= 0.0);
        CHECK(position.y() <= 100.0);
    }
}

TEST_CASE("Travel router activates volumes touched by a sloped Z move", "[ExclusionVolume][TravelRouting]")
{
    ExclusionVolumeTravelAvoidance router;
    router.init(static_config(exclusion_config()), Vec3d::Zero());
    const Polyline direct = travel({Vec2d(20.0, 50.0), Vec2d(80.0, 50.0)});

    const auto result = router.route(direct, -5.0, 15.0, 0);
    REQUIRE(result.status == ExclusionVolumeTravelAvoidance::Status::Rerouted);
    check_path_avoids_rect(result.path, Rect{40.0, 40.0, 60.0, 60.0});
}

TEST_CASE("Travel router reports unsafe endpoints unknown tools and blocked beds", "[ExclusionVolume][TravelRouting]")
{
    SECTION("endpoint inside") {
        ExclusionVolumeTravelAvoidance router;
        router.init(static_config(exclusion_config()), Vec3d::Zero());
        const auto result = router.route(travel({Vec2d(50.0, 50.0), Vec2d(80.0, 50.0)}), 5.0, 5.0, 0);
        CHECK(result.status == ExclusionVolumeTravelAvoidance::Status::EndpointInside);
        CHECK(result.detail == ExclusionVolumeTravelAvoidance::Detail::EndpointInside);
    }

    SECTION("unknown tool") {
        ExclusionVolumeTravelAvoidance router;
        router.init(static_config(exclusion_config()), Vec3d::Zero());
        const auto result = router.route(travel({Vec2d(20.0, 50.0), Vec2d(80.0, 50.0)}), 5.0, 5.0, -1);
        CHECK(result.status == ExclusionVolumeTravelAvoidance::Status::Failed);
        CHECK(result.detail == ExclusionVolumeTravelAvoidance::Detail::UnknownExtruder);
    }

    SECTION("obstacle separates the bed") {
        ExclusionVolumeTravelAvoidance router;
        router.init(static_config(exclusion_config(
            BedExcludeAreaMode::Shared, "0..10;45x0,55x0,55x100,45x100")), Vec3d::Zero());
        const auto result = router.route(travel({Vec2d(20.0, 50.0), Vec2d(80.0, 50.0)}), 5.0, 5.0, 0);
        CHECK(result.status == ExclusionVolumeTravelAvoidance::Status::Failed);
    }
}

TEST_CASE("Travel router uses only the active nozzle exclusion set", "[ExclusionVolume][TravelRouting][MultiNozzle]")
{
    const FullPrintConfig config = static_config(exclusion_config(
        BedExcludeAreaMode::PerExtruder, "", {"0..10;40x40,60x40,60x60,40x60", ""}));
    ExclusionVolumeTravelAvoidance router;
    router.init(config, Vec3d::Zero());
    const Polyline direct = travel({Vec2d(20.0, 50.0), Vec2d(80.0, 50.0)});

    CHECK(router.route(direct, 5.0, 5.0, 0).status == ExclusionVolumeTravelAvoidance::Status::Rerouted);
    const auto clear = router.route(direct, 5.0, 5.0, 1);
    CHECK(clear.status == ExclusionVolumeTravelAvoidance::Status::Unchanged);
    CHECK(clear.detail == ExclusionVolumeTravelAvoidance::Detail::NoActiveObstacles);
}

TEST_CASE("G-code processor ignores the artificial first XY origin and checks subsequent travel", "[ExclusionVolume][GCodeProcessor]")
{
    const FullPrintConfig config = static_config(exclusion_config());

    SECTION("first explicit position has no invented start") {
        const ProcessorExclusionResult result = run_processor(config, "G90\nG1 X80 Y50 Z5 F6000\n");
        CHECK(result.checked);
        CHECK_FALSE(result.conflict);
    }

    SECTION("next motion starts at the established position") {
        const ProcessorExclusionResult result = run_processor(config, "G90\nG1 X80 Y50 Z5 F6000\nG1 X20 Y50\n");
        CHECK(result.conflict);
        CHECK(result.travel_conflict);
    }
}

TEST_CASE("G-code processor tracks absolute relative and reset coordinate state", "[ExclusionVolume][GCodeProcessor]")
{
    const FullPrintConfig config = static_config(exclusion_config());

    SECTION("relative move crosses the volume") {
        const ProcessorExclusionResult result = run_processor(
            config, "G90\nG1 X20 Y50 Z5\nG91\nG1 X60 Y0\n");
        CHECK(result.conflict);
    }

    SECTION("G92 preserves a known transformed coordinate frame") {
        const ProcessorExclusionResult result = run_processor(
            config, "G90\nG1 X20 Y50 Z5\nG92 X0 Y0\nG1 X60 Y0\n");
        CHECK(result.conflict);
    }

    SECTION("homing does not create a synthetic checked move") {
        const ProcessorExclusionResult result = run_processor(config, "G28\nG1 X80 Y50 Z5\n");
        CHECK_FALSE(result.conflict);
    }
}

TEST_CASE("G-code processor classifies extrusion conflicts", "[ExclusionVolume][GCodeProcessor]")
{
    const FullPrintConfig config = static_config(exclusion_config());
    const ProcessorExclusionResult result = run_processor(
        config, "G90\nM83\nG1 X20 Y50 Z5\nG1 X80 Y50 E1\n");

    CHECK(result.conflict);
    CHECK(result.extrusion_conflict);
    CHECK_FALSE(result.travel_conflict);
}

TEST_CASE("G-code processor changes the active physical nozzle at tool selection", "[ExclusionVolume][GCodeProcessor][MultiNozzle]")
{
    const FullPrintConfig config = static_config(exclusion_config(
        BedExcludeAreaMode::PerExtruder, "", {"", "0..10;40x40,60x40,60x60,40x60"}));

    const ProcessorExclusionResult first_tool = run_processor(config, "G90\nT0\nG1 X20 Y50 Z5\nG1 X80 Y50\n");
    CHECK_FALSE(first_tool.conflict);

    const ProcessorExclusionResult second_tool = run_processor(config, "G90\nT1\nG1 X20 Y50 Z5\nG1 X80 Y50\n");
    CHECK(second_tool.conflict);
    CHECK(second_tool.conflict_extruder_id == 1);
}

TEST_CASE("Brim paths are clipped only by exclusions active on the first layer", "[ExclusionVolume][Brim]")
{
    const BrimRun active = generate_brim("0..1;32x35,38x35,38x65,32x65");
    const BrimRun raised = generate_brim("10..20;32x35,38x35,38x65,32x65");

    REQUIRE(active.total_length > 0.0);
    REQUIRE(raised.total_length > 0.0);
    CHECK_FALSE(active.intersects_test_region);
    CHECK(raised.intersects_test_region);
    CHECK(active.total_length < raised.total_length);
}

TEST_CASE("Support exclusion masks follow explicit and current-filament roles", "[ExclusionVolume][Support][MultiNozzle]")
{
    SECTION("explicit support roles select one physical nozzle each") {
        SupportPrintFixture fixture(support_config(1, 2));
        CHECK(support_exclusion_filaments(fixture.object(), false) == std::vector<unsigned int>{0});
        CHECK(support_exclusion_filaments(fixture.object(), true) == std::vector<unsigned int>{1});

        const auto base_masks = support_exclusion_areas_for_layers(fixture.object(), {{0.0, 1.0}}, false);
        const auto interface_masks = support_exclusion_areas_for_layers(fixture.object(), {{0.0, 1.0}}, true);
        REQUIRE(base_masks.size() == 1);
        REQUIRE(interface_masks.size() == 1);
        // PrintObject support geometry is centred around the object's local
        // origin; the plate-space regions above are shifted by (10, 10).
        CHECK(polygons_cover(base_masks.front(), -4.0, -4.0));
        CHECK_FALSE(polygons_cover(base_masks.front(), 14.0, -4.0));
        CHECK_FALSE(polygons_cover(interface_masks.front(), -4.0, -4.0));
        CHECK(polygons_cover(interface_masks.front(), 14.0, -4.0));
    }

    SECTION("current filament remains conservative across every used nozzle") {
        SupportPrintFixture fixture(support_config(0, 2));
        std::vector<unsigned int> filaments = support_exclusion_filaments(fixture.object(), false);
        std::sort(filaments.begin(), filaments.end());
        CHECK(filaments == std::vector<unsigned int>{0, 1});

        const auto masks = support_exclusion_areas_for_layers(fixture.object(), {{0.0, 1.0}, {11.0, 12.0}}, false);
        REQUIRE(masks.size() == 2);
        CHECK(polygons_cover(masks[0], -4.0, -4.0));
        CHECK(polygons_cover(masks[0], 14.0, -4.0));
        CHECK(masks[1].empty());
    }
}

TEST_CASE("Support trimming applies the correct nozzle mask to every polygon category", "[ExclusionVolume][Support][MultiNozzle]")
{
    SupportPrintFixture fixture(support_config(1, 2));
    const Polygon source = rectangle(-20.0, -20.0, 40.0, 20.0);

    SupportGeneratorLayer base;
    base.layer_type = SupporLayerType::Base;
    base.print_z = 1.0;
    base.height = 0.2;
    base.polygons = {source};
    base.contact_polygons = std::make_unique<Polygons>(Polygons{source});
    base.overhang_polygons = std::make_unique<Polygons>(Polygons{source});
    base.enforcer_polygons = std::make_unique<Polygons>(Polygons{source});

    SupportGeneratorLayer interface;
    interface.layer_type = SupporLayerType::TopInterface;
    interface.print_z = 1.0;
    interface.height = 0.2;
    interface.polygons = {source};

    const SupportGeneratorLayersPtr none;
    const SupportGeneratorLayersPtr base_layers{&base};
    const SupportGeneratorLayersPtr interface_layers{&interface};
    trim_support_layers_by_exclusion_volumes(
        fixture.object(), none, none, none, base_layers, interface_layers, none);

    // Base support uses filament 1 and interface support uses filament 2.
    CHECK_FALSE(polygons_cover(base.polygons, -4.0, -4.0));
    CHECK(polygons_cover(base.polygons, 14.0, -4.0));
    CHECK(polygons_cover(interface.polygons, -4.0, -4.0));
    CHECK_FALSE(polygons_cover(interface.polygons, 14.0, -4.0));

    // The same sanitization is mandatory for the auxiliary support categories.
    REQUIRE(base.contact_polygons != nullptr);
    REQUIRE(base.overhang_polygons != nullptr);
    REQUIRE(base.enforcer_polygons != nullptr);
    CHECK_FALSE(polygons_cover(*base.contact_polygons, -4.0, -4.0));
    CHECK_FALSE(polygons_cover(*base.overhang_polygons, -4.0, -4.0));
    CHECK_FALSE(polygons_cover(*base.enforcer_polygons, -4.0, -4.0));
}
