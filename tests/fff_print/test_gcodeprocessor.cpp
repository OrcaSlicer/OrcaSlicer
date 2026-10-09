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
