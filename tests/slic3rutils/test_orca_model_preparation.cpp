#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Orca/OrcaModelPreparation.hpp"
#include "slic3r/GUI/AI/Orca/ModelColorUpdate.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"
#include "test_utils.hpp"
#include <limits>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("Updating a matching mesh changes only its color assignments", "[ModelColorUpdate]")
{
    Model model;
    auto* target = model.add_object("edited model", "source.obj", TriangleMesh(its_make_cube(20, 30, 40)));
    target->add_instance()->set_offset(Vec3d(110, 75, 20));
    target->config.set_key_value("fill_density", new ConfigOptionPercent(23.));
    const auto identity = target->id();
    const auto matrix = target->instances.front()->get_matrix();
    auto* incoming = model.add_object("incoming", "source.obj", TriangleMesh(its_make_cube(20, 30, 40)));
    incoming->config.set("extruder", 3);
    incoming->volumes.front()->config.set("extruder", 3);
    TriangleSelector selector(incoming->volumes.front()->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder2);
    REQUIRE(incoming->volumes.front()->mmu_segmentation_facets.set(selector));
    REQUIRE(update_compatible_model_colors(*target, *incoming));
    CHECK(target->id() == identity);
    CHECK(target->name == "edited model");
    CHECK(target->instances.front()->get_matrix().isApprox(matrix));
    CHECK(target->config.opt_float("fill_density") == 23.);
    CHECK(target->config.extruder() == 3);
    CHECK(target->volumes.front()->mmu_segmentation_facets.get_data() == incoming->volumes.front()->mmu_segmentation_facets.get_data());
}

TEST_CASE("Updating colors rejects changed geometry without modifying the target", "[ModelColorUpdate]")
{
    Model model;
    auto* target = model.add_object("original", "source.obj", TriangleMesh(its_make_cube(20, 30, 40)));
    auto* incoming = model.add_object("modified", "source.obj", TriangleMesh(its_make_cube(21, 30, 40)));
    target->config.set("extruder", 2);
    const auto before = target->volumes.front()->mmu_segmentation_facets.timestamp();
    REQUIRE_FALSE(update_compatible_model_colors(*target, *incoming));
    CHECK(target->config.extruder() == 2);
    CHECK(target->volumes.front()->mmu_segmentation_facets.timestamp() == before);
}

TEST_CASE("Semantic midpoint leaves apply to the unchanged imported MMU topology", "[ModelColorUpdate][SubfaceColor]")
{
    const indexed_triangle_set mesh = its_make_cube(20, 30, 40);
    TriangleMesh triangle_mesh(mesh);
    TriangleSelector selector(triangle_mesh);
    selector.set_facet(0, EnforcerBlockerType::Extruder1);
    selector.set_facet(1, EnforcerBlockerType::Extruder2);
    auto painting = selector.serialize();
    const std::array<float, 3> skin {.8f,.5f,.3f}, white {.95f,.95f,.95f};
    const std::vector<std::pair<size_t, std::array<float, 3>>> roots {{0, skin}, {1, white}};
    const std::vector<AI::ModelSubfaceColorOverride> leaves {{0, 1, 0, white}};
    std::string error;
    REQUIRE(apply_subface_color_overrides(mesh, mesh, painting, roots, leaves, error));
    CHECK(error.empty());

    TriangleSelector restored(triangle_mesh);
    restored.deserialize(painting);
    EnforcerBlockerType state;
    CHECK_FALSE(restored.facet_state(0, state));
    CHECK(restored.facet_state(1, state));
    CHECK(state == EnforcerBlockerType::Extruder2);
    CHECK(restored.num_facets(EnforcerBlockerType::Extruder1) == 3);
    CHECK(restored.num_facets(EnforcerBlockerType::Extruder2) == 2);
}

TEST_CASE("Semantic midpoint import rejects changed topology transactionally", "[ModelColorUpdate][SubfaceColor]")
{
    const indexed_triangle_set expected = its_make_cube(20, 30, 40);
    const indexed_triangle_set changed = its_make_cube(21, 30, 40);
    TriangleMesh triangle_mesh(changed);
    TriangleSelector selector(triangle_mesh);
    selector.set_facet(0, EnforcerBlockerType::Extruder1);
    auto painting = selector.serialize();
    const auto before = painting;
    const std::array<float, 3> skin {.8f,.5f,.3f};
    std::string error;
    REQUIRE_FALSE(apply_subface_color_overrides(expected, changed, painting,
        {{0, skin}}, {{0, 1, 0, skin}}, error));
    CHECK_FALSE(error.empty());
    CHECK(painting == before);
}

TEST_CASE("Native preparation preserves source and targets total world height", "[ai][OrcaModelPreparation]")
{
    Model model;
    auto* object = model.add_object("fixed figurine", "original.obj", TriangleMesh(its_make_cube(20, 30, 40)));
    object->add_instance();
    object->volumes.front()->config.set_key_value("extruder", new ConfigOptionInt(2));
    TriangleSelector selector(object->volumes.front()->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder2);
    selector.set_facet(1, EnforcerBlockerType::Extruder1);
    REQUIRE(object->volumes.front()->mmu_segmentation_facets.set(selector));
    const auto source_id = object->volumes.front()->id();
    const auto source_mesh = object->volumes.front()->mesh().its.vertices;
    const auto source_paint = object->volumes.front()->mmu_segmentation_facets.timestamp();
    SECTION("ordinary orientation") {}
    SECTION("rotated mirrored instance") {
        object->instances.front()->set_rotation(Vec3d(0.3, 0.4, 0.7));
        object->instances.front()->set_mirror(Vec3d(-1, 1, 1));
        object->instances.front()->set_scaling_factor(Vec3d(1.2, 0.8, 2.0));
        object->instances.front()->set_offset(Vec3d(110, 90, 20));
        object->invalidate_bounding_box();
    }
    const auto original_matrix = object->instances.front()->get_matrix();
    const auto original_bounds = object->instance_bounding_box(0);
    Model scaled_model(model);
    auto* scaled = scaled_model.objects.front();
    apply_model_preparation(*scaled, prepare_model(*object, {120, false, 3}));
    CHECK_THAT(scaled->instance_bounding_box(0).size().z(), Catch::Matchers::WithinAbs(120, 0.001));
    CHECK_THAT(scaled->instance_bounding_box(0).min.z(), Catch::Matchers::WithinAbs(0, 0.001));
    CHECK_THAT(scaled->instance_bounding_box(0).center().x(), Catch::Matchers::WithinAbs(original_bounds.center().x(), 0.001));
    CHECK_THAT(scaled->instance_bounding_box(0).center().y(), Catch::Matchers::WithinAbs(original_bounds.center().y(), 0.001));
    Model based_model(scaled_model);
    auto* based = based_model.objects.front();
    apply_model_preparation(*based, prepare_model(*scaled, {120, true, 3}));
    CHECK_THAT(based->instance_bounding_box(0).size().z(), Catch::Matchers::WithinAbs(120, 0.001));
    CHECK_THAT(based->instance_bounding_box(0).min.z(), Catch::Matchers::WithinAbs(0, 0.001));
    REQUIRE(based->volumes.size() == 2);
    TriangleMesh base_mesh = based->volumes.back()->mesh();
    base_mesh.transform(based->instances.front()->get_matrix() * based->volumes.back()->get_matrix());
    CHECK_THAT(base_mesh.bounding_box().max.z(), Catch::Matchers::WithinAbs(3, 0.001));
    CHECK_THAT(base_mesh.bounding_box().min.z(), Catch::Matchers::WithinAbs(0, 0.001));
    CHECK(based->volumes.back()->extruder_id() == 2);
    CHECK(based->volumes.front()->id() == source_id);
    CHECK(based->volumes.front()->mesh().its.vertices == source_mesh);
    CHECK(based->volumes.front()->mmu_segmentation_facets.timestamp() == source_paint);
    CHECK(based->volumes.front()->mmu_segmentation_facets.get_data() == object->volumes.front()->mmu_segmentation_facets.get_data());
    TriangleMesh body_mesh = based->volumes.front()->mesh();
    body_mesh.transform(based->instances.front()->get_matrix() * based->volumes.front()->get_matrix());
    CHECK_THAT(body_mesh.bounding_box().min.z(), Catch::Matchers::WithinAbs(2.8, 0.001));
    CHECK(object->volumes.size() == 1);
    CHECK(object->instances.front()->get_matrix().isApprox(original_matrix));
    CHECK(object->instance_bounding_box(0).size().isApprox(original_bounds.size()));
    CHECK(based->input_file == "original.obj");
    CHECK_THROWS_WITH(prepare_model(*based, {120, true, 3}), "base_already_exists");
    CHECK_NOTHROW(prepare_model(*based, {130, false, 3}));
}

TEST_CASE("Generated artifact recognition survives portable 3MF source paths", "[ai][ModelColorUpdate]")
{
    const std::string name = "orcaslicer-ai-428a0fe0-8183-4afd-9322-e16be8e77df4.obj";
    CHECK(same_generated_artifact_name(name, "D:/generated/" + name));
    CHECK(same_generated_artifact_name("C:\\old\\" + name, "/new/" + name));
    CHECK_FALSE(same_generated_artifact_name("portrait.obj", "/new/portrait.obj"));
    CHECK_FALSE(same_generated_artifact_name("orcaslicer-ai-name.obj", "orcaslicer-ai-name.obj"));
    CHECK_FALSE(same_generated_artifact_name(name, "orcaslicer-ai-428a0fe0-8183-4afd-9322-e16be8e77df5.obj"));
    const std::string finish = "orcaslicer-ai-finish-8cf12cbe-5f0b-4145-8347-8f6f3d3ebd9b.obj";
    CHECK(same_generated_artifact_name(finish, "/new/" + finish));
    const std::string hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string glb = "orcaslicer-ai-glb-" + hash + ".obj";
    CHECK(same_generated_artifact_name(glb, "/new/ai-import/" + glb));
    CHECK(same_generated_artifact_name("C:\\old\\ai-import\\" + glb, "/new/ai-import/" + glb));
    CHECK_FALSE(same_generated_artifact_name(glb, "orcaslicer-ai-glb-" + hash.substr(0, 63) + "0.obj"));
    for (const std::string invalid : std::vector<std::string>{"model.obj", "orcaslicer-ai-glb-.obj",
             "orcaslicer-ai-glb-" + hash.substr(1) + ".obj", "orcaslicer-ai-glb-" + hash + "0.obj",
             "orcaslicer-ai-glb-g" + hash.substr(1) + ".obj", "orcaslicer-ai-glb-" + hash + ".glb"}) {
        CHECK_FALSE(same_generated_artifact_name(invalid, "/new/" + invalid));
        CHECK_FALSE(same_generated_artifact_name(invalid, glb));
    }
}

TEST_CASE("GLB import identity survives a normal 3MF save and reopen", "[ai][ModelColorUpdate]")
{
    const std::string name = "orcaslicer-ai-glb-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.obj";
    const std::string source = "/old/cache/ai-import/" + name;
    Model model;
    auto* object = model.add_object("generated GLB", source.c_str(), TriangleMesh(its_make_cube(8, 8, 40)));
    object->add_instance();
    ScopedTemporaryDir backup("glb-identity-source");
    model.set_backup_path(backup.string());
    ScopedTemporaryFile file(".3mf");
    const std::string path = file.string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    PlateData plate;
    plate.plate_index = 0;
    StoreParams params;
    params.path = path.c_str();
    params.model = &model;
    params.config = &config;
    // A normal project deliberately omits FullPathSources.
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));

    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    Model restored = Model::read_from_file(path, &restored_config, &substitutions,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig, &plates, &presets);
    release_PlateData_list(plates);
    for (auto* preset : presets) delete preset;
    REQUIRE(restored.objects.size() == 1);
    const auto* result = restored.objects.front();
    REQUIRE(result->volumes.size() == 1);
    CHECK(result->input_file == path);
    const auto& saved = result->volumes.front()->source.input_file;
    CHECK(saved == name);
    CHECK(same_generated_artifact_name(saved, "/another/cache/ai-import/" + name));
    CHECK(same_generated_artifact_name(saved, "D:\\new-cache\\ai-import\\" + name));
    CHECK_FALSE(same_generated_artifact_name(saved,
        "/another/cache/ai-import/orcaslicer-ai-glb-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdee.obj"));
    CHECK_FALSE(same_generated_artifact_name(saved, "/another/cache/ai-import/model.obj"));
}

TEST_CASE("Invalid preparation never mutates the current project", "[ai][OrcaModelPreparation]")
{
    Model model;
    auto* object = model.add_object("fixed", "original.obj", TriangleMesh(its_make_cube(20, 30, 40)));
    object->add_instance();
    for (double height : {0.0, -120.0, 1001.0, std::numeric_limits<double>::quiet_NaN()})
        CHECK_THROWS_AS(prepare_model(*object, {height, true, 3}), std::invalid_argument);
    CHECK_THROWS_AS(prepare_model(*object, {2, true, 3}), std::invalid_argument);
    object->add_instance();
    CHECK_THROWS_WITH(prepare_model(*object, {120, false, 3}), "requires_single_uncut_instance");
    CHECK(object->volumes.size() == 1);
    CHECK_THAT(object->instance_bounding_box(0).size().z(), Catch::Matchers::WithinAbs(40, 0.001));
}

TEST_CASE("Prepared geometry and painting survive an Orca 3MF save and reopen", "[ai][OrcaModelPreparation]")
{
    Model model;
    auto* object = model.add_object("fixed figurine", "original.obj", TriangleMesh(its_make_cube(8, 8, 40)));
    object->add_instance()->set_rotation(Vec3d(0, 0, 0.4));
    TriangleSelector selector(object->volumes.front()->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder1);
    REQUIRE(object->volumes.front()->mmu_segmentation_facets.set(selector));
    apply_model_preparation(*object, prepare_model(*object, {120, true, 3}));
    ScopedTemporaryDir backup("preparation-source");
    model.set_backup_path(backup.string());
    ScopedTemporaryFile file(".3mf");
    const std::string path = file.string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    PlateData plate;
    plate.plate_index = 0;
    StoreParams params;
    params.path = path.c_str();
    params.model = &model;
    params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));

    Model restored;
    ScopedTemporaryDir restored_backup("preparation-restored");
    restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool is_bbl = false, is_orca = false;
    Semver version;
    const bool loaded = load_bbs_3mf(path.c_str(), &restored_config, &substitutions, &restored, &plates,
        &presets, &is_bbl, &is_orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    for (auto* preset : presets) delete preset;
    REQUIRE(loaded);
    REQUIRE(restored.objects.size() == 1);
    const auto* result = restored.objects.front();
    REQUIRE(result->volumes.size() == 2);
    CHECK_THAT(result->instance_bounding_box(0).size().z(), Catch::Matchers::WithinAbs(120, 0.001));
    CHECK_FALSE(result->volumes.front()->mmu_segmentation_facets.empty());
    CHECK(result->volumes.back()->name == "AI round base");
    CHECK_THROWS_WITH(prepare_model(*result, {120, true, 3}), "base_already_exists");
}
