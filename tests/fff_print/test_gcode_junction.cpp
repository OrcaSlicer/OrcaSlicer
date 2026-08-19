#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>
#include <vector>

using namespace Slic3r;

// The time estimator plans every corner with the motion model of the target firmware. Marlin 2
// ships with CLASSIC_JERK commented out and Klipper never had jerk at all, so both corner with a
// junction deviation model. Its binding term on a flattened curve is the curvature estimate,
// v = sqrt(acceleration * radius), which is a property of the curve rather than of the number of
// vertices the slicer chose to describe it with. Classic jerk has no such term: its corner
// velocity is jerk / (2 * sin(turn / 2)), so the same arc is planned slower every time it is
// flattened more coarsely, and an arc left with uneven turns is planned as a saw tooth.
namespace {

constexpr float ACCELERATION = 5000.f; // mm/s^2, emitted as M204 S
constexpr float FEEDRATE     = 200.f;  // mm/s, the nominal speed of the arc

FullPrintConfig make_config(GCodeFlavor flavor, double square_corner_velocity_or_jerk, double junction_deviation)
{
    FullPrintConfig config; // default-initialized with the built-in defaults
    config.gcode_flavor.value = flavor;
    // Machine limits wide enough that only the junction model can slow the arc down.
    config.machine_max_acceleration_extruding.values = {20000., 20000.};
    config.machine_max_acceleration_x.values         = {20000., 20000.};
    config.machine_max_acceleration_y.values         = {20000., 20000.};
    config.machine_max_acceleration_e.values         = {20000., 20000.};
    config.machine_max_speed_x.values                = {500., 500.};
    config.machine_max_speed_y.values                = {500., 500.};
    config.machine_max_speed_e.values                = {500., 500.};
    // Orca keeps Klipper's square corner velocity in the X/Y jerk slots.
    config.machine_max_jerk_x.values                 = {square_corner_velocity_or_jerk, square_corner_velocity_or_jerk};
    config.machine_max_jerk_y.values                 = {square_corner_velocity_or_jerk, square_corner_velocity_or_jerk};
    config.machine_max_jerk_e.values                 = {10., 10.};
    config.machine_max_junction_deviation.values     = {junction_deviation, junction_deviation};
    return config;
}

// A circular extrusion of `radius`, flattened into segments spanning the given arc steps. The
// steps repeat until the circle closes, so one step gives a regular polygon and several give the
// uneven flattening that the slicer's Douglas-Peucker pass leaves on a curve.
std::string arc_gcode(double radius, const std::vector<double>& arc_steps_deg)
{
    std::ostringstream os;
    os << "; FEATURE: Outer wall\n";
    os << "M204 S" << ACCELERATION << "\n";
    os << "G1 X" << radius << " Y0 Z0.2 F" << FEEDRATE * 60. << "\n";
    os << "G1 E1 F1800\n";

    double extruded = 1.;
    double swept    = 0.;
    for (size_t i = 0; swept < 360.; ++i) {
        const double step = std::min(arc_steps_deg[i % arc_steps_deg.size()], 360. - swept);
        swept += step;
        const double a = swept * M_PI / 180.;
        // 0.05 mm of filament per mm of path, so these count as extrusion moves.
        extruded += 2. * radius * std::sin(step * M_PI / 360.) * 0.05;
        os << "G1 X" << radius * std::cos(a) << " Y" << radius * std::sin(a) << " E" << extruded
           << " F" << FEEDRATE * 60. << "\n";
    }
    return os.str();
}

// The speed the estimator paints onto the toolpath in the Actual Speed view, over the middle half
// of the arc. The arc is entered from a standstill and left at one, and ramping up takes
// v^2 / (2 * acceleration) of path, so its two ends really do accelerate and decelerate.
std::vector<float> arc_speeds(const FullPrintConfig& config, const std::string& gcode)
{
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream os(temp.string());
        os << gcode;
    }
    GCodeProcessor proc;
    proc.apply_config(config);
    // No producer marker in the gcode, so process_file keeps our applied config.
    proc.process_file(temp.string());

    std::vector<float> speeds;
    for (const auto& mv : proc.get_result().moves)
        if (mv.type == EMoveType::Extrude && mv.actual_feedrate > 0.f)
            speeds.push_back(mv.actual_feedrate);
    REQUIRE(speeds.size() > 12);
    const size_t skip = speeds.size() / 4;
    return std::vector<float>(speeds.begin() + skip, speeds.end() - skip);
}

// Peak-to-peak spread of the arc speed as a fraction of its fastest move. This is the saw tooth
// the Actual Speed view draws; on a constant radius arc it should be ~0.
double speed_swing(const std::vector<float>& speeds)
{
    const auto [lo, hi] = std::minmax_element(speeds.begin(), speeds.end());
    return double(*hi - *lo) / double(*hi);
}

double mean_speed(const std::vector<float>& speeds)
{
    return std::accumulate(speeds.begin(), speeds.end(), 0.) / double(speeds.size());
}

// Arc steps measured on the outer wall of a 13.5 mm cylinder sliced at the default 0.012 mm
// resolution, where Douglas-Peucker merged the mesh's regular 2 degree facets unevenly.
const std::vector<double> DECIMATED = {2., 4., 2., 6., 4., 2., 4., 6.};

} // namespace

TEST_CASE("A junction deviation flavor plans an arc at its curvature limit whatever the sampling density", "[GCodeJunction]")
{
    // sqrt(5000 * 4) = 141 mm/s, well under the 200 mm/s nominal, so curvature is what binds.
    const double radius   = 4.;
    const double expected = std::sqrt(double(ACCELERATION) * radius);
    const double step     = GENERATE(1., 2., 4.);

    // Classic jerk would answer 9 / (2 * sin(step / 2)) here, that is 516, 258 and 129 mm/s for
    // the three samplings of this one circle. The junction deviation model answers 141 for all
    // three, because the curve, not its flattening, is what limits the speed.
    // The residual swing is the toolhead speeding up between two corners planned at the same
    // speed, so it grows with the segment length: 5% covers the 0.28 mm chords of the 4 degree
    // sampling. A saw tooth caused by the junction model would be an order of magnitude larger.
    DYNAMIC_SECTION("Klipper, flattened every " << step << " degrees")
    {
        const auto speeds = arc_speeds(make_config(gcfKlipper, 9., 0.01), arc_gcode(radius, {step}));
        CHECK_THAT(mean_speed(speeds), Catch::Matchers::WithinRel(expected, 0.03));
        CHECK(speed_swing(speeds) < 0.05);
    }

    DYNAMIC_SECTION("Marlin 2 with junction deviation, flattened every " << step << " degrees")
    {
        const auto speeds = arc_speeds(make_config(gcfMarlinFirmware, 9., 0.013), arc_gcode(radius, {step}));
        CHECK_THAT(mean_speed(speeds), Catch::Matchers::WithinRel(expected, 0.03));
        CHECK(speed_swing(speeds) < 0.05);
    }
}

TEST_CASE("A classic jerk flavor plans the same arc slower the more coarsely it was flattened", "[GCodeJunction]")
{
    // The counterpart of the case above, and the reason a curved wall changes speed with the size
    // of the part: the slicer decimates a small radius harder, and classic jerk charges for it.
    // A junction deviation of 0 is how a profile says the printer was built with CLASSIC_JERK.
    // The radius is large enough that its curvature never binds (sqrt(5000 * 30) = 387 mm/s), so
    // the corners are the only thing between the two samplings: 9 / (2 * sin(1 deg)) = 258 mm/s
    // leaves the nominal 200 mm/s alone, while 9 / (2 * sin(3 deg)) = 86 mm/s does not.
    const double radius = 30.;
    const auto   fine   = arc_speeds(make_config(gcfMarlinFirmware, 9., 0.), arc_gcode(radius, {2.}));
    const auto   coarse = arc_speeds(make_config(gcfMarlinFirmware, 9., 0.), arc_gcode(radius, {6.}));

    CHECK(mean_speed(coarse) < 0.7 * mean_speed(fine));

    // The same two samplings under the junction deviation model land within a few percent.
    const auto fine_jd   = arc_speeds(make_config(gcfKlipper, 9., 0.01), arc_gcode(radius, {2.}));
    const auto coarse_jd = arc_speeds(make_config(gcfKlipper, 9., 0.01), arc_gcode(radius, {6.}));

    CHECK(mean_speed(coarse_jd) > 0.9 * mean_speed(fine_jd));
}

TEST_CASE("A junction deviation flavor coasts through an unevenly flattened arc that classic jerk brakes into", "[GCodeJunction]")
{
    // The 13.5 mm cylinder whose outer wall showed alternating accelerations in the Actual Speed
    // view. Its uneven turns make classic jerk swing between them; the curvature term does not
    // remove the unevenness, but it stops it from costing most of the wall's speed.
    const std::string gcode = arc_gcode(13.29, DECIMATED);

    const auto jerk      = arc_speeds(make_config(gcfMarlinFirmware, 9., 0.), gcode);
    const auto klipper   = arc_speeds(make_config(gcfKlipper, 9., 0.01), gcode);
    const auto marlin_jd = arc_speeds(make_config(gcfMarlinFirmware, 9., 0.013), gcode);

    CHECK(speed_swing(klipper) < 0.5 * speed_swing(jerk));
    CHECK(speed_swing(marlin_jd) < 0.5 * speed_swing(jerk));
    CHECK(mean_speed(klipper) > mean_speed(jerk));
    CHECK(mean_speed(marlin_jd) > mean_speed(jerk));
}

TEST_CASE("An arc too gentle to need slowing keeps its nominal speed", "[GCodeJunction]")
{
    // sqrt(5000 * 30) = 387 mm/s of curvature headroom, and 2 degree turns are what the default
    // 0.012 mm resolution leaves at this radius, so nothing should touch the nominal 200 mm/s.
    const std::string gcode = arc_gcode(30., {2.});

    const auto klipper   = arc_speeds(make_config(gcfKlipper, 9., 0.01), gcode);
    const auto marlin_jd = arc_speeds(make_config(gcfMarlinFirmware, 9., 0.013), gcode);

    CHECK_THAT(mean_speed(klipper), Catch::Matchers::WithinRel(double(FEEDRATE), 0.01));
    CHECK_THAT(mean_speed(marlin_jd), Catch::Matchers::WithinRel(double(FEEDRATE), 0.01));
    CHECK(speed_swing(klipper) < 0.01);
    CHECK(speed_swing(marlin_jd) < 0.01);
}

TEST_CASE("A classic jerk flavor keeps the legacy junction model", "[GCodeJunction]")
{
    // Bambu, Marlin legacy, RepRap and the rest are untouched by the junction deviation path, so
    // their corners still come from jerk and the BBS centripetal cruise cap still applies.
    const std::string gcode  = arc_gcode(4., {2.});
    const auto        speeds = arc_speeds(make_config(gcfMarlinLegacy, 9., 0.013), gcode);

    // Classic jerk allows 9 / (2 * sin(1 deg)) = 258 mm/s at a 2 degree turn, so the corners do
    // not bind, and what holds the arc below its nominal speed is the centripetal cruise cap.
    CHECK_THAT(mean_speed(speeds), Catch::Matchers::WithinRel(std::sqrt(double(ACCELERATION) * 4.), 0.05));
}
