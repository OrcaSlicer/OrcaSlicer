#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
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
    {"Snapmaker PLA Silk",  "PLA-SILK",  "PLA", "Snapmaker", "#30F060"},
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

// Orca: a tag's vendor field and a profile's filament_vendor rarely agree exactly. The match is
// case-insensitive and accepts a whole-word containment either way, so a tag that carries the
// whole product name still reaches the vendor's presets.
TEST_CASE("Vendor matching ignores case and accepts a product name in the vendor field", "[Snapmaker]")
{
    const std::vector<FilamentSpec> sunlu{
        {"SUNLU PLA", "SUNLU-PLA", "PLA", "SUNLU", "#102030"},
    };
    CHECK(match(sunlu, "Sunlu", "PLA", "", "102030FF") == "SUNLU-PLA");

    const std::vector<FilamentSpec> anycubic{
        {"Anycubic PLA Basic", "ANY-BASIC", "PLA", "Anycubic", "#102030"},
        {"Anycubic PLA Matte", "ANY-MATTE", "PLA", "Anycubic", "#F0A0B0"},
    };
    // A legacy tag names the whole product in the vendor field and leaves the sub type empty;
    // "Anycubic PLA Basic" still matches the profiles whose vendor is just "Anycubic".
    CHECK(match(anycubic, "Anycubic PLA Basic", "PLA", "", "102030FF") == "ANY-BASIC");
    // With no sub type to go on, the variant named in the vendor field decides, so Basic wins
    // even when Matte is the exact colour match.
    CHECK(match(anycubic, "Anycubic PLA Basic", "PLA", "", "F0A0B0FF") == "ANY-BASIC");
}

// Orca: a finish lives in the preset name - only a handful of profiles put it in filament_type,
// and first_visible_idx_by_type() strips anything after the first space - so the name match is
// what finds it. A silk spool takes the silk preset however far its colour is, and when the
// vendor ships none it falls back to that vendor's nearest preset rather than to no match:
// sync_ams_list() reads an empty filament_id as an empty slot, which drops the spool entirely.
TEST_CASE("A finish sub type is preferred, and falls back within the vendor", "[Snapmaker]")
{
    // Basic is an exact colour match and still loses to the Silk preset the spool asks for.
    CHECK(match(pla_variants, "Snapmaker", "PLA", "Silk", "102030FF") == "PLA-SILK");

    const std::vector<FilamentSpec> no_silk{
        {"Snapmaker PLA Basic", "PLA-BASIC", "PLA", "Snapmaker", "#102030"},
        {"Snapmaker PLA Matte", "PLA-MATTE", "PLA", "Snapmaker", "#F0A0B0"},
    };
    CHECK(match(no_silk, "Snapmaker", "PLA", "Silk", "102030FF") == "PLA-BASIC");
    CHECK(match(no_silk, "Snapmaker", "PLA", "Silk", "F0A0B0FF") == "PLA-MATTE");
}

// Orca: a grade or brand word names the same material, so it stays a preference. A Polar Filament
// "Pure" spool has no "Pure" preset to find - Snapmaker ships Generic PLA, High Speed, Silk and
// Matte for the U1 - and must still resolve by colour rather than coming back empty.
TEST_CASE("A brand sub type stays a preference", "[Snapmaker]")
{
    CHECK(match(pla_variants, "Snapmaker", "PLA", "Pure", "102030FF") == "PLA-BASIC");
    CHECK(match(pla_variants, "Snapmaker", "PLA", "Basic", "30F060FF") == "PLA-BASIC");
}

// Orca: CF10, CF15 and friends are fibre loadings of the same material, so a CF sub type has to
// match them all and colour decides between them. A trailing letter is a different product.
TEST_CASE("A CF sub type matches every CF fibre loading", "[Snapmaker]")
{
    const std::vector<FilamentSpec> cf_presets{
        {"Snapmaker PA12-CF",   "PA12-CF",   "PA12-CF", "Snapmaker", "#F0A0B0"},
        {"Snapmaker PA12-CF10", "PA12-CF10", "PA12-CF", "Snapmaker", "#102030"},
    };
    // Both match the sub type, so the closer colour wins - either way round.
    CHECK(match(cf_presets, "Snapmaker", "PA12-CF", "CF", "102030FF") == "PA12-CF10");
    CHECK(match(cf_presets, "Snapmaker", "PA12-CF", "CF", "F0A0B0FF") == "PA12-CF");
    // And a tag that reports the loading itself still matches the plain -CF preset.
    CHECK(match(cf_presets, "Snapmaker", "PA12-CF", "CF10", "F0A0B0FF") == "PA12-CF");
    CHECK(match(cf_presets, "Snapmaker", "PA12-CF", "CF15", "102030FF") == "PA12-CF10");
}

// Orca: the same for glass fibre. QIDI ships "QIDI PPS-GF" (filament_type PPS-GF), and a GF20
// spool has to reach it as readily as a preset that spells the loading out.
TEST_CASE("A GF sub type matches every GF fibre loading", "[Snapmaker]")
{
    const std::vector<FilamentSpec> gf_presets{
        {"QIDI PPS-GF",   "PPS-GF",   "PPS-GF", "QIDI", "#F0A0B0"},
        {"QIDI PPS-GF20", "PPS-GF20", "PPS-GF", "QIDI", "#102030"},
    };
    CHECK(match(gf_presets, "QIDI", "PPS-GF", "GF", "102030FF") == "PPS-GF20");
    CHECK(match(gf_presets, "QIDI", "PPS-GF", "GF", "F0A0B0FF") == "PPS-GF");
    // A tag reporting the loading reaches the plain -GF preset too.
    CHECK(match(gf_presets, "QIDI", "PPS-GF", "GF20", "F0A0B0FF") == "PPS-GF");
    CHECK(match(gf_presets, "QIDI", "PPS-GF", "GF20", "102030FF") == "PPS-GF20");
}

// Orca: the fibre word still has to end at a digit or a boundary. "CFX" is its own product, so a
// CF spool must not prefer it - here it is an exact colour match and still has to lose.
TEST_CASE("A CF sub type does not match a CFX preset", "[Snapmaker]")
{
    const std::vector<FilamentSpec> cfx_presets{
        {"Snapmaker PA12-CF",  "PA12-CF",  "PA12-CF", "Snapmaker", "#F0A0B0"},
        {"Snapmaker PA12-CFX", "PA12-CFX", "PA12-CF", "Snapmaker", "#102030"},
    };
    CHECK(match(cfx_presets, "Snapmaker", "PA12-CF", "CF", "102030FF") == "PA12-CF");
}

// Orca: combine_filament_type() has already folded CF into filament_type, so a CF spool may still
// take a correctly typed preset whose name does not repeat the fibre - unlike a finish sub type.
TEST_CASE("A CF sub type stays a preference once the type carries the fibre", "[Snapmaker]")
{
    const std::vector<FilamentSpec> unnamed_fibre{
        {"Snapmaker PA12 Structural", "PA12-STRUCT", "PA12-CF", "Snapmaker", "#102030"},
    };
    CHECK(match(unnamed_fibre, "Snapmaker", "PA12-CF", "CF", "102030FF") == "PA12-STRUCT");
}

// Orca: a profile with no declared colour used to score as black, which let it win every dark
// spool and beat profiles that do declare a colour. It is now unranked - a last resort only.
TEST_CASE("A preset with no colour is unranked, not black", "[Snapmaker]")
{
    const std::vector<FilamentSpec> mixed{
        {"Snapmaker PETG Clear", "PETG-CLEAR", "PETG", "Snapmaker", "#FFFFFF"},
        {"Snapmaker PETG",       "PETG-PLAIN", "PETG", "Snapmaker", ""},
    };
    // A dark spool: black would be the closer match, but the colourless preset cannot be ranked.
    CHECK(match(mixed, "Snapmaker", "PETG", "", "292929FF") == "PETG-CLEAR");
    CHECK(match(mixed, "Snapmaker", "PETG", "", "FEFEFEFF") == "PETG-CLEAR");

    // With nothing else to go on it is still returned, rather than reporting no match at all.
    const std::vector<FilamentSpec> colourless{
        {"Snapmaker PETG", "PETG-PLAIN", "PETG", "Snapmaker", ""},
    };
    CHECK(match(colourless, "Snapmaker", "PETG", "", "292929FF") == "PETG-PLAIN");
}

// Orca: Orca ships U1-compatible presets for a handful of vendors, so a spool from any other
// brand finds nothing in the vendor pass. Orca's own Generic presets cover the sub types the U1
// sees and carry no other brand's temperatures or flow, so they are the fallback - ahead of the
// caller's filament_id_by_type(), which ignores the sub type entirely.
TEST_CASE("A spool from an unknown vendor falls back to a Generic preset", "[Snapmaker]")
{
    const std::vector<FilamentSpec> library{
        {"Generic PLA",      "PLA-GENERIC", "PLA", "Generic", "#102030"},
        {"Generic PLA Silk", "PLA-SILK",    "PLA", "Generic", "#F0A0B0"},
    };
    // Nothing carries the spool's vendor, and the plain preset is the exact colour match, so only
    // the sub type can decide it.
    CHECK(match(library, "Polar Filament", "PLA", "Silk", "102030FF") == "PLA-SILK");
    // With no sub type the Generic preset of the type is taken, rather than whichever preset
    // filament_id_by_type() happens to find first.
    CHECK(match(library, "Polar Filament", "PLA", "", "102030FF") == "PLA-GENERIC");
    // Within the sub type, colour still ranks.
    const std::vector<FilamentSpec> two_silks{
        {"Generic PLA",         "PLA-GENERIC", "PLA", "Generic", "#102030"},
        {"Generic PLA Silk",    "PLA-SILK",    "PLA", "Generic", "#F0A0B0"},
        {"Generic PLA Silk HF", "PLA-SILK-HF", "PLA", "Generic", "#102030"},
    };
    CHECK(match(two_silks, "Polar Filament", "PLA", "Silk", "102030FF") == "PLA-SILK-HF");
}

// Orca: the fallback pass looks at Generic presets only. An unknown brand's Matte spool taking
// "Snapmaker PLA Matte", or a user's Matte preset from a third brand, would print with that
// brand's temperatures and flow, which is worse than a plain profile that carries nobody's.
TEST_CASE("The Generic pass ignores other brands' presets", "[Snapmaker]")
{
    const std::vector<FilamentSpec> other_brand{
        {"Generic PLA",         "PLA-GENERIC", "PLA", "Generic",   "#102030"},
        {"Snapmaker PLA Matte", "SM-MATTE",    "PLA", "Snapmaker", "#F0A0B0"},
    };
    // The Snapmaker preset names the sub type and matches the colour exactly, and still loses.
    CHECK(match(other_brand, "Anycubic", "PLA", "Matte", "F0A0B0FF") == "PLA-GENERIC");

    // A Generic preset that does name the finish is taken, however far its colour.
    const std::vector<FilamentSpec> generic_matte{
        {"Generic PLA",       "PLA-GENERIC", "PLA", "Generic", "#102030"},
        {"Generic PLA Matte", "PLA-MATTE",   "PLA", "Generic", "#F0A0B0"},
    };
    CHECK(match(generic_matte, "Anycubic", "PLA", "Matte", "102030FF") == "PLA-MATTE");
}

// Orca: a tag that reports no vendor matches no profile's filament_vendor, so the vendor pass
// finds nothing for it - a vendorless spool must not take the closest-coloured preset of any
// brand. The Generic pass then handles it, with or without a sub type.
TEST_CASE("A tag with no vendor does not match every vendor", "[Snapmaker]")
{
    const std::vector<FilamentSpec> library{
        {"Generic PLA",      "PLA-GENERIC", "PLA", "Generic",   "#102030"},
        {"Snapmaker PLA",    "SM-PLA",      "PLA", "Snapmaker", "#102030"},
        {"Generic PLA Silk", "PLA-SILK",    "PLA", "Generic",   "#F0A0B0"},
    };
    // The Snapmaker preset is an equally exact colour match and is never considered.
    CHECK(match(library, "", "PLA", "", "102030FF") == "PLA-GENERIC");
    // A vendorless tag that does name a sub type still reaches it through the Generic pass.
    CHECK(match(library, "", "PLA", "Silk", "102030FF") == "PLA-SILK");
}

// Orca: the Generic pass only helps for a type Orca ships a Generic preset of. For any other
// type the slot falls through to the caller's type-only lookup, as it did before - an unrelated
// vendor's filament is never dragged in.
TEST_CASE("A type with no Generic preset falls through", "[Snapmaker]")
{
    const std::vector<FilamentSpec> library{
        {"Generic PLA", "PLA-GENERIC", "PLA",    "Generic", "#102030"},
        {"QIDI PPS-GF", "PPS-GF",      "PPS-GF", "QIDI",    "#102030"},
    };
    CHECK(match(library, "Polar Filament", "PPS-GF", "GF", "102030FF").empty());
    // A sub type no preset carries is still only a preference, so the plain Generic preset of the
    // type is taken.
    CHECK(match(library, "Polar Filament", "PLA", "Pure", "102030FF") == "PLA-GENERIC");
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
    // A fibre loading is not a material type: MaterialType::all() has PA12-CF, never PA12-CF10.
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PA12", "CF10") == "PA12-CF");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PA12-CF", "CF10") == "PA12-CF");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("ABS", "GF30") == "ABS-GF");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PPS", "GF20") == "PPS-GF");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PPS-GF", "GF20") == "PPS-GF");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PLA", "Silk") == "PLA");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("PLA", "NONE") == "PLA");
    CHECK(SnapmakerPrinterAgent::combine_filament_type("", "CF") == "PLA");
}

namespace {

// The `result.status` part of a U1 answer to
// /printer/objects/query?print_task_config&filament_detect, with the fields the firmware publishes
// (Snapmaker u1-klipper, klippy/extras/print_task_config.py). T0 holds a Snapmaker tag, T1 a
// three-colour spool set up on the screen, T2 is empty but keeps its last record, and T3 holds a
// spool nobody has named.
const char *u1_status = R"({
  "print_task_config": {
    "filament_vendor":     ["Snapmaker", "Generic", "Snapmaker", "NONE"],
    "filament_type":       ["PLA", "PLA", "PLA", "NONE"],
    "filament_sub_type":   ["Matte", "Silk", "Basic", "NONE"],
    "filament_color":      [4293959856, 4294901760, 4279246896, 4294967295],
    "filament_color_rgba": ["F0A0B0FF", "FF0000FF", "102030FF", "FFFFFFFF"],
    "filament_color_multi": [
      {"nums": 1, "alpha": 255, "mode": 0, "colors": ["F0A0B0"]},
      {"nums": 3, "alpha": 255, "mode": 0, "colors": ["FF0000", "00FF00", "0000FF"]},
      {"nums": 1, "alpha": 255, "mode": 0, "colors": ["102030"]},
      {"nums": 1, "alpha": 255, "mode": 0, "colors": ["FFFFFF"]}
    ],
    "filament_official": [true, false, false, false],
    "filament_sku":      [900001, 0, 0, 0],
    "filament_edit":     [false, true, true, true],
    "filament_exist":    [true, true, false, true],
    "filament_soft":     [false, false, false, false]
  },
  "filament_detect": {
    "state": [1, 0, 0, 0],
    "info": [
      {"VENDOR": "Snapmaker", "MAIN_TYPE": "PLA", "SUB_TYPE": "Matte", "RGB_1": 15769776, "COLOR_NUMS": 1,
       "HOTEND_MIN_TEMP": 190, "HOTEND_MAX_TEMP": 230, "BED_TEMP": 60, "OFFICIAL": true},
      {}, {}, {}
    ]
  }
})";

const std::vector<FilamentSpec> u1_presets{
    {"Snapmaker PLA Basic", "PLA-BASIC", "PLA", "Snapmaker", "#102030"},
    {"Snapmaker PLA Matte", "PLA-MATTE", "PLA", "Snapmaker", "#F0A0B0"},
    {"Generic PLA Silk",    "GFL96",     "PLA", "Generic",   "#FFFFFF"},
};

std::vector<SnapmakerPrinterAgent::AmsTrayData> parse(const std::string &status)
{
    FilamentTestCollection filaments;
    populate(filaments, u1_presets);
    std::vector<SnapmakerPrinterAgent::AmsTrayData> trays;
    REQUIRE(SnapmakerPrinterAgent::parse_filament_status(nlohmann::json::parse(status), &filaments, trays));
    return trays;
}

} // namespace

// Orca: each toolhead becomes the tray in the same position, so T0..T3 sync to project
// filaments 1..4, each with the preset its record names and its own colour.
TEST_CASE("Each U1 toolhead becomes the tray in its position with its preset and colour", "[Snapmaker]")
{
    const auto trays = parse(u1_status);
    REQUIRE(trays.size() == 4);
    for (int i = 0; i < 4; ++i)
        CHECK(trays[i].slot_index == i);

    CHECK(trays[0].has_filament);
    CHECK(trays[0].tray_type == "PLA");
    CHECK(trays[0].tray_info_idx == "PLA-MATTE");
    CHECK(trays[0].tray_color == "F0A0B0FF");
    CHECK(trays[0].bed_temp == 60);
    CHECK(trays[0].nozzle_temp == 230);

    CHECK(trays[1].has_filament);
    CHECK(trays[1].tray_info_idx == "GFL96");
    CHECK(trays[1].tray_color == "FF0000FF");
}

// Orca: filament_color_multi holds every colour of a multi-colour spool. They reach the tray, so
// the project filament gets all of them, not the first one only. A single-colour spool keeps
// tray_color as its one colour.
TEST_CASE("A multi-colour U1 spool keeps every colour", "[Snapmaker]")
{
    const auto trays = parse(u1_status);
    REQUIRE(trays.size() == 4);
    CHECK(trays[1].tray_colors == std::vector<std::string>{"FF0000", "00FF00", "0000FF"});
    CHECK_FALSE(trays[1].colors_gradient);
    CHECK(trays[0].tray_colors.empty());
}

// Orca: nums counts the colours, but a record may list fewer of them; only listed colours are used.
TEST_CASE("A multi-colour count beyond the listed colours is clamped", "[Snapmaker]")
{
    const auto trays = parse(R"({"print_task_config": {
        "filament_vendor": ["Generic", "Generic"], "filament_type": ["PLA", "PLA"],
        "filament_sub_type": ["Silk", "Silk"], "filament_color_rgba": ["FF0000FF", "00FF00FF"],
        "filament_color_multi": [{"nums": 4, "colors": ["FF0000", "00FF00"]}, {"nums": 2}],
        "filament_exist": [true, true]}})");
    REQUIRE(trays.size() == 2);
    CHECK(trays[0].tray_colors == std::vector<std::string>{"FF0000", "00FF00"});
    CHECK(trays[1].tray_colors.empty());
}

// Orca: an empty head and a loaded head that reports NONE name no preset. Both leave
// tray_info_idx empty, which build_ams_payload turns into a placeholder, so the sync keeps the
// project filament there instead of painting it in the firmware's default white.
TEST_CASE("An empty or unnamed U1 toolhead names no preset", "[Snapmaker]")
{
    const auto trays = parse(u1_status);
    REQUIRE(trays.size() == 4);

    CHECK_FALSE(trays[2].has_filament);
    CHECK(trays[2].tray_info_idx.empty());
    CHECK(trays[2].tray_type.empty());

    CHECK(trays[3].has_filament);
    CHECK(trays[3].tray_info_idx.empty());
    CHECK(trays[3].tray_type.empty());
}

// Orca: a status without print_task_config, or with a field of the wrong type, fails the sync
// instead of throwing out of it.
TEST_CASE("A U1 status without a usable print_task_config is rejected", "[Snapmaker]")
{
    const char *bad = GENERATE(R"({})", R"({"print_task_config": []})", R"({"print_task_config": {}})",
                               R"({"print_task_config": {"filament_exist": "yes"}})",
                               R"({"print_task_config": {"filament_exist": [true], "filament_type": [3]}})");
    FilamentTestCollection filaments;
    populate(filaments, u1_presets);
    std::vector<SnapmakerPrinterAgent::AmsTrayData> trays;
    CHECK_FALSE(SnapmakerPrinterAgent::parse_filament_status(nlohmann::json::parse(bad), &filaments, trays));
    CHECK(trays.empty());
}
