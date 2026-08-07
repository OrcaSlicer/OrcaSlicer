#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <catch2/catch_all.hpp>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Sum of the positive E deltas (absolute mode) / E values (relative mode),
// i.e. the extruded filament. G92 re-base lines do not extrude and are
// skipped; the M82/M83 mode is tracked.
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
        size_t pos = line.find('E');
        while (pos != std::string::npos && pos > 0 && line[pos - 1] != ' ')
            pos = line.find('E', pos + 1);
        if (pos == std::string::npos)
            continue;
        const double e = std::strtod(line.c_str() + pos + 1, nullptr);
        if (line.size() > 2 && line[0] == 'G' && line[1] == '9' && line[2] == '2') {
            prev = e;
            have_prev = true;
            continue;
        }
        const double delta = absolute_e ? (have_prev ? e - prev : e) : e;
        if (delta > 0.0)
            total += delta;
        prev = e;
        have_prev = true;
    }
    return total;
}

// Number of occurrences of `needle` in the G-code.
static size_t count_str(const std::string& gcode, const std::string& needle)
{
    size_t count = 0;
    std::istringstream iss(gcode);
    std::string line;
    while (std::getline(iss, line))
        if (line.find(needle) != std::string::npos)
            ++count;
    return count;
}

// The emitted moves: from the "; EXECUTABLE_BLOCK_START" marker up to the
// trailing config dump ("; CONFIG_BLOCK_START"), which carries the slice
// settings. "; printing object" label comments carry a per-slice random
// object id and are dropped.
static std::string executable_part(const std::string& gcode)
{
    const size_t start = gcode.find("; EXECUTABLE_BLOCK_START");
    const size_t end = gcode.find("; CONFIG_BLOCK_START");
    const size_t begin = (start == std::string::npos) ? 0 : start;
    const std::string body = gcode.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    std::string out;
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line))
        if (line.find("printing object") == std::string::npos) {
            out += line;
            out += '\n';
        }
    return out;
}

// Among the first `nlayers` ";LAYER_CHANGE" blocks, count those whose
// extrusion stays within a single horizontal tower. A by-layer print of a
// two-tower object scores ~0; a regrouped one scores ~nlayers.
static size_t single_tower_layer_count(const std::string& gcode, size_t nlayers = 25)
{
    size_t count = 0;
    std::istringstream iss(gcode);
    std::string line;
    size_t layer = 0;
    bool in_first_layers = false;
    std::vector<double> xs;
    auto finish_layer = [&]() {
        if (layer >= nlayers)
            return;
        if (xs.empty())
            return;
        const double min = *std::min_element(xs.begin(), xs.end());
        const double max = *std::max_element(xs.begin(), xs.end());
        // The bridge test mesh has two towers ~45 mm apart at x -25..-20 and
        // x 20..25; a single-tower layer fits in a 10 mm band.
        if (max - min < 12.0)
            ++count;
        xs.clear();
        ++layer;
    };
    while (std::getline(iss, line)) {
        if (line.find(";LAYER_CHANGE") != std::string::npos) {
            finish_layer();
            in_first_layers = true;
            continue;
        }
        if (!in_first_layers || layer >= nlayers)
            continue;
        if (line.size() > 2 && line[0] == 'G' && (line[1] == '1' || line[1] == '0') &&
            line.find('E') != std::string::npos) {
            const size_t xp = line.find('X');
            if (xp != std::string::npos)
                xs.push_back(std::strtod(line.c_str() + xp + 1, nullptr));
        }
    }
    finish_layer();
    return count;
}

} // namespace

// Two towers (x 75-80 and 120-125) joined by a bridge at z 5-8: the lower
// towers are separate islands until the bridge layer merges them.
TEST_CASE("A bridge-shaped object prints each tower to full height before the bridge layers", "[DynamicCompositeObjects]")
{
    const std::string g_on = slice({ TestMesh::bridge }, {
        {"print_sequence", "dynamic composite objects"},
        {"skirt_loops", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
    });
    const std::string g_off = slice({ TestMesh::bridge }, {
        {"skirt_loops", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
    });

    REQUIRE(!g_on.empty());
    REQUIRE(g_on != g_off);

    // Same number of layers, same amount of extruded filament.
    REQUIRE(count_str(g_on, ";LAYER_CHANGE") == count_str(g_off, ";LAYER_CHANGE"));
    REQUIRE_THAT(extruded_sum(g_on), Catch::Matchers::WithinRel(extruded_sum(g_off), 1e-3));

    // The by-layer print visits both towers within every layer, while the
    // regrouped print stays on the first tower for most of the first 25 layers.
    const size_t st_on  = single_tower_layer_count(g_on);
    const size_t st_off = single_tower_layer_count(g_off);
    REQUIRE(st_on > 15);
    REQUIRE(st_off < 5);

    // The preview result comes from a fresh process_file pass over the exported
    // file (the export hook swaps it in), so it must describe the same layers.
    ScopedTemporaryFile temp_gcode(".gcode");
    {
        std::ofstream os(temp_gcode.string());
        os << g_on;
    }
    GCodeProcessor proc;
    proc.process_file(temp_gcode.string());
    std::set<unsigned int> preview_layers;
    for (const auto& move : proc.get_result().moves)
        preview_layers.insert(move.layer_id);
    REQUIRE(preview_layers.size() == count_str(g_on, ";LAYER_CHANGE"));
}

// Infill patterns split each tower layer into wall and infill chunks; those
// must still group into the two towers, or the regrouping disappears entirely
// once the model has infill.
TEST_CASE("Tower regrouping survives dense infill", "[DynamicCompositeObjects]")
{
    const std::string g_on = slice({ TestMesh::bridge }, {
        {"print_sequence", "dynamic composite objects"},
        {"skirt_loops", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"sparse_infill_density", "35%"},
        {"top_shell_layers", "2"},
        {"bottom_shell_layers", "2"},
    });
    const std::string g_off = slice({ TestMesh::bridge }, {
        {"skirt_loops", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"sparse_infill_density", "35%"},
        {"top_shell_layers", "2"},
        {"bottom_shell_layers", "2"},
    });
    REQUIRE(g_on != g_off);
    REQUIRE(single_tower_layer_count(g_on) > single_tower_layer_count(g_off));
}

// The DCO hook re-processes the exported file into the preview result; a plate
// past the first one has a non-zero origin, and the result must carry that
// offset or the preview renders the toolpath on the first plate.
TEST_CASE("Regrouped preview result carries the plate origin offset", "[DynamicCompositeObjects]")
{
    Print   print;
    Model   model;
    init_print({ TestMesh::bridge }, print, model, {
        {"print_sequence", "dynamic composite objects"},
        {"skirt_loops", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
    });
    // Plate 2 of a 2x1 layout: the plate origin is 256 mm along X. The model
    // coordinates are global (the object stays where it was placed), the file
    // is plate-local, and the preview result must come back to global.
    print.set_plate_origin(Vec3d(256.0, 0.0, 0.0));
    print.process();

    ScopedTemporaryFile  temp_gcode(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp_gcode.string(), &result, nullptr);
    REQUIRE(!result.moves.empty());

    // The bridge mesh towers sit at x -25..-20 and x 20..25 in model (global)
    // coordinates; the object is not moved, so the correct preview result has
    // it right there. Without the plate offset applied to the reprocessed
    // result, the moves come out plate-local (x -281..-231 here, the origin
    // subtracted) and the preview draws them on plate 1.
    bool bridge_at_expected = false;
    for (const auto& move : result.moves)
        if (move.type == EMoveType::Extrude && move.position.x() > -30.0f && move.position.x() < 30.0f)
            bridge_at_expected = true;
    REQUIRE(bridge_at_expected);
}

// The same geometry is left untouched when the clearance settings forbid the
// grouping (towers closer than the radius while the height difference exceeds
// the clearance height).
TEST_CASE("Two towers within the clearance region keep layer-by-layer ordering", "[DynamicCompositeObjects]")
{
    // Towers ~45 mm apart: place them closer than the 50 mm radius and cap the
    // clearance height below the 3.4 mm tower phase so the grouping is
    // forbidden.
    const std::string g_restricted = slice({ TestMesh::bridge }, {
        {"print_sequence", "dynamic composite objects"},
        {"skirt_loops", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"dynamic_composite_clearance_radius", "50"},
        {"dynamic_composite_clearance_height", "3"},
    });
    const std::string g_off = slice({ TestMesh::bridge }, {
        {"skirt_loops", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
    });
    // The config block carries the clearance settings, so compare only the
    // executable part: the restricted geometry must print layer by layer.
    REQUIRE(executable_part(g_restricted) == executable_part(g_off));

    // With a taller clearance height the same geometry is regrouped again.
    const std::string g_allowed = slice({ TestMesh::bridge }, {
        {"print_sequence", "dynamic composite objects"},
        {"skirt_loops", "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"dynamic_composite_clearance_radius", "50"},
        {"dynamic_composite_clearance_height", "6"},
    });
    REQUIRE(executable_part(g_allowed) != executable_part(g_off));
}
