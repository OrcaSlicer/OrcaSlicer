#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

struct FilamentSpec
{
    const char* name;
    const char* filament_id;
    const char* filament_type;
};

// A standalone filament collection of visible, compatible system base presets.
struct FilamentCollection : public PresetCollection
{
    explicit FilamentCollection(const std::vector<FilamentSpec>& specs)
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(), static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults()))
    {
        for (const FilamentSpec& spec : specs) {
            DynamicPrintConfig config(default_preset().config);
            config.option<ConfigOptionStrings>("filament_type", true)->values = {spec.filament_type};
            load_preset(std::string(), spec.name, config, /*select=*/false);
        }
        // A second pass: load_preset() keeps the presets sorted, so earlier references can move.
        for (auto it = begin(); it != end(); ++it) {
            const auto spec = std::find_if(specs.begin(), specs.end(), [&](const FilamentSpec& s) { return it->name == s.name; });
            if (spec == specs.end())
                continue;
            it->filament_id   = spec->filament_id;
            it->is_system     = true;
            it->is_default    = false;
            it->is_visible    = true;
            it->is_compatible = true;
        }
    }
};

} // namespace

TEST_CASE("A filament type resolves to the visible preset of that type", "[PresetCollection]")
{
    const FilamentCollection filaments({{"Generic PLA", "PLA-GENERIC", "PLA"}, {"Generic ABS", "ABS-GENERIC", "ABS"}});
    CHECK(filaments.filament_id_by_type("ABS") == "ABS-GENERIC");
}

TEST_CASE("A filament type with a modifier resolves to its base type", "[PresetCollection]")
{
    const FilamentCollection filaments({{"Generic PLA", "PLA-GENERIC", "PLA"}});
    CHECK(filaments.filament_id_by_type("PLA High Speed") == "PLA-GENERIC");
}

TEST_CASE("A filament type with no visible preset resolves to no preset, not another material", "[PresetCollection]")
{
    const FilamentCollection filaments({{"Generic PLA", "PLA-GENERIC", "PLA"}});
    CHECK(filaments.filament_id_by_type("ABS").empty());
}

TEST_CASE("A filament type with no visible preset has no preset index", "[PresetCollection]")
{
    const FilamentCollection filaments({{"Generic PLA", "PLA-GENERIC", "PLA"}});
    CHECK(filaments.first_visible_idx_by_type("ABS") == size_t(-1));
}
