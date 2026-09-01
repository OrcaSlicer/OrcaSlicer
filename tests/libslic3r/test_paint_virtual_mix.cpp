#include <catch2/catch_all.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

namespace {

bool used_paint_state(const ModelVolume &vol, size_t state)
{
    const auto &used = vol.mmu_segmentation_facets.get_data().used_states;
    return state < used.size() && used[state];
}

ModelVolume *add_painted_cube(Model &model, const std::vector<std::pair<int, int>> &facet_states)
{
    ModelObject *obj = model.add_object("paint_mix_cube", "", make_cube(20., 20., 20.));
    REQUIRE(obj != nullptr);
    obj->add_instance();
    REQUIRE_FALSE(obj->volumes.empty());
    ModelVolume *vol = obj->volumes.front();
    TriangleSelector sel(vol->mesh());
    for (const auto &facet_state : facet_states)
        sel.set_facet(facet_state.first, EnforcerBlockerType(facet_state.second));
    REQUIRE(vol->mmu_segmentation_facets.set(sel));
    return vol;
}

void paint_left_right_mixes(ModelVolume &vol)
{
    const indexed_triangle_set &its = vol.mesh().its;
    TriangleSelector            sel(vol.mesh());
    for (int i = 0; i < int(its.indices.size()); ++i) {
        const stl_triangle_vertex_indices &face = its.indices[size_t(i)];
        const Vec3f c = (its.vertices[face[0]] + its.vertices[face[1]] + its.vertices[face[2]]) / 3.f;
        const int   state = c.x() < 10.f ? 5 : 6;
        sel.set_facet(i, EnforcerBlockerType(state));
    }
    REQUIRE(vol.mmu_segmentation_facets.set(sel));
}

void cleanup_bbs(std::vector<Preset *> &presets, PlateDataPtrs &plates)
{
    for (Preset *preset : presets)
        delete preset;
    for (PlateData *plate : plates)
        delete plate;
}

} // namespace

TEST_CASE("spectrum_paint_id_limit caps persistable IDs at persist cap", "[paint_mix]")
{
    CHECK(spectrum_paint_id_limit(4, 0, 0) == 4);
    CHECK(spectrum_paint_id_limit(4, 6, 0) == 6);
    CHECK(spectrum_paint_id_limit(4, 0, 8) == 8);
    CHECK(spectrum_paint_id_limit(4, 6, 8) == 8);
    CHECK(spectrum_paint_id_limit(16, 16, 16) == SPECTRUM_PAINT_ID_PERSIST_CAP);
    CHECK(SPECTRUM_PAINT_ID_PERSIST_CAP == 16);
    CHECK(spectrum_paint_id_limit(16, 16, 16) == 16);
}

TEST_CASE("encode/decode Mix 5-6 via FacetsAnnotation string", "[paint_mix]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}});
    const std::string encoded5 = vol->mmu_segmentation_facets.get_triangle_as_string(0);
    const std::string encoded6 = vol->mmu_segmentation_facets.get_triangle_as_string(1);
    REQUIRE_FALSE(encoded5.empty());
    REQUIRE_FALSE(encoded6.empty());
    CHECK(used_paint_state(*vol, 5));
    CHECK(used_paint_state(*vol, 6));

    Model        model2;
    ModelObject *obj2 = model2.add_object("roundtrip", "", make_cube(20., 20., 20.));
    obj2->add_instance();
    ModelVolume *vol2 = obj2->volumes.front();
    vol2->mmu_segmentation_facets.set_triangle_from_string(0, encoded5);
    vol2->mmu_segmentation_facets.set_triangle_from_string(1, encoded6);
    CHECK(vol2->mmu_segmentation_facets.get_triangle_as_string(0) == encoded5);
    CHECK(vol2->mmu_segmentation_facets.get_triangle_as_string(1) == encoded6);
    CHECK(used_paint_state(*vol2, 5));
    CHECK(used_paint_state(*vol2, 6));
}

TEST_CASE("hex state 15 and 16 round-trip; clamp keeps 16", "[paint_mix]")
{
    Model        model;
    ModelVolume *vol15 = add_painted_cube(model, {{0, 15}});
    const std::string encoded15 = vol15->mmu_segmentation_facets.get_triangle_as_string(0);
    REQUIRE_FALSE(encoded15.empty());
    CHECK(used_paint_state(*vol15, 15));

    Model        model_rt;
    ModelObject *obj_rt = model_rt.add_object("state15", "", make_cube(20., 20., 20.));
    obj_rt->add_instance();
    ModelVolume *vol_rt = obj_rt->volumes.front();
    vol_rt->mmu_segmentation_facets.set_triangle_from_string(0, encoded15);
    CHECK(vol_rt->mmu_segmentation_facets.get_triangle_as_string(0) == encoded15);
    CHECK(used_paint_state(*vol_rt, 15));

    Model        model16;
    ModelVolume *vol16 = add_painted_cube(model16, {{0, 16}});
    REQUIRE(used_paint_state(*vol16, 16));
    const std::string encoded16 = vol16->mmu_segmentation_facets.get_triangle_as_string(0);
    REQUIRE_FALSE(encoded16.empty());

    Model        model16_rt;
    ModelObject *obj16_rt = model16_rt.add_object("state16", "", make_cube(20., 20., 20.));
    obj16_rt->add_instance();
    ModelVolume *vol16_rt = obj16_rt->volumes.front();
    vol16_rt->mmu_segmentation_facets.set_triangle_from_string(0, encoded16);
    CHECK(used_paint_state(*vol16_rt, 16));
    CHECK(spectrum_paint_id_limit(16, 16, 16) == SPECTRUM_PAINT_ID_PERSIST_CAP);
    vol16_rt->update_extruder_count(16, 16, 16);
    REQUIRE(used_paint_state(*vol16_rt, 16));
    vol16->update_extruder_count(16, 16, 16);
    REQUIRE(used_paint_state(*vol16, 16));
}

TEST_CASE("deserialize max_ebt from source palette 8 keeps painted 5-8", "[paint_mix]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}, {2, 7}, {3, 8}});
    TriangleSelector keep(vol->mesh());
    keep.deserialize(vol->mmu_segmentation_facets.get_data(), true,
                     EnforcerBlockerType(spectrum_paint_id_limit(4, 0, 8)));
    // set() is false when the bitstream is unchanged — that is the keep path.
    vol->mmu_segmentation_facets.set(keep);
    CHECK(used_paint_state(*vol, 5));
    CHECK(used_paint_state(*vol, 6));
    CHECK(used_paint_state(*vol, 7));
    CHECK(used_paint_state(*vol, 8));

    // Old gizmo used palette size 4 as max_ebt — that is the PR #5 Cursor wipe.
    TriangleSelector wipe(vol->mesh());
    wipe.deserialize(vol->mmu_segmentation_facets.get_data(), true, EnforcerBlockerType(4));
    REQUIRE(vol->mmu_segmentation_facets.set(wipe));
    CHECK_FALSE(used_paint_state(*vol, 5));
    CHECK_FALSE(used_paint_state(*vol, 8));
}

TEST_CASE("shrink to 4 with source palette size 8 leaves painted 5-8", "[paint_mix]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}, {2, 7}, {3, 8}});
    REQUIRE(used_paint_state(*vol, 5));
    REQUIRE(used_paint_state(*vol, 8));

    vol->update_extruder_count(4, 0, 8);
    CHECK(used_paint_state(*vol, 5));
    CHECK(used_paint_state(*vol, 6));
    CHECK(used_paint_state(*vol, 7));
    CHECK(used_paint_state(*vol, 8));

    Model        model_delete;
    ModelVolume *vol_del = add_painted_cube(model_delete, {{0, 5}, {1, 6}, {2, 7}, {3, 8}});
    // Production passes the post-delete physical count. Mix/source IDs 5-8 must not compact.
    vol_del->update_extruder_count_when_delete_filament(3, 1, 1, 0, 8);
    CHECK(used_paint_state(*vol_del, 5));
    CHECK(used_paint_state(*vol_del, 6));
    CHECK(used_paint_state(*vol_del, 7));
    CHECK(used_paint_state(*vol_del, 8));
}

TEST_CASE("max_filament_id 6 leaves Mix 5-6 after update_extruder_count(4)", "[paint_mix]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}});
    vol->update_extruder_count(4, 6, 0);
    CHECK(used_paint_state(*vol, 5));
    CHECK(used_paint_state(*vol, 6));

    Model        model_delete;
    ModelVolume *vol_del = add_painted_cube(model_delete, {{0, 5}, {1, 6}});
    vol_del->update_extruder_count_when_delete_filament(3, 1, 1, 6, 0);
    CHECK(used_paint_state(*vol_del, 5));
    CHECK(used_paint_state(*vol_del, 6));
}

TEST_CASE("spectrum_volume_extruder_keep splits physical/mix from source-palette gap", "[paint_mix]")
{
    CHECK(spectrum_volume_extruder_keep(1, 4, 0));
    CHECK(spectrum_volume_extruder_keep(4, 4, 0));
    CHECK_FALSE(spectrum_volume_extruder_keep(5, 4, 0));
    CHECK(spectrum_volume_extruder_keep(5, 4, 6));
    CHECK(spectrum_volume_extruder_keep(6, 4, 6));
    CHECK_FALSE(spectrum_volume_extruder_keep(7, 4, 6));
}

TEST_CASE("Adopt leftover volume extruder 5 is not Mix 5; painted 5-8 remain", "[paint_mix]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}, {2, 7}, {3, 8}});
    vol->config.set("extruder", 5);
    REQUIRE(vol->extruder_id() == 5);

    vol->update_extruder_count(4, 0, 8);
    CHECK(vol->extruder_id() != 5);
    CHECK_FALSE(vol->config.has("extruder"));
    CHECK(used_paint_state(*vol, 5));
    CHECK(used_paint_state(*vol, 6));
    CHECK(used_paint_state(*vol, 7));
    CHECK(used_paint_state(*vol, 8));
}

TEST_CASE("mix-shaped volume extruder 5 is kept with max_filament_id 6", "[paint_mix]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}});
    vol->config.set("extruder", 5);
    vol->update_extruder_count(4, 6, 8);
    CHECK(vol->extruder_id() == 5);
    CHECK(vol->config.has("extruder"));
    CHECK(used_paint_state(*vol, 5));
    CHECK(used_paint_state(*vol, 6));
}

TEST_CASE("object-inherited extruder 5 is cleared after Adopt-shaped clamp", "[paint_mix]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}});
    vol->get_object()->config.set("extruder", 5);
    REQUIRE(vol->extruder_id() == 5);
    REQUIRE_FALSE(vol->config.has("extruder"));

    vol->update_extruder_count(4, 0, 8);
    CHECK(vol->extruder_id() != 5);
    CHECK_FALSE(vol->get_object()->config.has("extruder"));
    CHECK(used_paint_state(*vol, 5));
}

TEST_CASE("delete-filament production max keeps Mix 6 paint", "[paint_mix]")
{
    const std::string two_mixes = "1,2,1,1,1;1,3,1,1,1";
    const size_t      post_n    = 3;
    // Trap: post-delete physical into max_filament_id drops Mix 6 (PR #6 Cursor High).
    REQUIRE(MixedFilamentManager::max_filament_id(two_mixes, post_n) == 5);
    const size_t pre_mix_max = spectrum_delete_filament_mix_max(two_mixes, post_n);
    REQUIRE(pre_mix_max == 6);
    REQUIRE(spectrum_paint_id_limit(post_n, pre_mix_max, 0) == 6);
    REQUIRE(spectrum_delete_filament_mix_max("", post_n) == post_n + 1);

    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}});
    vol->config.set("extruder", 6);
    vol->update_extruder_count_when_delete_filament(post_n, 1, 1, pre_mix_max, 0);
    CHECK(used_paint_state(*vol, 5));
    CHECK(used_paint_state(*vol, 6));
    CHECK(vol->extruder_id() == 6);
}

TEST_CASE("Plater on_filaments_delete binds spectrum_delete_filament_mix_max", "[paint_mix]")
{
    // libslic3r_tests does not link GUI. Bind the production call site so a Plater-only
    // revert to max_filament_id(defs, post_n) fails this suite (spec DoD-2).
    const boost::filesystem::path here(__FILE__);
    const boost::filesystem::path plater = here.parent_path().parent_path().parent_path() /
                                           "src" / "slic3r" / "GUI" / "Plater.cpp";
    REQUIRE(boost::filesystem::exists(plater));
    std::ifstream in(plater.string());
    REQUIRE(in.good());
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const auto count_pos  = src.find("void Plater::on_filament_count_change");
    const auto delete_pos = src.find("void Plater::on_filaments_delete");
    REQUIRE(count_pos != std::string::npos);
    REQUIRE(delete_pos != std::string::npos);
    REQUIRE(delete_pos > count_pos);

    auto delete_end = src.find("\nvoid Plater::", delete_pos + 1);
    const auto next_vec = src.find("\nstd::vector<", delete_pos + 1);
    if (next_vec != std::string::npos && (delete_end == std::string::npos || next_vec < delete_end))
        delete_end = next_vec;
    REQUIRE(delete_end != std::string::npos);

    const std::string count_fn  = src.substr(count_pos, delete_pos - count_pos);
    const std::string delete_fn = src.substr(delete_pos, delete_end - delete_pos);
    CHECK(count_fn.find("spectrum_delete_filament_mix_max") == std::string::npos);
    CHECK(delete_fn.find("spectrum_delete_filament_mix_max") != std::string::npos);
    CHECK(delete_fn.find("max_filament_id(mixed_defs, num_filaments)") == std::string::npos);
}

TEST_CASE("disable middle of three enabled mixes after painting Mix 6 shifts IDs", "[paint_mix]")
{
    const std::string three_mixes = "1,2,1,1,1;1,3,1,1,1;2,3,1,1,1";
    MixedFilamentManager mgr;
    mgr.load_definitions(three_mixes);
    REQUIRE(mgr.enabled_count() == 3);
    REQUIRE(mgr.total_filaments(4) == 7);
    REQUIRE(mgr.resolve(6, 4, 0) == 1);
    REQUIRE(mgr.resolve(6, 4, 1) == 3);

    // Manager drops disabled rows. Disabling the middle mix (1+3) makes former Mix 7 become Mix 6.
    const std::string two_mixes = "1,2,1,1,1;2,3,1,1,1";
    MixedFilamentManager after;
    after.load_definitions(two_mixes);
    REQUIRE(after.enabled_count() == 2);
    REQUIRE(after.total_filaments(4) == 6);
    REQUIRE(after.resolve(6, 4, 0) == 2);
    REQUIRE(after.resolve(6, 4, 1) == 3);
    REQUIRE_FALSE(after.is_mixed(7, 4));

    CHECK(mixed_filament_painted_ids_would_shift(three_mixes, two_mixes, 4, {6}));
    CHECK_FALSE(mixed_filament_painted_ids_would_shift(three_mixes, two_mixes, 4, {5}));
}

TEST_CASE("3mf round-trip keeps painted mix IDs 5-6", "[paint_mix]")
{
    const std::string logs_dir = "C:/dev/Orca/conductor/0008-PaintVirtualMixIds/logs";
    boost::filesystem::create_directories(logs_dir);
    const std::string out_path = logs_dir + "/project_paint_mix.3mf";

    const std::string base_path =
        "C:/dev/Orca/conductor/0006-CmykMultiMixShowpiece/logs/project_showpiece.3mf";

    DynamicPrintConfig          config;
    ConfigSubstitutionContext   ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    Model                       model;
    PlateDataPtrs               plates;
    std::vector<Preset *>       project_presets;
    bool                        is_bbl  = false;
    bool                        is_orca = false;
    Semver                      file_version;

    if (boost::filesystem::exists(base_path)) {
        REQUIRE(load_bbs_3mf(base_path.c_str(), &config, &ctxt, &model, &plates, &project_presets,
                             &is_bbl, &is_orca, &file_version, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
        model.clear_objects();
    }

    ModelObject *obj = model.add_object("paint_mix_cube", "", make_cube(20., 20., 20.));
    obj->add_instance();
    // Centered 20 mm cube: lift Z so it sits on the bed, shift XY onto the 300x270 plate.
    obj->instances[0]->set_offset(Vec3d(100., 80., 0.));
    ModelVolume *vol = obj->volumes.front();
    paint_left_right_mixes(*vol);
    REQUIRE(used_paint_state(*vol, 5));
    REQUIRE(used_paint_state(*vol, 6));

    config.set_key_value("mixed_filament_definitions", new ConfigOptionString("1,2,1,1,1;1,3,1,1,1"));
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(false));
    config.set_key_value("mixed_filament_gradient_mode", new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    auto *colours = config.option<ConfigOptionStrings>("filament_colour", true);
    REQUIRE(colours != nullptr);
    if (colours->values.size() < 4)
        colours->values = {"#08ABFBFF", "#D93B90FF", "#F9ED3DFF", "#9199A4FF"};

    if (plates.empty()) {
        auto *plate = new PlateData();
        plate->plate_index = 0;
        plates.push_back(plate);
    }
    plates.front()->objects_and_instances.clear();
    plates.front()->objects_and_instances.emplace_back(0, 0);

    StoreParams store;
    store.path            = out_path.c_str();
    store.model           = &model;
    store.config          = &config;
    store.plate_data_list = plates;
    store.project_presets = project_presets;
    store.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(store));

    DynamicPrintConfig        config2;
    ConfigSubstitutionContext ctxt2{ForwardCompatibilitySubstitutionRule::Enable};
    Model                     model2;
    PlateDataPtrs             plates2;
    std::vector<Preset *>     presets2;
    bool                      is_bbl2  = false;
    bool                      is_orca2 = false;
    Semver                    file_version2;
    REQUIRE(load_bbs_3mf(out_path.c_str(), &config2, &ctxt2, &model2, &plates2, &presets2,
                         &is_bbl2, &is_orca2, &file_version2, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));

    REQUIRE_FALSE(model2.objects.empty());
    REQUIRE_FALSE(model2.objects.front()->volumes.empty());
    const ModelVolume *vol2 = model2.objects.front()->volumes.front();
    CHECK(used_paint_state(*vol2, 5));
    CHECK(used_paint_state(*vol2, 6));
    const auto *mixed = config2.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(mixed != nullptr);
    CHECK(mixed->value.find("1,2") != std::string::npos);
    CHECK(mixed->value.find("1,3") != std::string::npos);

    cleanup_bbs(project_presets, plates);
    cleanup_bbs(presets2, plates2);
}

TEST_CASE("3mf round-trip keeps painted state 16", "[paint_mix]")
{
    const boost::filesystem::path tmp_path =
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca_paint_state16-%%%%-%%%%.3mf");
    const std::string out_path = tmp_path.string();
    struct RemoveTemp {
        boost::filesystem::path path;
        ~RemoveTemp()
        {
            boost::system::error_code ec;
            boost::filesystem::remove(path, ec);
        }
    } guard{tmp_path};

    DynamicPrintConfig          config;
    Model                       model;
    PlateDataPtrs               plates;
    std::vector<Preset *>       project_presets;

    ModelObject *obj = model.add_object("paint_state16_cube", "", make_cube(20., 20., 20.));
    obj->add_instance();
    obj->instances[0]->set_offset(Vec3d(100., 80., 0.));
    ModelVolume *vol = obj->volumes.front();
    TriangleSelector sel(vol->mesh());
    sel.set_facet(0, EnforcerBlockerType(16));
    REQUIRE(vol->mmu_segmentation_facets.set(sel));
    REQUIRE(used_paint_state(*vol, 16));

    auto *colours = config.option<ConfigOptionStrings>("filament_colour", true);
    REQUIRE(colours != nullptr);
    colours->values = {"#08ABFBFF", "#D93B90FF", "#F9ED3DFF", "#9199A4FF"};
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));

    auto *plate = new PlateData();
    plate->plate_index = 0;
    plate->objects_and_instances.emplace_back(0, 0);
    plates.push_back(plate);

    StoreParams store;
    store.path            = out_path.c_str();
    store.model           = &model;
    store.config          = &config;
    store.plate_data_list = plates;
    store.project_presets = project_presets;
    store.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(store));

    DynamicPrintConfig        config2;
    ConfigSubstitutionContext ctxt2{ForwardCompatibilitySubstitutionRule::Enable};
    Model                     model2;
    PlateDataPtrs             plates2;
    std::vector<Preset *>     presets2;
    bool                      is_bbl2  = false;
    bool                      is_orca2 = false;
    Semver                    file_version2;
    REQUIRE(load_bbs_3mf(out_path.c_str(), &config2, &ctxt2, &model2, &plates2, &presets2,
                         &is_bbl2, &is_orca2, &file_version2, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));

    REQUIRE_FALSE(model2.objects.empty());
    REQUIRE_FALSE(model2.objects.front()->volumes.empty());
    const ModelVolume *vol2 = model2.objects.front()->volumes.front();
    REQUIRE(used_paint_state(*vol2, 16));

    cleanup_bbs(project_presets, plates);
    cleanup_bbs(presets2, plates2);
}
