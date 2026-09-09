#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

using namespace Slic3r;

namespace {

indexed_triangle_set square_mesh()
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {1.0f, 1.0f, 0.0f}
    };
    mesh.indices = {{0, 1, 2}, {1, 3, 2}};
    return mesh;
}

std::vector<RGBA> red_colors()
{
    return {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f}
    };
}

indexed_triangle_set seamed_square_mesh(float gap = 0.0f)
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {1.0f + gap, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}
    };
    mesh.indices = {{0, 1, 2}, {3, 4, 5}};
    return mesh;
}

std::vector<RGBA> solid_colors(size_t count, const RGBA& color)
{
    return std::vector<RGBA>(count, color);
}

indexed_triangle_set stacked_triangle_mesh(size_t layers)
{
    indexed_triangle_set mesh;
    mesh.vertices.reserve(layers * 3);
    mesh.indices.reserve(layers);
    for (size_t layer = 0; layer < layers; ++layer) {
        const float z = float(layer) * 0.01f;
        const uint32_t first = uint32_t(mesh.vertices.size());
        mesh.vertices.emplace_back(0.0f, 0.0f, z);
        mesh.vertices.emplace_back(1.0f, 0.0f, z);
        mesh.vertices.emplace_back(0.0f, 1.0f, z);
        mesh.indices.emplace_back(first, first + 1, first + 2);
    }
    return mesh;
}

indexed_triangle_set separated_triangle_mesh(size_t count)
{
    indexed_triangle_set mesh;
    mesh.vertices.reserve(count * 3);
    mesh.indices.reserve(count);
    for (size_t item = 0; item < count; ++item) {
        const float x = float(item) * 2.0f;
        const uint32_t first = uint32_t(mesh.vertices.size());
        mesh.vertices.emplace_back(x, 0.0f, 0.0f);
        mesh.vertices.emplace_back(x + 1.0f, 0.0f, 0.0f);
        mesh.vertices.emplace_back(x, 1.0f, 0.0f);
        mesh.indices.emplace_back(first, first + 1, first + 2);
    }
    return mesh;
}

indexed_triangle_set overhang_region_mesh()
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {20.0f, 0.0f, 0.0f}, {20.0f, 10.0f, 0.0f}, {30.0f, 0.0f, 0.0f}, {30.0f, 10.0f, 0.0f},
        {0.0f, 0.0f, 5.0f}, {0.0f, 10.0f, 5.0f}, {10.0f, 0.0f, 5.0f}, {10.0f, 10.0f, 5.0f},
        {40.0f, 0.0f, 3.0f}, {40.0f, 0.1f, 3.0f}, {40.1f, 0.0f, 3.0f},
        {50.0f, 0.0f, 4.0f}, {51.0f, 0.0f, 4.0f}, {50.0f, 1.0f, 4.0f}
    };
    mesh.indices = {
        {0, 1, 2}, {1, 3, 2},       // Downward bed-contact surface.
        {4, 5, 6}, {5, 7, 6},       // Significant elevated downward region.
        {8, 9, 10},                  // Elevated but too small to be significant.
        {11, 12, 13}                 // Upward-facing surface.
    };
    return mesh;
}

} // namespace

TEST_CASE("vertex color smart region follows connected color blocks", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    std::vector<RGBA> colors = red_colors();
    colors[3] = {0.0f, 0.0f, 1.0f, 1.0f};
    REQUIRE(editor.initialize(square_mesh(), colors, error));

    AI::RegionSelectionSettings settings;
    settings.color_distance = 0.12f;
    CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);

    colors = red_colors();
    REQUIRE(editor.initialize(square_mesh(), colors, error));
    CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 2);
}

TEST_CASE("vertex color smart region crosses duplicated vertex seams", "[AI][VertexColorRegion]")
{
    AI::RegionSelectionSettings settings;
    settings.color_distance = 0.12f;
    settings.normal_angle_degrees = 45.0f;
    for (float gap : {0.0f, 1e-7f}) {
        indexed_triangle_set mesh = seamed_square_mesh(gap);
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 2);
    }
}

TEST_CASE("vertex color geometric seams preserve region boundaries", "[AI][VertexColorRegion]")
{
    AI::RegionSelectionSettings settings;
    settings.color_distance = 0.12f;
    settings.normal_angle_degrees = 45.0f;

    SECTION("positional gap") {
        indexed_triangle_set mesh = seamed_square_mesh(0.001f);
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    }

    SECTION("different color") {
        indexed_triangle_set mesh = seamed_square_mesh();
        std::vector<RGBA> colors = solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f});
        for (size_t index = 3; index < colors.size(); ++index)
            colors[index] = {0.0f, 0.0f, 1.0f, 1.0f};
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(std::move(mesh), std::move(colors), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    }

    SECTION("sharp normal") {
        indexed_triangle_set mesh;
        mesh.vertices = {
            {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}
        };
        mesh.indices = {{0, 1, 2}, {3, 4, 5}};
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    }

    SECTION("ambiguous coincident edge") {
        indexed_triangle_set mesh;
        mesh.vertices = {
            {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.2f, 0.2f, 0.0f}
        };
        mesh.indices = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}};
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    }
}

TEST_CASE("vertex color local patches support add and remove", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));

    AI::RegionSelectionSettings settings;
    settings.local_radius_ratio = 1.0f;
    CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Add, settings) == 2);
    CHECK(editor.update_selection(1, AI::RegionSelectionOperation::Remove, settings) == 0);
}

TEST_CASE("vertex color selection snapshots can be restored safely", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));

    AI::RegionSelectionSettings settings;
    settings.local_radius_ratio = 0.01f;
    REQUIRE(editor.update_selection(0, AI::RegionSelectionOperation::Add, settings) == 1);
    const std::vector<uint8_t> snapshot = editor.selected_faces();
    REQUIRE(editor.update_selection(1, AI::RegionSelectionOperation::Add, settings) == 2);

    REQUIRE(editor.restore_selection(snapshot));
    CHECK(editor.selected_face_count() == 1);
    CHECK(editor.selected_faces() == snapshot);
    CHECK_FALSE(editor.restore_selection({1}));
    CHECK(editor.selected_faces() == snapshot);
}

TEST_CASE("vertex color material selection spans disconnected matching faces", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = separated_triangle_mesh(3);
    std::vector<RGBA> colors = solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f});
    for (size_t vertex = 3; vertex < 6; ++vertex)
        colors[vertex] = {0.0f, 0.0f, 1.0f, 1.0f};

    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(std::move(mesh), std::move(colors), error));
    const std::vector<RGBA> palette {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f}
    };

    CHECK(editor.select_palette_material(palette, 0) == 2);
    CHECK(editor.selected_faces() == std::vector<uint8_t> {1, 0, 1});
    CHECK(editor.select_palette_material(palette, 1) == 1);
    CHECK(editor.selected_faces() == std::vector<uint8_t> {0, 1, 0});
}

TEST_CASE("vertex color material selection uses nearest palette color with stable ties", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = separated_triangle_mesh(2);
    std::vector<RGBA> colors {
        {0.8f, 0.1f, 0.1f, 1.0f}, {0.8f, 0.1f, 0.1f, 1.0f}, {0.8f, 0.1f, 0.1f, 1.0f},
        {0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f}
    };
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(std::move(mesh), std::move(colors), error));
    const std::vector<RGBA> palette {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f}
    };

    CHECK(editor.select_palette_material(palette, 0) == 2);
    const std::vector<uint8_t> snapshot = editor.selected_faces();
    CHECK(editor.select_palette_material(palette, 1) == 0);
    CHECK(editor.restore_selection(snapshot));
    CHECK(editor.selected_face_count() == 2);
}

TEST_CASE("vertex color material selection rejects invalid inputs without changing selection", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));
    const std::vector<RGBA> palette {{1.0f, 0.0f, 0.0f, 1.0f}};
    REQUIRE(editor.select_palette_material(palette, 0) == 2);
    const std::vector<uint8_t> snapshot = editor.selected_faces();

    CHECK(editor.select_palette_material({}, 0) == 2);
    CHECK(editor.selected_faces() == snapshot);
    CHECK(editor.select_palette_material(palette, 1) == 2);
    CHECK(editor.selected_faces() == snapshot);
}

TEST_CASE("vertex color overhang localization selects only significant elevated regions", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = overhang_region_mesh();
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));

    CHECK(editor.select_elevated_overhang_regions() == 2);
    CHECK(editor.selected_faces() == std::vector<uint8_t> {0, 0, 1, 1, 0, 0});
}

TEST_CASE("vertex color evidence selection ignores duplicate and invalid face indices", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));

    CHECK(editor.select_faces({1, 1, 99}) == 1);
    CHECK(editor.selected_faces() == std::vector<uint8_t> {0, 1});
}

TEST_CASE("vertex color evidence selection preserves an existing selection when evidence is empty", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));
    REQUIRE(editor.select_faces({0}) == 1);
    const std::vector<uint8_t> snapshot = editor.selected_faces();

    CHECK(editor.select_faces({2, 100}) == 0);
    CHECK(editor.selected_faces() == snapshot);
    CHECK(editor.select_faces({}) == 0);
    CHECK(editor.selected_faces() == snapshot);
}

TEST_CASE("vertex color overhang localization preserves selection when no region qualifies", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = overhang_region_mesh();
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
    REQUIRE(editor.select_palette_material({{1.0f, 0.0f, 0.0f, 1.0f}}, 0) == mesh.indices.size());
    const std::vector<uint8_t> snapshot = editor.selected_faces();

    AI::OverhangRegionSettings settings;
    settings.ground_band_mm = 6.0f;
    CHECK(editor.select_elevated_overhang_regions(settings) == 0);
    CHECK(editor.selected_faces() == snapshot);

    settings.ground_band_mm = 0.5f;
    settings.minimum_region_area_mm2 = 101.0f;
    CHECK(editor.select_elevated_overhang_regions(settings) == 0);
    CHECK(editor.selected_faces() == snapshot);
}

TEST_CASE("vertex color smart region stops at sharp geometry boundaries", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {1.0f, 0.0f, 1.0f}
    };
    mesh.indices = {{0, 1, 2}, {1, 0, 3}};

    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(std::move(mesh), red_colors(), error));
    AI::RegionSelectionSettings settings;
    settings.normal_angle_degrees = 45.0f;
    CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
}

TEST_CASE("vertex color picking returns the nearest visible face", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));

    const std::optional<size_t> hit = editor.pick_face({0.2, 0.2, 1.0}, {0.0, 0.0, -1.0});
    REQUIRE(hit);
    CHECK(*hit == 0);
    CHECK_FALSE(editor.pick_face({2.0, 2.0, 1.0}, {0.0, 0.0, -1.0}));
}

TEST_CASE("vertex color picking acceleration preserves nearest and deterministic hits", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = stacked_triangle_mesh(512);
    mesh.indices.push_back(mesh.indices.back());
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));

    const std::optional<size_t> hit = editor.pick_face({0.2, 0.2, 10.0}, {0.0, 0.0, -4.0});
    REQUIRE(hit);
    CHECK(*hit == 511);
    CHECK_FALSE(editor.pick_face({1.2, 1.2, 10.0}, {0.0, 0.0, -1.0}));
    CHECK_FALSE(editor.pick_face({0.2, 0.2, 10.0}, {0.0, 0.0, 0.0}));
}

TEST_CASE("vertex color OBJ copy preserves structure and rewrites selected vertices", "[AI][VertexColorRegion]")
{
    const boost::filesystem::path root =
        boost::filesystem::current_path() / "generated_models" / "test-local-recolor";
    boost::filesystem::create_directories(root);
    const boost::filesystem::path source = root / "source.obj";
    const boost::filesystem::path destination = root / "edited.obj";
    {
        boost::filesystem::ofstream stream(source, std::ios::trunc);
        stream << "o Body\n"
               << "v 0 0 0 1 0 0 1\n"
               << "v 1 0 0 1 0 0 1\n"
               << "v 0 1 0 1 0 0 1\n"
               << "v 1 1 0 0 0 1 1\n"
               << "g Surface\n"
               << "f 1 2 3\n"
               << "f 2 4 3\n";
    }

    AI::VertexColorRegionEditor editor;
    std::string error;
    std::vector<RGBA> colors = red_colors();
    colors[3] = {0.0f, 0.0f, 1.0f, 1.0f};
    REQUIRE(editor.initialize(square_mesh(), colors, error));
    AI::RegionSelectionSettings settings;
    settings.color_distance = 0.12f;
    REQUIRE(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    REQUIRE(editor.apply_color_to_obj_copy({0.0f, 1.0f, 0.0f, 1.0f}, source, destination, error));
    CHECK(editor.vertex_colors() == colors);
    CHECK(editor.selected_face_count() == 1);
    // A subsequent failed export must not contaminate the source editor either.
    CHECK_FALSE(editor.apply_color_to_obj_copy({1.0f, 1.0f, 0.0f, 1.0f}, source / "missing.obj", destination, error));
    CHECK(editor.vertex_colors() == colors);

    boost::filesystem::ifstream stream(destination);
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(contents.find("o Body") != std::string::npos);
    CHECK(contents.find("g Surface") != std::string::npos);
    CHECK(contents.find("f 1 2 3") != std::string::npos);
    CHECK(contents.find("v 0 0 0 0.000000 1.000000 0.000000 1.000000") != std::string::npos);
    CHECK(contents.find("v 1 1 0 0.000000 0.000000 1.000000 1.000000") != std::string::npos);
}

TEST_CASE("vertex color OBJ round trip preserves RGB channel order", "[AI][VertexColorRegion]")
{
    const boost::filesystem::path root =
        boost::filesystem::current_path() / "generated_models" / "test-local-recolor-rgb";
    boost::filesystem::create_directories(root);
    const boost::filesystem::path source = root / "source.obj";
    {
        boost::filesystem::ofstream stream(source, std::ios::trunc);
        stream << "v 0 0 0 1 1 1 1\n"
               << "v 1 0 0 1 1 1 1\n"
               << "v 0 1 0 1 1 1 1\n"
               << "v 1 1 0 1 1 1 1\n"
               << "f 1 2 3\n"
               << "f 2 4 3\n";
    }

    const std::array<std::pair<const char*, RGBA>, 3> cases {{
        {"red", {1.0f, 0.0f, 0.0f, 1.0f}},
        {"green", {0.0f, 1.0f, 0.0f, 1.0f}},
        {"blue", {0.0f, 0.0f, 1.0f, 1.0f}}
    }};
    for (const auto& [name, expected] : cases) {
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(square_mesh(), red_colors(), error));
        AI::RegionSelectionSettings settings;
        settings.color_distance = 1.0f;
        REQUIRE(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 2);

        const boost::filesystem::path destination = root / (std::string(name) + ".obj");
        REQUIRE(editor.apply_color_to_obj_copy(expected, source, destination, error));

        TriangleMesh mesh;
        ObjInfo obj_info;
        std::string message;
        REQUIRE(load_obj(destination.string().c_str(), &mesh, obj_info, message));
        REQUIRE(obj_info.vertex_colors.size() == 4);
        for (const RGBA& actual : obj_info.vertex_colors) {
            CHECK(actual[0] == Catch::Approx(expected[0]));
            CHECK(actual[1] == Catch::Approx(expected[1]));
            CHECK(actual[2] == Catch::Approx(expected[2]));
            CHECK(actual[3] == Catch::Approx(expected[3]));
        }
    }
}

TEST_CASE("uncolored OBJ loads safely without enabling local recoloring", "[AI][ModelPreview]")
{
    const boost::filesystem::path root =
        boost::filesystem::current_path() / "generated_models" / "test-uncolored-preview";
    boost::filesystem::create_directories(root);
    const boost::filesystem::path source = root / "uncolored.obj";
    {
        boost::filesystem::ofstream stream(source, std::ios::trunc);
        stream << "v 0 0 0\n"
               << "v 10 0 0\n"
               << "v 0 10 0\n"
               << "v 0 0 10\n"
               << "f 1 3 2\n"
               << "f 1 2 4\n"
               << "f 2 3 4\n"
               << "f 3 1 4\n";
    }

    TriangleMesh mesh;
    ObjInfo obj_info;
    std::string message;
    REQUIRE(load_obj(source.string().c_str(), &mesh, obj_info, message));
    CHECK(mesh.its.indices.size() == 4);
    CHECK(obj_info.vertex_colors.empty());

    AI::VertexColorRegionEditor editor;
    CHECK_FALSE(editor.initialize(std::move(mesh.its), std::move(obj_info.vertex_colors), message));
    CHECK_FALSE(editor.ready());
    CHECK(message == "Local recoloring requires OBJ vertex colors.");
}
