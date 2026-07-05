#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <fstream>
#include <map>

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
