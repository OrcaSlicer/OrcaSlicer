#include "slic3r/GUI/TextureImportDialog.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "slic3r/GUI/ModelColorImportResult.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("AI original color imports keep target count editable independently of physical feeds", "[ModelColorImport]")
{
    AI::ModelImportRequest request;
    const auto options = model_import_color_options(request);
    CHECK(options.initial_target_colors == 12);
    CHECK(options.initial_target_colors > options.physical_filament_limit);
    CHECK(options.fixed_palette.empty());
    CHECK(options.fixed_mapping_palette.empty());
    CHECK(options.initial_color_smoothing == 0);
    CHECK(options.preserve_existing_filaments);
    CHECK(options.z_up);
    const auto cube = its_make_cube(10, 10, 10);
    TexturedMesh source;
    for (const auto& v : cube.vertices) source.vertices.push_back({v.x(), v.y(), v.z()});
    for (const auto& f : cube.indices) source.indices.push_back({f.x(), f.y(), f.z()});
    source.precomputed_face_colors = {{255,0,0}, {0,255,0}, {0,0,255}, {255,255,0},
        {255,0,255}, {0,255,255}, {0,0,0}, {255,255,255}, {128,0,0}, {0,128,0}, {0,0,128}, {128,128,128}};
    REQUIRE(source.indices.size() == source.precomputed_face_colors.size());
    TexturePaintingSettings settings;
    settings.target_colors_num = options.initial_target_colors;
    settings.smooth_weight = options.initial_color_smoothing / 10.;
    settings.fixed_palette = options.fixed_palette;
    settings.fixed_mapping_palette = options.fixed_mapping_palette;
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(source, painted, settings));
    CHECK(painted.cluster_colors.size() > options.physical_filament_limit);
    CHECK(painted.indices == source.indices);
    // The AI handoff must not change defaults for ordinary file imports.
    const TextureImportOptions ordinary;
    CHECK(ordinary.initial_target_colors == 0);
    CHECK(ordinary.initial_color_smoothing == -1);
    CHECK(ordinary.physical_filament_limit == 0);
}

TEST_CASE("AI recoloring preserves accepted trial and local face overrides across a fresh match", "[ModelColorImport]")
{
    AI::ModelImportRequest request;
    request.color_trial = AI::ModelColorTrial {{{.8f, .6f, .5f}, {.7f, .2f, .2f}},
                                               {{.9f, .7f, .6f}, {.8f, .3f, .4f}}};
    request.face_color_overrides = {{7, {.2f, .7f, .3f}}};
    const auto accepted = model_import_color_options(request);
    CHECK(accepted.fixed_mapping_palette == std::vector<std::array<size_t, 3>> {{204, 153, 128}, {179, 51, 51}});
    CHECK(accepted.fixed_palette == std::vector<std::array<size_t, 3>> {{230, 179, 153}, {204, 77, 102}});
    CHECK(accepted.face_color_overrides == std::vector<std::pair<size_t, std::array<size_t, 3>>> {{7, {51, 179, 77}}});
    request.color_trial.reset();
    const auto fresh = model_import_color_options(request);
    CHECK(fresh.fixed_palette.empty());
    CHECK(fresh.fixed_mapping_palette.empty());
    CHECK(fresh.face_color_overrides == accepted.face_color_overrides);
    CHECK(fresh.initial_color_smoothing == 0);
}

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
