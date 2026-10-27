#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"

using namespace Slic3r;

namespace {

// A standalone filament collection, built the same way PresetBundle builds its own, so the
// matcher can be exercised without loading the shipped profiles or touching a printer.
struct FilamentTestCollection : public PresetCollection
{
    FilamentTestCollection()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {}
};

struct FilamentSpec
{
    const char *name;
    const char *filament_id;
    const char *filament_type;
    const char *vendor;
    const char *colour;        // "#RRGGBB", as a profile stores it
    const char *inherits = ""; // non-empty makes this a derived user preset
};

// Load the specs into the collection, then stamp the fields the matcher reads. Done in a second
// pass because load_preset() keeps m_presets sorted, so a reference taken during the first pass
// can be left dangling by a later load.
void populate(PresetCollection &filaments, const std::vector<FilamentSpec> &specs)
{
    for (const FilamentSpec &spec : specs) {
        DynamicPrintConfig config(filaments.default_preset().config);
        config.option<ConfigOptionStrings>("filament_type", true)->values           = {spec.filament_type};
        config.option<ConfigOptionStrings>("filament_vendor", true)->values         = {spec.vendor};
        config.option<ConfigOptionStrings>("default_filament_colour", true)->values = {spec.colour};
        config.option<ConfigOptionString>("inherits", true)->value                  = spec.inherits;
        filaments.load_preset(std::string(), spec.name, config, /*select=*/false);
    }
    for (auto it = filaments.begin(); it != filaments.end(); ++it) {
        const auto spec = std::find_if(specs.begin(), specs.end(),
                                       [&](const FilamentSpec &s) { return it->name == s.name; });
        if (spec == specs.end())
            continue;
        it->filament_id   = spec->filament_id;
        it->is_system     = std::string(spec->inherits).empty();
        it->is_default    = false;
        it->is_visible    = true;
        it->is_compatible = true;
    }
}

std::string match(const std::vector<FilamentSpec> &specs, const std::string &vendor,
                  const std::string &type, const std::string &sub_type, const std::string &colour)
{
    FilamentTestCollection filaments;
    populate(filaments, specs);
    return SnapmakerPrinterAgent::find_closest_color_preset_by_vendor_and_type(filaments, vendor, type, sub_type,
                                                                               colour);
}

// Two variants of the same vendor and type, as the U1 profiles ship them: the variant lives in the
// preset name, not in filament_type, and only their colours differ.
const std::vector<FilamentSpec> pla_variants{
    {"Snapmaker PLA Basic", "PLA-BASIC", "PLA", "Snapmaker", "#102030"},
    {"Snapmaker PLA Matte", "PLA-MATTE", "PLA", "Snapmaker", "#F0A0B0"},
};

} // namespace

// Orca: a spool reports its variant as a sub type, so a Matte spool must not resolve to whichever
// variant happens to sit nearest in colour - here Basic, which the spool colour matches exactly.
TEST_CASE("A spool's sub type beats a closer colour", "[Snapmaker]")
{
    CHECK(match(pla_variants, "Snapmaker", "PLA", "Matte", "102030FF") == "PLA-MATTE");
    // With no sub type to go on, colour decides as before.
    CHECK(match(pla_variants, "Snapmaker", "PLA", "", "102030FF") == "PLA-BASIC");
}

// Orca: the printer sends an empty colour for a spool with no tag, and std::stoul threw on it.
TEST_CASE("An unusable spool colour returns no match instead of throwing", "[Snapmaker]")
{
    CHECK(match(pla_variants, "Snapmaker", "PLA", "Matte", "").empty());
    CHECK(match(pla_variants, "Snapmaker", "PLA", "Matte", "not-a-colour").empty());
    CHECK(match(pla_variants, "Snapmaker", "PLA", "Matte", "#").empty());
}

// Orca: the sub type is matched on whole words, so "CF" must not be found inside "CF10". The CF10
// preset here is an exact colour match and still has to lose to the one the spool actually names.
TEST_CASE("A CF spool matches PA12-CF but not PA12-CF10", "[Snapmaker]")
{
    const std::vector<FilamentSpec> cf_presets{
        {"Snapmaker PA12-CF",   "PA12-CF",   "PA12-CF", "Snapmaker", "#F0A0B0"},
        {"Snapmaker PA12-CF10", "PA12-CF10", "PA12-CF", "Snapmaker", "#102030"},
    };
    CHECK(match(cf_presets, "Snapmaker", "PA12-CF", "CF", "102030FF") == "PA12-CF");
}

// Orca: sync_ams_list() resolves a filament_id to the first base preset carrying it, so a derived
// user copy can never be loaded and must not win the match - even with an exact colour.
TEST_CASE("A derived user preset never wins the match", "[Snapmaker]")
{
    const std::vector<FilamentSpec> with_copy{
        {"Snapmaker PLA Basic",      "PLA-BASIC", "PLA", "Snapmaker", "#F0A0B0"},
        {"Snapmaker PLA Basic copy", "PLA-COPY",  "PLA", "Snapmaker", "#102030", "Snapmaker PLA Basic"},
    };
    CHECK(match(with_copy, "Snapmaker", "PLA", "", "102030FF") == "PLA-BASIC");
}

// Orca: only CF and GF are material types; every other sub type is a finish or a brand name, and a
// base that already carries the suffix must not gain a second one.
TEST_CASE("Only CF and GF change the material type", "[Snapmaker]")
{
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PLA", "CF") == "PLA-CF");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PETG", "GF") == "PETG-GF");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PA12-CF", "CF") == "PA12-CF");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PLA", "Silk") == "PLA");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PLA", "NONE") == "PLA");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("", "CF") == "PLA");
}
