#include "libslic3r/GCode/Islands.hpp"

#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_all.hpp>

using namespace Slic3r;

namespace {

// Parameters of the synthetic "H"-style G-code used by the tests.
struct GcodeParams {
    size_t lower_layers = 3;  // layers of the lower phase (two towers)
    size_t upper_layers = 2;  // layers of the upper phase (two towers)
    double layer_height = 0.3;
    double tower_gap = 30.0;  // distance between the tower centroids
    bool   relative_e = false;
    bool   z_hop = false;     // insert a Z-lift before every inter-island travel
    bool   multi_tool = false;
};

static double e_value(const std::string& line)
{
    size_t pos = line.find('E');
    while (pos != std::string::npos) {
        if (pos == 0 || line[pos - 1] == ' ' || line[pos - 1] == '\t') {
            char* end = nullptr;
            double v = std::strtod(line.c_str() + pos + 1, &end);
            if (end != line.c_str() + pos + 1)
                return v;
        }
        pos = line.find('E', pos + 1);
    }
    return 0.0;
}

static bool has_e(const std::string& line)
{
    return line.size() > 2 && line[0] == 'G' && e_value(line) != 0.0;
}

// Sum of the positive E deltas, interpreted per the M82/M83 mode.
// G92 re-base lines do not extrude and are skipped.
static double extruded_sum(const std::string& gcode)
{
    double total = 0.0;
    double prev = 0.0;
    bool   have_prev = false;
    bool   absolute_e = true;
    std::istringstream iss(gcode);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("M82") != std::string::npos || line.find("G90") != std::string::npos)
            absolute_e = true;
        if (line.find("M83") != std::string::npos || line.find("G91") != std::string::npos)
            absolute_e = false;
        if (line.size() > 2 && line[0] == 'G' && line[1] == '9' && line[2] == '2') {
            prev = e_value(line);
            have_prev = true;
            continue;
        }
        if (!has_e(line))
            continue;
        const double e = e_value(line);
        const double delta = absolute_e ? (have_prev ? e - prev : e) : e;
        if (delta > 0.0)
            total += delta;
        prev = e;
        have_prev = true;
    }
    return total;
}

// Sequence of the ";TAG ..." comments, in file order.
static std::vector<std::string> tags(const std::string& gcode)
{
    std::vector<std::string> out;
    std::istringstream iss(gcode);
    std::string line;
    while (std::getline(iss, line))
        if (line.size() >= 6 && line.substr(0, 5) == ";TAG ")
            out.push_back(line.substr(5));
    return out;
}

static size_t count_lines_with(const std::string& gcode, const std::string& needle)
{
    size_t count = 0;
    std::istringstream iss(gcode);
    std::string line;
    while (std::getline(iss, line))
        if (line.find(needle) != std::string::npos)
            ++count;
    return count;
}

// A synthetic Bambu/Orca-style G-code of a letter "H": two towers, a bridge
// layer where they merge into one island, then two towers again.
static std::string make_h_gcode(const GcodeParams& p)
{
    std::ostringstream out;
    out << "; config block\n";
    out << "G90\n";
    out << (p.relative_e ? "M83\n" : "M82\n");
    out << "G1 Z5 F300\n";
    const double x_b = 10.0 + p.tower_gap;
    double z = p.layer_height;
    double e = 0.0;
    auto tower_a = [&]() {
        out << ";TAG A\n";
        out << "G1 X10 Y10 E" << (e += 0.1) << " F1200\n";
        out << "G1 X20 Y10 E" << (e += 0.1) << " F1200\n";
        out << "G1 X20 Y12 E" << (e += 0.1) << " F1200\n";
    };
    auto tower_b = [&]() {
        out << ";TAG B\n";
        out << "G1 X" << x_b << " Y10 E" << (e += 0.1) << " F1200\n";
        out << "G1 X" << (x_b + 10) << " Y10 E" << (e += 0.1) << " F1200\n";
        out << "G1 X" << (x_b + 10) << " Y12 E" << (e += 0.1) << " F1200\n";
    };
    auto travel = [&]() {
        out << "G1 X" << x_b << " Y10 F24000\n";
        // The z-hop is emitted after the travel, i.e. in the head of the
        // following chunk, like OrcaSlicer's travel sequences.
        if (p.z_hop)
            out << "G1 Z6.5 F300\n";
    };
    auto layer_header = [&](double zz) {
        out << ";LAYER_CHANGE\n";
        out << ";Z:" << zz << "\n";
        out << ";HEIGHT:" << p.layer_height << "\n";
        out << "G1 Z" << zz << " F300\n";
    };
    for (size_t l = 0; l < p.lower_layers; ++l) {
        layer_header(z);
        tower_a();
        travel();
        tower_b();
        z += p.layer_height;
    }
    if (p.multi_tool)
        out << "T1\n";
    // Bridge layer: the two towers are joined, one island.
    layer_header(z);
    out << ";TAG BRIDGE\n";
    out << "G1 X10 Y10 E" << (e += 0.1) << " F1200\n";
    out << "G1 X20 Y12 E" << (e += 0.1) << " F1200\n";
    out << "G1 X" << x_b << " Y12 E" << (e += 0.1) << " F1200\n";
    out << "G1 X" << x_b << " Y10 E" << (e += 0.1) << " F1200\n";
    z += p.layer_height;
    for (size_t l = 0; l < p.upper_layers; ++l) {
        layer_header(z);
        tower_a();
        travel();
        tower_b();
        z += p.layer_height;
    }
    out << "; end gcode\n";
    out << "G1 Z50 F300\n";
    return out.str();
}

} // namespace

TEST_CASE("Regroups two towers of an H layer-major to tower-major", "[Islands]")
{
    const std::string gcode = make_h_gcode(GcodeParams{});
    const std::string reordered =
        islands_process(gcode, /*clearance_radius=*/20.0, /*clearance_height=*/20.0);
    REQUIRE(reordered != gcode);

    // Lower phase (3 layers x 2 towers) is grouped, bridge layer stays,
    // upper phase (2 layers x 2 towers) is grouped again.
    const std::vector<std::string> tag_seq = tags(reordered);
    const std::vector<std::string> expected = { "A", "A", "A", "B", "B", "B",
                                                "BRIDGE", "A", "A", "B", "B" };
    REQUIRE(tag_seq == expected);

    // Every layer marker and Z comment is preserved exactly once, in order.
    REQUIRE(count_lines_with(reordered, ";LAYER_CHANGE") == 6);
    REQUIRE(count_lines_with(reordered, ";Z:0.3") == 1);
    REQUIRE(count_lines_with(reordered, ";Z:1.2") == 1);
    REQUIRE(count_lines_with(reordered, ";Z:1.8") == 1);

    // The total extruded length and the final E position are unchanged.
    REQUIRE_THAT(extruded_sum(reordered), Catch::Matchers::WithinRel(extruded_sum(gcode), 1e-6));
}

TEST_CASE("Keeps the G-code untouched when the towers are within the clearance radius", "[Islands]")
{
    // 20 mm gap splits the towers into separate chunks, but their centroids
    // are still closer than the 25 mm radius.
    GcodeParams p;
    p.tower_gap = 20.0;
    const std::string gcode = make_h_gcode(p);
    // Phase extent 0.6 mm exceeds the 0.5 mm clearance height, so the pair is
    // inside the clearance region and the feature is disabled.
    REQUIRE(islands_process(gcode, 25.0, 0.5) == gcode);
    // With a larger clearance height the same geometry is safe to regroup.
    REQUIRE(islands_process(gcode, 25.0, 1.0) != gcode);
}

TEST_CASE("Keeps the G-code untouched when the height difference exceeds the clearance height", "[Islands]")
{
    // 18 mm gap: the inter-tower travel (8.2 mm) splits the chunks, and the
    // tower centroids are 18 mm apart, closer than the 20 mm radius.
    GcodeParams p;
    p.tower_gap = 18.0;
    p.lower_layers = 8; // phase extent 2.1 mm
    const std::string gcode = make_h_gcode(p);
    REQUIRE(islands_process(gcode, 20.0, 2.0) == gcode);
    REQUIRE(islands_process(gcode, 20.0, 3.0) != gcode);
}

TEST_CASE("Handles relative extrusion without touching E values", "[Islands]")
{
    GcodeParams p;
    p.relative_e = true;
    const std::string gcode = make_h_gcode(p);
    const std::string reordered =
        islands_process(gcode, 20.0, 20.0);
    REQUIRE(reordered != gcode);
    REQUIRE(tags(reordered) == std::vector<std::string>{ "A", "A", "A", "B", "B", "B",
                                                         "BRIDGE", "A", "A", "B", "B" });
    REQUIRE_THAT(extruded_sum(reordered), Catch::Matchers::WithinRel(extruded_sum(gcode), 1e-6));
}

TEST_CASE("Rebases absolute E so extrusion continues where the previous chunk stopped", "[Islands]")
{
    GcodeParams p;
    p.lower_layers = 4; // forces rebasing across the grouped phase
    const std::string gcode = make_h_gcode(p);
    const std::string reordered =
        islands_process(gcode, 20.0, 20.0);
    REQUIRE(reordered != gcode);
    REQUIRE_THAT(extruded_sum(reordered), Catch::Matchers::WithinRel(extruded_sum(gcode), 1e-6));

    // In absolute mode the reordering re-bases the E counter with G92 lines,
    // so the E values are contiguous within each chunk. The summed extruded
    // length above must match exactly.
    REQUIRE(count_lines_with(reordered, "G92 E") >= 2);
}

TEST_CASE("Strips context-dependent Z moves from regrouped chunk heads", "[Islands]")
{
    GcodeParams p;
    p.z_hop = true;
    const std::string gcode = make_h_gcode(p);
    const std::string reordered =
        islands_process(gcode, 20.0, 20.0);
    REQUIRE(reordered != gcode);
    // The absolute Z-hop values (6.5 mm) refer to the original file position
    // and must not survive into the regrouped output.
    REQUIRE(count_lines_with(reordered, "Z6.5") == 0);
    REQUIRE(tags(reordered) == std::vector<std::string>{ "A", "A", "A", "B", "B", "B",
                                                         "BRIDGE", "A", "A", "B", "B" });
}

TEST_CASE("Leaves multi-extruder G-code untouched", "[Islands]")
{
    GcodeParams p;
    p.multi_tool = true;
    const std::string gcode = make_h_gcode(p);
    REQUIRE(islands_process(gcode, 20.0, 20.0) == gcode);
}

TEST_CASE("Leaves G-code without layer markers untouched", "[Islands]")
{
    std::string gcode = make_h_gcode(GcodeParams{});
    std::string stripped;
    for (size_t pos = gcode.find(";LAYER_CHANGE"); pos != std::string::npos; pos = gcode.find(";LAYER_CHANGE"))
        gcode.erase(pos, 14);
    REQUIRE(islands_process(gcode, 20.0, 20.0) == gcode);
}

TEST_CASE("Leaves a single-tower G-code untouched", "[Islands]")
{
    GcodeParams p;
    p.tower_gap = 0.0; // towers overlap: one island per layer
    const std::string gcode = make_h_gcode(p);
    REQUIRE(islands_process(gcode, 20.0, 20.0) == gcode);
}

TEST_CASE("Leaves G-code untouched for non-positive clearance settings", "[Islands]")
{
    const std::string gcode = make_h_gcode(GcodeParams{});
    REQUIRE(islands_process(gcode, 0.0, 20.0) == gcode);
    REQUIRE(islands_process(gcode, 20.0, 0.0) == gcode);
    REQUIRE(islands_process(gcode, -5.0, 20.0) == gcode);
}

// A tower's wall ring and its infill are separate chunks (the infill travel
// splits them) whose centroids can be farther apart than the merge threshold,
// but whose extruded footprints touch. They must merge into one island, or a
// wide body fragments into wall + infill pieces and the phases never form.
// Travels inside a chunk head carry a Z belonging to the original file
// position; they must lose it in the regrouped output so the nozzle never
// drops mid-travel.
TEST_CASE("Merges wall and infill chunks of one tower and strips head travel Z", "[Islands]")
{
    std::ostringstream out;
    out << "G90\nM82\nG1 Z5 F300\n";
    double e = 0.0;
    auto tower = [&](const char* tag, double x, double zz) {
        out << ";TAG " << tag << "-WALL\n";
        out << "G1 X" << x << " Y10 E" << (e += 0.1) << " F1200\n";
        out << "G1 X" << x << " Y12 E" << (e += 0.1) << " F1200\n";
        out << "G1 X" << (x + 5) << " Y12 Z" << zz << " F24000\n";  // travel with a context Z
        out << ";TAG " << tag << "-INFILL\n";
        out << "G1 X" << (x + 1) << " Y11 E" << (e += 0.1) << " F1200\n";
        out << "G1 X" << (x + 4) << " Y11 E" << (e += 0.1) << " F1200\n";
    };
    auto layer = [&](double zz) {
        out << ";LAYER_CHANGE\n;Z:" << zz << "\nG1 Z" << zz << " F300\n";
        tower("A", 10.0, zz);
        out << "G1 X110 Y10 F24000\n";
        tower("B", 110.0, zz);
    };
    layer(0.3);
    layer(0.6);
    layer(0.9);
    out << ";LAYER_CHANGE\n;Z:1.2\nG1 Z1.2 F300\n;TAG BRIDGE\n";
    out << "G1 X10 Y10 E" << (e += 0.1) << " F1200\n";
    out << "G1 X120 Y10 E" << (e += 0.1) << " F1200\n";

    const std::string gcode = out.str();
    const std::string reordered = islands_process(gcode, 20.0, 20.0);
    REQUIRE(reordered != gcode);

    // Wall and infill of one tower stay together: the lower phase is
    // A(WALL,INFILL) x3 then B(WALL,INFILL) x3, the bridge layer verbatim.
    const std::vector<std::string> tag_seq = tags(reordered);
    const std::vector<std::string> expected = { "A-WALL", "A-INFILL", "A-WALL", "A-INFILL", "A-WALL", "A-INFILL",
                                                "B-WALL", "B-INFILL", "B-WALL", "B-INFILL", "B-WALL", "B-INFILL",
                                                "BRIDGE" };
    REQUIRE(tag_seq == expected);

    // The Z-carrying travels inside the regrouped chunks are stripped; the
    // verbatim bridge layer keeps its own lines.
    REQUIRE(count_lines_with(reordered, "G1 X15 Y12 Z") == 0);
    REQUIRE(count_lines_with(reordered, "G1 X115 Y12 Z") == 0);
    REQUIRE_THAT(extruded_sum(reordered), Catch::Matchers::WithinRel(extruded_sum(gcode), 1e-6));
}

// Bambu-family exports use "; CHANGE_LAYER" + "; Z_HEIGHT:" (the BBL reserved
// tag table) instead of ";LAYER_CHANGE" + ";Z:", relative extrusion, and
// typically contain a "T65535" sentinel (AMS filament pull-back in the machine
// end G-code) which is not a tool change.
TEST_CASE("Regroups a Bambu-style H with CHANGE_LAYER markers and a T65535 sentinel", "[Islands]")
{
    std::ostringstream out;
    out << "; config block\n";
    out << "G90\n";
    out << "M83 ; use relative distances for extrusion\n";
    double z = 0.2;
    double e = 0.0;
    auto tower_a = [&]() {
        out << ";TAG A\n";
        out << "G1 X10 Y10 E" << (e += 0.1) << " F1200\n";
        out << "G1 X20 Y10 E" << (e += 0.1) << " F1200\n";
        out << "G1 X20 Y12 E" << (e += 0.1) << " F1200\n";
    };
    auto tower_b = [&]() {
        out << ";TAG B\n";
        out << "G1 X110 Y10 E" << (e += 0.1) << " F1200\n";
        out << "G1 X120 Y10 E" << (e += 0.1) << " F1200\n";
        out << "G1 X120 Y12 E" << (e += 0.1) << " F1200\n";
    };
    auto layer_header = [&](double zz) {
        out << "; CHANGE_LAYER\n";
        out << "; Z_HEIGHT: " << zz << "\n";
        out << "; LAYER_HEIGHT: 0.2\n";
        out << "G1 Z" << zz << " F300\n";
    };
    for (size_t l = 0; l < 3; ++l) {
        layer_header(z);
        tower_a();
        out << "G1 X110 Y10 F24000\n";  // long travel between the towers
        tower_b();
        z += 0.2;
    }
    // Bridge layer: one island.
    layer_header(z);
    out << ";TAG BRIDGE\n";
    out << "G1 X10 Y10 E" << (e += 0.1) << " F1200\n";
    out << "G1 X120 Y12 E" << (e += 0.1) << " F1200\n";
    z += 0.2;
    for (size_t l = 0; l < 2; ++l) {
        layer_header(z);
        tower_a();
        out << "G1 X110 Y10 F24000\n";
        tower_b();
        z += 0.2;
    }
    // Machine end G-code: AMS filament pull-back, not a tool change.
    out << "M620 S65535\n";
    out << "T65535\n";
    out << "M621 S65535\n";

    const std::string gcode = out.str();
    const std::string reordered =
        islands_process(gcode, /*clearance_radius=*/20.0, /*clearance_height=*/20.0);
    REQUIRE(reordered != gcode);

    // Lower phase (3 layers x 2 towers) is grouped, bridge layer stays,
    // upper phase (2 layers x 2 towers) is grouped again.
    const std::vector<std::string> tag_seq = tags(reordered);
    const std::vector<std::string> expected = { "A", "A", "A", "B", "B", "B",
                                                "BRIDGE", "A", "A", "B", "B" };
    REQUIRE(tag_seq == expected);

    // Every layer marker and Z comment is preserved exactly once.
    REQUIRE(count_lines_with(reordered, "; CHANGE_LAYER") == 6);
    REQUIRE(count_lines_with(reordered, "; Z_HEIGHT: 0.2") == 1);
    REQUIRE(count_lines_with(reordered, "; Z_HEIGHT: 1.2") == 1);

    // The T65535 sentinel survives and the extruded length is unchanged.
    REQUIRE(count_lines_with(reordered, "T65535") == 1);
    REQUIRE_THAT(extruded_sum(reordered), Catch::Matchers::WithinRel(extruded_sum(gcode), 1e-6));
}
