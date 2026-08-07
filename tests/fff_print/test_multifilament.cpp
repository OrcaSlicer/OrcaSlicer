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

