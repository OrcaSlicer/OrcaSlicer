#include <catch2/catch_all.hpp>

#include "libslic3r/TexturePainting.hpp"
#include "slic3r/GUI/ModelColorImportResult.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("Native color import feedback counts physical and mixed filament assignments", "[ModelColorImport]")
{
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(its_make_cube(10, 10, 10)), ModelVolumeType::MODEL_PART, false);
    const auto& mesh = volume->mesh().its;
    PaintedMesh painted;
    for (const auto& vertex : mesh.vertices)
        painted.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
    for (const auto& face : mesh.indices)
        painted.indices.push_back({face[0], face[1], face[2]});
    painted.cluster_colors = {{255, 0, 0}, {128, 0, 128}};
    for (size_t face = 0; face < mesh.indices.size(); ++face)
        painted.face_colors.push_back(painted.cluster_colors[face % 2]);
    // The native matcher remaps newly created mixtures into project IDs; these
    // must not be restricted to the 1-6 physical feed slots on the AI side.
    std::vector<FilamentMatch> matches(2);
    matches[0].cluster_index = 0;
    matches[0].filament_index = 0;
    matches[1].cluster_index = 1;
    matches[1].filament_index = 7;
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *volume));

    ModelColorImportResult result;
    collect_model_color_import_result(&result, model, painted.cluster_colors.size());
    CHECK(result.colors_applied);
    CHECK_FALSE(result.cancelled);
    CHECK(result.source_color_count == 2);
    CHECK(result.mapped_color_count == 2);
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder8));
}

TEST_CASE("Geometry-only imports do not report successful color matching", "[ModelColorImport]")
{
    Model model;
    model.add_object()->add_volume(TriangleMesh(its_make_cube(10, 10, 10)), ModelVolumeType::MODEL_PART, false);
    ModelColorImportResult result;
    collect_model_color_import_result(&result, model, 0);
    CHECK_FALSE(result.colors_applied);
    CHECK(result.mapped_color_count == 0);
}
