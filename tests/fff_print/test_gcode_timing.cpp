#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Regression coverage for filament/tool-change time being folded into the first
// pending motion block (an extrusion move) instead of the tool-change move, and
// for that delay being dropped entirely when too few motion blocks precede the
// change. See BambuStudio "seperate flush time from other types" (c54a8333c7)
// and the follow-up "unprocessed addtional time" fix (27ef0b1bef).
namespace {

constexpr size_t NORMAL = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);

FullPrintConfig make_config(double load_time, double unload_time, double tool_change_time)
{
    FullPrintConfig config; // default-initialized with the built-in defaults
    config.gcode_flavor.value = gcfMarlinFirmware;
    // Two filaments, both assigned to the same (single) extruder, so a T1 after
    // T0 is a same-extruder filament swap that costs unload + load time.
    config.filament_diameter.values = {1.75, 1.75};
    config.filament_map.values = {1, 1};
    config.machine_load_filament_time.value = load_time;
    config.machine_unload_filament_time.value = unload_time;
    config.machine_tool_change_time.value = tool_change_time;
    return config;
}

void run_processor(GCodeProcessor& proc, const FullPrintConfig& config, const char* gcode)
{
    // reserved_tag() selects between two tag tables based on this shared static, and
    // other tests in the binary mutate it -- pin it so our "; FEATURE:" role tags are
    // parsed deterministically regardless of test execution order.
    GCodeProcessor::s_IsBBLPrinter = true;
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream os(temp.string());
        os << gcode;
    }
    proc.apply_config(config);
    // No producer marker in the gcode, so process_file keeps our applied config.
    proc.process_file(temp.string());
}

// Estimated time per extrusion role, grouped exactly the way libvgcode builds the
// feature-type legend: sum MoveVertex.time over EMoveType::Extrude moves keyed by
// extrusion_role (see ViewerImpl.cpp:1017 -- only Extrude moves are counted).
std::map<ExtrusionRole, double> role_times(const GCodeProcessorResult& r)
{
    std::map<ExtrusionRole, double> m;
    for (const auto& mv : r.moves)
        if (mv.type == EMoveType::Extrude)
            m[mv.extrusion_role] += mv.time[NORMAL];
    return m;
}

// Sum of estimated time attributed to tool-change moves.
double sum_tool_change_time(const GCodeProcessorResult& r)
{
    double t = 0.0;
    for (const auto& mv : r.moves)
        if (mv.type == EMoveType::Tool_change)
            t += mv.time[NORMAL];
    return t;
}

// Total filament-change delay, accumulated independently of the timing machinery.
double filament_change_delay(const GCodeProcessorResult& r)
{
    const auto& s = r.print_statistics;
    return s.total_filament_load_time + s.total_filament_unload_time + s.total_tool_change_time;
}

} // namespace

TEST_CASE("Filament-change time is attributed to tool-change moves, not extrusion roles", "[GCodeTiming]")
{
    // Relative extrusion (M83) so every "E5" is a real 5mm extrusion move rather
    // than a zero-delta travel. Two real travels precede T0 so its delay is flushed
    // cleanly. The extrusions after T0 span several roles (Outer wall, Sparse infill,
    // Inner wall); the first pending block at T1 is an "Outer wall" move, so the
    // buggy code folds the T1 delay into that role. The per-role check below verifies
    // EVERY role stays clean, not just one, and catches any role-to-role misattribution.
    const char* gcode =
        "M83\n"
        "; FEATURE: Outer wall\n"
        "G1 X10 Y10 Z0.2 F600\n"
        "G1 X0 Y0 F6000\n"
        "T0\n"
        "; FEATURE: Outer wall\n"
        "G1 X50 Y0 E5 F1800\n"
        "G1 X50 Y50 E5\n"
        "; FEATURE: Sparse infill\n"
        "G1 X0 Y50 E5\n"
        "G1 X0 Y0 E5\n"
        "T1\n"
        "; FEATURE: Inner wall\n"
        "G1 X50 Y0 E5\n"
        "G1 X50 Y50 E5\n";

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);
    const GCodeProcessorResult& r_zero = proc_zero.get_result();

    const double load = 10.0;
    const double unload = 5.0;
    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(load, unload, 0.0), gcode);
    const GCodeProcessorResult& r_delay = proc_delay.get_result();

    const double delay = filament_change_delay(r_delay);

    // Preconditions: the filament changes were charged, and cost nothing in the
    // zero-time baseline.
    REQUIRE(delay > 0.0);
    REQUIRE_THAT(filament_change_delay(r_zero), WithinAbs(0.0, 1e-9));

    // The delay must not inflate the time of ANY extrusion role. Compare the full
    // per-role breakdown (exactly how the feature-type legend is built) between the
    // zero-delay and delayed runs -- every role must match to within tolerance.
    const auto roles_zero  = role_times(r_zero);
    const auto roles_delay = role_times(r_delay);
    // Guard: the gcode must genuinely exercise multiple distinct roles (Outer wall,
    // Sparse infill, Inner wall), otherwise this check would silently cover only one.
    REQUIRE(roles_zero.size() >= 3);
    REQUIRE(roles_zero.size() == roles_delay.size());
    for (const auto& [role, zero_time] : roles_zero) {
        INFO("extrusion role index = " << static_cast<int>(role));
        REQUIRE(roles_delay.count(role) == 1);
        REQUIRE_THAT(roles_delay.at(role), WithinAbs(zero_time, 1e-2));
    }

    // The delay must instead land on the tool-change moves, so per-move consumers
    // (layer-time view, layer slider) stay consistent.
    REQUIRE_THAT(sum_tool_change_time(r_delay), WithinAbs(delay, 1e-2));

    // Both tool changes occur on layer 1, so the delay must also be reflected in
    // the first-layer time.
    const double first_layer_delta = proc_delay.get_first_layer_time(PrintEstimatedStatistics::ETimeMode::Normal)
                                   - proc_zero.get_first_layer_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE_THAT(first_layer_delta, WithinAbs(delay, 1e-2));
}

TEST_CASE("Filament-change time is not dropped when few motion blocks precede the change", "[GCodeTiming]")
{
    // Only a single motion block precedes T0, so the buggy code's "fewer than two
    // pending blocks" early-out discards that filament-change delay entirely,
    // making the total print time inconsistent with the reported statistics.
    const char* gcode =
        "; FEATURE: Outer wall\n"
        "G1 X10 Y10 Z0.2 F600\n"
        "T0\n"
        "G1 X50 Y0 E5 F1800\n"
        "G1 X50 Y50 E5\n"
        "T1\n"
        "G1 X0 Y50 E5\n"
        "G1 X0 Y0 E5\n";

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);

    const double load = 10.0;
    const double unload = 5.0;
    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(load, unload, 0.0), gcode);
    const GCodeProcessorResult& r_delay = proc_delay.get_result();

    const double delay = filament_change_delay(r_delay);
    REQUIRE(delay > 0.0);

    // Every second of reported filament-change delay must be present in the total
    // estimated print time; none may be silently dropped.
    const double total_delta = proc_delay.get_time(PrintEstimatedStatistics::ETimeMode::Normal)
                             - proc_zero.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE_THAT(total_delta, WithinAbs(delay, 1e-2));
}

TEST_CASE("Back-to-back tool changes buffer then merge into one tool-change block", "[GCodeTiming]")
{
    // T0 is the very first line: the block queue is empty when its delay is synchronized,
    // so with only the single (artificial) tool-change block queued the delay can't be
    // attributed yet and is buffered. T1 follows immediately with no motion between; its
    // synchronize now sees two tool-change blocks queued, so its own delay joins the buffered
    // T0 entry at application time, the two same-type entries merge into one, and the sum
    // lands entirely on the first tool-change block. The trailing travels leave both runs
    // with >= 2 blocks so their end-of-file flush is identical and cancels in every delta.
    const char* gcode =
        "T0\n"                     // first charged change (load only); empty queue -> buffers (Tool_change,10)
        "T1\n"                     // same-extruder swap (unload+load); merges with buffered T0 entry to (Tool_change,25)
        "G1 X10 Y0 Z0.2 F6000\n"   // travels: keep >= 2 blocks queued at EOF (flushed identically by both runs)
        "G1 X10 Y10\n"
        "G1 X0 Y10\n";

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);
    const GCodeProcessorResult& r_zero = proc_zero.get_result();

    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(10.0, 5.0, 0.0), gcode);
    const GCodeProcessorResult& r_delay = proc_delay.get_result();

    // T0 load 10 + T1 unload 5 + T1 load 10 = 25.
    const double delay = filament_change_delay(r_delay);
    REQUIRE(delay > 0.0);
    REQUIRE_THAT(delay, WithinAbs(25.0, 1e-6));
    REQUIRE_THAT(filament_change_delay(r_zero), WithinAbs(0.0, 1e-9));

    // The whole buffered-then-merged delay must reach the total print time.
    const double total_delta = proc_delay.get_time(PrintEstimatedStatistics::ETimeMode::Normal)
                             - proc_zero.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE_THAT(total_delta, WithinAbs(delay, 1e-2));

    // ...and must land on the tool-change moves, not on any extrusion role.
    REQUIRE_THAT(sum_tool_change_time(r_delay), WithinAbs(25.0, 1e-2));
    REQUIRE_THAT(sum_tool_change_time(r_zero), WithinAbs(0.0, 1e-9));

    // Characterization (documents the current merge-collapse behavior, not a correctness
    // requirement): the two buffered same-type entries combine onto the FIRST artificial
    // tool-change block; the second receives nothing. Had the merge regressed, the 10 and 15
    // would land on separate moves instead of 25 and 0.
    std::vector<double> tc;
    for (const auto& mv : r_delay.moves)
        if (mv.type == EMoveType::Tool_change)
            tc.push_back(mv.time[NORMAL]);
    REQUIRE(tc.size() >= 2);
    REQUIRE_THAT(tc[0], WithinAbs(25.0, 1e-2));
    REQUIRE_THAT(tc[1], WithinAbs(0.0, 1e-9));
}

TEST_CASE("Trailing tool change at end of file is drained, not dropped", "[GCodeTiming]")
{
    // A tool change is the last line of the file, with only its single artificial block
    // queued. Its delay is buffered (fewer than two blocks) and there is no later motion to
    // flush it, so only the finalization pass can attribute it. Without the end-of-file drain
    // the delay would be stranded in the buffer and the total print time would disagree with
    // the reported filament-change statistics.
    const char* gcode =
        "G1 X10 Y0 Z0.2 F6000\n"   // three travels -> three blocks queued (no E, so no filament is selected)
        "G1 X10 Y10\n"
        "G1 X0 Y10\n"
        "G4 S0\n"                  // dwell with S present -> full flush; queue and buffer now empty
        "T0\n";                    // trailing change, nothing after: buffers (Tool_change,10), one block queued

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);
    const GCodeProcessorResult& r_zero = proc_zero.get_result();

    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(10.0, 5.0, 0.0), gcode);
    const GCodeProcessorResult& r_delay = proc_delay.get_result();

    // T0 is the first charged change on an empty extruder, so it costs the load time only.
    const double delay = filament_change_delay(r_delay);
    REQUIRE(delay > 0.0);
    REQUIRE_THAT(delay, WithinAbs(10.0, 1e-6));

    // The trailing change's delay must survive to the total: the zero run buffers nothing and
    // drops its artificial block, so the motion cancels and the delta is exactly the drained delay.
    const double total_delta = proc_delay.get_time(PrintEstimatedStatistics::ETimeMode::Normal)
                             - proc_zero.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE_THAT(total_delta, WithinAbs(delay, 1e-2));

    // The size-1 drain runs the body, so the delay lands on the artificial tool-change move.
    REQUIRE_THAT(sum_tool_change_time(r_delay), WithinAbs(10.0, 1e-2));
    REQUIRE_THAT(sum_tool_change_time(r_zero), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Carried-forward tool-change delay reaches the total without polluting roles", "[GCodeTiming]")
{
    // A wildcard dwell delay is buffered ahead of the tool-change delay, so when the blocks
    // are next flushed the dwell's (Noop) entry consumes the artificial tool-change block and
    // the tool-change entry finds no matching block and carries forward. It stays unmatched
    // through the remaining extrusion moves and is only resolved at finalization, where the
    // end-of-file fold adds it to the machine total and the custom-gcode cache -- never to a
    // move vertex, so it cannot leak into an extrusion role's time.
    const char* gcode =
        "M83\n"
        "G4 S3\n"                        // empty queue -> buffers (Noop,3) [wildcard delay]
        "T0\n"                           // one block queued -> buffers (Tool_change,10) behind the dwell
        "; FEATURE: Inner wall\n"
        "G1 X20 Y0 Z0.2 E5 F1800\n"      // extrusion m1: queue is [artificial_TC0, m1]
        "G4 S0\n"                        // flush: (Noop,3) consumes artificial_TC0; (Tool_change,10) carries forward
        "G1 X20 Y20 E5\n"                // extrusion m2
        "G1 X0 Y20 E5\n";                // extrusion m3: at EOF queue is [m2, m3], buffer is [(Tool_change,10)]

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);
    const GCodeProcessorResult& r_zero = proc_zero.get_result();

    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(10.0, 5.0, 0.0), gcode);
    const GCodeProcessorResult& r_delay = proc_delay.get_result();

    // T0 is the first charged change (load only); the fixed dwell delays are not in these counters.
    const double delay = filament_change_delay(r_delay);
    REQUIRE(delay > 0.0);
    REQUIRE_THAT(delay, WithinAbs(10.0, 1e-6));

    // The stranded tool-change delay must be drained into the total, not dropped. The 3s dwell
    // is identical in both runs and cancels along with all motion, leaving exactly the delay.
    const double total_delta = proc_delay.get_time(PrintEstimatedStatistics::ETimeMode::Normal)
                             - proc_zero.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE_THAT(total_delta, WithinAbs(delay, 1e-2));

    // Pollution safety: the drained delay must NOT appear in any extrusion role. Every role's
    // time must match between the zero and delayed runs -- this is what the total-only fold buys.
    const auto rz = role_times(r_zero);
    const auto rd = role_times(r_delay);
    REQUIRE(rz.size() >= 1);
    REQUIRE(rz.size() == rd.size());
    for (const auto& [role, zero_time] : rz) {
        INFO("extrusion role index = " << static_cast<int>(role));
        REQUIRE(rd.count(role) == 1);
        REQUIRE_THAT(rd.at(role), WithinAbs(zero_time, 1e-2));
    }
}

TEST_CASE("Per-slot machine limits follow the active nozzle", "[GCodeTiming][MultiNozzle]")
{
    // Single physical extruder carrying two nozzle variants: machine slot 0 (Standard) caps X/Y
    // speed at 200 mm/s, slot 1 (High Flow) at 50 mm/s. The estimator must clamp each move by the
    // slot of the nozzle the active filament occupies -- resolved from the grouping context handed
    // over before the replay plus the occupancy recorder, i.e. the exact in-slicer streaming path.
    FullPrintConfig config = make_config(0.0, 0.0, 0.0);
    config.extruder_type.values            = {static_cast<int>(etDirectDrive)};
    config.printer_extruder_id.values      = {1, 1};
    config.printer_extruder_variant.values = {"Direct Drive Standard", "Direct Drive High Flow"};
    // Slot-major layout: [slot0-Normal, slot0-Stealth, slot1-Normal, slot1-Stealth].
    config.machine_max_speed_x.values = {200., 200., 50., 50.};
    config.machine_max_speed_y.values = {200., 200., 50., 50.};
    config.machine_max_speed_z.values = {200., 200., 50., 50.};
    config.machine_max_speed_e.values = {200., 200., 50., 50.};
    // Keep acceleration and jerk far from limiting so move times are speed-dominated.
    for (auto *accel : {&config.machine_max_acceleration_x, &config.machine_max_acceleration_y,
                        &config.machine_max_acceleration_z, &config.machine_max_acceleration_e})
        accel->values = {100000., 100000., 100000., 100000.};
    config.machine_max_acceleration_travel.values    = {100000., 100000.};
    config.machine_max_acceleration_extruding.values = {100000., 100000.};
    config.machine_max_jerk_x.values = {10000., 10000.};
    config.machine_max_jerk_y.values = {10000., 10000.};
    config.machine_max_jerk_z.values = {10000., 10000.};
    config.machine_max_jerk_e.values = {10000., 10000.};

    // Grouping stub: filament 0 lives on the Standard nozzle (slot 0), filament 1 on the
    // High Flow nozzle (slot 1), both mounted on extruder 0.
    std::vector<MultiNozzleUtils::NozzleInfo> nozzles;
    {
        MultiNozzleUtils::NozzleInfo n;
        n.diameter = "0.4";
        n.volume_type = nvtStandard; n.extruder_id = 0; n.group_id = 0; nozzles.push_back(n);
        n.volume_type = nvtHighFlow; n.extruder_id = 0; n.group_id = 1; nozzles.push_back(n);
    }
    std::vector<int>          filament_nozzle_map = {0, 1};
    std::vector<unsigned int> used_filaments      = {0, 1};
    auto group = MultiNozzleUtils::LayeredNozzleGroupResult::create(filament_nozzle_map, nozzles, used_filaments);
    REQUIRE(group.has_value());
    auto context = std::make_shared<MultiNozzleUtils::LayeredNozzleGroupResult>(*group);

    // Two identical 100 mm X travels, one per filament; T..H.. carries the target nozzle id.
    // The trailing 1 mm move keeps two blocks queued at finalize, so the measured move's time is
    // flushed (a lone final block is never attributed); it adds 1 mm to the second bucket.
    const char* gcode =
        "M83\n"
        "T0 H0\n"
        "G1 X100 F30000\n"
        "T1 H1\n"
        "G1 X0 F30000\n"
        "G1 X1 F30000\n";

    // Travel time accumulated after each tool-change move (bucket 0 = before any T).
    auto travel_times_by_tool = [](const GCodeProcessorResult& r) {
        std::vector<double> out(1, 0.0);
        for (const auto& mv : r.moves) {
            if (mv.type == EMoveType::Tool_change)
                out.push_back(0.0);
            else if (mv.type == EMoveType::Travel)
                out.back() += mv.time[NORMAL];
        }
        return out;
    };

    SECTION("the move on the High Flow nozzle is clamped by its own slot") {
        GCodeProcessor proc;
        proc.initialize_from_context(context);
        run_processor(proc, config, gcode);
        auto times = travel_times_by_tool(proc.get_result());
        REQUIRE(times.size() == 3);
        REQUIRE_THAT(times[1], Catch::Matchers::WithinRel(100.0 / 200.0, 0.10));
        REQUIRE_THAT(times[2], Catch::Matchers::WithinRel(101.0 / 50.0, 0.10));
    }
    SECTION("an emitted envelope line reaches every slot") {
        const std::string enveloped = std::string("M201 X20000\nM203 X80\n") + gcode;
        GCodeProcessor proc;
        proc.initialize_from_context(context);
        run_processor(proc, config, enveloped.c_str());
        auto times = travel_times_by_tool(proc.get_result());
        REQUIRE(times.size() == 3);
        REQUIRE_THAT(times[1], Catch::Matchers::WithinRel(100.0 / 80.0, 0.10));
        REQUIRE_THAT(times[2], Catch::Matchers::WithinRel(101.0 / 80.0, 0.10));
    }
    SECTION("no grouping context degrades to slot 0") {
        GCodeProcessor proc;
        run_processor(proc, config, gcode);
        auto times = travel_times_by_tool(proc.get_result());
        REQUIRE(times.size() == 3);
        REQUIRE_THAT(times[1], Catch::Matchers::WithinRel(100.0 / 200.0, 0.10));
        REQUIRE_THAT(times[2], Catch::Matchers::WithinRel(101.0 / 200.0, 0.10));
    }
}

// Junction planning decides the speeds the "actual speed" / "actual flow" preview shows. Per-axis
// jerk limits a corner by the largest single-axis component of the velocity change, allowing sqrt(2)
// more speed on a diagonal than on an axis -- a four-lobed ripple around every circle. Klipper and
// Marlin 2 with M205 J plan with junction deviation instead, which sees only the corner angle.
namespace {

// One acceleration everywhere and axis limits far above it, so only the junction model under test
// can slow a corner down.
FullPrintConfig make_junction_config(GCodeFlavor flavor, double corner_velocity, double junction_deviation)
{
    FullPrintConfig config;
    config.gcode_flavor.value = flavor;
    config.filament_diameter.values = {1.75};
    config.filament_map.values = {1};

    const std::vector<double> accel   = {1000.0, 1000.0};
    const std::vector<double> axis    = {20000.0, 20000.0};
    const std::vector<double> speed   = {500.0, 500.0};
    config.machine_max_acceleration_extruding.values = accel;
    config.machine_max_acceleration_travel.values    = accel;
    config.machine_max_acceleration_retracting.values = accel;
    config.machine_max_acceleration_x.values = axis;
    config.machine_max_acceleration_y.values = axis;
    config.machine_max_acceleration_z.values = axis;
    config.machine_max_acceleration_e.values = axis;
    config.machine_max_speed_x.values = speed;
    config.machine_max_speed_y.values = speed;
    config.machine_max_speed_z.values = speed;
    config.machine_max_speed_e.values = speed;
    // Klipper reads this as the square corner velocity, Marlin as classic jerk.
    config.machine_max_jerk_x.values = {corner_velocity, corner_velocity};
    config.machine_max_jerk_y.values = {corner_velocity, corner_velocity};
    config.machine_max_jerk_z.values = {corner_velocity, corner_velocity};
    // Kept out of the way so it never binds in the classic-jerk comparisons.
    config.machine_max_jerk_e.values = {100.0, 100.0};
    config.machine_max_junction_deviation.values = {junction_deviation, junction_deviation};
    config.machine_min_extruding_rate.values = {0.0, 0.0};
    config.machine_min_travel_rate.values    = {0.0, 0.0};
    return config;
}

constexpr double junction_x = 60.0;
constexpr double junction_y = 60.0;

// Two 40mm moves meeting at (junction_x, junction_y) with the given turn, rotated by `orientation`.
// 40mm is long enough to reach the commanded 150mm/s and brake back to any corner speed these tests
// produce. `e_per_mm` of zero makes them travels, which keeps the junction vector purely geometric
// as the formulas below assume.
std::string corner_gcode(double turn_deg, double orientation_deg, double e_per_mm = 0.0)
{
    const double len   = 40.0;
    const double a_in  = orientation_deg * M_PI / 180.0;
    const double a_out = (orientation_deg + turn_deg) * M_PI / 180.0;
    std::ostringstream extrude;
    if (e_per_mm > 0.0)
        extrude << std::fixed << std::setprecision(4) << " E" << len * e_per_mm;

    std::ostringstream os;
    os << std::fixed << std::setprecision(4)
       << "M83\n"
       << "G1 Z0.2 F1200\n"
       << "G1 X" << junction_x - len * std::cos(a_in)  << " Y" << junction_y - len * std::sin(a_in)  << " F6000\n"
       << "G1 X" << junction_x                         << " Y" << junction_y   << extrude.str()      << " F9000\n"
       << "G1 X" << junction_x + len * std::cos(a_out) << " Y" << junction_y + len * std::sin(a_out)
       << extrude.str() << " F9000\n";
    return os.str();
}

// Speed allowed through the corner: the vertex ending the incoming move carries that block's exit
// speed, and the vertices the actual-speed pass inserts are all strictly interior.
double corner_speed(const GCodeProcessorResult& r)
{
    for (const auto& mv : r.moves)
        if ((mv.type == EMoveType::Travel || mv.type == EMoveType::Extrude) &&
            std::abs(mv.position.x() - junction_x) < 1e-3 &&
            std::abs(mv.position.y() - junction_y) < 1e-3)
            return mv.actual_feedrate;
    return -1.0;
}

double planned_corner_speed(GCodeFlavor flavor, double corner_velocity, double junction_deviation,
                            double turn_deg, double orientation_deg = 0.0, double e_per_mm = 0.0)
{
    GCodeProcessor proc;
    run_processor(proc, make_junction_config(flavor, corner_velocity, junction_deviation),
                  corner_gcode(turn_deg, orientation_deg, e_per_mm).c_str());
    return corner_speed(proc.get_result());
}

} // namespace

TEST_CASE("Klipper extrusion ratio changes use its own corner velocity, not E jerk", "[GCodeTiming][JunctionDeviation]")
{
    const double jerk_e = GENERATE(0.0, 100.0);
    auto config = make_junction_config(gcfKlipper, 5.0, 0.0);
    config.machine_max_jerk_e.values = {jerk_e, jerk_e};
    GCodeProcessor processor;
    run_processor(processor, config,
        "G90\nM83\nSET_VELOCITY_LIMIT ACCEL=1000 MINIMUM_CRUISE_RATIO=0\n"
        "G1 X20 Y60 F6000\nM400\n"
        "G1 X60 Y60 E0.8 F6000\nG1 X100 Y60 E1.6 F6000\nM400\n");
    // A 0.02 -> 0.04 extrusion ratio change is capped at 1 / 0.02 = 50 mm/s.
    REQUIRE_THAT(corner_speed(processor.get_result()), Catch::Matchers::WithinAbs(50.0, 0.01));
}

TEST_CASE("Klipper legacy smoothing stays absolute across acceleration changes", "[GCodeTiming][Klipper]")
{
    const bool modern = GENERATE(false, true);
    auto config = make_junction_config(gcfKlipper, 5.0, 0.0);
    const std::string gcode = std::string("G90\nSET_VELOCITY_LIMIT ACCEL=1000 ") +
        (modern ? "MINIMUM_CRUISE_RATIO=0.75\n" : "ACCEL_TO_DECEL=250\n") +
        "G1 X40 F30000\nM400\nM204 S2000\nG1 X0\nM400\n";
    GCodeProcessor processor;
    run_processor(processor, config, gcode.c_str());
    const double expected = modern ? 0.5 + 0.25 * std::sqrt(2.0) : 0.95;
    REQUIRE_THAT(processor.get_time(PrintEstimatedStatistics::ETimeMode::Normal), WithinAbs(expected, 1e-5));
}

TEST_CASE("Klipper corners are planned with junction deviation derived from the square corner velocity",
          "[GCodeTiming][JunctionDeviation]")
{
    // jd = scv^2 * (sqrt(2) - 1) / max_accel, then v^2 = jd * accel * sin(t/2) / (1 - sin(t/2)).
    // The acceleration cancels: the corner speed depends only on the scv and the angle.
    const double scv = 5.0;

    SECTION("a right angle is taken at exactly the square corner velocity") {
        // sin(t/2) = sqrt(0.5) at 90 degrees, so v == scv -- the definition of the square corner
        // velocity, and what makes the mapping above the right one.
        REQUIRE_THAT(planned_corner_speed(gcfKlipper, scv, 0.0, 90.0), Catch::Matchers::WithinRel(scv, 0.02));
    }

    SECTION("a shallow corner is taken far faster than the per-axis jerk model allows") {
        // 6 degrees: sin(t/2) = cos(3 deg), so v = 5 * sqrt((sqrt(2) - 1) * 728.68) = 86.9mm/s. Per-axis
        // jerk ignores the angle and caps the velocity *change* (2v*sin(3 deg)), giving 47.8mm/s.
        const double jd_speed     = planned_corner_speed(gcfKlipper, scv, 0.0, 6.0);
        const double jerk_speed   = planned_corner_speed(gcfMarlinLegacy, scv, 0.0, 6.0);
        REQUIRE_THAT(jd_speed, Catch::Matchers::WithinRel(86.87, 0.02));
        REQUIRE_THAT(jerk_speed, Catch::Matchers::WithinRel(47.75, 0.02));
    }
}

TEST_CASE("Junction deviation limits a corner by its angle alone, not by its orientation",
          "[GCodeTiming][JunctionDeviation]")
{
    // The four-lobed ripple on circular walls is per-axis jerk being anisotropic: a velocity change
    // lying on an axis gets sqrt(2) less headroom than the same change on the diagonal.
    const double scv = 5.0;
    const double turn = 6.0;

    SECTION("Klipper plans both orientations identically") {
        const double on_axis  = planned_corner_speed(gcfKlipper, scv, 0.0, turn, 0.0);
        const double diagonal = planned_corner_speed(gcfKlipper, scv, 0.0, turn, 45.0);
        REQUIRE(on_axis > 0.0);
        REQUIRE_THAT(diagonal, Catch::Matchers::WithinRel(on_axis, 0.02));
    }

    SECTION("the classic jerk model keeps its orientation dependence") {
        const double on_axis  = planned_corner_speed(gcfMarlinLegacy, scv, 0.0, turn, 0.0);
        const double diagonal = planned_corner_speed(gcfMarlinLegacy, scv, 0.0, turn, 45.0);
        REQUIRE(on_axis > 0.0);
        REQUIRE(diagonal / on_axis > 1.2);
    }
}

TEST_CASE("Junction deviation is only used where the firmware actually plans with it",
          "[GCodeTiming][JunctionDeviation]")
{
    const double jerk = 5.0;

    SECTION("Marlin 2 with M205 J disabled keeps the classic jerk planning") {
        // machine_max_junction_deviation == 0 is how a Marlin 2 printer says it runs classic jerk.
        const double classic = planned_corner_speed(gcfMarlinLegacy, jerk, 0.0, 90.0);
        REQUIRE(classic > 0.0);
        REQUIRE_THAT(planned_corner_speed(gcfMarlinFirmware, jerk, 0.0, 90.0),
                     Catch::Matchers::WithinRel(classic, 1e-4));
    }

    SECTION("Marlin 2 with M205 J enabled switches to junction deviation") {
        // sqrt(1000 * 0.05 * 2.4142136) = 11.0mm/s, independent of the jerk values it no longer reads.
        REQUIRE_THAT(planned_corner_speed(gcfMarlinFirmware, jerk, 0.05, 90.0),
                     Catch::Matchers::WithinRel(10.99, 0.02));
    }

    SECTION("machines without junction deviation are untouched by the jerk values it would ignore") {
        // A flavor that never enters the junction deviation path must ignore the setting entirely.
        const double without = planned_corner_speed(gcfMarlinLegacy, jerk, 0.0, 90.0);
        REQUIRE_THAT(planned_corner_speed(gcfMarlinLegacy, jerk, 0.05, 90.0),
                     Catch::Matchers::WithinRel(without, 1e-4));
    }
}

TEST_CASE("How fast a corner is taken does not depend on how much is extruded through it",
          "[GCodeTiming][JunctionDeviation]")
{
    // The junction cosine is taken over XYZE, so the direction vectors have to be unit length or the
    // E term makes the two paths look more parallel than they are and the corner comes out too fast,
    // the more so the higher the flow. Marlin normalizes over XYZE on any extruding move
    // (planner.cpp, esteps > 0) and Klipper leaves E out of the cosine altogether
    // (toolhead.py::Move.calc_junction); on both, this corner is planned by its geometry alone.
    const double scv  = 5.0;
    const double turn = 6.0;
    const double geometric = planned_corner_speed(gcfKlipper, scv, 0.0, turn);
    REQUIRE(geometric > 0.0);

    // 0.029mm/mm is an ordinary 0.42 x 0.2 line on 1.75mm filament; 0.1 is a fat large-nozzle one.
    // Unnormalized these came out at 94.4 and 150.0mm/s against a geometric 86.9.
    for (double e_per_mm : {0.029, 0.1})
        REQUIRE_THAT(planned_corner_speed(gcfKlipper, scv, 0.0, turn, 0.0, e_per_mm),
                     Catch::Matchers::WithinRel(geometric, 0.02));

    SECTION("and the same holds on Marlin 2") {
        const double marlin = planned_corner_speed(gcfMarlinFirmware, scv, 0.05, turn);
        REQUIRE(marlin > 0.0);
        REQUIRE_THAT(planned_corner_speed(gcfMarlinFirmware, scv, 0.05, turn, 0.0, 0.029),
                     Catch::Matchers::WithinRel(marlin, 0.02));
    }
}
