#include <catch2/catch_all.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/OverhangSeamLoops.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"
#include <sstream>
#include <string>
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

void process_gcode(const std::string &gcode, GCodeProcessorResult &result)
{
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
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
void process_two_objects(GCodeProcessorResult &result)
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
    process_gcode(gcode.str(), result);
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
    // The beads' center lines, brim and support included, from the first layer's bottom to the second's top.
    CHECK_THAT((plate.box.min - Vec3d(8., 8., 0.)).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
    CHECK_THAT((plate.box.max - Vec3d(60., 50., 0.4)).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
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

// One layer with a closed square wall printed in `role`, then an inner-wall move that closes the
// seam candidate. `outer_tag_first` puts an "Outer wall" tag with no move before the wall's own
// tag, which leaves the seam detector active without a first vertex. `sloped` starts the wall
// with a short rise in Z, as a scarf seam does.
std::string square_wall(ExtrusionRole role, bool outer_tag_first, bool sloped)
{
    std::ostringstream gcode;
    gcode << "M83\nG90\n;" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) << "\n"
          << "G1 X10 Y10 Z0.2 F12000\n";
    if (outer_tag_first)
        gcode << "; FEATURE: Outer wall\n";
    gcode << "; FEATURE: " << ExtrusionEntity::role_to_string(role) << "\n";
    if (sloped)
        gcode << "G1 X10.1 Y10 Z0.3 E0.01 F3000\nG1 X50 Y10 E2\n";
    else
        gcode << "G1 X10.1 Y10 E0.01 F3000\nG1 X50 Y10 E2\n";
    gcode << "G1 X50 Y50 E2\nG1 X10 Y50 E2\nG1 X10 Y10 E2\n"
          << "; FEATURE: Inner wall\nG1 X12 Y12 E0.1\nG1 X30 Y12 E1\n";
    return gcode.str();
}

// The loop square_wall() prints, as the generator hands it over.
OverhangSeamLoop square_loop(bool sloped)
{
    const double z_end = sloped ? 0.3 : 0.2;
    return { OverhangSeamKey::from_gcode(10., 10., 0.2, 0), OverhangSeamKey::from_gcode(10., 10., z_end, 0), 1, "square" };
}

// Streams `gcode` through the processor as an export does, with `loops` handed over for the
// first layer and `layer_tags` reported as written by the generator.
// The parts of a GCodeProcessorResult the tests read; the result itself cannot be returned by value.
struct StreamResult
{
    std::vector<GCodeProcessorResult::MoveVertex> moves;
    OverhangSeamStats                             overhang_seam_stats;
};

StreamResult process_stream(const std::string &gcode, const std::vector<OverhangSeamLoop> &loops, size_t layer_tags,
                                    bool sloped = false, bool reset_after_publish = false, bool spiral_vase = false)
{
    FullPrintConfig config;
    config.gcode_flavor.value = gcfMarlinFirmware;
    config.spiral_mode.value  = spiral_vase;
    // s_IsBBLPrinter selects the "; FEATURE: " role tags this G-code uses.
    const bool       was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
    GCodeProcessor::s_IsBBLPrinter = true;
    GCodeProcessor processor;
    processor.reset();
    processor.initialize("stream.gcode");
    processor.overhang_seam_channel().publish(1, loops);
    if (reset_after_publish)
        processor.reset();
    processor.initialize_result_moves();
    processor.apply_config(config);
    // A scarf seam turns this on; the detector then follows the loop start up in Z.
    processor.detect_layer_based_on_tag(sloped || spiral_vase);
    processor.set_overhang_seam_layer_tags(layer_tags);
    processor.process_buffer(gcode);
    processor.finalize(false);
    return { std::move(processor.result().moves), processor.get_result().overhang_seam_stats };
}

template<class Result> std::vector<GCodeProcessorResult::MoveVertex> seams_of(const Result &result)
{
    std::vector<GCodeProcessorResult::MoveVertex> seams;
    std::copy_if(result.moves.begin(), result.moves.end(), std::back_inserter(seams),
                 [](const GCodeProcessorResult::MoveVertex &move) { return move.type == EMoveType::Seam; });
    return seams;
}

} // namespace

TEST_CASE("A handed-over overhang outer wall gets the seam an outer wall would get", "[GCodeProcessor][OverhangSeam]")
{
    const bool outer_tag_first = GENERATE(false, true);
    const bool sloped          = GENERATE(false, true);
    INFO("outer tag first " << outer_tag_first << ", sloped " << sloped);

    const StreamResult overhang = process_stream(square_wall(erOverhangPerimeter, outer_tag_first, sloped), { square_loop(sloped) }, 1, sloped);
    const StreamResult outer    = process_stream(square_wall(erExternalPerimeter, outer_tag_first, sloped), {}, 1, sloped);

    const auto expected = seams_of(outer);
    const auto actual   = seams_of(overhang);
    REQUIRE(expected.size() == 1);
    REQUIRE(actual.size() == expected.size());
    CHECK(actual[0].gcode_id == expected[0].gcode_id);
    CHECK_THAT(actual[0].position.x(), Catch::Matchers::WithinAbs(expected[0].position.x(), 1e-5));
    CHECK_THAT(actual[0].position.y(), Catch::Matchers::WithinAbs(expected[0].position.y(), 1e-5));
    CHECK_THAT(actual[0].position.z(), Catch::Matchers::WithinAbs(expected[0].position.z(), 1e-5));
    CHECK_THAT(actual[0].actual_feedrate, Catch::Matchers::WithinAbs(expected[0].actual_feedrate, 1e-4));

    // The wall keeps its real role.
    CHECK(std::any_of(overhang.moves.begin(), overhang.moves.end(), [](const GCodeProcessorResult::MoveVertex &move) {
        return move.type == EMoveType::Extrude && move.extrusion_role == erOverhangPerimeter;
    }));
    const OverhangSeamStats &stats = overhang.overhang_seam_stats;
    CHECK(stats.registered == 1);
    CHECK(stats.end_consistent == 1);
    CHECK(stats.missed == 0);
    CHECK(stats.suspect == 0);
    CHECK_FALSE(stats.needs_warning());
}

TEST_CASE("An overhang wall that is not handed over gets no seam", "[GCodeProcessor][OverhangSeam]")
{
    const StreamResult result = process_stream(square_wall(erOverhangPerimeter, false, false), {}, 1);
    CHECK(seams_of(result).empty());
    CHECK(result.overhang_seam_stats.registered == 0);
    CHECK_FALSE(result.overhang_seam_stats.needs_warning());
}

TEST_CASE("Handed-over loops that do not fit the G-code are reported", "[GCodeProcessor][OverhangSeam]")
{
    const std::string gcode = square_wall(erOverhangPerimeter, false, false);

    SECTION("a start no move begins at is missed")
    {
        OverhangSeamLoop loop = square_loop(false);
        loop.start            = OverhangSeamKey::from_gcode(20., 20., 0.2, 0);
        const StreamResult result = process_stream(gcode, { loop }, 1);
        CHECK(seams_of(result).empty());
        CHECK(result.overhang_seam_stats.missed == 1);
        CHECK(result.overhang_seam_stats.needs_warning());
        REQUIRE(result.overhang_seam_stats.problem_objects.size() == 1);
        CHECK(result.overhang_seam_stats.problem_objects[0].object == "square");
        CHECK(result.overhang_seam_stats.problem_objects[0].first_layer == 1);
        CHECK(result.overhang_seam_stats.problem_objects[0].count == 1);
    }
    SECTION("a start of another filament is missed")
    {
        OverhangSeamLoop loop = square_loop(false);
        loop.start.filament   = 1;
        const StreamResult result = process_stream(gcode, { loop }, 1);
        CHECK(seams_of(result).empty());
        CHECK(result.overhang_seam_stats.missed == 1);
    }
    SECTION("a loop that ends elsewhere is suspect")
    {
        OverhangSeamLoop loop = square_loop(false);
        loop.end              = OverhangSeamKey::from_gcode(11., 11., 0.2, 0);
        const StreamResult result = process_stream(gcode, { loop }, 1);
        CHECK(seams_of(result).size() == 1);
        CHECK(result.overhang_seam_stats.suspect == 1);
        CHECK(result.overhang_seam_stats.needs_warning());
    }
    SECTION("layer tags that differ from the generator's count are reported")
    {
        const StreamResult result = process_stream(gcode, { square_loop(false) }, 2);
        CHECK(result.overhang_seam_stats.end_consistent == 1);
        CHECK(result.overhang_seam_stats.layer_tags_mismatch());
        CHECK(result.overhang_seam_stats.needs_warning());
    }
}

TEST_CASE("A handed-over loop merged into the previous seam candidate is suspect", "[GCodeProcessor][OverhangSeam]")
{
    // Two walls with no move between them: the detector keeps the first wall's candidate.
    const auto two_walls = [](ExtrusionRole second) {
        std::ostringstream gcode;
        gcode << "M83\nG90\n;" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) << "\n"
              << "G1 X10 Y10 Z0.2 F12000\n; FEATURE: Outer wall\n"
              << "G1 X50 Y10 E2 F3000\nG1 X50 Y50 E2\nG1 X10 Y50 E2\nG1 X10 Y10 E2\n"
              << "; FEATURE: " << ExtrusionEntity::role_to_string(second) << "\n"
              << "G1 X30 Y10 E1\nG1 X30 Y30 E1\nG1 X10 Y30 E1\nG1 X10 Y10 E1\n"
              << "; FEATURE: Inner wall\nG1 X12 Y12 E0.1\nG1 X30 Y12 E1\n";
        return gcode.str();
    };
    const StreamResult overhang = process_stream(two_walls(erOverhangPerimeter), { square_loop(false) }, 1);
    const StreamResult outer    = process_stream(two_walls(erExternalPerimeter), {}, 1);

    const auto expected = seams_of(outer);
    const auto actual   = seams_of(overhang);
    REQUIRE_FALSE(expected.empty());
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        INFO("seam " << i);
        CHECK(actual[i].gcode_id == expected[i].gcode_id);
        CHECK_THAT(actual[i].position.x(), Catch::Matchers::WithinAbs(expected[i].position.x(), 1e-5));
        CHECK_THAT(actual[i].position.y(), Catch::Matchers::WithinAbs(expected[i].position.y(), 1e-5));
        CHECK_THAT(actual[i].position.z(), Catch::Matchers::WithinAbs(expected[i].position.z(), 1e-5));
        CHECK_THAT(actual[i].actual_feedrate, Catch::Matchers::WithinAbs(expected[i].actual_feedrate, 1e-4));
    }
    CHECK(overhang.overhang_seam_stats.suspect == 1);
    CHECK(overhang.overhang_seam_stats.missed == 0);
}

TEST_CASE("A processor reset drops loops handed over before it", "[GCodeProcessor][OverhangSeam]")
{
    const StreamResult result = process_stream(square_wall(erOverhangPerimeter, false, false), { square_loop(false) }, 1, false, true);
    CHECK(seams_of(result).empty());
    CHECK(result.overhang_seam_stats.registered == 0);
}

TEST_CASE("Outer walls of a sphere's lower half are all matched on export", "[GCodeProcessor][OverhangSeam]")
{
    // The lower half of a sphere has outer walls that start on an overhang.
    Print print;
    Model model;
    Test::init_print({ Test::TestMesh::sphere_50mm }, print, model,
                     { { "detect_overhang_wall", true }, { "layer_height", 0.4 }, { "initial_layer_print_height", 0.4 },
                       { "wall_loops", 2 }, { "sparse_infill_density", "0%" }, { "top_shell_layers", 0 }, { "bottom_shell_layers", 0 } });
    GCodeProcessorResult result;
    Test::gcode(print, &result);

    const OverhangSeamStats &stats = result.overhang_seam_stats;
    REQUIRE(stats.registered > 0);
    CHECK(stats.missed == 0);
    CHECK(stats.suspect == 0);
    CHECK(stats.end_consistent == stats.registered);
    CHECK_FALSE(stats.layer_tags_mismatch());
}

namespace {

// A closed square wall whose first two moves are printed in `start` and the rest in `rest`.
std::string mixed_wall(ExtrusionRole start, ExtrusionRole rest, bool sloped)
{
    std::ostringstream gcode;
    gcode << "M83\nG90\n;" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) << "\n"
          << "G1 X10 Y10 Z0.2 F12000\n; FEATURE: " << ExtrusionEntity::role_to_string(start) << "\n";
    if (sloped)
        gcode << "G1 X10.1 Y10 Z0.3 E0.01 F3000\nG1 X50 Y10 E2\n";
    else
        gcode << "G1 X10.1 Y10 E0.01 F3000\nG1 X50 Y10 E2\n";
    gcode << "; FEATURE: " << ExtrusionEntity::role_to_string(rest) << "\n"
          << "G1 X50 Y50 E2\nG1 X10 Y50 E2\nG1 X10 Y10 E2\n"
          << "; FEATURE: Inner wall\nG1 X12 Y12 E0.1\nG1 X30 Y12 E1\n";
    return gcode.str();
}

} // namespace

TEST_CASE("An outer wall that starts on an overhang gets the seam an outer wall would get", "[GCodeProcessor][OverhangSeam]")
{
    // No loops are handed over, as for a G-code file: the "Outer wall" segment tells the loop apart.
    const bool sloped = GENERATE(false, true);
    INFO("sloped " << sloped);
    const StreamResult overhang = process_stream(mixed_wall(erOverhangPerimeter, erExternalPerimeter, sloped), {}, 1, sloped);
    const StreamResult outer    = process_stream(mixed_wall(erExternalPerimeter, erExternalPerimeter, sloped), {}, 1, sloped);

    const auto expected = seams_of(outer);
    const auto actual   = seams_of(overhang);
    REQUIRE(expected.size() == 1);
    REQUIRE(actual.size() == expected.size());
    CHECK(actual[0].gcode_id == expected[0].gcode_id);
    CHECK_THAT(actual[0].position.x(), Catch::Matchers::WithinAbs(expected[0].position.x(), 1e-5));
    CHECK_THAT(actual[0].position.y(), Catch::Matchers::WithinAbs(expected[0].position.y(), 1e-5));
    CHECK_THAT(actual[0].position.z(), Catch::Matchers::WithinAbs(expected[0].position.z(), 1e-5));
    CHECK_THAT(actual[0].actual_feedrate, Catch::Matchers::WithinAbs(expected[0].actual_feedrate, 1e-4));
}

TEST_CASE("A spiral vase print keeps the old seam detection", "[GCodeProcessor][OverhangSeam]")
{
    // A mixed wall starting on an overhang, which a non-vase print would mark.
    const StreamResult result = process_stream(mixed_wall(erOverhangPerimeter, erExternalPerimeter, false), {}, 1, false, false, true);
    CHECK(seams_of(result).empty());
    CHECK_FALSE(result.overhang_seam_stats.overhang_only_paths);
}

TEST_CASE("An inner wall that starts on an overhang gets no seam", "[GCodeProcessor][OverhangSeam]")
{
    const StreamResult result = process_stream(mixed_wall(erOverhangPerimeter, erPerimeter, false), {}, 1);
    CHECK(seams_of(result).empty());
}

TEST_CASE("A path printed only as overhang wall is flagged for G-code files", "[GCodeProcessor][OverhangSeam]")
{
    // The path ends with a travel while still printed as "Overhang wall".
    const auto wall_then_travel = [](ExtrusionRole rest) {
        std::ostringstream gcode;
        gcode << "M83\nG90\n;" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) << "\n"
              << "G1 X10 Y10 Z0.2 F12000\n; FEATURE: Overhang wall\nG1 X50 Y10 E2 F3000\n"
              << "; FEATURE: " << ExtrusionEntity::role_to_string(rest) << "\n"
              << "G1 X50 Y50 E2\nG1 X10 Y50 E2\nG1 X10 Y10 E2\nG1 X80 Y80 F12000\n";
        return gcode.str();
    };
    CHECK(process_stream(wall_then_travel(erOverhangPerimeter), {}, 1).overhang_seam_stats.overhang_only_paths);
    CHECK_FALSE(process_stream(wall_then_travel(erExternalPerimeter), {}, 1).overhang_seam_stats.overhang_only_paths);
    CHECK_FALSE(process_stream(wall_then_travel(erPerimeter), {}, 1).overhang_seam_stats.overhang_only_paths);
}

TEST_CASE("A missed loop does not stop later loops of the layer from matching", "[GCodeProcessor][OverhangSeam]")
{
    OverhangSeamLoop missed = square_loop(false);
    missed.start            = OverhangSeamKey::from_gcode(20., 20., 0.2, 0);
    const StreamResult result = process_stream(square_wall(erOverhangPerimeter, false, false), { missed, square_loop(false) }, 1);
    CHECK(seams_of(result).size() == 1);
    CHECK(result.overhang_seam_stats.missed == 1);
    CHECK(result.overhang_seam_stats.end_consistent == 1);
}

TEST_CASE("The warning names an object's earliest problem layer", "[GCodeProcessor][OverhangSeam]")
{
    // A loop of layer 1 opens a candidate that never closes; a loop of layer 2 is never found.
    // The layer 2 miss is reported first, at the layer change, the layer 1 problem only at the end.
    OverhangSeamLoop layer_1 = square_loop(false);
    OverhangSeamLoop layer_2 = square_loop(false);
    layer_2.start            = OverhangSeamKey::from_gcode(20., 20., 0.4, 0);
    layer_2.layer_num        = 2;
    OverhangSeamChannel channel;
    channel.publish(1, { layer_1 });
    channel.publish(2, { layer_2 });
    OverhangSeamMatcher matcher;
    matcher.on_layer_change(channel);
    REQUIRE(matcher.match_start(layer_1.start, false));
    matcher.on_layer_change(channel);
    matcher.finish(channel, 2);

    const OverhangSeamStats &stats = matcher.stats();
    CHECK(stats.missed == 1);
    CHECK(stats.suspect == 1);
    REQUIRE(stats.problem_objects.size() == 1);
    CHECK(stats.problem_objects[0].first_layer == 1);
    CHECK(stats.problem_objects[0].count == 2);
}

TEST_CASE("A G-code file ending on a path printed only as overhang wall is flagged", "[GCodeProcessor][OverhangSeam]")
{
    std::ostringstream gcode;
    gcode << "M83\nG90\n;" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) << "\n"
          << "G1 X10 Y10 Z0.2 F12000\n; FEATURE: Overhang wall\n"
          << "G1 X50 Y10 E2 F3000\nG1 X50 Y50 E2\nG1 X10 Y50 E2\nG1 X10 Y10 E2\nM400\nM84\n";
    CHECK(process_stream(gcode.str(), {}, 1).overhang_seam_stats.overhang_only_paths);
}

TEST_CASE("Exported overhang outer walls get the seams of outer walls", "[GCodeProcessor][OverhangSeam]")
{
    // One wall, so every wall is an outer wall, and no skirt, brim or scarf. Replaying the same
    // G-code with "Overhang wall" relabelled "Outer wall" gives the reference seams.
    Print print;
    Model model;
    Test::init_print({ Test::TestMesh::sphere_50mm }, print, model,
                     { { "detect_overhang_wall", true }, { "layer_height", 0.4 }, { "initial_layer_print_height", 0.4 },
                       { "wall_loops", 1 }, { "sparse_infill_density", "0%" }, { "top_shell_layers", 0 }, { "bottom_shell_layers", 0 },
                       { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "seam_slope_type", "none" }, { "seam_gap", "10%" },
                       { "z_offset", 0 } });
    GCodeProcessorResult exported;
    std::string          gcode = Test::gcode(print, &exported);
    REQUIRE(exported.overhang_seam_stats.registered > 0);

    const std::string overhang = ExtrusionEntity::role_to_string(erOverhangPerimeter);
    const std::string outer    = ExtrusionEntity::role_to_string(erExternalPerimeter);
    for (size_t pos = gcode.find(overhang); pos != std::string::npos; pos = gcode.find(overhang, pos + outer.size()))
        gcode.replace(pos, overhang.size(), outer);
    ScopedTemporaryFile temp(".gcode");
    std::ofstream(temp.string()) << gcode;
    GCodeProcessor processor;
    processor.process_file(temp.string());
    GCodeProcessorResult relabelled;
    relabelled = std::move(processor.extract_result());

    const auto actual   = seams_of(exported);
    const auto expected = seams_of(relabelled);
    REQUIRE_FALSE(expected.empty());
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        INFO("seam " << i);
        CHECK(actual[i].gcode_id == expected[i].gcode_id);
        CHECK_THAT(actual[i].actual_feedrate, Catch::Matchers::WithinAbs(expected[i].actual_feedrate, 1e-4));
        CHECK_THAT(actual[i].position.x(), Catch::Matchers::WithinAbs(expected[i].position.x(), 1e-4));
        CHECK_THAT(actual[i].position.y(), Catch::Matchers::WithinAbs(expected[i].position.y(), 1e-4));
        CHECK_THAT(actual[i].position.z(), Catch::Matchers::WithinAbs(expected[i].position.z(), 1e-4));
    }
}
