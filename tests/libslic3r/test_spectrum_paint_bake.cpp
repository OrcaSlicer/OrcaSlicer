#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentMatch.hpp"
#include "libslic3r/MixedFilamentPaintBake.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
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
    ModelObject *obj = model.add_object("spectrum_paint_bake_cube", "", make_cube(20., 20., 20.));
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

ColorRGB decode_hex_or_fail(const std::string &hex)
{
    ColorRGB c;
    REQUIRE(decode_color(hex, c));
    return c;
}

std::vector<ColorRGB> panchroma_physicals()
{
    return {
        decode_hex_or_fail("#08ABFB"),
        decode_hex_or_fail("#D93B90"),
        decode_hex_or_fail("#F9ED3D"),
        decode_hex_or_fail("#9199A4"),
    };
}

const std::vector<std::string> k_mountain_hexes = {
    "#A47C6FFF", "#000000FF", "#99401BFF", "#99A29AFF",
    "#90817CFF", "#98614CFF", "#838E91FF", "#AE9C92FF",
};

int mix_period(const MixedFilament &mf)
{
    int p = mf.ratio_a + mf.ratio_b;
    if (mf.component_c != 0)
        p += mf.ratio_c;
    return p;
}

void cleanup_bbs(std::vector<Preset *> &presets, PlateDataPtrs &plates)
{
    for (Preset *preset : presets)
        delete preset;
    for (PlateData *plate : plates)
        delete plate;
}

std::vector<std::string> unique_mix_source_hexes(size_t n, const std::vector<ColorRGB> &phys)
{
    std::vector<std::string> out;
    std::set<std::string>    recipes;
    for (int r = 0; r < 256 && out.size() < n; r += 17) {
        for (int g = 0; g < 256 && out.size() < n; g += 19) {
            for (int b = 0; b < 256 && out.size() < n; b += 23) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
                ColorRGB c;
                if (!decode_color(buf, c))
                    continue;
                const MixMatchResult m = match_printable_mix(c, phys, nullptr, 4, 70);
                if (!m.valid || m.kind != MixMatchResult::Kind::Mix || m.recipe_row.empty())
                    continue;
                if (!recipes.insert(m.recipe_row).second)
                    continue;
                out.emplace_back(buf);
            }
        }
    }
    return out;
}

// 0010 pair-ratio lattice: 6 pairs × {1:1, 2:1, 1:2, 3:1, 1:3}. Used when the RGB
// stride helper cannot produce n unique Mix recipes (13-unique collapse fixture).
std::vector<std::string> unique_mix_hexes_from_pair_lattice(size_t n, const std::vector<ColorRGB> &phys)
{
    std::vector<std::string> out;
    std::set<std::string>    recipes;
    static const int k_pairs[][2]  = {{1, 2}, {1, 3}, {1, 4}, {2, 3}, {2, 4}, {3, 4}};
    static const int k_ratios[][2] = {
        {1, 1}, {2, 1}, {1, 2}, {3, 1}, {1, 3}, {4, 1}, {1, 4}, {3, 2}, {2, 3},
        {5, 1}, {1, 5}, {5, 2}, {2, 5}, {4, 3}, {3, 4},
    };
    auto try_row = [&](unsigned a, unsigned b, unsigned c, int ra, int rb, int rc) {
        if (out.size() >= n)
            return;
        MixedFilament mf;
        mf.component_a = a;
        mf.component_b = b;
        mf.component_c = c;
        mf.ratio_a     = ra;
        mf.ratio_b     = rb;
        mf.ratio_c     = rc;
        mf.enabled     = true;
        const ColorRGB       pred = predicted_swatch_for_mix(mf, phys);
        const MixMatchResult m    = match_printable_mix(pred, phys, nullptr, 4, 70);
        if (!m.valid || m.kind != MixMatchResult::Kind::Mix || m.recipe_row.empty())
            return;
        if (!recipes.insert(m.recipe_row).second)
            return;
        out.push_back(encode_color(pred));
    };
    for (const auto &pair : k_pairs) {
        for (const auto &ratio : k_ratios)
            try_row(unsigned(pair[0]), unsigned(pair[1]), 0, ratio[0], ratio[1], 0);
    }
    static const int k_triples[][3] = {{1, 2, 3}, {1, 2, 4}, {1, 3, 4}, {2, 3, 4}};
    static const int k_tri_ratios[][3] = {
        {1, 1, 1}, {2, 1, 1}, {1, 2, 1}, {1, 1, 2}, {3, 1, 1}, {1, 3, 1}, {1, 1, 3},
        {2, 2, 1}, {2, 1, 2}, {1, 2, 2}, {3, 2, 1}, {3, 1, 2}, {2, 3, 1},
    };
    for (const auto &trip : k_triples) {
        for (const auto &ratio : k_tri_ratios)
            try_row(unsigned(trip[0]), unsigned(trip[1]), unsigned(trip[2]),
                    ratio[0], ratio[1], ratio[2]);
    }
    return out;
}

void require_contiguous_mix_dests(const SpectrumPaintBakePlan &plan, size_t mix_base, size_t source_count)
{
    std::vector<unsigned> mix_dests;
    for (size_t src = 1; src <= source_count; ++src) {
        const unsigned dest = unsigned(plan.slot_map[src]);
        if (dest > unsigned(mix_base))
            mix_dests.push_back(dest);
    }
    std::sort(mix_dests.begin(), mix_dests.end());
    mix_dests.erase(std::unique(mix_dests.begin(), mix_dests.end()), mix_dests.end());
    REQUIRE(mix_dests.size() == plan.mix_count);
    for (size_t i = 0; i < mix_dests.size(); ++i)
        REQUIRE(mix_dests[i] == unsigned(mix_base + i + 1));
}

} // namespace

TEST_CASE("mountain 8 hexes bake to Grey physical 4 and several mixes", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(k_mountain_hexes, phys, 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.slot_map[0] == EnforcerBlockerType::NONE);
    REQUIRE(size_t(plan.slot_map[2]) == 4); // #000000FF → Grey physical 4
    REQUIRE(plan.slot_map[2] == EnforcerBlockerType::Extruder4);

    REQUIRE(size_t(plan.slot_map[1]) > 4); // earth #A47C6F is Mix not Grey-only
    REQUIRE(plan.slot_map[1] != EnforcerBlockerType::Extruder4);

    const MixMatchResult brown = match_printable_mix(decode_hex_or_fail("#99401B"), phys, nullptr);
    REQUIRE(brown.valid);
    REQUIRE(brown.kind == MixMatchResult::Kind::Mix);
    REQUIRE(mix_period(brown.mix) <= 4);
    REQUIRE(size_t(plan.slot_map[3]) > 4);

    REQUIRE(plan.mix_count >= 3);
    REQUIRE(4 + plan.mix_count <= SPECTRUM_PAINT_ID_PERSIST_CAP);
    REQUIRE_FALSE(plan.mixed_filament_definitions.empty());
    MixedFilamentManager mgr;
    mgr.load_definitions(plan.mixed_filament_definitions);
    REQUIRE(mgr.enabled_count() == plan.mix_count);
    REQUIRE(mgr.serialize_definitions() == plan.mixed_filament_definitions);
    require_contiguous_mix_dests(plan, 4, k_mountain_hexes.size());
}

TEST_CASE("duplicate source hexes share dest Mix ID", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const std::vector<std::string> source = {"#A47C6FFF", "#A47C6F", "#99401BFF"};
    const SpectrumPaintBakePlan    plan   = plan_spectrum_paint_bake(source, phys, 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.slot_map[1] == plan.slot_map[2]);
    REQUIRE(plan.slot_map[1] != plan.slot_map[3]);
}

TEST_CASE("identity NONE stays NONE", "[spectrum_paint_bake]")
{
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake({"#08ABFB"}, panchroma_physicals(), 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.slot_map[0] == EnforcerBlockerType::NONE);
}

TEST_CASE("empty source is invalid", "[spectrum_paint_bake]")
{
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake({}, panchroma_physicals(), 4);
    REQUIRE_FALSE(plan.valid);
    REQUIRE_FALSE(plan.error.empty());
}

TEST_CASE("source palette larger than persist cap is refused", "[spectrum_paint_bake]")
{
    std::vector<std::string> at_cap(SPECTRUM_PAINT_ID_PERSIST_CAP, "#08ABFB");
    const SpectrumPaintBakePlan plan_at_cap = plan_spectrum_paint_bake(at_cap, panchroma_physicals(), 4);
    REQUIRE(plan_at_cap.valid);
    REQUIRE(plan_at_cap.error.empty());

    std::vector<std::string> too_many(SPECTRUM_PAINT_ID_PERSIST_CAP + 1, "#08ABFB");
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(too_many, panchroma_physicals(), 4);
    REQUIRE_FALSE(plan.valid);
    REQUIRE_FALSE(plan.error.empty());
}

TEST_CASE("twelve unique Mix recipes plus four physicals bake within persist cap", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const std::vector<std::string> hexes = unique_mix_source_hexes(12, phys);
    REQUIRE(hexes.size() == 12);
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(hexes, phys, 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.mix_count <= 12);
    require_contiguous_mix_dests(plan, 4, hexes.size());
}

TEST_CASE("thirteen unique Mix recipes plus four physicals bake without collapse", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    std::vector<std::string> hexes = unique_mix_source_hexes(13, phys);
    const size_t helper_size = hexes.size();
    if (hexes.size() < 13)
        hexes = unique_mix_hexes_from_pair_lattice(13, phys);
    INFO("unique-mix 13 helper_size=" << helper_size
         << (helper_size < 13 ? " (pair-ratio lattice fallback)" : " (RGB stride helper)"));
    REQUIRE(hexes.size() == 13);
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(hexes, phys, 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.mix_count == 13);
    REQUIRE(4 + plan.mix_count <= SPECTRUM_PAINT_ID_PERSIST_CAP);
    require_contiguous_mix_dests(plan, 4, hexes.size());
}

TEST_CASE("twenty-nine unique Mix recipes plus four physicals collapse within persist cap", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    std::vector<std::string> hexes = unique_mix_source_hexes(29, phys);
    const size_t helper_size = hexes.size();
    if (hexes.size() < 29)
        hexes = unique_mix_hexes_from_pair_lattice(29, phys);
    INFO("unique-mix 29 helper_size=" << helper_size
         << (helper_size < 29 ? " (pair-ratio lattice fallback)" : " (RGB stride helper)"));
    REQUIRE(hexes.size() == 29);
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(hexes, phys, 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.mix_count <= 28);
    REQUIRE(4 + plan.mix_count <= SPECTRUM_PAINT_ID_PERSIST_CAP);
    require_contiguous_mix_dests(plan, 4, hexes.size());
}

TEST_CASE("apply remaps painted 1-8 including Grey physical 4", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(k_mountain_hexes, phys, 4);
    REQUIRE(plan.valid);

    Model model;
    std::vector<std::pair<int, int>> facets;
    for (int i = 0; i < 8; ++i)
        facets.emplace_back(i, i + 1);
    ModelVolume *vol = add_painted_cube(model, facets);
    REQUIRE(apply_spectrum_paint_bake(*vol, plan.slot_map));

    CHECK(used_paint_state(*vol, 4));
    for (size_t src = 1; src <= 8; ++src) {
        const size_t dest = size_t(plan.slot_map[src]);
        CHECK(used_paint_state(*vol, dest));
    }
    for (size_t mix_id = 5; mix_id <= 8; ++mix_id) {
        bool expected = false;
        for (size_t src = 1; src <= 8; ++src) {
            if (size_t(plan.slot_map[src]) == mix_id)
                expected = true;
        }
        CHECK(used_paint_state(*vol, mix_id) == expected);
    }
}

TEST_CASE("out-of-range paint state maps to physical 1", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    std::vector<std::string>    six  = k_mountain_hexes;
    six.resize(6);
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(six, phys, 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.slot_map[8] == EnforcerBlockerType::Extruder1);

    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 8}});
    REQUIRE(used_paint_state(*vol, 8));
    REQUIRE(apply_spectrum_paint_bake(*vol, plan.slot_map));
    CHECK(used_paint_state(*vol, 1));
    CHECK_FALSE(used_paint_state(*vol, 8));
}

TEST_CASE("3mf round-trip of baked paint and mix defs", "[spectrum_paint_bake]")
{
    const std::string logs_dir = "C:/dev/Orca/conductor/0011-SpectrumFromPaint/logs";
    boost::filesystem::create_directories(logs_dir);
    const std::string out_path = logs_dir + "/project_spectrum_paint_bake.3mf";

    const std::vector<ColorRGB> phys = panchroma_physicals();
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(k_mountain_hexes, phys, 4);
    REQUIRE(plan.valid);

    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    Model                     model;
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;

    ModelObject *obj = model.add_object("bake_cube", "", make_cube(20., 20., 20.));
    obj->add_instance();
    obj->instances[0]->set_offset(Vec3d(100., 80., 0.));
    ModelVolume *vol = obj->volumes.front();
    std::vector<std::pair<int, int>> facets;
    for (int i = 0; i < 8; ++i)
        facets.emplace_back(i, i + 1);
    TriangleSelector sel(vol->mesh());
    for (const auto &facet_state : facets)
        sel.set_facet(facet_state.first, EnforcerBlockerType(facet_state.second));
    REQUIRE(vol->mmu_segmentation_facets.set(sel));
    REQUIRE(apply_spectrum_paint_bake(*vol, plan.slot_map));

    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(plan.mixed_filament_definitions));
    config.set_key_value("spectrum_paint_mapped", new ConfigOptionBool(true));
    config.set_key_value("spectrum_source_filament_colour", new ConfigOptionStrings(k_mountain_hexes));
    auto *colours = config.option<ConfigOptionStrings>("filament_colour", true);
    REQUIRE(colours != nullptr);
    colours->values = {"#08ABFBFF", "#D93B90FF", "#F9ED3DFF", "#9199A4FF"};

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
    CHECK(used_paint_state(*vol2, 4));
    const auto *mixed = config2.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(mixed != nullptr);
    CHECK(mixed->value == plan.mixed_filament_definitions);
    const auto *mapped = config2.option<ConfigOptionBool>("spectrum_paint_mapped");
    REQUIRE(mapped != nullptr);
    CHECK(mapped->value);
    const auto *source = config2.option<ConfigOptionStrings>("spectrum_source_filament_colour");
    REQUIRE(source != nullptr);
    CHECK(source->values == k_mountain_hexes);

    cleanup_bbs(project_presets, plates);
    cleanup_bbs(presets2, plates2);
}

TEST_CASE("mountain fixture bakes to slice-ready 3mf", "[spectrum_paint_bake]")
{
    const std::string logs_dir = "C:/dev/Orca/conductor/0011-SpectrumFromPaint/logs";
    boost::filesystem::create_directories(logs_dir);
    const std::string mountain_path = logs_dir + "/8color-mountain.3mf";
    const std::string pre_bake_path = logs_dir + "/mountain_pre_bake.3mf";
    const std::string baked_path    = logs_dir + "/mountain_baked.3mf";
    if (!boost::filesystem::exists(mountain_path))
        SKIP("mountain 3mf not present on this machine");

    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    Model                     model;
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl  = false;
    bool                      is_orca = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(mountain_path.c_str(), &config, &ctxt, &model, &plates, &project_presets,
                         &is_bbl, &is_orca, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE_FALSE(model.objects.empty());

    snapshot_spectrum_source_palette_if_empty(config);
    auto *colours = config.option<ConfigOptionStrings>("filament_colour", true);
    REQUIRE(colours != nullptr);
    colours->values = {"#08ABFBFF", "#D93B90FF", "#F9ED3DFF", "#9199A4FF"};
    config.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    config.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(false));
    config.set_key_value("mixed_filament_gradient_mode", new ConfigOptionBool(false));

    if (plates.empty()) {
        auto *plate = new PlateData();
        plate->plate_index = 0;
        plates.push_back(plate);
    }
    plates.front()->objects_and_instances.clear();
    plates.front()->objects_and_instances.emplace_back(0, 0);

    StoreParams pre;
    pre.path            = pre_bake_path.c_str();
    pre.model           = &model;
    pre.config          = &config;
    pre.plate_data_list = plates;
    pre.project_presets = project_presets;
    pre.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(pre));

    const ConfigOptionStrings *source = config.option<ConfigOptionStrings>("spectrum_source_filament_colour");
    REQUIRE(source != nullptr);
    REQUIRE(source->values.size() == 8);

    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(source->values, panchroma_physicals(), 4);
    REQUIRE(plan.valid);
    REQUIRE(size_t(plan.slot_map[2]) == 4);
    REQUIRE(plan.mix_count >= 3);

    for (ModelObject *obj : model.objects) {
        for (ModelVolume *vol : obj->volumes)
            REQUIRE(apply_spectrum_paint_bake(*vol, plan.slot_map));
        if (obj->instances.empty())
            obj->add_instance();
        obj->ensure_on_bed();
        const BoundingBoxf3 bb = obj->instance_bounding_box(0);
        const Vec3d         c  = bb.center();
        obj->translate_instance(0, Vec3d(150. - c.x(), 135. - c.y(), 0.));
        obj->ensure_on_bed();
        const BoundingBoxf3 placed = obj->instance_bounding_box(0);
        UNSCOPED_INFO("mountain bb min=(" << placed.min.x() << "," << placed.min.y() << "," << placed.min.z()
                                        << ") max=(" << placed.max.x() << "," << placed.max.y() << "," << placed.max.z() << ")");
        REQUIRE(placed.size().x() < 300.);
        REQUIRE(placed.size().y() < 270.);
    }

    const std::vector<std::string> source_hexes = source->values;
    const std::string donor8 =
        "C:/dev/Orca/conductor/0008-PaintVirtualMixIds/logs/project_paint_mix.3mf";
    const std::string donor6 =
        "C:/dev/Orca/conductor/0006-CmykMultiMixShowpiece/logs/project_showpiece.3mf";
    const std::string donor_path = boost::filesystem::exists(donor8) ? donor8 : donor6;
    REQUIRE(boost::filesystem::exists(donor_path));

    DynamicPrintConfig        donor;
    ConfigSubstitutionContext donor_ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    Model                     donor_model;
    PlateDataPtrs             donor_plates;
    std::vector<Preset *>     donor_presets;
    bool                      donor_bbl = false;
    bool                      donor_orca = false;
    Semver                    donor_ver;
    REQUIRE(load_bbs_3mf(donor_path.c_str(), &donor, &donor_ctxt, &donor_model, &donor_plates,
                         &donor_presets, &donor_bbl, &donor_orca, &donor_ver, nullptr,
                         LoadStrategy::LoadConfig));
    donor.set_key_value("mixed_filament_definitions", new ConfigOptionString(plan.mixed_filament_definitions));
    donor.set_key_value("spectrum_paint_mapped", new ConfigOptionBool(true));
    donor.set_key_value("spectrum_source_filament_colour", new ConfigOptionStrings(source_hexes));
    donor.set_key_value("dithering_local_z_mode", new ConfigOptionBool(false));
    donor.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(false));
    donor.set_key_value("mixed_filament_gradient_mode", new ConfigOptionBool(false));
    donor.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    auto *donor_colours = donor.option<ConfigOptionStrings>("filament_colour", true);
    REQUIRE(donor_colours != nullptr);
    donor_colours->values = {"#08ABFBFF", "#D93B90FF", "#F9ED3DFF", "#9199A4FF"};

    PlateDataPtrs baked_plates;
    if (!donor_plates.empty()) {
        baked_plates = donor_plates;
        donor_plates.clear();
    } else if (!plates.empty()) {
        baked_plates = plates;
        plates.clear();
    } else {
        auto *plate = new PlateData();
        plate->plate_index = 0;
        baked_plates.push_back(plate);
    }
    baked_plates.front()->objects_and_instances.clear();
    baked_plates.front()->objects_and_instances.emplace_back(0, 0);

    StoreParams baked;
    baked.path            = baked_path.c_str();
    baked.model           = &model;
    baked.config          = &donor;
    baked.plate_data_list = baked_plates;
    baked.project_presets = donor_presets.empty() ? project_presets : donor_presets;
    baked.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(baked));

    DynamicPrintConfig        config2;
    ConfigSubstitutionContext ctxt2{ForwardCompatibilitySubstitutionRule::Enable};
    Model                     model2;
    PlateDataPtrs             plates2;
    std::vector<Preset *>     presets2;
    bool                      is_bbl2  = false;
    bool                      is_orca2 = false;
    Semver                    file_version2;
    REQUIRE(load_bbs_3mf(baked_path.c_str(), &config2, &ctxt2, &model2, &plates2, &presets2,
                         &is_bbl2, &is_orca2, &file_version2, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    const auto *mixed = config2.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(mixed != nullptr);
    REQUIRE_FALSE(mixed->value.empty());
    const auto *mapped = config2.option<ConfigOptionBool>("spectrum_paint_mapped");
    REQUIRE(mapped != nullptr);
    CHECK(mapped->value);
    const auto *source2 = config2.option<ConfigOptionStrings>("spectrum_source_filament_colour");
    REQUIRE(source2 != nullptr);
    CHECK(source2->values.size() == 8);
    const auto *semm = config2.option<ConfigOptionBool>("single_extruder_multi_material");
    REQUIRE(semm != nullptr);
    CHECK_FALSE(semm->value);
    CHECK(used_paint_state(*model2.objects.front()->volumes.front(), 4));

    cleanup_bbs(project_presets, plates);
    cleanup_bbs(donor_presets, baked_plates);
    cleanup_bbs(presets2, plates2);
}

TEST_CASE("mix_base 4 with 4 Panchroma first Mix dest is 5", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(k_mountain_hexes, phys, 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.mix_count >= 1);

    unsigned first_mix = 0;
    for (size_t src = 1; src <= k_mountain_hexes.size(); ++src) {
        const unsigned dest = unsigned(plan.slot_map[src]);
        if (dest > 4 && (first_mix == 0 || dest < first_mix))
            first_mix = dest;
    }
    REQUIRE(first_mix == 5);
}

TEST_CASE("mix_base 4 with only 3 decoded physicals never emits Mix dest 4", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = {
        decode_hex_or_fail("#08ABFB"),
        decode_hex_or_fail("#D93B90"),
        decode_hex_or_fail("#F9ED3D"),
    };
    REQUIRE(phys.size() == 3);
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(k_mountain_hexes, phys, 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.mix_count >= 1);

    bool saw_mix = false;
    for (size_t src = 1; src <= k_mountain_hexes.size(); ++src) {
        const unsigned dest = unsigned(plan.slot_map[src]);
        if (dest > 4) {
            saw_mix = true;
            REQUIRE(dest >= 5);
            REQUIRE(dest != 4);
        }
    }
    REQUIRE(saw_mix);
}

TEST_CASE("mix_base 6 with 4 Panchroma first Mix dest is 7", "[spectrum_paint_bake]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const SpectrumPaintBakePlan plan = plan_spectrum_paint_bake(k_mountain_hexes, phys, 6);
    REQUIRE(plan.valid);
    REQUIRE(plan.mix_count >= 1);

    unsigned first_mix = 0;
    for (size_t src = 1; src <= k_mountain_hexes.size(); ++src) {
        const unsigned dest = unsigned(plan.slot_map[src]);
        if (dest > 6 && (first_mix == 0 || dest < first_mix))
            first_mix = dest;
    }
    REQUIRE(first_mix == 7);
    REQUIRE(first_mix != 5);
}

TEST_CASE("spectrum map undo record pick and apply_keys round-trip", "[spectrum_paint_bake]")
{
    // Timestamp 100 is the named Map snapshot; unnamed topmost after Map is 101.
    SpectrumMapUndoRecord rec;
    rec.active                 = true;
    rec.map_snapshot_timestamp = 100;
    rec.pre.mapped             = false;
    rec.pre.mixed_filament_definitions.clear();
    rec.post.mapped            = true;
    rec.post.mixed_filament_definitions = "1:2:1:0";

    REQUIRE_FALSE(spectrum_map_undo_is_post(rec, 100));
    REQUIRE_FALSE(spectrum_map_undo_is_post(rec, 50));
    // Named Map 100 + unnamed topmost 101: post keys apply after the named Map time.
    REQUIRE(spectrum_map_undo_is_post(rec, 101));

    const SpectrumMapUndoKeys &at_map = spectrum_map_undo_pick(rec, 100);
    REQUIRE_FALSE(at_map.mapped);
    REQUIRE(at_map.mixed_filament_definitions.empty());

    const SpectrumMapUndoKeys &after = spectrum_map_undo_pick(rec, 101);
    REQUIRE(after.mapped);
    REQUIRE(after.mixed_filament_definitions == "1:2:1:0");

    DynamicPrintConfig project;
    DynamicPrintConfig print_cfg;
    REQUIRE(apply_spectrum_map_keys(project, rec.post, &print_cfg));
    const auto *mapped = project.option<ConfigOptionBool>("spectrum_paint_mapped");
    REQUIRE(mapped != nullptr);
    CHECK(mapped->value);
    const auto *proj_mix = project.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(proj_mix != nullptr);
    CHECK(proj_mix->value == "1:2:1:0");
    const auto *print_mix = print_cfg.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(print_mix != nullptr);
    CHECK(print_mix->value == "1:2:1:0");

    REQUIRE(apply_spectrum_map_keys(project, rec.pre, &print_cfg));
    const auto *mapped2 = project.option<ConfigOptionBool>("spectrum_paint_mapped");
    REQUIRE(mapped2 != nullptr);
    CHECK_FALSE(mapped2->value);
    const auto *proj_mix2 = project.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(proj_mix2 != nullptr);
    CHECK(proj_mix2->value.empty());
    const auto *print_mix2 = print_cfg.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(print_mix2 != nullptr);
    CHECK(print_mix2->value.empty());

    REQUIRE_FALSE(apply_spectrum_map_keys(project, rec.pre, &print_cfg));
}

TEST_CASE("spectrum map undo drop_if_rewritten", "[spectrum_paint_bake]")
{
    SpectrumMapUndoRecord rec;
    rec.active                 = true;
    rec.map_snapshot_timestamp = 100;

    spectrum_map_undo_drop_if_rewritten(rec, 100);
    REQUIRE_FALSE(rec.active);

    rec.active = true;
    spectrum_map_undo_drop_if_rewritten(rec, 50);
    REQUIRE_FALSE(rec.active);

    rec.active = true;
    spectrum_map_undo_drop_if_rewritten(rec, 101);
    REQUIRE(rec.active);
}

TEST_CASE("spectrum map undo named_time after history lag", "[spectrum_paint_bake]")
{
    REQUIRE(spectrum_map_undo_named_time(100, 100) == 100);
    REQUIRE(spectrum_map_undo_named_time(100, 101) == 100);
    REQUIRE(spectrum_map_undo_named_time(2, 4) == 3);
    REQUIRE(spectrum_map_undo_named_time(2, 5) == 4);
    REQUIRE(spectrum_map_undo_named_time(10, 9) == 10);

    SpectrumMapUndoRecord rec;
    rec.active                 = true;
    rec.map_snapshot_timestamp = spectrum_map_undo_named_time(2, 4);
    REQUIRE(rec.map_snapshot_timestamp == 3);
    REQUIRE_FALSE(spectrum_map_undo_is_post(rec, 3));
    REQUIRE(spectrum_map_undo_is_post(rec, 4));
    REQUIRE_FALSE(spectrum_map_undo_is_post(rec, 2));

    spectrum_map_undo_drop_if_rewritten(rec, 3);
    REQUIRE_FALSE(rec.active);

    rec.active = true;
    spectrum_map_undo_drop_if_rewritten(rec, 4);
    REQUIRE(rec.active);
}

TEST_CASE("spectrum mix dialog undo pick and apply_keys round-trip", "[spectrum_mix_dialog_undo]")
{
    // Named dialog snapshot 100; unnamed topmost after dialog is 101.
    SpectrumMixDialogUndoRecord rec;
    rec.active             = true;
    rec.snapshot_timestamp = 100;
    rec.pre.mixed_filament_definitions.clear();
    rec.pre.enable_prime_tower              = false;
    rec.pre.dithering_local_z_mode          = false;
    rec.pre.dithering_local_z_whole_objects = false;
    rec.pre.mixed_filament_gradient_mode    = false;
    rec.post.mixed_filament_definitions     = "1,2,1,1,1";
    rec.post.enable_prime_tower             = true;
    rec.post.dithering_local_z_mode         = true;
    rec.post.dithering_local_z_whole_objects = true;
    rec.post.mixed_filament_gradient_mode   = true;

    REQUIRE_FALSE(spectrum_mix_dialog_undo_is_post(rec, 100));
    REQUIRE_FALSE(spectrum_mix_dialog_undo_is_post(rec, 50));
    REQUIRE(spectrum_mix_dialog_undo_is_post(rec, 101));

    const SpectrumMixDialogUndoKeys &at_named = spectrum_mix_dialog_undo_pick(rec, 100);
    REQUIRE(at_named.mixed_filament_definitions.empty());
    REQUIRE_FALSE(at_named.enable_prime_tower);

    const SpectrumMixDialogUndoKeys &after = spectrum_mix_dialog_undo_pick(rec, 101);
    REQUIRE(after.mixed_filament_definitions == "1,2,1,1,1");
    REQUIRE(after.enable_prime_tower);
    REQUIRE(after.dithering_local_z_mode);
    REQUIRE(after.dithering_local_z_whole_objects);
    REQUIRE(after.mixed_filament_gradient_mode);

    DynamicPrintConfig project;
    DynamicPrintConfig print_cfg;
    REQUIRE(apply_spectrum_mix_dialog_keys(project, print_cfg, rec.post));
    const auto *proj_mix = project.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(proj_mix != nullptr);
    CHECK(proj_mix->value == "1,2,1,1,1");
    const auto *print_mix = print_cfg.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(print_mix != nullptr);
    CHECK(print_mix->value == "1,2,1,1,1");
    const auto *tower = print_cfg.option<ConfigOptionBool>("enable_prime_tower");
    REQUIRE(tower != nullptr);
    CHECK(tower->value);
    const auto *lz = print_cfg.option<ConfigOptionBool>("dithering_local_z_mode");
    REQUIRE(lz != nullptr);
    CHECK(lz->value);
    const auto *fd = print_cfg.option<ConfigOptionBool>("dithering_local_z_whole_objects");
    REQUIRE(fd != nullptr);
    CHECK(fd->value);
    const auto *gr = print_cfg.option<ConfigOptionBool>("mixed_filament_gradient_mode");
    REQUIRE(gr != nullptr);
    CHECK(gr->value);
    REQUIRE(project.option<ConfigOptionBool>("spectrum_paint_mapped") == nullptr);

    // Applying pre clears a non-empty mix-def string.
    REQUIRE(apply_spectrum_mix_dialog_keys(project, print_cfg, rec.pre));
    const auto *proj_mix2 = project.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(proj_mix2 != nullptr);
    CHECK(proj_mix2->value.empty());
    const auto *print_mix2 = print_cfg.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(print_mix2 != nullptr);
    CHECK(print_mix2->value.empty());
    REQUIRE_FALSE(print_cfg.option<ConfigOptionBool>("enable_prime_tower")->value);
    REQUIRE_FALSE(apply_spectrum_mix_dialog_keys(project, print_cfg, rec.pre));
}

TEST_CASE("spectrum mix defs compose Map-then-Dialog", "[spectrum_mix_dialog_undo]")
{
    SpectrumMapUndoRecord map;
    map.active                 = true;
    map.map_snapshot_timestamp = 100;
    map.pre.mapped             = false;
    map.pre.mixed_filament_definitions  = "map-pre";
    map.post.mapped            = true;
    map.post.mixed_filament_definitions = "map-post";

    SpectrumMixDialogUndoRecord dialog;
    dialog.active             = true;
    dialog.snapshot_timestamp = 110;
    dialog.pre.mixed_filament_definitions  = "dialog-pre";
    dialog.post.mixed_filament_definitions = "dialog-post";

    // T 111 / 110 / 100 — pin named vs named+1.
    REQUIRE(spectrum_mix_defs_at(map, dialog, 111) == "dialog-post");
    REQUIRE(spectrum_mix_defs_at(map, dialog, 110) == "map-post");
    REQUIRE(spectrum_mix_defs_at(map, dialog, 100) == "map-pre");
    REQUIRE(spectrum_mix_defs_at(map, dialog, 50) == "map-pre");
}

TEST_CASE("spectrum mix defs compose Dialog-then-Map", "[spectrum_mix_dialog_undo]")
{
    SpectrumMixDialogUndoRecord dialog;
    dialog.active             = true;
    dialog.snapshot_timestamp = 100;
    dialog.pre.mixed_filament_definitions  = "dialog-pre";
    dialog.post.mixed_filament_definitions = "dialog-post";

    SpectrumMapUndoRecord map;
    map.active                 = true;
    map.map_snapshot_timestamp = 110;
    map.pre.mapped             = false;
    map.pre.mixed_filament_definitions  = "map-pre";
    map.post.mapped            = true;
    map.post.mixed_filament_definitions = "map-post";

    REQUIRE(spectrum_mix_defs_at(map, dialog, 111) == "map-post");
    REQUIRE(spectrum_mix_defs_at(map, dialog, 110) == "dialog-post");
    REQUIRE(spectrum_mix_defs_at(map, dialog, 100) == "dialog-pre");
    REQUIRE(spectrum_mix_defs_at(map, dialog, 50) == "dialog-pre");
}

TEST_CASE("spectrum mix defs both inactive returns empty", "[spectrum_mix_dialog_undo]")
{
    SpectrumMapUndoRecord       map;
    SpectrumMixDialogUndoRecord dialog;
    REQUIRE(spectrum_mix_defs_at(map, dialog, 100).empty());
}

TEST_CASE("spectrum mix dialog undo drop_if_rewritten", "[spectrum_mix_dialog_undo]")
{
    SpectrumMixDialogUndoRecord rec;
    rec.active             = true;
    rec.snapshot_timestamp = 100;

    spectrum_mix_dialog_undo_drop_if_rewritten(rec, 100);
    REQUIRE_FALSE(rec.active);

    rec.active = true;
    spectrum_mix_dialog_undo_drop_if_rewritten(rec, 50);
    REQUIRE_FALSE(rec.active);

    rec.active = true;
    spectrum_mix_dialog_undo_drop_if_rewritten(rec, 101);
    REQUIRE(rec.active);
}

TEST_CASE("spectrum mix dialog two OKs compose mix and process", "[spectrum_mix_dialog_undo]")
{
    SpectrumMapUndoRecord map; // inactive

    SpectrumMixDialogUndoRecord d1;
    d1.active             = true;
    d1.snapshot_timestamp = 100;
    d1.pre.mixed_filament_definitions.clear();
    d1.pre.enable_prime_tower              = false;
    d1.pre.dithering_local_z_mode          = false;
    d1.pre.dithering_local_z_whole_objects = false;
    d1.pre.mixed_filament_gradient_mode    = false;
    d1.post.mixed_filament_definitions     = "1,2,1,1,1";
    d1.post.enable_prime_tower             = true;
    d1.post.dithering_local_z_mode         = false;
    d1.post.dithering_local_z_whole_objects = false;
    d1.post.mixed_filament_gradient_mode   = false;

    SpectrumMixDialogUndoRecord d2;
    d2.active             = true;
    d2.snapshot_timestamp = 110;
    d2.pre                = d1.post;
    d2.post.mixed_filament_definitions     = "1,2,1,1,1;1,3,1,1,1";
    d2.post.enable_prime_tower             = true;
    d2.post.dithering_local_z_mode         = true;
    d2.post.dithering_local_z_whole_objects = false;
    d2.post.mixed_filament_gradient_mode   = false;

    const std::vector<SpectrumMixDialogUndoRecord> dialogs{d1, d2};

    auto check_at = [&](size_t T, const std::string &mix, bool tower, bool local_z) {
        REQUIRE(spectrum_mix_defs_at(map, dialogs, T) == mix);
        SpectrumMixDialogUndoKeys keys;
        REQUIRE(spectrum_mix_dialog_keys_at(dialogs, T, keys));
        REQUIRE(keys.mixed_filament_definitions == mix);
        REQUIRE(keys.enable_prime_tower == tower);
        REQUIRE(keys.dithering_local_z_mode == local_z);
        REQUIRE_FALSE(keys.dithering_local_z_whole_objects);
        REQUIRE_FALSE(keys.mixed_filament_gradient_mode);
    };

    check_at(111, "1,2,1,1,1;1,3,1,1,1", true, true);
    check_at(110, "1,2,1,1,1", true, false);
    // Literal empty — must not equal D2 pre ("1,2,1,1,1").
    check_at(100, "", false, false);
    check_at(50, "", false, false);
}

TEST_CASE("spectrum mix defs compose Map plus two dialogs", "[spectrum_mix_dialog_undo]")
{
    SpectrumMapUndoRecord map;
    map.active                 = true;
    map.map_snapshot_timestamp = 90;
    map.pre.mapped             = false;
    map.pre.mixed_filament_definitions  = "map-pre";
    map.post.mapped            = true;
    map.post.mixed_filament_definitions = "map-post";

    SpectrumMixDialogUndoRecord d1;
    d1.active             = true;
    d1.snapshot_timestamp = 100;
    d1.pre.mixed_filament_definitions  = "d1-pre";
    d1.pre.enable_prime_tower          = false;
    d1.pre.dithering_local_z_mode      = false;
    d1.pre.dithering_local_z_whole_objects = false;
    d1.pre.mixed_filament_gradient_mode    = false;
    d1.post.mixed_filament_definitions = "d1-post";
    d1.post.enable_prime_tower         = true;
    d1.post.dithering_local_z_mode     = false;
    d1.post.dithering_local_z_whole_objects = false;
    d1.post.mixed_filament_gradient_mode    = false;

    SpectrumMixDialogUndoRecord d2;
    d2.active             = true;
    d2.snapshot_timestamp = 110;
    d2.pre.mixed_filament_definitions  = "d2-pre";
    d2.pre.enable_prime_tower          = true;
    d2.pre.dithering_local_z_mode      = false;
    d2.post.mixed_filament_definitions = "d2-post";
    d2.post.enable_prime_tower         = true;
    d2.post.dithering_local_z_mode     = true;
    d2.post.dithering_local_z_whole_objects = false;
    d2.post.mixed_filament_gradient_mode    = false;

    const std::vector<SpectrumMixDialogUndoRecord> dialogs{d1, d2};

    auto check_at = [&](size_t T, const std::string &mix, bool mapped, bool tower, bool local_z) {
        REQUIRE(spectrum_mix_defs_at(map, dialogs, T) == mix);
        REQUIRE(spectrum_map_undo_pick(map, T).mapped == mapped);
        SpectrumMixDialogUndoKeys keys;
        REQUIRE(spectrum_mix_dialog_keys_at(dialogs, T, keys));
        REQUIRE(keys.enable_prime_tower == tower);
        REQUIRE(keys.dithering_local_z_mode == local_z);
    };

    check_at(111, "d2-post", true, true, true);
    check_at(110, "d1-post", true, true, false);
    check_at(100, "map-post", true, false, false);
    check_at(90, "map-pre", false, false, false);
}

TEST_CASE("spectrum mix dialog vector drop_if_rewritten", "[spectrum_mix_dialog_undo]")
{
    SpectrumMixDialogUndoRecord d1;
    d1.active             = true;
    d1.snapshot_timestamp = 100;
    SpectrumMixDialogUndoRecord d2;
    d2.active             = true;
    d2.snapshot_timestamp = 110;

    {
        std::vector<SpectrumMixDialogUndoRecord> records{d1, d2};
        spectrum_mix_dialog_undo_drop_if_rewritten(records, 110);
        REQUIRE(records.size() == 1);
        REQUIRE(records.front().snapshot_timestamp == 100);
        REQUIRE(records.front().active);
    }
    {
        std::vector<SpectrumMixDialogUndoRecord> records{d1, d2};
        spectrum_mix_dialog_undo_drop_if_rewritten(records, 100);
        REQUIRE(records.empty());
    }
    {
        std::vector<SpectrumMixDialogUndoRecord> records{d1, d2};
        spectrum_mix_dialog_undo_drop_if_rewritten(records, 111);
        REQUIRE(records.size() == 2);
        REQUIRE(records[0].active);
        REQUIRE(records[1].active);
    }
}

TEST_CASE("spectrum mix dialog keys_at empty vector returns false", "[spectrum_mix_dialog_undo]")
{
    const std::vector<SpectrumMixDialogUndoRecord> empty;
    SpectrumMixDialogUndoKeys                      out;
    out.mixed_filament_definitions = "sentinel";
    out.enable_prime_tower         = true;
    REQUIRE_FALSE(spectrum_mix_dialog_keys_at(empty, 100, out));

    SpectrumMapUndoRecord map;
    SpectrumMixDialogUndoRecord inactive;
    inactive.active = false;
    REQUIRE(spectrum_mix_defs_at(map, std::vector<SpectrumMixDialogUndoRecord>{inactive}, 100).empty());
    REQUIRE(spectrum_mix_defs_at(map, empty, 100).empty());
}
