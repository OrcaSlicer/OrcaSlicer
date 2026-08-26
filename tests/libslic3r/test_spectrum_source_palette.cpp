#include <catch2/catch_all.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"

using namespace Slic3r;

namespace {

const std::vector<std::string> k_mountain_hexes = {
    "#A47C6FFF",
    "#000000FF",
    "#99401BFF",
    "#99A29AFF",
    "#90817CFF",
    "#98614CFF",
    "#838E91FF",
    "#AE9C92FF",
};

void seed_eight_colours(PresetBundle &bundle, const std::vector<std::string> &hexes)
{
    auto *colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour", true);
    REQUIRE(colours != nullptr);
    colours->values = hexes;

    // Deliberately different so a snapshot of filament_multi_colour would fail equality.
    auto *multi = bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour", true);
    REQUIRE(multi != nullptr);
    multi->values.assign(hexes.size(), "#DEADBEFF");

    bundle.filament_presets.resize(hexes.size(), "dummy-filament");
}

const ConfigOptionStrings *source_opt(const PresetBundle &bundle)
{
    return bundle.project_config.option<ConfigOptionStrings>("spectrum_source_filament_colour");
}

} // namespace

TEST_CASE("spectrum_source_filament_colour is not a filament_option_key", "[spectrum_source]")
{
    const auto &keys = print_config_def.filament_option_keys();
    CHECK(std::find(keys.begin(), keys.end(), "spectrum_source_filament_colour") == keys.end());
}

TEST_CASE("string overload set_num_filaments snapshots eight hexes including alpha", "[spectrum_source]")
{
    PresetBundle bundle;
    seed_eight_colours(bundle, k_mountain_hexes);

    bundle.set_num_filaments(4);

    auto *physical = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(physical != nullptr);
    REQUIRE(physical->values.size() == 4);

    const ConfigOptionStrings *source = source_opt(bundle);
    REQUIRE(source != nullptr);
    REQUIRE(source->values.size() == 8);
    CHECK(source->values == k_mountain_hexes);
    CHECK(source->values.front() == "#A47C6FFF");
}

TEST_CASE("vector overload set_num_filaments snapshots eight hexes including alpha", "[spectrum_source]")
{
    PresetBundle bundle;
    seed_eight_colours(bundle, k_mountain_hexes);

    const std::vector<std::string> new_colors = {
        "#08ABFBFF", "#D93B90FF", "#F9ED3DFF", "#9199A4FF"};
    bundle.set_num_filaments(4, new_colors);

    auto *physical = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(physical != nullptr);
    REQUIRE(physical->values.size() == 4);

    const ConfigOptionStrings *source = source_opt(bundle);
    REQUIRE(source != nullptr);
    REQUIRE(source->values.size() == 8);
    CHECK(source->values == k_mountain_hexes);
}

TEST_CASE("second shrink does not overwrite spectrum_source_filament_colour", "[spectrum_source]")
{
    PresetBundle bundle;
    seed_eight_colours(bundle, k_mountain_hexes);

    bundle.set_num_filaments(4);
    const ConfigOptionStrings *source_after_first = source_opt(bundle);
    REQUIRE(source_after_first != nullptr);
    const std::vector<std::string> first_snapshot = source_after_first->values;

    bundle.set_num_filaments(2);
    const ConfigOptionStrings *source_after_second = source_opt(bundle);
    REQUIRE(source_after_second != nullptr);
    CHECK(source_after_second->values == first_snapshot);
    CHECK(source_after_second->values == k_mountain_hexes);

    auto *physical = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(physical != nullptr);
    REQUIRE(physical->values.size() == 2);
}

TEST_CASE("snapshot copies filament_colour not filament_multi_colour", "[spectrum_source]")
{
    PresetBundle bundle;
    seed_eight_colours(bundle, k_mountain_hexes);

    bundle.set_num_filaments(4);

    const ConfigOptionStrings *source = source_opt(bundle);
    REQUIRE(source != nullptr);
    REQUIRE(source->values == k_mountain_hexes);
    for (const std::string &hex : source->values)
        CHECK(hex != "#DEADBEFF");
}

TEST_CASE("spectrum_source_filament_colour survives project_config save_to_json", "[spectrum_source]")
{
    PresetBundle bundle;
    seed_eight_colours(bundle, k_mountain_hexes);
    bundle.set_num_filaments(4);

    const auto tmp = boost::filesystem::temp_directory_path() /
                     boost::filesystem::unique_path("spectrum-source-%%%%.json");
    bundle.project_config.save_to_json(tmp.string(), "project_settings", "project", "1.0.0");

    std::ifstream in(tmp.string());
    REQUIRE(in.good());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    boost::filesystem::remove(tmp);

    REQUIRE(json.find("spectrum_source_filament_colour") != std::string::npos);
    for (const std::string &hex : k_mountain_hexes)
        REQUIRE(json.find(hex) != std::string::npos);

    auto *physical = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(physical != nullptr);
    REQUIRE(physical->values.size() == 4);
}

TEST_CASE("mountain 3mf loads paint facets and eight filament_colour hexes", "[spectrum_source]")
{
    const std::string path =
        "C:/dev/Orca/conductor/0007-AdoptPainted3mfUltraS/logs/8color-mountain.3mf";
    if (!boost::filesystem::exists(path))
        SKIP("mountain 3mf not present on this machine");

    DynamicPrintConfig config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    Model model;
    PlateDataPtrs plates;
    std::vector<Preset *> project_presets;
    bool is_bbl = false;
    bool is_orca = false;
    Semver file_version;
    const bool ok = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &project_presets,
                                 &is_bbl, &is_orca, &file_version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    REQUIRE(ok);
    REQUIRE(model.objects.size() == 1);

    bool painted = false;
    for (ModelObject *obj : model.objects) {
        for (ModelVolume *vol : obj->volumes)
            painted = painted || vol->is_mm_painted();
    }
    CHECK(painted);

    auto *colours = config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(colours != nullptr);
    REQUIRE(colours->values.size() == 8);
    CHECK(colours->values == k_mountain_hexes);

    snapshot_spectrum_source_palette_if_empty(config);
    const ConfigOptionStrings *source = config.option<ConfigOptionStrings>("spectrum_source_filament_colour");
    REQUIRE(source != nullptr);
    CHECK(source->values == k_mountain_hexes);

    for (Preset *preset : project_presets)
        delete preset;
    for (PlateData *plate : plates)
        delete plate;
}

TEST_CASE("adopted mountain 3mf round-trip keeps paint and source palette", "[spectrum_source]")
{
    const std::string src_path =
        "C:/dev/Orca/conductor/0007-AdoptPainted3mfUltraS/logs/8color-mountain.3mf";
    const std::string out_path =
        "C:/dev/Orca/conductor/0007-AdoptPainted3mfUltraS/logs/mountain_adopted.3mf";
    if (!boost::filesystem::exists(src_path))
        SKIP("mountain 3mf not present on this machine");

    DynamicPrintConfig config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    Model model;
    PlateDataPtrs plates;
    std::vector<Preset *> project_presets;
    bool is_bbl = false;
    bool is_orca = false;
    Semver file_version;
    REQUIRE(load_bbs_3mf(src_path.c_str(), &config, &ctxt, &model, &plates, &project_presets,
                         &is_bbl, &is_orca, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));

    snapshot_spectrum_source_palette_if_empty(config);
    auto *colours = config.option<ConfigOptionStrings>("filament_colour", true);
    REQUIRE(colours != nullptr);
    colours->values = {"#08ABFBFF", "#D93B90FF", "#F9ED3DFF", "#9199A4FF"};

    StoreParams store;
    store.path = out_path.c_str();
    store.model = &model;
    store.config = &config;
    store.plate_data_list = plates;
    store.project_presets = project_presets;
    store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(store));

    DynamicPrintConfig config2;
    ConfigSubstitutionContext ctxt2{ForwardCompatibilitySubstitutionRule::Enable};
    Model model2;
    PlateDataPtrs plates2;
    std::vector<Preset *> presets2;
    bool is_bbl2 = false;
    bool is_orca2 = false;
    Semver file_version2;
    REQUIRE(load_bbs_3mf(out_path.c_str(), &config2, &ctxt2, &model2, &plates2, &presets2,
                         &is_bbl2, &is_orca2, &file_version2, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));

    const ConfigOptionStrings *source =
        config2.option<ConfigOptionStrings>("spectrum_source_filament_colour");
    REQUIRE(source != nullptr);
    REQUIRE(source->values == k_mountain_hexes);

    auto *physical = config2.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(physical != nullptr);
    REQUIRE(physical->values.size() == 4);
    CHECK(physical->values[0] == "#08ABFBFF");
    CHECK(physical->values[3] == "#9199A4FF");

    bool painted = false;
    for (ModelObject *obj : model2.objects)
        for (ModelVolume *vol : obj->volumes)
            painted = painted || vol->is_mm_painted();
    CHECK(painted);
    REQUIRE(model2.objects.size() == 1);

    for (Preset *preset : project_presets)
        delete preset;
    for (Preset *preset : presets2)
        delete preset;
    for (PlateData *plate : plates)
        delete plate;
    for (PlateData *plate : plates2)
        delete plate;
}
