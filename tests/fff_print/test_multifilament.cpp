#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// 0-based tool indices used by extrusions whose role comment contains `role` (needs gcode_comments).
static std::set<int> tools_for_role(const std::string& gcode, const std::string& role)
{
    std::set<int> tools;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char)cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && std::string(line.comment()).find(role) != std::string::npos)
            tools.insert(current_tool);
    });
    return tools;
}

// X where the nozzle sits while each tagged _WAIT_FOR_TEMP_ON_WIPE_TOWER M109 blocks:
// the nearest preceding G1 carrying an X (the park travel emitted just before the wait).
static std::vector<double> wait_park_xs(const std::string& gcode)
{
    std::vector<std::string> lines;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);)
        lines.emplace_back(std::move(line));
    std::vector<double> xs;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("M109", 0) != 0 || lines[i].find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos)
            continue;
        for (size_t j = i; j-- > 0;) {
            if (lines[j].rfind("G1 ", 0) != 0)
                continue;
            const size_t x_pos = lines[j].find('X');
            if (x_pos == std::string::npos)
                continue;
            xs.push_back(std::stod(lines[j].substr(x_pos + 1)));
            break;
        }
    }
    return xs;
}

// Estimated print time at each 1-based line of an exported G-code file, from a second
// GCodeProcessor pass over it. MoveVertex::time is the duration of one move and gcode_id is the
// line it came from (already rebased past the M73 insertions), so the running sum before the first
// move of a line is the elapsed time at that line. The file carries its own config footer, so
// process_file configures the processor -- including the shared s_IsBBLPrinter static that other
// tests in this binary mutate -- from the settings the export itself used.
static std::vector<double> elapsed_time_by_line(const std::string& gcode)
{
    ScopedTemporaryFile temp_gcode(".gcode");
    {
        std::ofstream os(temp_gcode.string());
        os << gcode;
    }
    GCodeProcessor processor;
    processor.process_file(temp_gcode.string());

    constexpr size_t    NORMAL  = size_t(PrintEstimatedStatistics::ETimeMode::Normal);
    const size_t        n_lines = size_t(std::count(gcode.begin(), gcode.end(), '\n')) + 2;
    std::vector<double> elapsed(n_lines, 0.);
    double              running = 0.;
    size_t              next    = 0;
    for (const auto& move : processor.get_result().moves) {
        const size_t id = std::min<size_t>(move.gcode_id, n_lines - 1);
        while (next <= id)
            elapsed[next++] = running;
        running += move.time[NORMAL];
    }
    while (next < n_lines)
        elapsed[next++] = running;
    return elapsed;
}

// The temperature-relevant projection of `gcode`: every M104/M109/Tn line, plus the toolchange and
// priming markers that anchor them, in order. A preheat -- an M104 the GCodeProcessor backtrace
// inserts mid-object, outside any block, naming a tool other than the one currently loaded -- also
// carries "lead <n>s", the estimated time from there to the tool change it heats for, which is the
// property preheat_time controls. No other temperature command gets one: for an M104 retargeting
// the active tool (the first-layer-to-other-layers bump) or one inside a block, the distance to the
// next Tn is a layer time or a handful of moves and says nothing about preheat_time. Everything
// else is dropped, so the trace does not move when travel, tower geometry or line numbering do.
static std::vector<std::string> temperature_trace(const std::string& gcode)
{
    std::vector<std::string> lines;
    std::istringstream       stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        line.erase(0, line.find_first_not_of(" \t"));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        lines.emplace_back(std::move(line));
    }
    const std::vector<double> elapsed = elapsed_time_by_line(gcode);

    const auto is_tool = [](const std::string& l) { return l.size() >= 2 && l[0] == 'T' && std::isdigit((unsigned char) l[1]); };
    const auto is_temp = [](const std::string& l) { return l.rfind("M104", 0) == 0 || l.rfind("M109", 0) == 0; };
    const auto marker  = [](const std::string& l) -> const char* {
        for (const char* m : { "; CP TOOLCHANGE START", "; CP TOOLCHANGE END", "; CP PRIMING START", "; CP PRIMING END" })
            if (l.find(m) != std::string::npos)
                return m;
        return nullptr;
    };

    // Tool a "T<n>" line, or the "T<n>" argument of an M104, names -- or -1 when it names none.
    const auto tool_of = [&is_tool](const std::string& l) -> int {
        size_t t = std::string::npos; // index of the 'T'
        if (is_tool(l))
            t = 0;
        else if (l.rfind("M104", 0) == 0 && l.find(" T") != std::string::npos)
            t = l.find(" T") + 1;
        if (t == std::string::npos || t + 1 >= l.size() || !std::isdigit((unsigned char) l[t + 1]))
            return -1;
        return std::stoi(l.substr(t + 1));
    };

    std::vector<std::string> trace;
    bool                     in_block     = false;
    int                      current_tool = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (const char* m = marker(lines[i])) {
            in_block = std::string(m).find("START") != std::string::npos;
            trace.emplace_back(m); // the marker alone: some carry a trailing tool id, some do not
        } else if (is_tool(lines[i]) || is_temp(lines[i])) {
            std::string entry = lines[i];
            const int   named = tool_of(lines[i]);
            if (!in_block && lines[i].rfind("M104", 0) == 0 && current_tool != -1 && named != -1 && named != current_tool) {
                size_t tn = i;
                while (tn < lines.size() && !is_tool(lines[tn]))
                    ++tn;
                if (tn < lines.size()) {
                    char lead[32];
                    std::snprintf(lead, sizeof(lead), "\tlead %.1fs", elapsed[tn + 1] - elapsed[i + 1]);
                    entry += lead;
                }
            }
            if (is_tool(lines[i]))
                current_tool = named;
            trace.emplace_back(std::move(entry));
        }
    }
    return trace;
}

// "M104 S240 T0 ; preheat T0 time: 31s<TAB>lead 30.9s" carries the same quantity twice, and both
// vary by toolchain: the backtrace picks the first line at least preheat_time out, so a sub-tenth
// difference in the estimate selects a neighbouring move and "lead" steps by that move's duration.
// Tolerate "lead", still far below the tens of seconds a displaced preheat would shift it. Check
// "time:" against its own entry's "lead" instead of across runs -- being a rounding of it, that
// still catches a change in how it is derived without tracking the absolute estimate.
static constexpr double TRACE_TIME_TOLERANCE_S = 1.5;
static constexpr double TRACE_ROUNDING_SLACK_S = 0.05; // correct rounding keeps |time - lead| <= 0.5

struct TraceEntry
{
    std::string           text;   // timing values replaced by a placeholder
    std::optional<double> time_s;
    std::optional<double> lead_s;
};

static TraceEntry parse_trace_entry(const std::string& entry)
{
    TraceEntry out;
    std::string text = entry;

    // Split off the tail only when it really is a "lead <n>s", so an unexpected one still compares.
    const size_t tab = text.find('\t');
    if (tab != std::string::npos) {
        const std::string tail = text.substr(tab + 1); // "lead 30.2s"
        const size_t      sp   = tail.find(' ');
        if (sp != std::string::npos && sp + 1 < tail.size()
            && std::isdigit(static_cast<unsigned char>(tail[sp + 1]))) {
            out.lead_s = std::stod(tail.substr(sp + 1));
            text.erase(tab);
        }
    }

    static constexpr std::string_view k_time = "time: ";
    const size_t                      at     = text.find(k_time);
    // Require a digit first: a dots-only run would otherwise reach std::stod and throw.
    if (at != std::string::npos && at + k_time.size() < text.size()
        && std::isdigit(static_cast<unsigned char>(text[at + k_time.size()]))) {
        const size_t first = at + k_time.size();
        size_t       last  = first;
        while (last < text.size() && (std::isdigit(static_cast<unsigned char>(text[last])) || text[last] == '.'))
            ++last;
        out.time_s = std::stod(text.substr(first, last - first));
        text.replace(first, last - first, "<n>"); // surrounding text, incl. the "s", still compared
    }

    out.text = std::move(text);
    return out;
}

static bool timings_match(const std::optional<double>& a, const std::optional<double>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    return !a.has_value() || std::abs(*a - *b) <= TRACE_TIME_TOLERANCE_S;
}

// "time:" must be its own entry's "lead" rounded to a whole second.
static bool time_is_rounded_lead(const TraceEntry& e)
{
    if (!e.time_s.has_value() || !e.lead_s.has_value())
        return true; // nothing to cross-check
    return std::abs(*e.time_s - *e.lead_s) <= 0.5 + TRACE_ROUNDING_SLACK_S;
}

// `a` is the slice under test, `b` the recorded golden.
static bool trace_entries_match(const std::string& a, const std::string& b)
{
    const auto x = parse_trace_entry(a);
    const auto y = parse_trace_entry(b);
    if (x.text != y.text)
        return false;
    // A field appearing or disappearing is a real change even though the values are tolerated.
    if (x.time_s.has_value() != y.time_s.has_value())
        return false;
    return timings_match(x.lead_s, y.lead_s) && time_is_rounded_lead(x);
}

// Tool index = filament id - 1; brim and skirt follow the wall filament.
TEST_CASE("Each feature prints with its assigned filament", "[MultiFilament]")
{
    auto [infill_filament, wall_filament] = GENERATE(table<int, int>({ {1, 1}, {1, 2}, {2, 1}, {2, 2} }));
    DYNAMIC_SECTION("infill filament " << infill_filament << ", wall filament " << wall_filament) {
        const std::string gcode = slice({ cube(20) },
            multifilament_config(2, {
                { "sparse_infill_filament_id",  infill_filament },
                { "internal_solid_filament_id", infill_filament },
                { "top_surface_filament_id",    infill_filament },
                { "bottom_surface_filament_id", infill_filament },
                { "outer_wall_filament_id",     wall_filament },
                { "inner_wall_filament_id",     wall_filament },
                { "skirt_loops",                1 },
                { "brim_type",                  "outer_only" },
                { "brim_width",                 5 },
            }));
        const std::set<int> wall_tool{ wall_filament - 1 };
        const std::set<int> infill_tool{ infill_filament - 1 };
        CHECK(tools_for_role(gcode, "perimeter") == wall_tool);
        CHECK(tools_for_role(gcode, "infill")    == infill_tool); // sparse + solid + top/bottom
        CHECK(tools_for_role(gcode, "brim")      == wall_tool);
        CHECK(tools_for_role(gcode, "skirt")     == wall_tool);
    }
}

TEST_CASE("Each feature prints with its assigned filament (three filaments)", "[MultiFilament]")
{
    const std::string gcode = slice({ cube(20) },
        multifilament_config(3, {
            { "sparse_infill_filament_id",  2 },
            { "internal_solid_filament_id", 2 },
            { "top_surface_filament_id",    2 },
            { "bottom_surface_filament_id", 2 },
            { "outer_wall_filament_id",     3 },
            { "inner_wall_filament_id",     3 },
            { "skirt_loops",                0 },
            { "brim_type",                  "no_brim" },
        }));
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 2 }); // filament 3
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 1 }); // filament 2
}

// The override must survive tool ordering: object 1's walls print on their filament's
// tool, object 0 stays on the first. If dropped, every wall prints on tool 0.
TEST_CASE("Per-object wall filament override is honored", "[MultiFilament]")
{
    const std::string gcode = slice_with_object_overrides(
        { cube(20), cube(20) },
        multifilament_config(2, {
            { "skirt_loops",    0 },
            { "brim_type",      "no_brim" },
            { "print_sequence", "by object" },
        }),
        { {}, { { "outer_wall_filament_id", 2 }, { "inner_wall_filament_id", 2 } } });
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 0, 1 });
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 0 }); // infill not overridden: stays on F1
}

// With wait_for_temp_on_wipe_tower the blocking M109 moves from right after the Tn command to
// a stop point parked beside the wipe tower (heat-up drool falls next to the tower, not onto
// its top): tagged with _WAIT_FOR_TEMP_ON_WIPE_TOWER, after the toolchange and before the
// repositioning move and the first extrusion of the purge. The restore that used to block there
// demotes to a non-blocking M104 and moves ahead of the Tn, so the incoming tool heats up over
// the change itself. Ordering and the off-tower stop are the contract here.
TEST_CASE("Toolchange temperature wait moves to the wipe tower when enabled", "[MultiFilament]")
{
    const bool wait_on_tower = GENERATE(false, true);
    DYNAMIC_SECTION("wait_for_temp_on_wipe_tower " << (wait_on_tower ? 1 : 0)) {
        const std::string gcode = slice_with_object_overrides(
            { cube(20), cube(20) },
            multifilament_config(2, {
                { "nozzle_diameter",                "0.4,0.4" },
                { "printer_extruder_id",            "1,2" },
                { "printer_extruder_variant",       "Direct Drive Standard,Direct Drive Standard" },
                { "extruder_printable_height",      "0,0" },
                { "single_extruder_multi_material", 0 },
                { "enable_prime_tower",             1 },
                { "prime_tower_width",              35 },
                { "wipe_tower_x",                   "50" },
                { "wipe_tower_y",                   "50" },
                { "ooze_prevention",                1 },
                { "standby_temperature_delta",      -40 },
                // The post-processor's own preheat pass also inserts an M104 for the incoming
                // filament ahead of the Tn; switch it off so the temperature commands under test
                // are the only ones in the toolchange block.
                { "preheat_time",                   0 },
                { "wait_for_temp_on_wipe_tower",    wait_on_tower ? 1 : 0 },
            }),
            // One filament per object -> a toolchange on every layer. Assigned at the object
            // level: the used-filament count that gates the prime tower is derived from
            // object/volume configs on the harness's single apply (region filament ids such
            // as sparse_infill_filament_id are not counted there and the tower would be
            // silently disabled).
            { { { "extruder", 1 } }, { { "extruder", 2 } } });

        // Split into lines and scan the "; CP TOOLCHANGE START".."; CP TOOLCHANGE END" blocks.
        std::vector<std::string> lines;
        std::istringstream gcode_stream(gcode);
        for (std::string line; std::getline(gcode_stream, line);)
            lines.emplace_back(std::move(line));
        const auto is_tool_line   = [](const std::string& l) { return l.size() >= 2 && l[0] == 'T' && std::isdigit((unsigned char)l[1]); };
        const auto is_m109_line   = [](const std::string& l) { return l.rfind("M109", 0) == 0; };
        // A non-blocking set-temperature naming one specific tool, e.g. "M104 S255 T1".
        const auto is_m104_for_tool = [](const std::string& l, int tool) {
            if (l.rfind("M104", 0) != 0)
                return false;
            const std::string token = " T" + std::to_string(tool);
            const size_t      at    = l.find(token);
            return at != std::string::npos && !std::isdigit((unsigned char)l[at + token.size()]);
        };
        const auto is_tagged_wait = [](const std::string& l) { return l.find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") != std::string::npos; };
        const auto is_extruding   = [](const std::string& l) {
            if (l.rfind("G1 ", 0) != 0)
                return false;
            const size_t e = l.find(" E");
            return e != std::string::npos && l.find_first_of("XY") != std::string::npos && l[e + 2] != '-';
        };

        int checked_blocks = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("; CP TOOLCHANGE START") == std::string::npos)
                continue;
            size_t block_end = i;
            while (block_end < lines.size() && lines[block_end].find("; CP TOOLCHANGE END") == std::string::npos)
                ++block_end;
            size_t tool_line = block_end;
            for (size_t j = i; j < block_end; ++j)
                if (is_tool_line(lines[j])) { tool_line = j; break; }
            if (tool_line == block_end)
                continue; // final unload block, no toolchange
            ++checked_blocks;

            // Where the incoming tool's target temperature is raised, relative to its Tn.
            const int new_tool = std::stoi(lines[tool_line].substr(1));
            size_t    preheat = tool_line, restore = block_end;
            for (size_t j = i; j < tool_line; ++j)
                if (is_m104_for_tool(lines[j], new_tool)) { preheat = j; break; }
            for (size_t j = tool_line + 1; j < block_end; ++j)
                if (is_m104_for_tool(lines[j], new_tool)) { restore = j; break; }

            size_t tagged_wait = block_end, untagged_m109 = block_end, first_extrusion = block_end;
            for (size_t j = tool_line + 1; j < block_end; ++j) {
                if (is_m109_line(lines[j]) && tagged_wait == block_end && is_tagged_wait(lines[j]))
                    tagged_wait = j;
                if (is_m109_line(lines[j]) && untagged_m109 == block_end && !is_tagged_wait(lines[j]))
                    untagged_m109 = j;
                if (first_extrusion == block_end && is_extruding(lines[j]))
                    first_extrusion = j;
            }
            INFO("toolchange block at line " << i + 1);
            if (wait_on_tower) {
                // The only blocking wait is the tagged one, parked beside the tower before the purge.
                REQUIRE(tagged_wait < block_end);
                CHECK(untagged_m109 == block_end);
                // The target is raised ahead of the toolchange, so the incoming tool heats up
                // while it is picked up, and nothing sets it again afterwards.
                CHECK(preheat < tool_line);
                CHECK(restore == block_end);
                REQUIRE(first_extrusion < block_end);
                CHECK(tagged_wait < first_extrusion);
                // The travel preceding the wait parks outside the tower footprint. The tower
                // auto-sizes, so derive its extent from the purge extrusions of this block.
                size_t stop_line = block_end;
                for (size_t j = tagged_wait; j-- > tool_line;)
                    if (lines[j].rfind("G1 ", 0) == 0 && lines[j].find('X') != std::string::npos) { stop_line = j; break; }
                REQUIRE(stop_line < block_end);
                const double stop_x = std::stod(lines[stop_line].substr(lines[stop_line].find('X') + 1));
                double purge_min_x = std::numeric_limits<double>::max(), purge_max_x = std::numeric_limits<double>::lowest();
                for (size_t j = tagged_wait; j < block_end; ++j) {
                    const size_t x_pos = lines[j].find('X');
                    if (!is_extruding(lines[j]) || x_pos == std::string::npos)
                        continue;
                    const double x = std::stod(lines[j].substr(x_pos + 1));
                    purge_min_x = std::min(purge_min_x, x);
                    purge_max_x = std::max(purge_max_x, x);
                }
                REQUIRE(purge_min_x <= purge_max_x);
                INFO("stop travel: " << lines[stop_line] << " purge x range: " << purge_min_x << ".." << purge_max_x);
                const bool beside_tower = stop_x < purge_min_x - 0.5 || stop_x > purge_max_x + 0.5;
                CHECK(beside_tower);
            } else {
                // Stock behavior: the blocking wait follows the toolchange command directly, and
                // nothing raises the incoming tool's target before it.
                REQUIRE(untagged_m109 < block_end);
                CHECK(tagged_wait == block_end);
                CHECK(preheat == tool_line);
                if (first_extrusion < block_end)
                    CHECK(untagged_m109 < first_extrusion);
            }
            i = block_end;
        }
        REQUIRE(checked_blocks > 0);
        if (!wait_on_tower)
            CHECK(gcode.find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos);
    }
}

// Priming runs before the first layer is set up, so set_extruder sees no layer at all: its
// on_first_layer() test is false and print_z is the initial layer height rather than 0. The
// tower nonetheless blocks on the first layer temperature there, so the pre-heat raised ahead
// of each priming Tn has to name that same temperature — pre-heating to the "other layers"
// value instead leaves the tagged M109 asking the firmware to cool back down before the
// priming lines are extruded.
TEST_CASE("Wipe tower priming pre-heats to the first layer temperature", "[MultiFilament]")
{
    const std::string gcode = slice_with_object_overrides(
        { cube(20), cube(20) },
        multifilament_config(2, {
            { "nozzle_diameter",                        "0.4,0.4" },
            { "printer_extruder_id",                    "1,2" },
            { "printer_extruder_variant",               "Direct Drive Standard,Direct Drive Standard" },
            { "extruder_printable_height",              "0,0" },
            { "single_extruder_multi_material",         0 },
            { "single_extruder_multi_material_priming", 1 },
            { "enable_prime_tower",                     1 },
            { "prime_tower_width",                      35 },
            { "wipe_tower_x",                           "50" },
            { "wipe_tower_y",                           "50" },
            { "preheat_time",                           0 }, // see the wait test above
            // Distinct enough that picking the wrong one is unambiguous.
            { "nozzle_temperature_initial_layer",       "215,215" },
            { "nozzle_temperature",                     "240,240" },
            { "wait_for_temp_on_wipe_tower",            1 },
        }),
        { { { "extruder", 1 } }, { { "extruder", 2 } } });

    std::vector<std::string> lines;
    std::istringstream gcode_stream(gcode);
    for (std::string line; std::getline(gcode_stream, line);)
        lines.emplace_back(std::move(line));
    // Temperature of an M104/M109, or -1 when the line is neither.
    const auto temp_of = [](const std::string& l) {
        if (l.rfind("M104", 0) != 0 && l.rfind("M109", 0) != 0)
            return -1;
        const size_t s = l.find('S');
        return s == std::string::npos ? -1 : std::stoi(l.substr(s + 1));
    };

    size_t start = lines.size(), end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (start == lines.size() && lines[i].find("; CP PRIMING START") != std::string::npos)
            start = i;
        else if (start < lines.size() && lines[i].find("; CP PRIMING END") != std::string::npos) {
            end = i;
            break;
        }
    }
    REQUIRE(start < end);

    int checked_waits = 0;
    for (size_t i = start; i < end; ++i) {
        if (lines[i].find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos)
            continue;
        ++checked_waits;
        INFO("priming wait at line " << i + 1 << ": " << lines[i]);
        CHECK(temp_of(lines[i]) == 215); // the tower waits on the first layer temperature
        // The most recent set-temperature before it is the pre-heat, and must agree with it.
        int preheat = -1;
        for (size_t j = i; j-- > start;)
            if ((preheat = temp_of(lines[j])) != -1)
                break;
        CHECK(preheat == 215);
    }
    REQUIRE(checked_waits > 0); // the feature under test is active
}

// The temperature-wait park picks its side of the tower by testing bed containment with the
// tower position at psWipeTower generation time, while WipeTowerIntegration shifts the cached
// moves by the CURRENT position at export. Moving the tower normally invalidates only
// psSkirtBrim (tower gcode is position-independent), but the park makes it bed-relative, so a
// GUI-style move-and-reslice on the same Print must regenerate the tower — otherwise the stale
// park prints outside the bed. Contract: every tagged wait parks inside the printable area.
TEST_CASE("Wipe tower temperature-wait park is regenerated when the tower moves", "[MultiFilament]")
{
    // Two objects, one filament each: a toolchange (and a tagged wait) on every layer, like
    // the wait test above — but on a single-extruder machine profile: the synthetic
    // dual-extruder keys would drag in the extruder-variant expansion, which is not
    // idempotent on the default machine profile and would pollute the re-apply diff below.
    // Rectangle wall and no brim keep the tower-local footprint inside [0, 35], so the park
    // sits at the generator's 2mm side gap: local -2 or 37.
    DynamicPrintConfig config = multifilament_config(2, {
        { "single_extruder_multi_material", 0 },
        { "enable_prime_tower",             1 },
        { "prime_tower_width",              35 },
        { "wipe_tower_wall_type",           "rectangle" }, // the default rib bulges past the width
        { "prime_tower_brim_width",         0 },           // the default 3 widens the first-layer envelope
        { "printable_area",                 "0x0,200x0,200x200,0x200" },
        { "wipe_tower_x",                   "0" },
        { "wipe_tower_y",                   "50" },
        { "ooze_prevention",                1 },
        { "standby_temperature_delta",      -40 },
        { "wait_for_temp_on_wipe_tower",    1 },
    });
    // init_print force-sets this on its own copy; set it here too so the re-apply below
    // diffs in wipe_tower_x ONLY — the exact GUI increment under test.
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        { { "extruder", 1 } }, { { "extruder", 2 } } }; // object-level, see the wait test above
    init_print(std::vector<TriangleMesh>{ cube(20), cube(20) }, print, model, config, &overrides);

    const std::string         at_edge       = gcode(print);
    const std::vector<double> at_edge_parks = wait_park_xs(at_edge);
    REQUIRE(!at_edge_parks.empty()); // the feature under test is active
    for (double x : at_edge_parks) {
        INFO("wait park X " << x << " with the tower at x=0 on a 200mm bed");
        CHECK(x >= -0.05);
        CHECK(x <= 200.05);
    }
    REQUIRE(print.is_step_done(psWipeTower));

    // Move the tower to the right bed edge (164 + 35 = 199 keeps the body printable) and
    // re-apply on the SAME Print, as the GUI does. Base the re-apply on the print's own
    // resolved config so the diff is wipe_tower_x alone — re-applying the caller's config
    // would also diff the apply-time extruder normalization write-backs, and those keys
    // regenerate the tower for the wrong reason. The cached right-side park would export
    // at 164 + 37 = 201, off the bed; regeneration clamps the park against the bed edge.
    // Assemble the moved config exactly the way init_print assembled the first one — the
    // apply-time normalization is only idempotent when both applies start from the same
    // derivation, and any stray diff key would regenerate the tower for the wrong reason.
    config.set_deserialize_strict({ { "wipe_tower_x", "164" } });
    DynamicPrintConfig moved_config = DynamicPrintConfig::full_print_config();
    moved_config.apply(config);
    moved_config.set_key_value("gcode_comments", new ConfigOptionBool(true));
    print.apply(model, moved_config);
    CHECK_FALSE(print.is_step_done(psWipeTower)); // the move must re-generate the tower

    const std::string         moved       = gcode(print);
    const std::vector<double> moved_parks = wait_park_xs(moved);
    REQUIRE(!moved_parks.empty()); // the waits must survive the re-slice
    for (double x : moved_parks) {
        INFO("wait park X " << x << " with the tower at x=164 on a 200mm bed");
        CHECK(x >= -0.05);
        CHECK(x <= 200.05);
    }
}

// The flag-off half of the three tests above. Every site wait_for_temp_on_wipe_tower touches is
// guarded -- set_extruder's pre-toolchange preheat block and its post_toolchange skip,
// toolchange_Change's park, the interface-temp guard in WipeTower2::tool_change, and append_tcr2's
// tagged-M109 filter -- so with the option off the feature has to be inert and temperature emission
// has to stay exactly as it was before the option existed. That is pinned against a trace captured
// from main rather than against expectations written from the current code, which would be
// re-derived from the very code they are meant to guard.
//
// Note what main emits here, since it is easy to misread as a missing wait: with preheat_time set,
// the toolchange carries no blocking M109 at all. GCodeProcessor's backtrace moves the heat-up to
// an M104 preheat_time seconds earlier and demotes the in-place command, which is the entire point
// of preheating. The lead times below are what pin that placement.
TEST_CASE("Toolchange temperature commands are unchanged when the wipe tower wait is off", "[MultiFilament][Regression]")
{
    // 20x20x5 cubes at the default 0.2mm layer height are 25 layers, one filament each, so there is
    // a toolchange -- and a preheat ahead of it -- on every layer.
    const std::string gcode = slice_with_object_overrides(
        { make_cube(20., 20., 5.), make_cube(20., 20., 5.) },
        multifilament_config(2, {
            { "nozzle_diameter",                        "0.4,0.4" },
            { "printer_extruder_id",                    "1,2" },
            { "printer_extruder_variant",               "Direct Drive Standard,Direct Drive Standard" },
            { "extruder_printable_height",              "0,0" },
            { "single_extruder_multi_material",         0 },
            { "single_extruder_multi_material_priming", 1 }, // reaches toolchange_Change's priming path
            { "enable_prime_tower",                     1 },
            { "prime_tower_width",                      35 },
            { "wipe_tower_x",                           "50" },
            { "wipe_tower_y",                           "50" },
            // GCodeProcessor::apply_config enables the preheat backtrace on
            // ooze_prevention && preheat_time > 0 && !SEMM && filaments > 1. That is what puts an
            // M104 preheat_time seconds ahead of every Tn, and it also gives set_extruder's
            // standby/restore pair, which the option demotes and moves when it is on.
            { "ooze_prevention",                        1 },
            { "standby_temperature_delta",              -40 },
            { "preheat_time",                           30 },
            { "preheat_steps",                          1 },
            // enable_tower_interface_features is deliberately left off: the interface temperature
            // is observable only through a change_filament_gcode template that reads
            // new_filament_temp, since append_tcr2 strips the tower's own M109 for it, and the
            // default template here has none. The option's interface-temp guard is covered by the
            // enabled-path tests above instead.
            //
            // Distinct enough that a wrong pick between the two is unambiguous in the trace.
            { "nozzle_temperature_initial_layer",       "215,215" },
            { "nozzle_temperature",                     "240,240" },
            { "wait_for_temp_on_wipe_tower",            0 },
        }),
        // Object-level, so the used-filament count that gates the prime tower is derived from it.
        { { { "extruder", 1 } }, { { "extruder", 2 } } });

    const std::vector<std::string> trace = temperature_trace(gcode);
    REQUIRE(trace.size() > 1);
    CHECK(gcode.find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos);

    const std::string golden_path = std::string(TEST_DATA_DIR PATH_SEPARATOR "wipe_tower_temperature_trace_main.txt");

    // Regenerate by appending this test and its helpers to the same file on main (dropping the
    // wait_for_temp_on_wipe_tower key, which main's config does not know), rebuilding
    // fff_print_tests there, running it with ORCA_UPDATE_WIPE_TOWER_TEMP_TRACE=1, copying the file
    // it writes back here, and filling in the commit it was captured from.
    if (std::getenv("ORCA_UPDATE_WIPE_TOWER_TEMP_TRACE") != nullptr) {
        std::ofstream out(golden_path);
        REQUIRE(out.good());
        out << "# Temperature and tool-change commands of a wait_for_temp_on_wipe_tower-off slice,\n"
               "# captured from the main branch at <fill in the commit>. Regeneration is described\n"
               "# at the test that reads this file: \"Toolchange temperature commands are unchanged\n"
               "# when the wipe tower wait is off\" in tests/fff_print/test_multifilament.cpp.\n";
        for (const std::string& entry : trace)
            out << entry << "\n";
        WARN("Rewrote " << golden_path << " from this run; it no longer reflects main.");
        return;
    }

    std::vector<std::string> golden;
    {
        std::ifstream in(golden_path);
        INFO("reading " << golden_path);
        REQUIRE(in.good());
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty() && line[0] != '#')
                golden.push_back(std::move(line));
        }
    }
    REQUIRE(!golden.empty());

    // Reported separately from the golden comparison below: it is a different failure.
    for (size_t i = 0; i < trace.size(); ++i) {
        const auto entry = parse_trace_entry(trace[i]);
        if (time_is_rounded_lead(entry))
            continue;
        INFO("at trace entry " << i + 1);
        INFO("  " << trace[i]);
        FAIL("\"time:\" is not its entry's \"lead\" rounded to a whole second");
    }

    const size_t common = std::min(trace.size(), golden.size());
    for (size_t i = 0; i < common; ++i) {
        if (trace_entries_match(trace[i], golden[i]))
            continue;
        // Report the first difference only: past it the two are misaligned and every later entry
        // would be reported as a difference too.
        INFO("first difference at trace entry " << i + 1);
        INFO("  main:   " << golden[i]);
        INFO("  branch: " << trace[i]);
        FAIL("temperature emission differs from main with wait_for_temp_on_wipe_tower off");
    }
    CHECK(trace.size() == golden.size());
}

// max_layer_height can be shorter than the extruder count (normalization sizes it to the
// filament count under single_extruder_multi_material). calc_max_layer_height() in ToolOrdering
// indexed it per-nozzle and read past the end. Shortened directly here to isolate that read;
// the other per-extruder keys stay extruder-length so slicing reaches the code under test.
TEST_CASE("Multi-extruder slice stays in bounds with a short max_layer_height", "[MultiFilament]")
{
    DynamicPrintConfig config = multifilament_config(2);
    config.set_deserialize_strict({
        { "nozzle_diameter",           "0.4,0.4" },
        { "printer_extruder_id",       "1,2" },
        { "printer_extruder_variant",  "Direct Drive Standard,Direct Drive Standard" },
        { "extruder_printable_height", "0,0" },
        { "max_layer_height",          "0.3" }, // deliberately one entry short
    });
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    REQUIRE_FALSE(print.objects().front()->layers().empty());
}


// Re-applying the exact same, unchanged config after a completed slice -- which the GUI does on
// every post-slice background_process update -- must report APPLY_STATUS_UNCHANGED. It did not:
// on a Print's first-ever apply BOTH normalize_fdm_2 passes run before that apply's PrintObjects/
// regions exist, count 0 used filaments and no-op, so the un-normalized settings get stored.
// normalize_fdm_2 never turns a forced-off setting back on, and an undercount of <= 1 forces
// enable_prime_tower off while independent_support_layer_height survives; the NEXT apply, counting
// correctly, keeps the tower on and forces islh off instead -- a settings flip-flop that
// invalidated the just-finished slice. No mapping/multi-tool machinery involved:
// the trigger is only the used-filament count, so a plain two-filament single-nozzle config
// reproduces it.
TEST_CASE("Reapplying an unchanged config after slicing reports no change", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(2, {
        { "wall_filament",                    "1" },
        { "sparse_infill_filament",           "2" },
        { "solid_infill_filament",            "2" },
        { "enable_prime_tower",               "1" },
        { "independent_support_layer_height", "1" },
        // Matches what init_print's internal config carries, so this reapply of the bare test
        // config differs from the print's actual first-apply config in nothing but what
        // Print::apply()'s own normalization might (wrongly) change -- isolating the bug instead
        // of also picking up an incidental, unrelated mismatch between the two configs.
        { "gcode_comments",                   "1" },
    });
    Model model;
    Print print;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    print.process();
    const bool ept_before  = print.config().enable_prime_tower.value;
    const bool islh_before = print.config().independent_support_layer_height.value;

    // Re-apply the exact same, unchanged config again -- matching the GUI's post-slice
    // background_process update, which always rebuilds from the current presets rather than
    // reusing the print's own already-resolved config.
    const auto status = print.apply(model, config);
    const bool ept_after  = print.config().enable_prime_tower.value;
    const bool islh_after = print.config().independent_support_layer_height.value;

    INFO("status = " << (int) status);
    INFO("enable_prime_tower: before=" << ept_before << " after=" << ept_after);
    INFO("independent_support_layer_height: before=" << islh_before << " after=" << islh_after);
    CHECK(ept_before == ept_after);
    CHECK(islh_before == islh_after);
    // The strongest true form: not just that the two settings held steady, but that the reapply
    // reported no change at all, so nothing invalidated the finished slice.
    CHECK((int) status == (int) PrintBase::APPLY_STATUS_UNCHANGED);
}

static std::string strip_config_block(const std::string& gcode) {
    size_t start = gcode.find("; CONFIG_BLOCK_START");
    return start == std::string::npos ? gcode : gcode.substr(0, start);
}

// multifilament_config sizes flush_volumes_matrix filaments^2 for a single nozzle, but
// get_flush_volumes_matrix DIVIDES it by the nozzle count before ToolOrdering and GCode index
// the quotient as filaments^2 again -- on a multi-nozzle config the matrix must be
// filaments^2 * nozzles or those reads run past the end and the flush-minimizing tool order
// flips on whatever the heap holds (out of scope here: these tests pin the tower generator,
// so they must not slice with an undersized matrix).
static void resize_flush_matrix(DynamicPrintConfig& config, size_t filaments, size_t nozzles) {
    std::vector<double> flush;
    flush.reserve(filaments * filaments * nozzles);
    for (size_t n = 0; n < nozzles; ++n)
        for (size_t i = 0; i < filaments; ++i)
            for (size_t j = 0; j < filaments; ++j)
                flush.push_back(i == j ? 0. : 280.);
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(flush));
    for (const char* key : { "flush_multiplier", "flush_multiplier_fast" }) {
        auto* opt = config.option<ConfigOptionFloats>(key, true);
        opt->values.assign(nozzles, opt->values.empty() ? 1. : opt->values.front());
    }
}

// Two cubes at fixed positions with one filament per object -> a toolchange on every layer.
// Assignment must be at the object level or the used-filament gate silently disables the tower
// (see the wait test above). No auto-arrange: for two identical meshes the arranger can swap
// the tie between runs, which swaps the per-object filament assignment and with it the first
// tool of the whole print -- these tests compare slices, so the placement has to be pinned.
static std::string slice_two_assigned_cubes(const DynamicPrintConfig& config)
{
    TriangleMesh a = cube(20);
    a.translate(80, 80, 0);
    TriangleMesh b = cube(20);
    b.translate(110, 80, 0);
    std::vector<TriangleMesh> meshes;
    meshes.push_back(std::move(a));
    meshes.push_back(std::move(b));
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides =
        { { { "extruder", 1 } }, { { "extruder", 2 } } };
    Print print;
    Model model;
    init_print(std::move(meshes), print, model, config, &overrides, /*arrange=*/false);
    return strip_config_block(gcode(print));
}

// First 1-based line where the two g-codes part, with both lines -- {0, ...} when equal. The
// exports are tens of thousands of lines, so a failed whole-string assertion prints nothing
// usable; the caller reports this instead.
struct GcodeDiff { size_t line; std::string a, b; };
static GcodeDiff first_diff_line(const std::string& a, const std::string& b)
{
    std::istringstream sa(a), sb(b);
    std::string la, lb;
    for (size_t line = 1;; ++line) {
        const bool ga = static_cast<bool>(std::getline(sa, la));
        const bool gb = static_cast<bool>(std::getline(sb, lb));
        if (!ga && !gb)
            return { 0, {}, {} };
        if (!ga || !gb || la != lb)
            return { line, ga ? la : "<end of file>", gb ? lb : "<end of file>" };
    }
}

// Both tower generators must produce the same g-code for the same scene every time. The Type1
// (BBL) generator did not: it indexed the per-filament filament_change_length / _nc vectors with
// the filament id, and those options default to a SINGLE element, so any config that does not list
// every filament (a hand-built config, or a filament preset that omits the key) read past the end
// of the vector for every filament but the first. The garbage that came back was divided and fed
// to std::ceil, which on an out-of-range result yields INT_MIN, and the tower's whole depth plan
// followed from it -- so the same config sliced repeatedly in one process gave different tower
// block counts and a different number of toolchanges depending only on what was on the heap.
static DynamicPrintConfig tower_determinism_config(const char* wipe_tower_type) {
    DynamicPrintConfig config = multifilament_config(2, {
        { "nozzle_diameter",                "0.4,0.4" },
        { "printer_extruder_id",            "1,2" },
        { "printer_extruder_variant",       "Direct Drive Standard,Direct Drive Standard" },
        { "extruder_printable_height",      "0,0" },
        { "single_extruder_multi_material", 0 },
        // Pin the filament-to-extruder grouping: in the default Auto mode the grouping
        // optimizer itself picks a different map between identical slices of one process
        // (observed as 67 vs 132 toolchanges for this very scene), which would swamp the
        // tower-generator determinism under test here.
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2" },
        { "enable_prime_tower",             1 },
        { "prime_tower_width",              35 },
        { "wipe_tower_x",                   "50" },
        { "wipe_tower_y",                   "50" },
        { "layer_height",                   "0.3" },
        { "wipe_tower_type",                wipe_tower_type },
    });
    resize_flush_matrix(config, 2, 2);
    return config;
}

// It takes more than two slices to see that: what the out-of-range read returns is whatever the
// allocator last left at that address, so the first slices of a process agree and the divergence
// appears once the heap has been churned. Six was the smallest count that caught it across
// MALLOC_PERTURB_ values (it first diverged on the third slice at 255 and the fifth at 42), so a
// two-slice check passes with the fix reverted and guards nothing.
TEST_CASE("Slicing the same scene repeatedly emits the same g-code", "[MultiFilament]") {
    const char* wipe_tower_type = GENERATE("type1", "type2");
    DYNAMIC_SECTION("wipe_tower_type = " << wipe_tower_type) {
        const DynamicPrintConfig config = tower_determinism_config(wipe_tower_type);
        // The generation timestamp in the header is the one line that legitimately differs.
        auto slice_without_header = [&config]() {
            const std::string gcode = slice_two_assigned_cubes(config);
            const size_t generated = gcode.find("; generated by");
            REQUIRE(generated != std::string::npos);
            return gcode.substr(gcode.find('\n', generated) + 1);
        };
        const std::string first = slice_without_header();
        REQUIRE(first.find("; WIPE_TOWER_START") != std::string::npos);
        // Only Type1 ever diverged, and each slice costs real time, so Type2 gets a cheaper check.
        const int slices = strcmp(wipe_tower_type, "type1") == 0 ? 6 : 2;
        for (int i = 1; i < slices; ++i) {
            const GcodeDiff diff = first_diff_line(slice_without_header(), first);
            INFO("slice " << i << " of " << slices << " first differs from slice 0 at line "
                 << diff.line << ":\n  " << diff.a << "\nvs\n  " << diff.b);
            CHECK(diff.line == 0);
        }
    }
}

// Sum of positive extrusion per "; NOZZLE_CHANGE_START <dir>".."; NOZZLE_CHANGE_END" block,
// grouped by the direction tag (e.g. "OF0 NF1 ON0 NN1").
static std::map<std::string, std::vector<double>> nozzle_change_e_sums(const std::string& gcode)
{
    std::map<std::string, std::vector<double>> sums;
    std::istringstream in(gcode);
    std::string dir;
    double esum = 0.;
    for (std::string line; std::getline(in, line);) {
        if (const size_t at = line.find("NOZZLE_CHANGE_START"); at != std::string::npos) {
            dir  = line.substr(at + std::string("NOZZLE_CHANGE_START").size() + 1);
            esum = 0.;
        } else if (line.find("NOZZLE_CHANGE_END") != std::string::npos) {
            sums[dir].push_back(esum);
            dir.clear();
        } else if (!dir.empty() && line.rfind("G1 ", 0) == 0) {
            const size_t e = line.find(" E");
            if (e != std::string::npos && line[e + 2] != '-')
                esum += std::stod(line.substr(e + 2));
        }
    }
    return sums;
}

// The nozzle-change ramming length is filament_change_length[old_tool] / e_flow, and the two
// filaments here share every setting -- so the T0->T1 and T1->T0 blocks must extrude the same
// steady-state length. filament_change_length defaults to a SINGLE element, so before the tower
// indexed it with get_at's clamping, old_tool 1 read past the end and this symmetry broke (the
// garbage is often stable within a process, which is why the repeated-slicing test above cannot
// be relied on to see it). Medians ignore the first layer's flow-ratio outlier.
TEST_CASE("Nozzle change ramming is symmetric for identical filaments", "[MultiFilament]")
{
    const std::string gcode = slice_two_assigned_cubes(tower_determinism_config("type1"));
    REQUIRE(gcode.find("; WIPE_TOWER_START") != std::string::npos);
    auto sums = nozzle_change_e_sums(gcode);
    REQUIRE(sums.size() == 2); // both directions present
    std::vector<double> medians;
    for (auto& [dir, e] : sums) {
        INFO("direction " << dir);
        REQUIRE_FALSE(e.empty());
        std::sort(e.begin(), e.end());
        medians.push_back(e[e.size() / 2]);
        CHECK(medians.back() > 0.);
    }
    CHECK_THAT(medians[0], Catch::Matchers::WithinRel(medians[1], 0.01));
}

// The tower's own heater lines (M104/M109 tagged N0, "generated by slicer") index
// physical_extruder_map by tool number, but that option's stock default is a SINGLE element and
// most multi-tool profiles never define it -- the read went past the end of the vector and the
// heater's tool index was whatever the heap held (observed as e.g. "M104 T21979", differing
// between runs of the same binary). Only the Type1 generator emits heater lines from inside the
// tower. Both range and run-to-run stability are asserted; a vendor-style map that does cover the
// tools must still translate through it.
static std::vector<int> tower_heater_tool_indices(const std::string& gcode) {
    std::vector<int> tools;
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);) {
        line = line.substr(0, line.find(';'));
        if (line.rfind("M104", 0) != 0 && line.rfind("M109", 0) != 0)
            continue;
        if (line.find(" N0") == std::string::npos)
            continue; // not a tower-generated heater line
        size_t t = line.find(" T");
        if (t == std::string::npos)
            continue;
        tools.push_back(std::stoi(line.substr(t + 2)));
    }
    return tools;
}

TEST_CASE("The prime tower never heats a tool index the printer cannot have", "[MultiFilament]") {
    const bool vendor_tool_map = GENERATE(false, true);
    DYNAMIC_SECTION("vendor tool map = " << vendor_tool_map) {
        DynamicPrintConfig config = multifilament_config(2, {
            { "nozzle_diameter",                "0.4,0.4" },
            { "printer_extruder_id",            "1,2" },
            { "printer_extruder_variant",       "Direct Drive Standard,Direct Drive Standard" },
            { "extruder_printable_height",      "0,0" },
            { "single_extruder_multi_material", 0 },
            // Pinned for the same reason as in tower_determinism_config: Auto grouping is
            // not stable between slices, and this test compares two of them.
            { "filament_map_mode",              "Manual" },
            { "filament_map",                   "1,2" },
            { "enable_prime_tower",             1 },
            { "prime_tower_width",              35 },
            { "wipe_tower_x",                   "50" },
            { "wipe_tower_y",                   "50" },
            { "wipe_tower_type",                "type1" },
        });
        resize_flush_matrix(config, 2, 2);
        if (vendor_tool_map)
            config.set_key_value("physical_extruder_map", new ConfigOptionInts({ 1, 0 }));
        const std::string first  = slice_two_assigned_cubes(config);
        const std::string second = slice_two_assigned_cubes(config);
        REQUIRE(first.find("; WIPE_TOWER_START") != std::string::npos);

        const std::vector<int> tools = tower_heater_tool_indices(first);
        REQUIRE_FALSE(tools.empty());
        for (int tool : tools) {
            CHECK(tool >= 0);
            CHECK(tool < 2); // two filaments on two tools
        }
        // Same scene, same binary, same indices -- an uninitialized read is not.
        CHECK(tools == tower_heater_tool_indices(second));
    }
}

TEST_CASE("Mapping protocol none preserves predicates; snapmaker flips them", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",                "0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
    });
    config.set_deserialize_strict({ { "filament_mapping_protocol", "snapmaker" } });
    REQUIRE(device_owned_mapping_protocol(config));
    REQUIRE(physical_filament_features_enabled(config));
    config.set_deserialize_strict({ { "filament_mapping_protocol", "none" } });
    REQUIRE(!device_owned_mapping_protocol(config));
    REQUIRE(!physical_filament_features_enabled(config));
}

TEST_CASE("Snapmaker protocol slices logically despite stray manual maps", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(3, {
        { "nozzle_diameter",                "0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "filament_mapping_protocol",      "snapmaker" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "3,1,2" },      // stray pre-protocol mapping
        { "filament_physical_map",          "2,2,0" },      // stray merge claim
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "3" },
        { "enable_prime_tower",             "1" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    // Logical emission: filament i prints as T(i-1); the stray map must not reroute Ts.
    REQUIRE(gcode.find("\nT0") != std::string::npos);
    REQUIRE(gcode.find("\nT1") != std::string::npos);
    REQUIRE(gcode.find("\nT2") != std::string::npos);
}

// On a device-owned mapping protocol (filament_mapping_protocol != none, e.g. snapmaker),
// DynamicPrintConfig::normalize_fdm_1()'s protocol clause forces filament_map/filament_map_mode/
// filament_physical_map to fixed values on every apply, so the engine always slices in pure
// logical space - but it left filament_volume_map and filament_nozzle_map untouched.
// Print::update_filament_maps_to_config() (Print.cpp) writes its own derived values
// for those two back into m_config once slicing completes; on a filament_count > extruder_count
// printer the unclamped identity loop in the (pre-fix) clause also disagreed with the engine's
// own derivation for filament_map itself (ToolOrdering::get_recommended_filament_maps()'s
// non-BBL multi-extruder branch maps filament i -> extruder i only up to the physical extruder
// count, falling back to master_extruder_id beyond it). Either mismatch is a permanent
// full_config_diff on every later apply of an otherwise unchanged config: the first Slice click
// invalidates instead of starting the background process, requiring a second click.
TEST_CASE("Reapplying an unchanged snapmaker-protocol config after slicing does not report a diff", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(5, {
        { "nozzle_diameter",                "0.4,0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "filament_mapping_protocol",      "snapmaker" },
        // Stray manual map/mode, as a migrated project would carry - normalize_fdm_1 must
        // override both on every apply so they cannot reintroduce a diff.
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "3,1,2,4,2" },
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "5" },
        { "enable_prime_tower",             "1" },
        { "gcode_comments",                 "1" },
    });
    Model model;
    Print print;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    print.process();

    // Re-apply the exact same, unchanged config again - matching the GUI's post-slice
    // background_process update, which always rebuilds from the current presets rather than
    // reusing the print's own already-resolved config.
    const auto status = print.apply(model, config);
    INFO("status = " << (int)status);
    CHECK((int)status == (int)PrintBase::APPLY_STATUS_UNCHANGED);
}

// The printer-agnostic half of device-resolved mapping: with enable_filament_mapping the PROJECT
// may carry more filaments than the printer has tools, and the g-code addresses one LOGICAL tool
// per filament for the printer's own firmware to resolve. No protocol, no send-time delivery: the
// file stands on its own, so it can be exported or uploaded and mapped from the printer's screen.
// A single PLATE is separately bounded by protocol_max_plate_filaments -- for the generic flag,
// by the tool count -- so this five-filament project prints a plate that stays within four.
TEST_CASE("More filaments than tools slice to logical tool indices when the device resolves mapping", "[MultiFilament]") {
    DynamicPrintConfig config = multifilament_config(5, {
        { "nozzle_diameter",                "0.4,0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_filament_mapping",        "1" },
        { "wall_filament",                  "1" },
        { "sparse_infill_filament",         "2" },
        { "solid_infill_filament",          "4" },
        { "enable_prime_tower",             "1" },
        { "gcode_comments",                 "1" },
    });
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    // One logical tool per used filament -- the ids are the project's, not a slicer remap.
    for (const char* tool : { "\nT0", "\nT1", "\nT3" }) {
        INFO("expected tool command " << tool);
        CHECK(gcode.find(tool) != std::string::npos);
    }

    // The engine keeps no mapping of its own: filament_map is pinned to the clamped identity
    // derivation, so a reapply of the same config reports no change (no permanent config diff).
    Model model;
    Print print;
    Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
    print.process();
    const auto status = print.apply(model, config);
    INFO("status = " << (int) status);
    CHECK((int) status == (int) PrintBase::APPLY_STATUS_UNCHANGED);
}

// Dense tool numbering (protocol_requires_dense_tool_numbering) is the third, independent
// capability: the WonderMaker ZR Ultra S resolves the mapping itself AND only permutes its tools,
// so it has no macro past its last one. A plate that legitimately uses project filaments 4 and 7
// must therefore reach it as T0/T1, which is what FilamentCompaction arranges before slicing.
// The Snapmaker case below is the control: its extruder_map_table is indexed BY the project slot,
// so the same plate must keep the un-renumbered tool ids there.
TEST_CASE("A dense-numbering printer slices a sparse plate to consecutive tool ids", "[MultiFilament]") {
    struct Case { const char* name; const char* protocol; bool dense; };
    const Case c = GENERATE(
        Case{ "wondermaker renumbers to T0..T2", "wondermaker", true  },
        Case{ "snapmaker keeps the project ids", "snapmaker",   false });

    DYNAMIC_SECTION(c.name) {
        // Eight project filaments on a four-tool printer. The plate prints filament 1 (the
        // objects' own), 4 (outer wall) and 7 (infill) -- three filaments, but spread across the
        // project so the highest id is well past the tool count.
        DynamicPrintConfig config = multifilament_config(8, {
            { "nozzle_diameter",                "0.4,0.4,0.4,0.4" },
            { "single_extruder_multi_material", "0" },
            { "outer_wall_filament_id",         4 },
            { "sparse_infill_filament_id",      7 },
            { "internal_solid_filament_id",     7 },
            { "enable_prime_tower",             "0" },
            { "gcode_comments",                 "1" },
            // validate() checks this too: the default Marlin flavour with relative E needs it.
            { "layer_change_gcode",             "G92 E0" },
        });
        config.set_deserialize_strict({ { "filament_mapping_protocol", c.protocol } });

        const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
        REQUIRE(!gcode.empty());

        // Which tool ids the g-code actually commands.
        std::set<int> tools;
        for (int tool = 0; tool < 8; ++tool)
            if (gcode.find("\nT" + std::to_string(tool)) != std::string::npos)
                tools.insert(tool);

        if (c.dense) {
            // Renumbered: three filaments become T0/T1/T2, and nothing addresses a tool the
            // firmware has no macro for.
            CHECK(tools == std::set<int>{ 0, 1, 2 });
        } else {
            // Untouched: the U1 wants the project's own ids, so filaments 4 and 7 stay T3 and T6.
            CHECK(tools == std::set<int>{ 0, 3, 6 });
        }
    }
}

// A mixed filament is numbered after every physical one and never commanded itself; on a
// dense-numbering printer the renumbering must hand the printer only the components' tools.
TEST_CASE("A dense-numbering printer commands only a mix's components", "[MultiFilament]") {
    // Six project filaments on four heads: the plate prints filament 3 and a mix of 1 and 3 in
    // slot 5. Two physical filaments -> T0 (filament 1) and T1 (filament 3), nothing else.
    DynamicPrintConfig config = multifilament_config(6, {
        { "nozzle_diameter",                "0.4,0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "filament_is_mixed",              "0,0,0,0,1,0" },
        { "filament_mixed_components",      ";;;;1,3;" },
        { "filament_mixed_sublayer_ratios", ";;;;0.5,0.5;" },
        { "filament_mixed_gradient",        "0,0,0,0,0,0" },
        { "filament_mixed_gradient_range",  ";;;;;" },
        { "filament_mixed_gradient_curve",  ";;;;;" },
        { "filament_mixed_gradient_per_part","0,0,0,0,0,0" },
        { "wall_filament",                  "3" },
        { "sparse_infill_filament",         "5" },
        { "solid_infill_filament",          "5" },
        { "enable_prime_tower",             "0" },
        { "layer_change_gcode",             "G92 E0" },
    });
    config.set_deserialize_strict({ { "filament_mapping_protocol", "wondermaker" } });

    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_with_hole }, config);
    REQUIRE(!gcode.empty());

    std::set<int> tools;
    for (int tool = 0; tool < 6; ++tool)
        if (gcode.find("\nT" + std::to_string(tool)) != std::string::npos)
            tools.insert(tool);
    CHECK(tools == std::set<int>{ 0, 1 });
}

// Count decoupling and per-plate routing capacity are different capabilities. The Snapmaker U1
// owns a 32-entry extruder_map_table and merges surplus logical tools onto its four heads, so a
// five-filament plate is fine there. Firmware that only permutes its tools (the WonderMaker ZR
// Ultra S offers exactly four mappable slots and has no T4 macro) is all we may assume behind the
// printer-agnostic enable_filament_mapping flag, so the same plate must be rejected before it
// becomes an unprintable file. Print::validate() is the gate; protocol_max_plate_filaments() the
// per-protocol capability.
TEST_CASE("A plate may not use more filaments than the printer can route", "[MultiFilament]") {
    struct Case { const char* name; const char* protocol; bool flag; int solid_infill_filament; bool valid; };
    const Case c = GENERATE(
        Case{ "snapmaker routes five filaments on four heads",   "snapmaker", false, 5, true  },
        // The U1 as actually configured: a native protocol AND the generic flag on. The protocol
        // must win. Reading it from Print's own m_config cannot see it (no member in the static
        // PrintConfig struct), which capped this printer at four and blocked the slice.
        Case{ "a protocol outranks the flag on the same printer", "snapmaker", true, 5, true  },
        Case{ "the generic flag may not exceed the tool count",  "none",      true,  5, false },
        Case{ "four filaments on four heads is fine either way", "none",      true,  4, true  });

    DYNAMIC_SECTION(c.name) {
        DynamicPrintConfig config = multifilament_config(5, {
            { "nozzle_diameter",                "0.4,0.4,0.4,0.4" },
            { "single_extruder_multi_material", "0" },
            { "enable_filament_mapping",        c.flag ? "1" : "0" },
            { "wall_filament",                  "1" },
            { "sparse_infill_filament",         "2" },
            { "solid_infill_filament",          c.solid_infill_filament },
            // Unrelated to the gate under test, but validate() checks it too: the default Marlin
            // flavour with relative E requires a per-layer reset.
            { "layer_change_gcode",             "G92 E0" },
        });
        config.set_deserialize_strict({ { "filament_mapping_protocol", c.protocol } });

        Model model;
        Print print;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const std::string err = print.validate().string;
        INFO("validate() said: " << err);
        CHECK(err.empty() == c.valid);
        // Named explicitly so an unrelated validation failure cannot green this case.
        if (!c.valid)
            CHECK(err.find("filaments on one plate") != std::string::npos);
    }
}

// A mixed (virtual) filament is numbered after every physical one but never reaches the printer:
// only its components are commanded as tools. The plate bound has to see through it, or a
// four-filament plate that blends two of them is rejected on a four-tool printer.
TEST_CASE("A mixed filament counts as its components against the plate bound", "[MultiFilament]") {
    struct Case { const char* name; const char* components; bool valid; };
    const Case c = GENERATE(
        Case{ "a mix of tools 1 and 2 fits four tools",      "1,2", true  },
        Case{ "a mix that pulls in filament 6 does not fit", "1,6", false });

    DYNAMIC_SECTION(c.name) {
        // Six project filaments on four heads: 1-4 physical, 5 a mix, 6 physical but unused.
        DynamicPrintConfig config = multifilament_config(6, {
            { "nozzle_diameter",                "0.4,0.4,0.4,0.4" },
            { "single_extruder_multi_material", "0" },
            { "enable_filament_mapping",        "1" },
            { "filament_is_mixed",              "0,0,0,0,1,0" },
            { "filament_mixed_components",      std::string(";;;;") + c.components + ";" },
            { "filament_mixed_sublayer_ratios", ";;;;0.5,0.5;" },
            { "filament_mixed_gradient",        "0,0,0,0,0,0" },
            { "filament_mixed_gradient_range",  ";;;;;" },
            { "filament_mixed_gradient_curve",  ";;;;;" },
            { "filament_mixed_gradient_per_part","0,0,0,0,0,0" },
            { "wall_filament",                  "1" },
            { "sparse_infill_filament",         "5" },
            { "solid_infill_filament",          "5" },
            { "layer_change_gcode",             "G92 E0" },
        });

        Model model;
        Print print;
        Slic3r::Test::init_print({ TestMesh::cube_with_hole }, print, model, config);
        const std::string err = print.validate().string;
        INFO("validate() said: " << err);
        CHECK(err.empty() == c.valid);
        if (!c.valid)
            CHECK(err.find("filaments on one plate") != std::string::npos);
    }
}
