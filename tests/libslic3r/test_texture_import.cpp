#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "texture_import_cgal.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace {

void check_texture_import(const std::string& fixture)
{
    const std::string filename = std::string(TEST_DATA_DIR) + "/texture_import/" + fixture;
    INFO(filename);
    const Slic3r::Model model = Slic3r::Model::read_from_file(filename);
    REQUIRE_FALSE(model.objects.empty());
    REQUIRE(model.texture_mesh);
    const Slic3r::TexturedMesh& mesh = *model.texture_mesh;

    Slic3r::TexturePaintingSettings settings;
    settings.target_colors_num = 0; // Match the dialog's initial automatic color count.
    settings.smooth_weight = 0.5;
    settings.mesh_repair_decision = Slic3r::TexturePaintingSettings::MeshRepairDecision::ImportWithoutRepair;
    Slic3r::PaintedMesh painted;
    // Follow the dialog worker's format-dependent path, including the boundary
    // smoothing that needs a consistent CGAL Kd_tree layout across libraries.
    if (!mesh.precomputed_face_colors.empty())
        REQUIRE(Slic3r::face_colors_to_painting(mesh, painted, settings));
    else
        REQUIRE(Slic3r::texture_to_painting(mesh, painted, settings));

    REQUIRE_FALSE(painted.indices.empty());
    REQUIRE(painted.face_colors.size() == painted.indices.size());
    REQUIRE(std::any_of(painted.face_colors.begin(), painted.face_colors.end(),
        [](const auto& color) { return color[0] > color[2]; }));
    REQUIRE(std::any_of(painted.face_colors.begin(), painted.face_colors.end(),
        [](const auto& color) { return color[2] > color[0]; }));

    const std::vector<std::array<float, 4>> filament_colors{
        {1.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f}};
    const auto matches = Slic3r::match_clusters_to_filaments(
        painted.cluster_colors, filament_colors, {"Red PLA", "Blue PLA"});
    REQUIRE(matches.size() == painted.cluster_colors.size());
    REQUIRE(std::any_of(matches.begin(), matches.end(),
        [](const auto& match) { return match.filament_index == 0; }));
    REQUIRE(std::any_of(matches.begin(), matches.end(),
        [](const auto& match) { return match.filament_index == 1; }));
}

} // namespace

TEST_CASE("Color-boundary smoothing shares its CGAL tree layout with the geometry backend", "[TextureImport][CGAL][Regression]")
{
    // An ABI mismatch need not crash every link of the importer. Compare the
    // layouts directly so a lucky choice of inline symbols cannot mask it.
    REQUIRE(sizeof(TextureImportTest::SearchTree) == TextureImportTest::backend_search_tree_size());
}

TEST_CASE("A JPEG textured GLB retains both colors through filament matching", "[TextureImport][Regression]")
{
    check_texture_import("two_color_jpeg.glb");
}

TEST_CASE("A PNG textured GLB retains both colors through filament matching", "[TextureImport][Regression]")
{
    check_texture_import("two_color_png.glb");
}

TEST_CASE("An OBJ preserves both MTL colors through filament matching", "[TextureImport][OBJ][Regression]")
{
    check_texture_import("two_color_sphere/test_two_color_sphere.obj");
}

TEST_CASE("An OBJ resolves its MTL and external JPEG through filament matching", "[TextureImport][OBJ][Regression]")
{
    check_texture_import("two_color_sphere/test_two_color_sphere_textured.obj");
}

TEST_CASE("Converting units scales the texture mesh with the model objects", "[TextureImport][Regression]")
{
    const bool from_meters = GENERATE(true, false);
    const double to_mm = from_meters ? 1000. : 25.4;
    INFO((from_meters ? "from meters" : "from inches"));

    Slic3r::Model model;
    Slic3r::ModelObject* object = model.add_object();
    object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(0.02, 0.02, 0.02)));
    model.texture_mesh = std::make_shared<Slic3r::TexturedMesh>();
    model.texture_mesh->vertices = {{0.f, 0.f, 0.f}, {0.02f, 0.02f, 0.02f}};

    if (from_meters)
        model.convert_from_meters(false);
    else
        model.convert_from_imperial_units(false);

    REQUIRE_THAT(object->volumes.front()->mesh().bounding_box().size().x(), Catch::Matchers::WithinRel(0.02 * to_mm, 1e-4));
    REQUIRE_THAT(double(model.texture_mesh->vertices.back()[0]), Catch::Matchers::WithinRel(0.02 * to_mm, 1e-4));
}
