#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/OozeShield.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

DynamicPrintConfig ooze_shield_multimaterial_config(std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "ooze_shield",                    1 },
        { "ooze_shield_distance",           2 },
        { "enable_prime_tower",             1 },
        { "gcode_comments",                 1 },
        { "single_extruder_multi_material", 0 },
        { "layer_height",                   0.2 },
    });
    if (extra.size() > 0)
        config.set_deserialize_strict(extra);
    return config;
}

static const std::vector<std::vector<ConfigBase::SetDeserializeItem>> &two_cube_extruder_overrides()
{
    static const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        { { "extruder", 1 } },
        { { "extruder", 2 } },
    };
    return overrides;
}

std::string slice_two_cubes_with_ooze_shield(std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    return slice_with_object_overrides(
        { cube(20), cube(20) },
        ooze_shield_multimaterial_config(extra),
        two_cube_extruder_overrides());
}

static void init_two_cubes_with_ooze_shield(
    Print &print,
    Model &model,
    const DynamicPrintConfig &config)
{
    init_print({ cube(20), cube(20) }, print, model, config, &two_cube_extruder_overrides());
}

static void init_two_pyramids_with_ooze_shield(
    Print &print,
    Model &model,
    const DynamicPrintConfig &config)
{
    init_print({ mesh(TestMesh::pyramid), mesh(TestMesh::pyramid) }, print, model, config, &two_cube_extruder_overrides());
}

// True when some toolchange block is followed by an ooze-shield extrusion before the next perimeter.
static bool ooze_shield_follows_toolchange_before_perimeter(const std::string &gcode)
{
    std::vector<std::string> lines;
    {
        std::istringstream stream(gcode);
        for (std::string line; std::getline(stream, line);)
            lines.emplace_back(std::move(line));
    }

    const auto is_extruding = [](const std::string &line) {
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G2 ", 0) != 0 && line.rfind("G3 ", 0) != 0)
            return false;
        const size_t e = line.find(" E");
        return e != std::string::npos && line.find_first_of("XY") != std::string::npos && line[e + 2] != '-';
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find("; CP TOOLCHANGE END") == std::string::npos)
            continue;

        ssize_t shield_line = -1;
        ssize_t perimeter_line = -1;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            if (lines[j].find("; CP TOOLCHANGE START") != std::string::npos)
                break;
            if (!is_extruding(lines[j]))
                continue;
            if (shield_line < 0 && lines[j].find("ooze shield") != std::string::npos)
                shield_line = static_cast<ssize_t>(j);
            if (perimeter_line < 0 && lines[j].find("perimeter") != std::string::npos) {
                perimeter_line = static_cast<ssize_t>(j);
                break;
            }
        }
        if (shield_line >= 0 && perimeter_line >= 0)
            return shield_line < perimeter_line;
    }
    return false;
}

static double shield_polygon_area(const Polygons &polygons)
{
    double area = 0.;
    for (const Polygon &poly : polygons)
        area += std::abs(poly.area());
    return unscaled<double>(unscale<double>(area));
}

static bool shield_intersects_wipe_tower(const Print &print, const Polygons &shield)
{
    Points corners = print.first_layer_wipe_tower_corners(false);
    if (corners.size() < 3 || shield.empty())
        return false;
    Polygon tower;
    tower.points = std::move(corners);
    const Polygons tower_poly = offset(tower, float(scale_(0.1)));
    return !intersection(shield, tower_poly).empty();
}

static int count_toolchanges_on_shield_layers(const std::string &gcode, size_t max_shield_layer)
{
    int toolchanges = 0;
    size_t current_layer = 0;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind(";LAYER:", 0) == 0)
            current_layer = static_cast<size_t>(std::stoul(line.substr(7)));
        else if (line.find("; CP TOOLCHANGE END") != std::string::npos && current_layer <= max_shield_layer)
            ++toolchanges;
    }
    return toolchanges;
}

} // namespace

TEST_CASE("Ooze shield is disabled for single-material prints", "[OozeShield]")
{
    Print print;
    Model model;
    init_and_process_print({ cube(20) }, print, {
        { "ooze_shield", 1 },
        { "ooze_shield_distance", 2 },
    });

    CHECK_FALSE(print.has_ooze_shield());
    CHECK(print.ooze_shield().empty());
}

TEST_CASE("Ooze shield geometry offsets from the model outline", "[OozeShield]")
{
    Print print;
    Model model;
    init_two_cubes_with_ooze_shield(print, model, ooze_shield_multimaterial_config({
        { "ooze_shield_distance", 3 },
    }));
    print.process();

    REQUIRE(print.has_ooze_shield());
    REQUIRE_FALSE(print.ooze_shield().empty());
    REQUIRE_FALSE(print.ooze_shield().front().empty());

    const std::vector<Polygons> polygons = OozeShield::generate_layer_polygons(print);
    REQUIRE_FALSE(polygons.empty());
    REQUIRE_FALSE(polygons.front().empty());

    const BoundingBox model_bbox = get_extents(print.get_object(0)->layers().front()->lslices);
    const BoundingBox shield_bbox = get_extents(polygons.front());
    CHECK(shield_bbox.min.x() < model_bbox.min.x());
    CHECK(shield_bbox.max.x() > model_bbox.max.x());
    CHECK(shield_bbox.min.y() < model_bbox.min.y());
    CHECK(shield_bbox.max.y() > model_bbox.max.y());
}

TEST_CASE("Ooze shield layer count respects the last tool-change layer cap", "[OozeShield]")
{
    Print print;
    Model model;
    init_two_cubes_with_ooze_shield(print, model, ooze_shield_multimaterial_config());
    print.process();

    REQUIRE(print.has_ooze_shield());
    const size_t max_layer = OozeShield::max_shield_layer(print);
    REQUIRE(max_layer > 0);
    CHECK(print.ooze_shield().size() <= max_layer + 1);

    const std::vector<Polygons> polygons = OozeShield::generate_layer_polygons(print);
    CHECK(polygons.size() <= max_layer + 1);
}

TEST_CASE("Ooze shield distance increases the generated outline", "[OozeShield]")
{
    Print print_near;
    Print print_far;
    Model model_near;
    Model model_far;
    init_two_cubes_with_ooze_shield(print_near, model_near, ooze_shield_multimaterial_config({
        { "ooze_shield_distance", 2 },
    }));
    init_two_cubes_with_ooze_shield(print_far, model_far, ooze_shield_multimaterial_config({
        { "ooze_shield_distance", 5 },
    }));
    print_near.process();
    print_far.process();

    const double near_area = shield_polygon_area(OozeShield::generate_layer_polygons(print_near).front());
    const double far_area  = shield_polygon_area(OozeShield::generate_layer_polygons(print_far).front());
    CHECK(far_area > near_area);
}

TEST_CASE("Ooze shield geometry avoids the wipe tower footprint", "[OozeShield]")
{
    Print print;
    Model model;
    init_two_cubes_with_ooze_shield(print, model, ooze_shield_multimaterial_config({
        { "wipe_tower_x", "10" },
        { "wipe_tower_y", "10" },
    }));
    print.process();

    const std::vector<Polygons> polygons = OozeShield::generate_layer_polygons(print);
    REQUIRE_FALSE(polygons.empty());
    for (const Polygons &layer : polygons)
        CHECK_FALSE(shield_intersects_wipe_tower(print, layer));
}

TEST_CASE("Multi-material G-code contains ooze shield extrusions", "[OozeShield]")
{
    const std::string exported = slice_two_cubes_with_ooze_shield();
    CHECK(role_passes(exported, "ooze shield") > 0);
}

TEST_CASE("Ooze shield is extruded after toolchange and before perimeter", "[OozeShield]")
{
    const std::string exported = slice_two_cubes_with_ooze_shield();
    CHECK(ooze_shield_follows_toolchange_before_perimeter(exported));

    const std::vector<std::string> seq = role_sequence(exported, { "ooze shield", "perimeter" });
    REQUIRE_FALSE(seq.empty());
    const auto shield_it = std::find(seq.begin(), seq.end(), "ooze shield");
    const auto perimeter_it = std::find(seq.begin(), seq.end(), "perimeter");
    REQUIRE(shield_it != seq.end());
    REQUIRE(perimeter_it != seq.end());
    CHECK(shield_it < perimeter_it);
}

TEST_CASE("Ooze shield pass count covers toolchanges on active layers", "[OozeShield]")
{
    Print print;
    Model model;
    init_two_cubes_with_ooze_shield(print, model, ooze_shield_multimaterial_config({
        { "layer_change_gcode", ";LAYER:[layer_num]\n" },
    }));
    print.process();

    const size_t max_layer = OozeShield::max_shield_layer(print);
    REQUIRE(max_layer > 0);

    const std::string exported = gcode(print);
    const int toolchanges = count_toolchanges_on_shield_layers(exported, max_layer);
    REQUIRE(toolchanges > 0);

    const int shield_passes = role_passes(exported, "ooze shield");
    CHECK(shield_passes > 0);
    CHECK(shield_passes >= toolchanges);
}

TEST_CASE("Ooze shield angle taper changes the generated outline", "[OozeShield]")
{
    Print print_no_taper;
    Print print_tapered;
    Model model_no_taper;
    Model model_tapered;
    init_two_pyramids_with_ooze_shield(print_no_taper, model_no_taper, ooze_shield_multimaterial_config({
        { "ooze_shield_angle", 89.9 },
    }));
    init_two_pyramids_with_ooze_shield(print_tapered, model_tapered, ooze_shield_multimaterial_config({
        { "ooze_shield_angle", 30 },
    }));
    print_no_taper.process();
    print_tapered.process();

    const std::vector<Polygons> no_taper = OozeShield::generate_layer_polygons(print_no_taper);
    const std::vector<Polygons> tapered  = OozeShield::generate_layer_polygons(print_tapered);
    REQUIRE(no_taper.size() > 1);
    REQUIRE(tapered.size() > 1);

    auto total_area = [](const std::vector<Polygons> &layers) {
        double area = 0.;
        for (const Polygons &layer : layers)
            area += shield_polygon_area(layer);
        return area;
    };

    // Taper adjusts every layer pair; aggregate area should differ on a shrinking pyramid.
    CHECK(total_area(no_taper) != total_area(tapered));
}

TEST_CASE("Ooze shield outline ignores tree support envelope", "[OozeShield]")
{
    Print print_plain;
    Print print_supported;
    Model model_plain;
    Model model_supported;
    init_print(std::vector<TriangleMesh>{ mesh(TestMesh::overhang), mesh(TestMesh::overhang) },
        print_plain, model_plain, ooze_shield_multimaterial_config({
        { "enable_support", 0 },
    }), &two_cube_extruder_overrides());
    init_print(std::vector<TriangleMesh>{ mesh(TestMesh::overhang), mesh(TestMesh::overhang) },
        print_supported, model_supported, ooze_shield_multimaterial_config({
        { "enable_support", 1 },
        { "support_type",   "tree(auto)" },
    }), &two_cube_extruder_overrides());
    print_plain.process();
    print_supported.process();

    REQUIRE(print_supported.get_object(0)->support_layers().size() > 0);

    const double plain_area     = shield_polygon_area(OozeShield::generate_layer_polygons(print_plain).front());
    const double supported_area = shield_polygon_area(OozeShield::generate_layer_polygons(print_supported).front());
    CHECK(plain_area == supported_area);
}
