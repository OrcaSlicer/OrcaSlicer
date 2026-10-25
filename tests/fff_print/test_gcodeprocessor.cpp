#include <catch2/catch_all.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "libslic3r/libslic3r.h"
#include "libslic3r/BeltTransform.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace Slic3r;

// Bambu firmware uses the " FEATURE: " style reserved tags, everything else the Slic3r-compatible
// "TYPE:" style, so which list applies depends on the printer kind passed in.
TEST_CASE("Reserved keyword detection follows the printer kind it is given", "[GCodeProcessor]")
{
    struct Case
    {
        const char* name;
        std::string gcode;
        bool        reserved_on_bbl;
        bool        reserved_on_non_bbl;
    };

    const auto test_case = GENERATE(values<Case>({
        {"compatible feature tag", ";TYPE:Prime tower", false, true},
        {"compatible layer tag", ";LAYER_CHANGE", false, true},
        {"bbl feature tag", "; FEATURE: Outer wall", true, false},
        {"tag shared by both lists", ";_GP_FIRST_LINE_M73_PLACEHOLDER", true, true},
        {"bbl spells this one with a leading space", ";COLOR_CHANGE", false, true},
        {"ordinary comment", "; heat the bed", false, false},
        {"not a comment at all", "G1 X10 Y10 F3000", false, false},
        // A tag counts only as the whole comment's prefix, so neither a tag mentioned mid-comment
        // nor one trailing a real command is a reserved use.
        {"tag text later in the comment", "; the TYPE:Prime tower marker", false, false},
        {"tag trailing a command", "G1 X10 ;TYPE:Prime tower", false, false},
    }));

    DYNAMIC_SECTION(test_case.name)
    {
        std::vector<std::string> tags;
        REQUIRE(GCodeProcessor::contains_reserved_tags(test_case.gcode, 5, tags, true) == test_case.reserved_on_bbl);

        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(test_case.gcode, 5, tags, false) == test_case.reserved_on_non_bbl);
    }
}

TEST_CASE("Reserved keyword detection reports every offending line", "[GCodeProcessor]")
{
    const std::string gcode = ";TYPE:Prime tower\nG1 X10\n;LAYER_CHANGE\n";

    std::vector<std::string> tags;
    REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 5, tags, false));
    REQUIRE(tags.size() == 2);
    // Reported in the order they appear, which is what makes the max_count cut-off meaningful.
    CHECK(tags[0] == "TYPE:Prime tower");
    CHECK(tags[1] == "LAYER_CHANGE");

    SECTION("the reported count is capped at max_count")
    {
        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 1, tags, false));
        CHECK(tags.size() == 1);
        CHECK(tags[0] == "TYPE:Prime tower");
    }

    SECTION("a max_count of zero still reports the first tag")
    {
        tags.clear();
        REQUIRE(GCodeProcessor::contains_reserved_tags(gcode, 0, tags, false));
        CHECK(tags.size() == 1);
    }

    SECTION("g-code with nothing reserved in it reports nothing")
    {
        tags.clear();
        CHECK_FALSE(GCodeProcessor::contains_reserved_tags("G28\n; home all axes\n", 5, tags, false));
        CHECK(tags.empty());
    }
}

namespace {

void process_gcode(const std::string &gcode, GCodeProcessorResult &result, const DynamicPrintConfig &overrides = {})
{
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
    config.apply(overrides, true);
    // s_IsBBLPrinter selects the "; FEATURE: " role tags the G-code uses.
    const bool       was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
    GCodeProcessor::s_IsBBLPrinter = true;
    ScopedTemporaryFile temp(".gcode");
    std::ofstream(temp.string()) << gcode;
    GCodeProcessor processor;
    processor.apply_config(config);
    processor.process_file(temp.string());
    result = std::move(processor.extract_result());
}

// Closed outer-wall squares, each after a fast travel and before an inner-wall move, so the processor
// records seams and inserts actual speed moves. virtual_moves adds a VG1 move after each square.
void process_squares(int squares, GCodeProcessorResult &result, bool virtual_moves = false)
{
    std::ostringstream gcode;
    gcode << "M83\nG90\n";
    for (int i = 0; i < squares; ++i) {
        gcode << "G1 X10 Y10 Z" << 0.2 * (i + 1) << " F12000\n"
              << "; FEATURE: Outer wall\n"
              << "G1 X50 Y10 E2 F3000\nG1 X50 Y50 E2\nG1 X10 Y50 E2\nG1 X10 Y10 E2\n"
              << "; FEATURE: Inner wall\n"
              << "G1 X12 Y12 E0.1\nG1 X30 Y12 E1\n";
        if (virtual_moves)
            gcode << "VG1 X20 Y30 F12000\n";
    }
    process_gcode(gcode.str(), result);
}

// Objects A and B on the first layer and A again on the second, with A's brim and support. The skirt and the
// prime tower belong to neither.
void process_two_objects(GCodeProcessorResult &result, const DynamicPrintConfig &overrides = {})
{
    std::ostringstream gcode;
    gcode << "M83\nG90\n"
          << "; CHANGE_LAYER\n; LAYER_HEIGHT: 0.2\nG1 Z0.2 F12000\n"
          << "; FEATURE: Skirt\nG1 X0 Y100 E5 F3000\n"
          << "; FEATURE: Brim\nG1 X8 Y8 F12000\nG1 X12 Y8 E1 F3000\n"
          << "; FEATURE: Support\nG1 X10 Y20 F12000\nG1 X10 Y30 E1 F3000\n"
          << "; FEATURE: Outer wall\nG1 X10 Y10 F12000\nG1 X20 Y10 E1 F3000\n"
          << "; FEATURE: Outer wall\nG1 X50 Y50 F12000\nG1 X60 Y50 E2 F3000\n"
          << "; FEATURE: Prime tower\nG1 X80 Y80 F12000\nG1 X90 Y80 E1 F3000\n"
          << "; CHANGE_LAYER\n; LAYER_HEIGHT: 0.2\nG1 Z0.4 F12000\n"
          << "; FEATURE: Outer wall\nG1 X10 Y10 F12000\nG1 X20 Y10 E1 F3000\n";
    process_gcode(gcode.str(), result, overrides);
}

// Bead centers of process_two_objects(), half the 0.2 mm layer below the nozzle.
const Vec3d a_brim(10., 8., 0.1), a_support(10., 25., 0.1), a_wall_0(15., 10., 0.1), a_wall_1(15., 10., 0.3), b_wall(55., 50., 0.1);

Vec3d center_of(const GCodeProcessorResult::ObjectMass::Sum &sum) { return sum.moment / sum.mass; }

// One filament, so each bead weighs as much as the E it was extruded with.
Vec3d weighted_center(std::initializer_list<std::pair<double, Vec3d>> beads)
{
    double mass = 0.;
    Vec3d  moment = Vec3d::Zero();
    for (const auto &[e, center] : beads) {
        mass += e;
        moment += e * center;
    }
    return moment / mass;
}

bool is_block_move(const GCodeProcessorResult::MoveVertex &move)
{
    return !move.internal_only && (move.type == EMoveType::Extrude || move.type == EMoveType::Travel);
}

} // namespace

TEST_CASE("Loaded manual tool change comments switch subsequent moves when enabled", "[GCodeProcessor][Regression]")
{
    const bool manual_filament_change = GENERATE(false, true);
    CAPTURE(manual_filament_change);
    const DynamicPrintConfig config = Test::multifilament_config(2, {
        { "gcode_flavor",           "marlin" },
        { "filament_type",          "PLA;PETG" },
        { "manual_filament_change", manual_filament_change },
    });
    const bool was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
    GCodeProcessor::s_IsBBLPrinter = false;

    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream out(temp.string());
        REQUIRE(out.is_open());
        out << "; generated by OrcaSlicer\n"
            << "G90\nM83\nT0\n"
            << "G1 X10 Y10 Z0.2 F3000\n"
            << "G1 X20 E1\n"
            << "; MANUAL_TOOL_CHANGE T1\n"
            << "G1 X30\nG1 X40 E1\nG1 X50\nG1 X60 E1\n";
        // Let process_file load the embedded DynamicPrintConfig, as the standalone viewer does.
        out << "; CONFIG_BLOCK_START\n";
        for (const std::string &key : config.keys())
            out << "; " << key << " = " << config.opt_serialize(key) << '\n';
        out << "; CONFIG_BLOCK_END\n";
        REQUIRE(out.good());
    }

    GCodeProcessor processor;
    processor.process_file(temp.string());
    const auto &result = processor.get_result();
    REQUIRE(result.filaments_count == 2);

    constexpr unsigned int tool_change_line = 7;
    size_t before = 0;
    size_t after = 0;
    for (const auto &move : result.moves) {
        if (!is_block_move(move))
            continue;
        CAPTURE(move.gcode_id, move.type);
        if (move.gcode_id < tool_change_line) {
            ++before;
            CHECK(move.extruder_id == 0);
        } else if (move.gcode_id > tool_change_line) {
            ++after;
            CHECK(move.extruder_id == (manual_filament_change ? 1 : 0));
        }
    }
    REQUIRE(before > 0);
    REQUIRE(after > 0);
}

TEST_CASE("Actual speed moves are inserted on their block's segment just before its move", "[GCodeProcessor]")
{
    // 60 squares take several planner passes, which remap the blocks kept between passes.
    const int                  squares       = GENERATE(10, 60);
    const bool                 virtual_moves = GENERATE(false, true);
    GCodeProcessorResult       result;
    process_squares(squares, result, virtual_moves);
    const auto                &moves   = result.moves;
    constexpr size_t           normal  = size_t(PrintEstimatedStatistics::ETimeMode::Normal);

    size_t inserted = 0;
    for (size_t i = 1; i < moves.size(); ++i) {
        if (!moves[i].internal_only)
            continue;
        ++inserted;
        // Inserted moves have zero time, but a VG1 block's time is written to whatever move its move_id names.
        if (!virtual_moves)
            CHECK(moves[i].time[normal] == 0.f);
        size_t block = i + 1;
        while (block < moves.size() && moves[block].internal_only)
            ++block;
        size_t previous = i - 1;
        while (previous > 0 && moves[previous].internal_only)
            --previous;
        REQUIRE(block < moves.size());
        CHECK(moves[block].gcode_id == moves[i].gcode_id);
        const Vec3f segment = moves[block].position - moves[previous].position;
        const Vec3f offset  = moves[i].position - moves[previous].position;
        CHECK(segment.cross(offset).norm() / segment.norm() < 1e-3f);
    }
    REQUIRE(inserted > 0);
}

TEST_CASE("A seam takes the actual speed of the move it follows", "[GCodeProcessor]")
{
    GCodeProcessorResult result;
    // 10 squares fit in one planner pass, so the seam's move and the block after it are timed together.
    process_squares(10, result);
    const auto                &moves  = result.moves;

    size_t seams = 0;
    for (size_t i = 1; i < moves.size(); ++i)
        if (moves[i].type == EMoveType::Seam && is_block_move(moves[i - 1])) {
            ++seams;
            CHECK_THAT(moves[i].actual_feedrate, Catch::Matchers::WithinAbs(moves[i - 1].actual_feedrate, 1e-4));
        }
    REQUIRE(seams > 0);
}

TEST_CASE("A pause keeps the moves around it in order and on their own layers", "[GCodeProcessor][Regression]")
{
    // libvgcode numbers layers from the order of the moves, so the moves around a pause must stay in layer and
    // line order. The pause vertex itself moves to the first extrusion after it and keeps its own line.
    std::ostringstream gcode;
    gcode << "M83\nG90\n";
    for (int layer = 1; layer <= 3; ++layer) {
        gcode << "; CHANGE_LAYER\n; LAYER_HEIGHT: 0.2\n";
        if (layer == 2)
            gcode << "G1 E-0.8 F3000\n; PAUSE_PRINTING\nM601\n";
        gcode << "G1 X10 Y10 F12000\nG1 Z" << 0.2 * layer << "\n";
        if (layer == 2)
            gcode << "G1 E0.8 F3000\n";
        gcode << "; FEATURE: Outer wall\n"
              << "G1 X50 Y10 E2 F3000\nG1 X50 Y50 E2\nG1 X10 Y50 E2\nG1 X10 Y10 E2\n";
    }
    GCodeProcessorResult result;
    process_gcode(gcode.str(), result);
    const auto &moves = result.moves;

    REQUIRE(result.custom_gcode_per_print_z.size() == 1);
    CHECK_THAT(result.custom_gcode_per_print_z.front().print_z, Catch::Matchers::WithinAbs(0.4, 1e-4));
    REQUIRE(std::count_if(moves.begin(), moves.end(), [](const auto &move) { return move.internal_only; }) > 0);

    const auto pause = std::find_if(moves.begin(), moves.end(), [](const auto &move) { return move.type == EMoveType::Pause_Print; });
    REQUIRE(pause != moves.end());
    CHECK(pause->layer_id == 1);
    CHECK_THAT(pause->position.z(), Catch::Matchers::WithinAbs(0.4f, 1e-4f));

    const GCodeProcessorResult::MoveVertex *previous = nullptr;
    for (const auto &move : moves) {
        if (move.type == EMoveType::Pause_Print)
            continue;
        if (previous != nullptr) {
            CAPTURE(previous->gcode_id, move.gcode_id, static_cast<int>(move.type));
            CHECK(move.layer_id >= previous->layer_id);
            CHECK(move.gcode_id >= previous->gcode_id);
        }
        previous = &move;
    }
}

TEST_CASE("Line ends of the exported G-code mark every newline in the file", "[GCodeProcessor]")
{
    struct Case
    {
        const char* name;
        bool        preheat_backtrace;
        bool        pre_heating;
    };
    const auto test_case = GENERATE(values<Case>({
        { "written by size", false, false },
        { "written by time for the preheat backtrace", true, false },
        { "rewritten by the pre-heating pass", false, true },
    }));
    INFO(test_case.name);
    DynamicPrintConfig config = Test::multifilament_config(2, {
        { "single_extruder_multi_material", 0 },
        { "ooze_prevention",                test_case.preheat_backtrace },
        { "preheat_time",                   30 },
        { "enable_pre_heating",             test_case.pre_heating },
    });
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { { "extruder", 1 } }, { { "extruder", 2 } } };
    Test::init_print({ Test::cube(20), Test::cube(20) }, print, model, config, &overrides);
    GCodeProcessorResult result;
    const std::string    gcode = Test::gcode(print, &result);
    REQUIRE((gcode.find("preheat T") != std::string::npos) == test_case.preheat_backtrace);
    REQUIRE((gcode.find(GCodeProcessor::Machine_Start_GCode_End_Tag) != std::string::npos) == test_case.pre_heating);
    REQUIRE(gcode.size() > GCodeProcessor::Output_Block_Size);

    std::vector<size_t> newline_ends;
    for (size_t i = gcode.find('\n'); i != std::string::npos; i = gcode.find('\n', i + 1))
        newline_ends.push_back(i + 1);
    REQUIRE(result.lines_ends.size() == newline_ends.size());
    const auto difference = std::mismatch(result.lines_ends.begin(), result.lines_ends.end(), newline_ends.begin());
    INFO("first difference at line " << difference.first - result.lines_ends.begin() + 1);
    CHECK(difference.first == result.lines_ends.end());
}

TEST_CASE("Reloaded moves name their lines in G-code a script rewrote in place", "[GCodeProcessor]")
{
    Print print;
    Model model;
    Test::init_print({ Test::cube(20) }, print, model);
    GCodeProcessorResult result;
    const std::string    gcode          = Test::gcode(print, &result);
    const auto           exported_moves = result.moves;

    // A script that prepends one comment and, writing in text mode on Windows, turns every LF into CRLF.
    const std::string prepended = ";EDITED\r\n";
    std::string       edited    = prepended;
    for (const char c : gcode) {
        if (c == '\n')
            edited += '\r';
        edited += c;
    }
    ScopedTemporaryFile temp(".gcode");
    save_string_file(temp.path(), edited);
    result.filename = temp.string();
    print.reload_gcode_moves(&result);

    std::vector<size_t> newline_ends;
    for (size_t i = edited.find('\n'); i != std::string::npos; i = edited.find('\n', i + 1))
        newline_ends.push_back(i + 1);
    CHECK(result.lines_ends == newline_ends);

    // Every move that came from a line now names the same line one further down.
    REQUIRE(result.moves.size() == exported_moves.size());
    const auto difference = std::mismatch(exported_moves.begin(), exported_moves.end(), result.moves.begin(),
                                          [](const auto &exported, const auto &reloaded) {
                                              return reloaded.gcode_id == (exported.gcode_id == 0 ? 0 : exported.gcode_id + 1);
                                          });
    INFO("first difference at move " << difference.first - exported_moves.begin());
    CHECK(difference.first == exported_moves.end());
}

TEST_CASE("Rewritten G-code that cannot be re-read keeps the moves and hides the G-code window", "[GCodeProcessor]")
{
    Print print;
    Model model;
    Test::init_print({ Test::cube(20) }, print, model);
    GCodeProcessorResult result;
    const std::string    gcode          = Test::gcode(print, &result);
    const auto           exported_moves = result.moves;

    // A script that strips the trailing config block, which the G-code reader needs.
    const size_t config_block = gcode.find("; CONFIG_BLOCK_START");
    REQUIRE(config_block != std::string::npos);
    ScopedTemporaryFile temp(".gcode");
    save_string_file(temp.path(), gcode.substr(0, config_block));
    result.filename = temp.string();
    print.reload_gcode_moves(&result);

    CHECK(result.lines_ends.empty());
    REQUIRE(result.moves.size() == exported_moves.size());
    CHECK(result.moves.back().gcode_id == exported_moves.back().gcode_id);
}

TEST_CASE("The plate's center of mass takes every extrusion of G-code without a print behind it", "[GCodeProcessor]")
{
    GCodeProcessorResult result;
    process_two_objects(result);

    CHECK(result.object_masses.empty());
    CHECK(result.body_masses.empty());
    const GCodeProcessorResult::ObjectMass &plate = result.plate_mass;
    REQUIRE(plate.printed_up_to_layer.size() == 2);
    CHECK_THAT((center_of(plate.printed_up_to_layer.front()) -
                weighted_center({ { 1., a_brim }, { 1., a_support }, { 1., a_wall_0 }, { 2., b_wall } })).norm(),
               Catch::Matchers::WithinAbs(0., 1e-5));
    CHECK_THAT((center_of(plate.printed_up_to_layer.back()) -
                weighted_center({ { 1., a_brim }, { 1., a_support }, { 1., a_wall_0 }, { 2., b_wall }, { 1., a_wall_1 } })).norm(),
               Catch::Matchers::WithinAbs(0., 1e-5));

    // Each bead weighs its volume at the default density and spreads along its move, (a^2 + ab + b^2) / 3 for one from
    // a to b: the brim from x 8 to 12 at y 8, the support at x 10 from y 20 to 30, A's walls from x 10 to 20 at y 10 and
    // B's from x 50 to 60 at y 50 with twice the filament, all at z 0.1 but A's second wall at 0.3.
    const GCodeProcessorResult::ObjectMass::Sum total = plate.total();
    CHECK_THAT(total.mass / total.volume, Catch::Matchers::WithinRel(double(DEFAULT_FILAMENT_DENSITY), 1e-6));
    const Vec3d second = total.second / total.mass;
    CHECK_THAT(second.x(), Catch::Matchers::WithinRel((304. / 3. + 100. + 2. * 700. / 3. + 2. * 9100. / 3.) / 6., 1e-6));
    CHECK_THAT(second.y(), Catch::Matchers::WithinRel((64. + 1900. / 3. + 2. * 100. + 2. * 2500.) / 6., 1e-6));
    CHECK_THAT(second.z(), Catch::Matchers::WithinRel((5. * 0.01 + 0.09) / 6., 1e-5));
    // Each bead runs along one axis, so x y is the fixed coordinate times the middle of the other.
    CHECK_THAT(total.cross.x() / total.mass,
               Catch::Matchers::WithinRel((8. * 10. + 10. * 25. + 2. * 10. * 15. + 2. * 50. * 55.) / 6., 1e-6));
    // The beads' center lines, brim and support included, from the first layer's bottom to the second's top.
    CHECK_THAT((plate.box.min - Vec3d(8., 8., 0.)).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
    CHECK_THAT((plate.box.max - Vec3d(60., 50., 0.4)).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
    CHECK_FALSE(plate.belt_box.defined);
}

TEST_CASE("A belt print's mass keeps its weight and takes its center, spread and box to the model's frame", "[GCodeProcessor]")
{
    DynamicPrintConfig belt;
    belt.set_deserialize_strict({ { "belt_printer", 1 }, { "belt_slice_rotation", "x" }, { "belt_slice_rotation_angle", 45 } });
    GCodeProcessorResult result;
    process_two_objects(result, belt);
    FullPrintConfig belt_config;
    belt_config.apply(belt, true);
    // The back-transform shears; the preview moves it as well, to lay the toolpaths on the model.
    const Transform3d trafo = Geometry::translation_transform(Vec3d(5., -20., 3.)) *
                              BeltTransformPipeline::build_gcode_back_transform(belt_config);
    REQUIRE_FALSE(trafo.linear().isDiagonal());
    const GCodeProcessorResult::ObjectMass &plate  = result.plate_mass;
    GCodeProcessorResult::ObjectMass        mapped = plate;
    mapped.transform(trafo);

    const auto spread = [](const GCodeProcessorResult::ObjectMass::Sum &sum) {
        const Vec3d center = center_of(sum);
        return Matrix3d(sum.second_moments() / sum.mass - center * center.transpose());
    };
    REQUIRE(mapped.printed_up_to_layer.size() == plate.printed_up_to_layer.size());
    for (size_t layer = 0; layer < plate.printed_up_to_layer.size(); ++layer) {
        const GCodeProcessorResult::ObjectMass::Sum &before = plate.printed_up_to_layer[layer];
        const GCodeProcessorResult::ObjectMass::Sum &after  = mapped.printed_up_to_layer[layer];
        CHECK_THAT(after.mass, Catch::Matchers::WithinRel(before.mass, 1e-12));
        CHECK_THAT(after.volume, Catch::Matchers::WithinRel(before.volume, 1e-12));
        CHECK_THAT((center_of(after) - trafo * center_of(before)).norm(), Catch::Matchers::WithinAbs(0., 1e-9));
        CHECK_THAT((spread(after) - trafo.linear() * spread(before) * trafo.linear().transpose()).norm(),
                   Catch::Matchers::WithinAbs(0., 1e-6));
    }

    // The mapped ends of the beads' center lines at the bottom and the top of their layers: the brim, the support, A's and B's
    // walls on the first layer and A's on the second.
    BoundingBoxf3 beads;
    for (const auto &[from, to, bottom] : { std::tuple(Vec2d(8., 8.), Vec2d(12., 8.), 0.), std::tuple(Vec2d(10., 20.), Vec2d(10., 30.), 0.),
                                            std::tuple(Vec2d(10., 10.), Vec2d(20., 10.), 0.), std::tuple(Vec2d(50., 50.), Vec2d(60., 50.), 0.),
                                            std::tuple(Vec2d(10., 10.), Vec2d(20., 10.), 0.2) })
        for (const Vec2d &end : { from, to })
            for (const double z : { bottom, bottom + 0.2 })
                beads.merge(trafo * Vec3d(end.x(), end.y(), z));
    REQUIRE(mapped.box.defined);
    CHECK_THAT((mapped.box.min - beads.min).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
    CHECK_THAT((mapped.box.max - beads.max).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
}

TEST_CASE("Each sliced cube's center of mass is its center, and the brim lowers the plate's printed one", "[GCodeProcessor]")
{
    const bool copies = GENERATE(false, true);
    INFO((copies ? "two copies of one cube" : "two cubes"));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "skirt_loops", 0 }, { "brim_type", "outer_only" }, { "brim_width", 5 }, { "combine_brims", 0 } });
    std::vector<TriangleMesh> cubes{ Test::cube(20) };
    if (!copies)
        cubes.emplace_back(Test::cube(20));
    Print print;
    Model model;
    Test::init_print(std::move(cubes), print, model, config, nullptr, true, copies ? 2 : 1);
    GCodeProcessorResult result;
    Test::gcode(print, &result);

    CHECK(result.body_masses.empty());
    REQUIRE(result.object_masses.size() == 2);
    for (const ModelObject *object : model.objects)
        for (size_t instance = 0; instance < object->instances.size(); ++instance) {
            const Vec3d center = object->instance_bounding_box(instance).center();
            const auto  mass   = std::min_element(result.object_masses.begin(), result.object_masses.end(), [&center](const auto &l, const auto &r) {
                return (center_of(l.total()) - center).squaredNorm() < (center_of(r.total()) - center).squaredNorm();
            });
            // Off the center only by the infill's alignment and the top and bottom shells.
            const Vec3d part = center_of(mass->total());
            CHECK_THAT(part.x(), Catch::Matchers::WithinAbs(center.x(), 0.5));
            CHECK_THAT(part.y(), Catch::Matchers::WithinAbs(center.y(), 0.5));
            CHECK_THAT(part.z(), Catch::Matchers::WithinAbs(center.z(), 1.));
            // The outer walls' center lines run half a line inside the cube's sides, of copies touching each other too.
            const BoundingBoxf3 box = object->instance_bounding_box(instance);
            for (int axis = 0; axis < 3; ++axis) {
                CHECK_THAT(mass->box.min[axis], Catch::Matchers::WithinAbs(box.min[axis], 0.3));
                CHECK_THAT(mass->box.max[axis], Catch::Matchers::WithinAbs(box.max[axis], 0.3));
            }
        }
    GCodeProcessorResult::ObjectMass::Sum objects;
    for (const GCodeProcessorResult::ObjectMass &object : result.object_masses)
        objects.add(object.total());
    const GCodeProcessorResult::ObjectMass::Sum plate = result.plate_mass.total();
    CHECK(plate.mass > objects.mass);
    CHECK(center_of(plate).z() < center_of(objects).z());
}

TEST_CASE("Each cube's raft is its support, centered below it", "[GCodeProcessor]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "raft_layers", 3 } });
    Print print;
    Model model;
    Test::init_print({ Test::cube(20), Test::cube(20) }, print, model, config);
    GCodeProcessorResult result;
    Test::gcode(print, &result);

    REQUIRE(result.support_masses.size() == 2);
    for (size_t i = 0; i < 2; ++i) {
        const GCodeProcessorResult::ObjectMass::Sum support = result.support_masses[i].total();
        const Vec3d                                 object  = center_of(result.object_masses[i].total());
        REQUIRE(support.mass > 0.);
        CHECK_THAT(center_of(support).x(), Catch::Matchers::WithinAbs(object.x(), 1.));
        CHECK_THAT(center_of(support).y(), Catch::Matchers::WithinAbs(object.y(), 1.));
        CHECK(center_of(support).z() < 1.);
    }
}

TEST_CASE("A spiral vase cube counts all its extrusions, rising through each layer", "[GCodeProcessor]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "spiral_mode", 1 }, { "wall_loops", 1 },
                                    { "top_shell_layers", 0 }, { "sparse_infill_density", 0 } });
    Print print;
    Model model;
    Test::init_print({ Test::cube(20) }, print, model, config);
    GCodeProcessorResult result;
    Test::gcode(print, &result);

    REQUIRE(result.object_masses.size() == 1);
    CHECK_THAT(result.object_masses.front().total().mass, Catch::Matchers::WithinRel(result.plate_mass.total().mass, 1e-6));
}

TEST_CASE("Each separate part of an assembly gets its center of mass, overlapping parts one", "[GCodeProcessor]")
{
    const bool overlapping = GENERATE(false, true);
    // Separated infills finds the bodies first, which the G-code export then takes.
    const bool separated = GENERATE(false, true);
    INFO((overlapping ? "overlapping parts" : "separate parts") << (separated ? ", separated infills" : ""));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "separated_infills", separated ? 1 : 0 } });
    TriangleMesh first = make_cube(20, 20, 20);
    TriangleMesh second = make_cube(20, 20, 20);
    first.translate(50, 50, 0);
    second.translate(overlapping ? 60 : 90, 50, 0);
    Print print;
    Model model;
    Test::init_print({ first }, print, model, config, nullptr, false);
    model.objects.front()->add_volume(std::move(second), ModelVolumeType::MODEL_PART, false);
    print.apply(model, config);
    GCodeProcessorResult result;
    Test::gcode(print, &result);

    REQUIRE(result.object_masses.size() == 1);
    CHECK(result.object_masses.front().assembly);
    // One body is the object itself.
    if (overlapping) {
        CHECK(result.body_masses.empty());
        return;
    }
    REQUIRE(result.body_masses.size() == 2);
    CHECK(print.objects().front()->separated_body_bboxes().size() == (separated ? 2 : 0));
    const ModelObject &object = *model.objects.front();
    for (const ModelVolume *volume : object.volumes) {
        const Vec3d center = volume->mesh().transformed_bounding_box(object.instances.front()->get_matrix() * volume->get_matrix()).center();
        const auto  body   = std::min_element(result.body_masses.begin(), result.body_masses.end(), [&center](const auto &l, const auto &r) {
            return (center_of(l.total()) - center).squaredNorm() < (center_of(r.total()) - center).squaredNorm();
        });
        const Vec3d part = center_of(body->total());
        CHECK_THAT(part.x(), Catch::Matchers::WithinAbs(center.x(), 0.5));
        CHECK_THAT(part.y(), Catch::Matchers::WithinAbs(center.y(), 0.5));
        CHECK_THAT(part.z(), Catch::Matchers::WithinAbs(center.z(), 1.));
    }
}

TEST_CASE("Each extrusion weighs its filament's density", "[GCodeProcessor]")
{
    // Two like cubes, the second's filament three times as dense.
    DynamicPrintConfig config = Test::multifilament_config(2, { { "filament_density", "1,3" }, { "skirt_loops", 0 }, { "brim_type", "no_brim" } });
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { { "extruder", 1 } }, { { "extruder", 2 } } };
    Print print;
    Model model;
    Test::init_print({ Test::cube(20), Test::cube(20) }, print, model, config, &overrides);
    GCodeProcessorResult result;
    Test::gcode(print, &result);

    REQUIRE(result.object_masses.size() == 2);
    std::vector<const GCodeProcessorResult::ObjectMass *> masses;
    for (const ModelObject *object : model.objects) {
        const Vec3d center = object->instance_bounding_box(0).center();
        masses.emplace_back(&*std::min_element(result.object_masses.begin(), result.object_masses.end(), [&center](const auto &l, const auto &r) {
            return (center_of(l.total()) - center).squaredNorm() < (center_of(r.total()) - center).squaredNorm();
        }));
    }
    CHECK_THAT(masses[1]->total().mass / masses[0]->total().mass, Catch::Matchers::WithinRel(3., 0.02));
    // The plate's center lies three quarters of the way to the dense cube.
    const Vec3d plate = center_of(result.plate_mass.total());
    const Vec3d light = center_of(masses[0]->total());
    const Vec3d dense = center_of(masses[1]->total());
    CHECK_THAT((plate - light).dot(dense - light) / (dense - light).squaredNorm(), Catch::Matchers::WithinAbs(0.75, 0.01));
}

namespace {

// A printer with `nozzles` nozzles over a 200 mm square bed: nozzle `filament_1_nozzle` reaches only
// `nozzle_area`, the others the whole bed. A 20 mm cube centered on `center` prints with filament 1,
// mapped to that nozzle and printable on every nozzle.
void slice_cube_in_nozzle_area(const Vec2ds &nozzle_area, const Vec2d &center, GCodeProcessorResult &result, int nozzles = 2,
                               int filament_1_nozzle = 1)
{
    const auto per_nozzle = [nozzles](const std::string &value) {
        std::string values = value;
        for (int i = 1; i < nozzles; ++i)
            values += "," + value;
        return values;
    };
    std::string extruder_ids = "1";
    for (int i = 2; i <= nozzles; ++i)
        extruder_ids += "," + std::to_string(i);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter",           per_nozzle("0.4") },
        { "printer_extruder_id",       extruder_ids },
        { "printer_extruder_variant",  per_nozzle("Direct Drive Standard") },
        { "extruder_offset",           per_nozzle("0x0") },
        { "printable_area",            "0x0,200x0,200x200,0x200" },
        { "printable_height",          100 },
        { "extruder_printable_height", per_nozzle("100") },
        // One filament, so one purge volume per nozzle, as the GUI and the CLI size it.
        { "flush_volumes_matrix",      per_nozzle("0") },
        { "skirt_loops",               0 },
        { "brim_type",                 "no_brim" },
        { "layer_change_gcode",        "G92 E0\n" }, // validate() relative-E reset
        { "filament_map_mode",         "Manual" },
        { "filament_map",              filament_1_nozzle },
        { "filament_printable",        -1 },
    });
    // Other per-nozzle printer options, at their defaults.
    for (const char *key : { "extruder_type", "nozzle_volume_type", "min_layer_height", "max_layer_height", "extruder_max_nozzle_count" })
        dynamic_cast<ConfigOptionVectorBase *>(config.option(key))->resize(nozzles);
    const Vec2ds bed { Vec2d(0., 0.), Vec2d(200., 0.), Vec2d(200., 200.), Vec2d(0., 200.) };
    std::vector<Vec2ds> areas(nozzles, bed);
    areas[filament_1_nozzle - 1] = nozzle_area;
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups(areas));

    TriangleMesh mesh = Test::cube(20);
    mesh.translate(float(center.x() - 10.), float(center.y() - 10.), 0.f);
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ std::move(mesh) }, print, model, config, nullptr, false);
    REQUIRE_THAT(print.validate().string, Catch::Matchers::Equals(std::string()));
    Test::gcode(print, &result);
}

// A nozzle's area inset from the bed on one side, on two adjacent sides (leaving an L-shaped
// unprintable region), on three (a U) and on all four (a frame). Each contains the bed center and
// has an X 150 edge; all but the first also have a Y 50 edge, meeting it in an inner corner.
Vec2ds inset_nozzle_area(int sides)
{
    switch (sides) {
    case 1: return { Vec2d(0., 0.), Vec2d(150., 0.), Vec2d(150., 200.), Vec2d(0., 200.) };
    case 2: return { Vec2d(0., 50.), Vec2d(150., 50.), Vec2d(150., 200.), Vec2d(0., 200.) };
    case 3: return { Vec2d(50., 50.), Vec2d(150., 50.), Vec2d(150., 200.), Vec2d(50., 200.) };
    default: return { Vec2d(50., 50.), Vec2d(150., 50.), Vec2d(150., 150.), Vec2d(50., 150.) };
    }
}

// Runs the post-slice area check alone on one object whose filament 1 extrudes through `path`, in bed
// coordinates, on a plate whose origin is `plate_origin`. Filament 1 maps to `filament_1_nozzle`;
// nozzle 1 reaches only `nozzle_1_area` of a 200 mm square bed and nozzle 2 all of it.
void check_path_in_nozzle_area(const Vec2ds &nozzle_1_area, const Vec2ds &path, const Vec2d &plate_origin, GCodeProcessorResult &result,
                               int filament_1_nozzle = 1)
{
    const Vec2ds bed { Vec2d(0., 0.), Vec2d(200., 0.), Vec2d(200., 200.), Vec2d(0., 200.) };
    // Built as Print::get_extruder_unprintable_polygons() builds them.
    const std::vector<Polygons> unprintable_areas { diff(Polygon::new_scale(bed), Polygon::new_scale(nozzle_1_area)), Polygons() };

    GCodeProcessor processor;
    processor.set_xy_offset(plate_origin.x(), plate_origin.y());
    for (const Vec2d &point : path) {
        GCodeProcessorResult::MoveVertex move;
        move.type            = EMoveType::Extrude;
        move.extrusion_role  = erExternalPerimeter;
        move.extruder_id     = 0; // filament 1
        move.object_label_id = 1;
        // The processor stores moves with the plate origin added.
        move.position = Vec3f(float(point.x() + plate_origin.x()), float(point.y() + plate_origin.y()), 0.2f);
        move.print_z  = 0.2f;
        processor.result().moves.emplace_back(move);
    }
    processor.check_multi_extruder_gcode_valid(2, bed, 100., {}, unprintable_areas, { 100., 100. }, { filament_1_nozzle }, { {}, {} });
    result = std::move(processor.extract_result());
}

// Orca: Attach the same standalone inline marker emitted by GCode.cpp without coupling tests to
// comment spacing. Callers may retain an existing description before the added semicolon-delimited field.
std::string bind_overhang_profile(std::string arc,
    GCodeProcessor::ETags marker = GCodeProcessor::ETags::Overhang_Arc_Apply)
{
    assert(!arc.empty() && arc.back() == '\n');
    arc.insert(arc.size() - 1, ";" + GCodeProcessor::reserved_tag(marker));
    return arc;
}

} // namespace

TEST_CASE("A path inside its nozzle's printable area passes the area check however many sides the area is inset on", "[GCodeProcessor][Regression]")
{
    // A cube at the bed center, or at X 125-145, Y 55-75: 5 mm inside the X 150 and Y 50 edges, in the
    // inner corner where they meet. The area inset on one side has no Y 50 edge, so no corner.
    const auto [sides, in_corner] = GENERATE(table<int, bool>(
        { { 1, false }, { 2, false }, { 2, true }, { 3, false }, { 3, true }, { 4, false }, { 4, true } }));
    const Vec2d center = in_corner ? Vec2d(135., 65.) : Vec2d(100., 100.);
    INFO("area inset on " << sides << " side(s), cube " << (in_corner ? "in the inner corner" : "at the bed center"));
    GCodeProcessorResult result;
    slice_cube_in_nozzle_area(inset_nozzle_area(sides), center, result);

    CHECK((result.gcode_check_result.error_code & 1) == 0);
    CHECK(result.gcode_check_result.print_area_error_infos.empty());
    // Nor is nozzle 1 marked as unable to print filament 1.
    REQUIRE_FALSE(result.limit_filament_maps.empty());
    CHECK((result.limit_filament_maps[0] & 1) == 0);
}

TEST_CASE("A path leaving its nozzle's printable area fails the area check however many sides the area is inset on", "[GCodeProcessor][Regression]")
{
    const int sides = GENERATE(1, 2, 3, 4);
    INFO("area inset on " << sides << " side(s)");
    // X 165-185: on the bed, past the area's X 150 edge in every shape.
    GCodeProcessorResult result;
    slice_cube_in_nozzle_area(inset_nozzle_area(sides), Vec2d(175., 100.), result);

    CHECK((result.gcode_check_result.error_code & 1) == 1);
    REQUIRE(result.gcode_check_result.print_area_error_infos.count(0) == 1);
    CHECK_FALSE(result.gcode_check_result.print_area_error_infos.at(0).empty());
    REQUIRE_FALSE(result.limit_filament_maps.empty());
    CHECK((result.limit_filament_maps[0] & 1) == 1);
}

TEST_CASE("A path flush against its nozzle's printable area edge fails the area check and one 0.1 mm inside it passes", "[GCodeProcessor][Regression]")
{
    // Nozzle 1 reaches X 0-150, Y 50-200, leaving an L-shaped unprintable region whose bounding box is
    // the whole bed. The check tests path centerlines, not the width of the extruded line.
    const Vec2ds nozzle_1_area = inset_nozzle_area(2);
    const bool   x_edge        = GENERATE(true, false);
    const bool   flush         = GENERATE(true, false);
    // A 20 mm square with one side along the edge, or that side alone, whose bounding box has no area.
    const bool line = GENERATE(false, true);
    // A plate origin shifts the path but not the areas, and the check must undo the shift. Plates
    // after the first sit at positive X and negative Y.
    const bool  first_plate  = GENERATE(true, false);
    const Vec2d plate_origin = first_plate ? Vec2d(0., 0.) : Vec2d(240., -240.);
    INFO((line ? "line " : "square ") << (flush ? "flush against" : "0.1 mm inside") << " the " << (x_edge ? "X 150" : "Y 50")
                                      << " edge, plate origin at " << plate_origin.x() << ", " << plate_origin.y());
    const double gap    = flush ? 0. : 0.1;
    const Vec2d  a      = x_edge ? Vec2d(150. - gap, 100.) : Vec2d(100., 50. + gap);
    const Vec2d  b      = x_edge ? Vec2d(150. - gap, 120.) : Vec2d(120., 50. + gap);
    const Vec2d  inward = x_edge ? Vec2d(-20., 0.) : Vec2d(0., 20.);
    const Vec2ds path   = line ? Vec2ds{ a, b } : Vec2ds{ a, b, Vec2d(b + inward), Vec2d(a + inward) };

    GCodeProcessorResult result;
    check_path_in_nozzle_area(nozzle_1_area, path, plate_origin, result);

    const int expected = flush ? 1 : 0;
    CHECK((result.gcode_check_result.error_code & 1) == expected);
    REQUIRE_FALSE(result.limit_filament_maps.empty());
    CHECK((result.limit_filament_maps[0] & 1) == expected);
}

TEST_CASE("A filament mapped to no nozzle has no errors listed under a nozzle", "[GCodeProcessor][Regression]")
{
    // A filament map entry of 0 names no nozzle. This path, past the bed's X 200 edge, would otherwise
    // be listed under nozzle index -1, which the error message uses to look up the nozzle's name.
    const Vec2ds path { Vec2d(210., 100.), Vec2d(230., 100.), Vec2d(230., 120.), Vec2d(210., 120.) };
    GCodeProcessorResult result;
    check_path_in_nozzle_area(inset_nozzle_area(2), path, Vec2d(0., 0.), result, 0);

    CHECK(result.gcode_check_result.print_area_error_infos.count(-1) == 0);
}

TEST_CASE("Nozzles up to the 64th are marked as unable to print a filament they can't", "[GCodeProcessor][Regression]")
{
    // 64 nozzles over a 200 mm square bed. Filament 1 maps to one of them and prints a 20 mm square
    // at X 160-180, Y 100-120, Z 10, which that nozzle alone can't print.
    const int nozzle = GENERATE(1, 31, 32, 63); // 0-based
    CAPTURE(nozzle);
    const Vec2ds bed { Vec2d(0., 0.), Vec2d(200., 0.), Vec2d(200., 200.), Vec2d(0., 200.) };
    std::vector<Polygons>      unprintable_areas(MAXIMUM_EXTRUDER_NUMBER);
    std::vector<double>        printable_heights(MAXIMUM_EXTRUDER_NUMBER, 100.);
    std::vector<std::set<int>> unprintable_filament_types(MAXIMUM_EXTRUDER_NUMBER);
    int                        expected_error = 0;
    SECTION("it can't reach the square")
    {
        const Vec2ds nozzle_area { Vec2d(0., 0.), Vec2d(150., 0.), Vec2d(150., 200.), Vec2d(0., 200.) };
        unprintable_areas[nozzle] = diff(Polygon::new_scale(bed), Polygon::new_scale(nozzle_area));
        expected_error            = 1;
    }
    SECTION("the square is above its printable height")
    {
        printable_heights[nozzle] = 5.;
        expected_error            = 1 << 1;
    }
    SECTION("it can't print the filament's type") { unprintable_filament_types[nozzle] = { 0 }; }

    GCodeProcessor processor;
    for (const Vec2d &point : { Vec2d(160., 100.), Vec2d(180., 100.), Vec2d(180., 120.), Vec2d(160., 120.) }) {
        GCodeProcessorResult::MoveVertex move;
        move.type            = EMoveType::Extrude;
        move.extrusion_role  = erExternalPerimeter;
        move.extruder_id     = 0; // filament 1
        move.object_label_id = 1;
        move.position        = Vec3f(float(point.x()), float(point.y()), 10.f);
        move.print_z         = 10.f;
        processor.result().moves.emplace_back(move);
    }
    processor.check_multi_extruder_gcode_valid(int(MAXIMUM_EXTRUDER_NUMBER), bed, 100., {}, unprintable_areas, printable_heights,
                                               { nozzle + 1 }, unprintable_filament_types);
    const GCodeProcessorResult &result = processor.result();

    CHECK(result.gcode_check_result.error_code == expected_error);
    REQUIRE(result.limit_filament_maps.size() == 1);
    CHECK(result.limit_filament_maps[0] == uint64_t(1) << nozzle);
}

TEST_CASE("A cube on the 64th nozzle of a 64-nozzle printer slices, and fails the area check only outside that nozzle's area", "[GCodeProcessor][Regression]")
{
    // Filament 1 maps to nozzle 64, which reaches X 0-150. The cube sits at the bed center, or at X 165-185.
    const int  nozzles = 64;
    const bool outside = GENERATE(false, true);
    INFO("cube " << (outside ? "outside" : "inside") << " nozzle 64's area");
    GCodeProcessorResult result;
    slice_cube_in_nozzle_area(inset_nozzle_area(1), outside ? Vec2d(175., 100.) : Vec2d(100., 100.), result, nozzles, nozzles);

    const int expected = outside ? 1 : 0;
    CHECK((result.gcode_check_result.error_code & 1) == expected);
    CHECK(result.gcode_check_result.print_area_error_infos.count(nozzles - 1) == size_t(expected));
    REQUIRE(result.limit_filament_maps.size() == 1);
    CHECK(result.limit_filament_maps[0] == uint64_t(expected) << (nozzles - 1));
}

TEST_CASE("Line overhang profiles color local spans while preserving processor moves",
          "[GCodeProcessor][Overhang][Regression]")
{
    const bool bbl = GENERATE(false, true);
    const bool reverse = GENERATE(false, true);
    const bool interior = GENERATE(false, true);
    // Orca: Restore the process-wide tag dialect so randomized test ordering cannot leak it.
    const bool       was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
    GCodeProcessor::s_IsBBLPrinter = bbl;
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    const std::string setup = "G90\nM83\nT0\nG1 X0 Y0 Z0.2 F600\n" +
        tag(GCodeProcessor::ETags::Role, ExtrusionEntity::role_to_string(erSolidInfill)) +
        tag(GCodeProcessor::ETags::Height, "0.2") + tag(GCodeProcessor::ETags::Width, "0.4") +
        tag(GCodeProcessor::ETags::Overhang, "100");
    const std::string line = "G1 X8 Y4 Z0.6 E1 F3600 ; solid infill\n";
    GCodeProcessor baseline;
    baseline.apply_config(FullPrintConfig());
    baseline.initialize_result_moves();
    baseline.process_buffer(setup + "M106 S128\nG1 F1800\n" + line);
    baseline.finalize(false);
    GCodeProcessor profiled;
    profiled.apply_config(FullPrintConfig());
    profiled.initialize_result_moves();
    profiled.process_buffer(setup + tag(GCodeProcessor::ETags::Overhang_Line, interior ? "4,0,0,100" :
        (reverse ? "4,0,0,0" : "4,0,100,0")));
    profiled.process_buffer(tag(GCodeProcessor::ETags::Overhang_Line, interior ? "4,2,0,0" :
        (reverse ? "4,2,0,100" : "4,2,0,0")) + "M106 S128\nG1 F1800\n" +
        bind_overhang_profile(line, GCodeProcessor::ETags::Overhang_Line_Apply));
    profiled.finalize(false);
    const auto &result = profiled.get_result();
    const auto &original = baseline.get_result().moves;
    REQUIRE(result.overhang_line_profiles.size() == 1);
    REQUIRE(result.moves.size() == original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        CHECK_THAT((result.moves[i].position - original[i].position).norm(), Catch::Matchers::WithinAbs(0.0, 1e-6));
        CHECK_THAT(result.moves[i].delta_extruder, Catch::Matchers::WithinAbs(original[i].delta_extruder, 1e-6));
        CHECK_THAT(result.moves[i].feedrate, Catch::Matchers::WithinAbs(original[i].feedrate, 1e-6));
        CHECK_THAT(result.moves[i].actual_feedrate, Catch::Matchers::WithinAbs(original[i].actual_feedrate, 1e-6));
        CHECK_THAT(result.moves[i].fan_speed, Catch::Matchers::WithinAbs(original[i].fan_speed, 1e-6));
        for (size_t mode = 0; mode < result.moves[i].time.size(); ++mode)
            CHECK_THAT(result.moves[i].time[mode], Catch::Matchers::WithinAbs(original[i].time[mode], 1e-6));
    }
    const auto mode = PrintEstimatedStatistics::ETimeMode::Normal;
    CHECK_THAT(profiled.get_time(mode), Catch::Matchers::WithinAbs(baseline.get_time(mode), 1e-6));
    const size_t unsupported_cell = interior ? 1 : (reverse ? 3 : 0);
    double covered_length = 0.0;
    double unsupported_length = 0.0;
    std::array<double, 2> preview_time{};
    result.for_each_preview_move([&](const auto &prev, const auto &curr, bool) {
        for (size_t i = 0; i < preview_time.size(); ++i)
            preview_time[i] += curr.time[i];
        if (curr.type != EMoveType::Extrude)
            return;
        const double middle = 0.5 * (prev.position.x() + curr.position.x());
        const bool unsupported = middle >= 2.0 * unsupported_cell && middle < 2.0 * (unsupported_cell + 1);
        CHECK_THAT(curr.overhang_percentage, Catch::Matchers::WithinAbs(unsupported ? 100.0 : 0.0, 1e-6));
        const double length = (curr.position - prev.position).norm();
        covered_length += length;
        if (unsupported)
            unsupported_length += length;
    });
    const double length = Vec3d(8.0, 4.0, 0.4).norm();
    CHECK_THAT(covered_length, Catch::Matchers::WithinAbs(length, 1e-5));
    CHECK_THAT(unsupported_length, Catch::Matchers::WithinAbs(length / 4.0, 1e-5));
    for (size_t i = 0; i < preview_time.size(); ++i) {
        double total = 0.0;
        for (const auto &move : original)
            total += move.time[i];
        CHECK_THAT(preview_time[i], Catch::Matchers::WithinAbs(total, 1e-5));
    }
    GCodeProcessorResult copied;
    copied = result;
    REQUIRE(copied.overhang_line_profiles.size() == 1);
    for (size_t i = 0; i < 4; ++i)
        CHECK_THAT(copied.overhang_line_profiles.front().percentages[i],
            Catch::Matchers::WithinAbs(result.overhang_line_profiles.front().percentages[i], 1e-6));
    copied.reset();
    CHECK(copied.overhang_line_profiles.empty());
}

TEST_CASE("Line overhang profiles reject incomplete data and unrelated motions",
          "[GCodeProcessor][Overhang][Regression]")
{
    const std::string data = GENERATE("", "4,0", "1,0,100", "65537,0,0", "4,1,0,100,0", "4,0,0,nan,0,0",
        "4,0,0,inf,0,0", "4,0,0,1001,0,0", "4,0,0,-1,0,0", "4,0,0,100junk,0,0", "4,0,0,100,0,0,", "4,0,0,100");
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    GCodeProcessor processor;
    processor.apply_config(FullPrintConfig());
    processor.process_buffer("G90\nM83\nT0\nG1 Z0.2\n" + tag(GCodeProcessor::ETags::Overhang, "33") +
        tag(GCodeProcessor::ETags::Overhang_Line, data) +
        bind_overhang_profile("G1 X8 E1 F600\n", GCodeProcessor::ETags::Overhang_Line_Apply));
    CHECK(processor.get_result().overhang_line_profiles.empty());
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(33.0, 1e-6));
    const std::string complete = tag(GCodeProcessor::ETags::Overhang_Line, "4,0,0,100,0,0");
    processor.process_buffer(complete + "G1 X9 E0.1 ; mentions OVERHANG_LINE_APPLY here\n" +
        bind_overhang_profile("G1 X10 E0.1\n", GCodeProcessor::ETags::Overhang_Line_Apply));
    CHECK(processor.get_result().overhang_line_profiles.empty());
    processor.process_buffer(complete + "G3 X11 Y1 I0 J1 E0.1\n" +
        bind_overhang_profile("G1 X12 E0.1\n", GCodeProcessor::ETags::Overhang_Line_Apply));
    CHECK(processor.get_result().overhang_line_profiles.empty());
    processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, "4,0,0,100,0,0") +
        bind_overhang_profile("G1 X13 E0.1\n", GCodeProcessor::ETags::Overhang_Line_Apply));
    CHECK(processor.get_result().overhang_line_profiles.empty());
    processor.process_buffer(complete);
    processor.reset();
    processor.apply_config(FullPrintConfig());
    processor.process_buffer("G90\nM83\nT0\nG1 Z0.2\n" +
        bind_overhang_profile("G1 X8 E1 F600\n", GCodeProcessor::ETags::Overhang_Line_Apply));
    CHECK(processor.get_result().overhang_line_profiles.empty());
}

// Orca: Both firmware tessellations retain their original move count and timing inputs. A monotone
// profile has an exact local maximum at one segment endpoint, independent of its geometric direction.
TEST_CASE("Overhang arc profiles color local segments without changing motion", "[GCodeProcessor][Overhang][ArcFitting][Regression]")
{
    const auto flavor = GENERATE(gcfMarlinFirmware, gcfKlipper);
    const bool clockwise = GENERATE(false, true);
    const bool decreasing = GENERATE(false, true);
    FullPrintConfig config;
    config.gcode_flavor.value = flavor;
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    const std::string setup = "G90\nM83\nT0\nG1 X10 Y0 Z0.2 F600\n" +
        tag(GCodeProcessor::ETags::Height, "0.2") + tag(GCodeProcessor::ETags::Width, "0.4") +
        tag(GCodeProcessor::ETags::Overhang_Z_Distance, "0.2") + tag(GCodeProcessor::ETags::Overhang, "100");
    const std::string arc = std::string(clockwise ? "G2" : "G3") + " X0 Y10 I-10 J0 E1 F600 ; arc description\n";
    const std::string marked_arc = bind_overhang_profile(arc);
    // Orca: Arbitrary non-motion commands may appear between chunks and the explicitly marked arc;
    // they require no whitelist. A lower-case Klipper command also exercises case-insensitive handling.
    const std::string control = "M106 S128\nM73 P50\nM107\nG4 P1\nM104 S200\nM204 S1000\nM400\n;VM104 S200\n" +
        std::string(flavor == gcfKlipper ?
            "set_velocity_limit SQUARE_CORNER_VELOCITY=5\nSET_PRESSURE_ADVANCE ADVANCE=0.04\nRESPOND TYPE=echo MSG=preview\n" : "");
    GCodeProcessor baseline;
    baseline.apply_config(config);
    baseline.process_buffer(setup + control + arc);
    GCodeProcessor profiled;
    profiled.apply_config(config);
    profiled.process_buffer(setup);
    // Orca: Chunk boundaries may coincide with streaming-buffer boundaries.
    profiled.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, decreasing ? "5,0,100,75,50" : "5,0,0,25,50"));
    profiled.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, decreasing ? "5,3,25,0" : "5,3,75,100") + control + marked_arc);
    const auto &original = baseline.get_result().moves;
    const auto &moves = profiled.get_result().moves;
    REQUIRE(moves.size() == original.size());
    // Orca: Timing may insert extra speed markers; profile progress follows the original arc segments.
    const size_t count = std::count_if(moves.begin(), moves.end(), [](const auto &move) {
        return move.type == EMoveType::Extrude && !move.internal_only;
    });
    REQUIRE(count > 2);
    size_t segment = 0;
    for (size_t i = 0; i < moves.size(); ++i) {
        CHECK_THAT((moves[i].position - original[i].position).norm(), Catch::Matchers::WithinAbs(0.0, 1e-6));
        CHECK_THAT(moves[i].delta_extruder, Catch::Matchers::WithinAbs(original[i].delta_extruder, 1e-6));
        CHECK_THAT(moves[i].feedrate, Catch::Matchers::WithinAbs(original[i].feedrate, 1e-6));
        if (moves[i].type == EMoveType::Extrude && !moves[i].internal_only) {
            const double expected = 100.0 * (decreasing ? count - segment : segment + 1) / count;
            ++segment;
            CHECK_THAT(moves[i].overhang_percentage, Catch::Matchers::WithinAbs(expected, 1e-3));
            CHECK_THAT(moves[i].overhang_z_distance, Catch::Matchers::WithinAbs(0.2, 1e-6));
        }
    }
    // Orca: Neither the next line nor the next untagged arc may inherit the local profile.
    profiled.process_buffer("G1 X1 Y10 E1\n");
    CHECK_THAT(profiled.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(100.0, 1e-6));
    profiled.process_buffer("G3 X11 Y0 I10 J0 E1\n");
    CHECK_THAT(profiled.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(100.0, 1e-6));
}

// Orca: A tiny arc becomes one preview segment, but an unsupported interior sample must still survive
// even when both endpoints are fully supported. The following move keeps the independent scalar value.
TEST_CASE("Overhang arc profiles preserve interior peaks on short arcs", "[GCodeProcessor][Overhang][ArcFitting][Regression]")
{
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
    GCodeProcessor processor;
    processor.apply_config(config);
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    processor.process_buffer("G90\nM83\nT0\nG1 X0.05 Y0 Z0.2 F600\n" + tag(GCodeProcessor::ETags::Overhang, "33") +
        tag(GCodeProcessor::ETags::Overhang_Arc, "5,0,0,0,100,0,0") +
        bind_overhang_profile("G3 X0 Y0.05 I-0.05 J0 E0.01\n"));
    const auto &moves = processor.get_result().moves;
    REQUIRE(std::count_if(moves.begin(), moves.end(), [](const auto &move) { return move.type == EMoveType::Extrude; }) == 1);
    CHECK_THAT(moves.back().overhang_percentage, Catch::Matchers::WithinAbs(100.0, 1e-6));
    processor.process_buffer("G1 X10 E1\n");
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(33.0, 1e-6));
    // Orca: Complete samples without a standalone marker must not color or leak past an unrelated arc;
    // merely mentioning the marker inside human-readable text is intentionally insufficient.
    processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, "3,0,0,50,100") +
        "G3 X9.95 Y0 I-0.05 J0 E0.01 ; do not apply OVERHANG_ARC_APPLY here\n");
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(33.0, 1e-6));
}

// Orca: Corrupt, incomplete or stale chunks must fall back to the scalar tag, never partially color
// an arc or carry a previous profile across an intervening command or a parser reset.
TEST_CASE("Overhang arc profiles reject invalid chunks and stale associations", "[GCodeProcessor][Overhang][ArcFitting][Regression]")
{
    // Orca: Empty headers and samples must be invalid with both std::from_chars and the legacy
    // floating-point backend used by older macOS standard libraries, not silently become zero.
    const std::string data = GENERATE("", "3", "3,0", "1,0,0", "65537,0,0", "3,-1,0", "3,1,50,100", "3,0,0,nan,100", "3,0,0,inf,100",
        "3,0,0,1001,100", "3,0,0,-1,100", "3,0,0,50,100junk", "3,0,0,50,100,", "3,0,0,50", "3,0,0,,100",
        ",0,0,50,100", "3,,0,50,100", "3,0,,50,100",
        "3,0,0,50,100,0", "3,0,0,50,100\nG2 X1 Y1 E1");
    CAPTURE(data);
    GCodeProcessor processor;
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    const auto setup = [&]() {
        processor.apply_config(FullPrintConfig());
        processor.process_buffer("G90\nM83\nT0\nG1 X10 Y0 Z0.2 F600\n" + tag(GCodeProcessor::ETags::Overhang, "33"));
    };
    setup();
    processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, data) +
        bind_overhang_profile("G3 X0 Y10 I-10 J0 E1\n"));
    size_t checked = 0;
    for (const auto &move : processor.get_result().moves)
        if (move.type == EMoveType::Extrude) {
            CHECK_THAT(move.overhang_percentage, Catch::Matchers::WithinAbs(33.0, 1e-6));
            ++checked;
        }
    REQUIRE(checked > 1);
    processor.process_buffer(tag(GCodeProcessor::ETags::Overhang_Arc, "3,0,0,50,100"));
    processor.reset();
    setup();
    processor.process_buffer("G3 X0 Y10 I-10 J0 E1\n");
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(33.0, 1e-6));
}

// Orca: Post-processing may retain an arc's marker after removing or damaging its chunks. Such an
// arc must keep the preview unavailable unless a complete, valid profile was actually supplied.
TEST_CASE("Overhang arc markers require complete profiles to enable the preview", "[GCodeProcessor][Overhang][ArcFitting][Regression]")
{
    const bool bbl_printer = GENERATE(false, true);
    const std::string profile = GENERATE("", "3,0,0,50", "3,0,0,,100", "3,0,0,50,100");
    CAPTURE(bbl_printer, profile);
    // Orca: Restore the process-wide tag dialect so randomized test ordering cannot leak it.
    const bool       was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
    GCodeProcessor::s_IsBBLPrinter = bbl_printer;
    GCodeProcessor processor;
    processor.apply_config(FullPrintConfig());
    processor.process_buffer("G90\nM83\nT0\nG1 X10 Y0 Z0.2 F600\n");
    if (!profile.empty())
        processor.process_buffer(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Arc) + profile + "\n");
    CHECK_FALSE(processor.get_result().has_overhang_metadata);
    processor.process_buffer(bind_overhang_profile("G3 X0 Y10 I-10 J0 E1\n"));
    const bool complete = profile == "3,0,0,50,100";
    CHECK(processor.get_result().has_overhang_metadata == complete);
    REQUIRE_FALSE(processor.get_result().moves.empty());
    REQUIRE(processor.get_result().moves.back().type == EMoveType::Extrude);
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage,
        Catch::Matchers::WithinAbs(complete ? 100.0 : 0.0, 1e-5));
}

TEST_CASE("Overhang metadata availability follows valid G-code tags", "[GCodeProcessor][Overhang]")
{
    const bool bbl_printer = GENERATE(false, true);
    CAPTURE(bbl_printer);
    // Orca: Restore the process-wide tag dialect so randomized test ordering cannot leak it.
    const bool       was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
    GCodeProcessor::s_IsBBLPrinter = bbl_printer;
    // Orca: Drive the streaming parser used for freshly generated G-code and verify that only a
    // valid tag advertises the optional preview mode in both supported tag dialects.
    GCodeProcessor processor;
    const std::string tag_prefix = bbl_printer ? "; OVERHANG: " : ";OVERHANG:";
    CHECK(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang) == tag_prefix.substr(1));
    // Orca: Reference spacing alone is not sufficient to offer the Overhang view.
    processor.process_buffer(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Z_Distance) + "0.2\n");
    CHECK_FALSE(processor.get_result().has_overhang_metadata);
    processor.process_buffer(tag_prefix + "37.5\n");
    CHECK(processor.get_result().has_overhang_metadata);

    // Orca: Resetting the processor must hide the mode again until another valid tag is parsed.
    processor.reset();
    CHECK_FALSE(processor.get_result().has_overhang_metadata);
    processor.process_buffer(tag_prefix + "invalid\n");
    CHECK_FALSE(processor.get_result().has_overhang_metadata);

    // Orca: Complete samples alone remain inert; only the inline marker on their exact arc publishes them.
    processor.reset();
    processor.apply_config(FullPrintConfig());
    processor.process_buffer("G90\nM83\nT0\nG1 X10 Y0 Z0.2 F600\n;" +
        GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Arc) + "3,0,0,50,100\n");
    CHECK_FALSE(processor.get_result().has_overhang_metadata);
    processor.process_buffer(bind_overhang_profile("G3 X0 Y10 I-10 J0 E1\n"));
    CHECK(processor.get_result().has_overhang_metadata);
}

// Orca: Malformed or non-finite percentages must neither enable the view nor leak into its color lookup.
TEST_CASE("Overhang percentages reject malformed and non-finite metadata", "[GCodeProcessor][Overhang][Regression]")
{
    // Orca: An empty scalar tag must not advertise metadata, just like an empty arc sample.
    const std::string value = GENERATE("", "nan", "inf", "-inf", "37.5garbage", "invalid");
    const bool preceding_valid_tag = GENERATE(false, true);
    CAPTURE(value, preceding_valid_tag);
    GCodeProcessor processor;
    processor.apply_config(FullPrintConfig());
    const std::string tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang);
    processor.process_buffer("G90\nM83\nT0\n");
    if (preceding_valid_tag)
        processor.process_buffer(tag + "50\n");
    processor.process_buffer(tag + value + "\nG1 X10 Z0.2 E1 F600\n");
    CHECK(processor.get_result().has_overhang_metadata == preceding_valid_tag);
    REQUIRE_FALSE(processor.get_result().moves.empty());
    CHECK_THAT(processor.get_result().moves.back().overhang_percentage, Catch::Matchers::WithinAbs(0.0, 1e-6));
}

// Model infill consumes preview metadata; support and auxiliary roles have none and reset it.
TEST_CASE("Overhang metadata follows model infill roles and excludes supports", "[GCodeProcessor][Overhang][Regression]")
{
    struct Case {
        ExtrusionRole role;
        float expected_percentage;
    };
    const std::array<Case, 17> cases{{
        {erExternalPerimeter, 42.0f},
        {erBridgeInfill, 42.0f},
        {erInternalBridgeInfill, 42.0f},
        {erInternalInfill, 42.0f},
        {erSolidInfill, 42.0f},
        {erTopSolidInfill, 42.0f},
        {erBottomSurface, 42.0f},
        {erGapFill, 42.0f},
        {erIroning, GCodeProcessor::NO_OVERHANG_PERCENTAGE},
        {erSupportMaterial, GCodeProcessor::NO_OVERHANG_PERCENTAGE},
        {erSupportMaterialInterface, GCodeProcessor::NO_OVERHANG_PERCENTAGE},
        {erSupportTransition, GCodeProcessor::NO_OVERHANG_PERCENTAGE},
        {erSkirt, GCodeProcessor::NO_OVERHANG_PERCENTAGE},
        {erBrim, GCodeProcessor::NO_OVERHANG_PERCENTAGE},
        {erWipeTower, GCodeProcessor::NO_OVERHANG_PERCENTAGE},
        {erCustom, GCodeProcessor::NO_OVERHANG_PERCENTAGE},
        {erNone, GCodeProcessor::NO_OVERHANG_PERCENTAGE},
    }};

    for (const Case &test : cases) {
        DYNAMIC_SECTION(ExtrusionEntity::role_to_string(test.role)) {
            GCodeProcessor processor;
            processor.apply_config(FullPrintConfig());
            const std::string overhang_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang) + "42\n";
            const std::string role_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) +
                ExtrusionEntity::role_to_string(test.role) + "\n";
            processor.process_buffer("G90\nM83\nT0\n" + overhang_tag + role_tag + "G1 X10 Z0.2 E1 F600\n");

            REQUIRE_FALSE(processor.get_result().moves.empty());
            const auto &move = processor.get_result().moves.back();
            REQUIRE(move.type == EMoveType::Extrude);
            CHECK(move.extrusion_role == test.role);
            CHECK(GCodeProcessor::measures_overhang(test.role) == (test.expected_percentage >= 0.0f));
            CHECK_THAT(move.overhang_percentage, Catch::Matchers::WithinAbs(test.expected_percentage, 1e-6));
            // A measured role entered after an unmeasured one does not inherit the reading before it.
            processor.process_buffer(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) +
                ExtrusionEntity::role_to_string(erExternalPerimeter) + "\nG1 X20 E1\n");
            CHECK_THAT(processor.get_result().moves.back().overhang_percentage,
                Catch::Matchers::WithinAbs(GCodeProcessor::measures_overhang(test.role) ? 42.0 : 0.0, 1e-6));
        }
    }
}

// Orca: A run of equal cells is written once as value*count. It expands to the cells it stands for, chunk offsets
// count cells, and a run may neither overflow the declared total nor stand for fewer than two cells.
TEST_CASE("Overhang line profiles expand runs of equal cells", "[GCodeProcessor][Overhang][Regression]")
{
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    const auto profile_of = [&](const std::vector<std::string> &chunks) {
        GCodeProcessor processor;
        processor.apply_config(FullPrintConfig());
        std::string gcode = "G90\nM83\nT0\nG1 Z0.2\n" + tag(GCodeProcessor::ETags::Overhang, "33");
        for (const std::string &chunk : chunks)
            gcode += tag(GCodeProcessor::ETags::Overhang_Line, chunk);
        processor.process_buffer(gcode + bind_overhang_profile("G1 X8 E1 F600\n", GCodeProcessor::ETags::Overhang_Line_Apply));
        const auto &profiles = processor.get_result().overhang_line_profiles;
        return profiles.empty() ? std::vector<float>{} : profiles.front().percentages;
    };
    const std::vector<float> expected{0.f, 0.f, 250.5f, 100.f, 100.f, 100.f};
    const auto chunks = GENERATE(as<std::vector<std::string>>{},
        std::vector<std::string>{"6,0,0*2,250.5,100*3"},
        std::vector<std::string>{"6,0,0*2,250.5", "6,3,100*3"},
        std::vector<std::string>{"6,0,0,0", "6,2,250.5,100,100*2"});
    CAPTURE(chunks);
    const auto profile = profile_of(chunks);
    REQUIRE(profile.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        CHECK_THAT(profile[i], Catch::Matchers::WithinAbs(expected[i], 1e-6));

    for (const char *invalid : {"4,0,0*1,100*3", "4,0,0*5", "4,0,0*", "4,0,*2,0*2", "4,0,0*2.5,0*2", "4,0,0*-2,0*2",
             "4,0,1001*4"}) {
        CAPTURE(invalid);
        CHECK(profile_of({invalid}).empty());
    }
}

// Orca: The pressure equalizer and the fan mover split a G1 after its profile was written, keeping the marker on
// every piece. The preview spreads the profile over all the pieces as it does over the unsplit line.
TEST_CASE("Line overhang profiles cover every piece of a split G1", "[GCodeProcessor][Overhang][Regression]")
{
    const auto mark = [](const std::string &line) {
        return bind_overhang_profile(line, GCodeProcessor::ETags::Overhang_Line_Apply);
    };
    const std::string pieces = GENERATE_COPY(
        // Unsplit.
        std::string("G1 X40 E4\n"),
        // Pressure equalizer: equal pieces, each after its own feedrate, the first with the line's comment.
        "G1 F1200\n" + mark("G1 X10 E1 ; solid infill\n") + "G1 F1500\n" + mark("G1 X20 E1\n") +
            "G1 F1800\n" + mark("G1 X30 E1\n") + mark("G1 X40 E1\n"),
        // Fan mover: two halves around the fan command it moved into the line.
        mark("G1 X25 E2.5\n") + "M106 S255\n" + mark("G1 X40 E1.5\n"));
    const bool unsplit = pieces == "G1 X40 E4\n";
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    GCodeProcessor processor;
    processor.apply_config(FullPrintConfig());
    processor.initialize_result_moves();
    // The last quarter of the line, x = 30 .. 40, is unsupported.
    processor.process_buffer("G90\nM83\nT0\nG1 X0 Y0 Z0.2 F600\n" + tag(GCodeProcessor::ETags::Overhang, "100") +
        tag(GCodeProcessor::ETags::Overhang_Line, "4,0,0*3,100") + (unsplit ? mark(pieces) : pieces));
    processor.finalize(false);
    const auto &result = processor.get_result();
    REQUIRE(result.overhang_line_profiles.size() == 1);
    CHECK_THAT(result.overhang_line_profiles.front().end.x(), Catch::Matchers::WithinAbs(40.0, 1e-4));
    double length = 0.0;
    double unsupported_length = 0.0;
    result.for_each_preview_move([&](const auto &prev, const auto &curr, bool) {
        if (curr.type != EMoveType::Extrude)
            return;
        const double middle = 0.5 * (prev.position.x() + curr.position.x());
        CHECK_THAT(curr.overhang_percentage, Catch::Matchers::WithinAbs(middle > 30.0 ? 100.0 : 0.0, 1e-6));
        length += (curr.position - prev.position).norm();
        if (middle > 30.0)
            unsupported_length += (curr.position - prev.position).norm();
    });
    CHECK_THAT(length, Catch::Matchers::WithinAbs(40.0, 1e-4));
    CHECK_THAT(unsupported_length, Catch::Matchers::WithinAbs(10.0, 1e-4));
}

// Orca: Only marked, collinear pieces continuing the line extend its profile. An unmarked G1, a turn, a travel or
// new metadata ends it, so a later move keeps its own scalar reading.
TEST_CASE("Line overhang profiles stop at the first move that does not continue the split line", "[GCodeProcessor][Overhang][Regression]")
{
    const auto mark = [](const std::string &line) {
        return bind_overhang_profile(line, GCodeProcessor::ETags::Overhang_Line_Apply);
    };
    // A damaged chunk of the next line's own profile also ends the split line: that line falls back to its scalar.
    const std::string next = GENERATE_COPY(std::string("G1 X40 E2\n"), mark("G1 X20 Y20 E2\n"),
        "G1 X30\n" + mark("G1 X40 E1\n"), mark("G1 X10 E1\n"),
        ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Line) + "4,0,0,abc,0,0\n" + mark("G1 X40 E2\n"),
        ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Line) + "4,2,0,0\n" + mark("G1 X40 E2\n"));
    CAPTURE(next);
    const auto tag = [](GCodeProcessor::ETags type, const std::string &value) {
        return ";" + GCodeProcessor::reserved_tag(type) + value + "\n";
    };
    GCodeProcessor processor;
    processor.apply_config(FullPrintConfig());
    processor.process_buffer("G90\nM83\nT0\nG1 X0 Y0 Z0.2 F600\n" + tag(GCodeProcessor::ETags::Overhang, "100") +
        tag(GCodeProcessor::ETags::Overhang_Line, "4,0,0*3,100") + mark("G1 X20 E2\n") + next);
    const auto &result = processor.get_result();
    REQUIRE(result.overhang_line_profiles.size() == 1);
    CHECK_THAT(result.overhang_line_profiles.front().end.x(), Catch::Matchers::WithinAbs(20.0, 1e-4));
    REQUIRE(result.moves.back().type == EMoveType::Extrude);
    CHECK(result.moves.back().overhang_profile_id == 0);
    CHECK_THAT(result.moves.back().overhang_percentage, Catch::Matchers::WithinAbs(100.0, 1e-6));
}
