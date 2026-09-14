#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "../test_utils.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <sstream>

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

TEST_CASE("Similar color clicks accumulate connected patches without selecting disconnected colors", "[VertexColorRegion]")
{
    auto mesh = seamed_square_mesh();
    // A disconnected triangle has the same blue as the second patch.
    mesh.vertices.insert(mesh.vertices.end(), {{10, 0, 0}, {11, 0, 0}, {10, 1, 0}});
    mesh.indices.emplace_back(6, 7, 8);
    auto colors = solid_colors(mesh.vertices.size(), {0, 0, 1, 1});
    for (size_t i = 0; i < 3; ++i) colors[i] = {1, 0, 0, 1};
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(std::move(mesh), std::move(colors), error));
    AI::RegionSelectionSettings settings;
    REQUIRE(editor.update_selection(0, AI::RegionSelectionOperation::AddSimilar, settings) == 1);
    const auto first = editor.selected_faces();
    REQUIRE(editor.update_selection(1, AI::RegionSelectionOperation::AddSimilar, settings) == 2);
    CHECK(editor.selected_faces() == std::vector<uint8_t>{1, 1, 0});
    CHECK(editor.update_selection(1, AI::RegionSelectionOperation::AddSimilar, settings) == 2);
    REQUIRE(editor.restore_selection(first));
    CHECK(editor.selected_faces() == std::vector<uint8_t>{1, 0, 0});
    CHECK(editor.update_selection(1, AI::RegionSelectionOperation::Replace, settings) == 1);
    CHECK(editor.selected_faces() == std::vector<uint8_t>{0, 1, 0});
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

TEST_CASE("vertex color OBJ copy preserves groups and isolates selected faces", "[AI][VertexColorRegion]")
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
    TriangleMesh result;
    ObjInfo info;
    REQUIRE(load_obj(destination.string().c_str(), &result, info, error));
    REQUIRE(result.its.indices.size() == 2);
    for (size_t corner = 0; corner < 3; ++corner) {
        CHECK(info.vertex_colors[result.its.indices[0][corner]] == RGBA{0, 1, 0, 1});
        CHECK(info.vertex_colors[result.its.indices[1][corner]] == colors[square_mesh().indices[1][corner]]);
    }
}

TEST_CASE("Recoloring shared faces leaves neighboring corner colors and source topology intact", "[VertexColorRegion][Regression]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    auto colors = red_colors();
    colors[3] = {0.1f, 0.2f, 0.3f, 1};
    const auto mesh = square_mesh();
    REQUIRE(editor.initialize(mesh, colors, error));
    REQUIRE(editor.select_faces({0}) == 1);
    const RGBA green {0, 1, 0, 1};
    const RGBA blue {0, 0, 1, 1};
    REQUIRE(editor.apply_color(green));
    REQUIRE(editor.has_color_overrides());
    CHECK(editor.mesh().indices == mesh.indices);
    CHECK(editor.mesh().vertices == mesh.vertices);
    CHECK(editor.vertex_colors() == colors);
    for (size_t corner = 0; corner < 3; ++corner) {
        CHECK(editor.corner_color(0, corner) == green);
        CHECK(editor.corner_color(1, corner) == colors[mesh.indices[1][corner]]);
    }
    REQUIRE(editor.select_palette_material({green, red_colors()[0]}, 0) == 1);
    CHECK(editor.selected_faces() == std::vector<uint8_t>{1, 0});
    REQUIRE(editor.select_faces({1}) == 1);
    REQUIRE(editor.apply_color(blue));
    for (size_t corner = 0; corner < 3; ++corner) {
        CHECK(editor.corner_color(0, corner) == green);
        CHECK(editor.corner_color(1, corner) == blue);
    }
    editor.clear();
    CHECK_FALSE(editor.has_color_overrides());
}

TEST_CASE("Face edits survive OBJ and GLB exports without bleeding across shared edges", "[VertexColorRegion][Regression]")
{
    const auto source_extension = GENERATE(std::string(".obj"), std::string(".glb"));
    const auto destination_extension = GENERATE(std::string(".obj"), std::string(".glb"));
    ScopedTemporaryFile source(source_extension);
    ScopedTemporaryFile destination(destination_extension);
    auto original_colors = red_colors();
    original_colors[3] = {0.2f, 0.3f, 0.4f, 1};
    std::string error;
    REQUIRE(AI::write_model_artifact(source.path(), square_mesh(), original_colors, error));
    TriangleMesh input;
    ObjInfo input_colors;
    REQUIRE(AI::load_model_artifact(source.path(), input, input_colors, error));
    AI::VertexColorRegionEditor editor;
    REQUIRE(editor.initialize(input.its, input_colors.vertex_colors, error));
    REQUIRE(editor.select_faces({0}) == 1);
    const RGBA blue {0, 0, 1, 1};
    REQUIRE(editor.apply_color_to_obj_copy(blue, source.path(), destination.path(), error));
    CHECK_FALSE(editor.has_color_overrides());
    TriangleMesh output;
    ObjInfo output_colors;
    REQUIRE(AI::load_model_artifact(destination.path(), output, output_colors, error));
    REQUIRE(output.its.indices.size() == input.its.indices.size());
    REQUIRE(output_colors.vertex_colors.size() == output.its.vertices.size());
    for (size_t face = 0; face < input.its.indices.size(); ++face) {
        for (size_t corner = 0; corner < 3; ++corner) {
            const int original = input.its.indices[face][corner];
            const int derived = output.its.indices[face][corner];
            const RGBA expected = face == 0 ? blue : input_colors.vertex_colors[original];
            for (size_t axis = 0; axis < 3; ++axis)
                CHECK_THAT(output.its.vertices[derived][axis], Catch::Matchers::WithinAbs(input.its.vertices[original][axis], 1e-6));
            for (size_t channel = 0; channel < 4; ++channel)
                CHECK_THAT(output_colors.vertex_colors[derived][channel], Catch::Matchers::WithinAbs(expected[channel], 1e-6));
        }
    }
}

TEST_CASE("Quad recoloring preserves per-corner UV and normal references with relative OBJ indices", "[VertexColorRegion][Regression]")
{
    ScopedTemporaryFile source(".obj");
    ScopedTemporaryFile destination(".obj");
    {
        boost::filesystem::ofstream stream(source.path());
        stream << "v 0 0 0 1 0 0\nv 1 0 0 1 0 0\nv 1 1 0 1 0 0\nv 0 1 0 1 0 0\n"
               << "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\ng Cloth\n"
               << "f -4/1/1 -3/2/1 -2/3/1 -1/4/1\n";
    }
    std::string error;
    TriangleMesh input;
    ObjInfo info;
    REQUIRE(load_obj(source.string().c_str(), &input, info, error));
    AI::VertexColorRegionEditor editor;
    REQUIRE(editor.initialize(input.its, info.vertex_colors, error));
    REQUIRE(editor.select_faces({1}) == 1);
    REQUIRE(editor.apply_color_to_obj_copy({0, 0, 1, 1}, source.path(), destination.path(), error));
    boost::filesystem::ifstream stream(destination.path());
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(contents.find("g Cloth") != std::string::npos);
    std::istringstream lines(contents);
    std::vector<std::string> corner_references;
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream tokens(line);
        std::string tag;
        tokens >> tag;
        if (tag != "f") continue;
        std::string token;
        while (tokens >> token) {
            REQUIRE(token.find('/') != std::string::npos);
            corner_references.push_back(token.substr(token.find('/')));
        }
    }
    CHECK(corner_references == std::vector<std::string>{"/1/1", "/2/1", "/3/1", "/1/1", "/3/1", "/4/1"});
    TriangleMesh result;
    ObjInfo result_colors;
    REQUIRE(load_obj(destination.string().c_str(), &result, result_colors, error));
    REQUIRE(result.its.indices.size() == 2);
    for (size_t corner = 0; corner < 3; ++corner) {
        CHECK(result_colors.vertex_colors[result.its.indices[0][corner]] == RGBA{1, 0, 0, 1});
        CHECK(result_colors.vertex_colors[result.its.indices[1][corner]] == RGBA{0, 0, 1, 1});
    }
}

TEST_CASE("A changed source face layout cannot overwrite an existing recolor result", "[VertexColorRegion][Regression]")
{
    ScopedTemporaryFile source(".obj");
    ScopedTemporaryFile destination(".obj");
    std::string error;
    REQUIRE(AI::write_model_artifact(source.path(), square_mesh(), red_colors(), error));
    AI::VertexColorRegionEditor editor;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));
    REQUIRE(editor.select_faces({0}) == 1);
    REQUIRE(editor.apply_color({0, 1, 0, 1}));
    {
        boost::filesystem::ofstream stream(source.path(), std::ios::app);
        stream << "f 1 2 3\n";
    }
    {
        boost::filesystem::ofstream stream(destination.path());
        stream << "previous result";
    }
    CHECK_FALSE(editor.apply_color_to_obj_copy({0, 0, 1, 1}, source.path(), destination.path(), error));
    CHECK(editor.has_color_overrides());
    CHECK(editor.corner_color(0, 0) == RGBA{0, 1, 0, 1});
    boost::filesystem::ifstream stream(destination.path());
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(contents == "previous result");
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
