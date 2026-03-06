#include <catch2/catch_all.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/FilamentMatcher.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// A filament PresetCollection built in memory, matching how PresetBundle builds
// its own (PresetBundle.cpp). PresetCollection holds a mutex, so it is neither
// copyable nor movable -- the fixture owns it in place and hands out a
// reference rather than returning one by value.
struct Filaments
{
    PresetCollection coll;

    Filaments()
        : coll(Preset::TYPE_FILAMENT,
               Preset::filament_options(),
               static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()),
               "Default Filament")
    {}

    // `colour` is the preset's declared default_filament_colour ("#RRGGBB"), or
    // "" for a preset that declares none. `inherits` names a parent so the test
    // can build a derived preset, which is what every GUI-created preset is.
    Preset &add(const std::string &name,
                const std::string &filament_id,
                const std::string &type,
                const std::string &vendor,
                const std::string &colour,
                const std::string &inherits = "")
    {
        DynamicPrintConfig cfg;
        cfg.set_key_value("filament_type", new ConfigOptionStrings{type});
        cfg.set_key_value("filament_vendor", new ConfigOptionStrings{vendor});
        cfg.set_key_value("default_filament_colour", new ConfigOptionStrings{colour});
        if (!inherits.empty())
            cfg.set_key_value("inherits", new ConfigOptionString(inherits));

        Preset &p = coll.load_preset("", name, cfg, /*select=*/false);
        p.filament_id   = filament_id;
        p.is_visible    = true;
        p.is_compatible = true;
        return p;
    }

    // filament_id_by_type() (the level 4d rung) only considers system base
    // presets, so a test that exercises it has to mark its presets as system.
    Preset &add_system(const std::string &name,
                       const std::string &filament_id,
                       const std::string &type)
    {
        Preset &p   = add(name, filament_id, type, "", "");
        p.is_system = true;
        return p;
    }
};

// The six eSun PLA presets from the reported case, with their real colours.
// "PLA+ Black" is #000000 and so sits nearest a reported 060606; the matte
// profile it must not displace is #202020.
void add_esun_pla(Filaments &f)
{
    f.add("eSun Matte PLA Almond", "GFL99", "PLA", "eSun", "#E9E795");
    f.add("eSun Matte PLA Black", "GFL99", "PLA", "eSun", "#202020");
    f.add("eSun PLA Matte @Qidi X-Plus 4 0.4 nozzle", "GFL99", "PLA", "eSun", "#404040");
    f.add("eSun PLA+ Black", "GFL99", "PLA", "eSun", "#000000");
    f.add("eSun PLA+ Brown", "GFL99", "PLA", "eSun", "#4A2500");
}

FilamentMatchInput qidi_slot(const std::string &vendor,
                             const std::string &name,
                             const std::string &type,
                             const std::string &colour)
{
    FilamentMatchInput in;
    in.prefix        = "QD_0";
    in.vendor_name   = vendor;
    in.filament_name = name;
    in.tray_type     = type;
    in.color         = colour;
    return in;
}

} // namespace

// ---------------------------------------------------------------------------
// The reported defect: colour must not be allowed to select identity.
// ---------------------------------------------------------------------------

TEST_CASE("A reported product name outranks a nearer colour", "[FilamentMatcher]")
{
    Filaments f;
    add_esun_pla(f);

    // Reported: eSUN "PLA Matte", colour 060606. By colour alone "eSun PLA+
    // Black" (#000000, distance 108) beats "eSun Matte PLA Black" (#202020,
    // distance 2028) by 19x -- the name is the only thing that separates them.
    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("eSUN", "PLA_Matte", "PLA", "060606"));

    REQUIRE(r.preset_name == "eSun Matte PLA Black");
}

TEST_CASE("Name matching ignores word order", "[FilamentMatcher]")
{
    // "PLA Matte" must match a preset named "Matte PLA" just as well.
    Filaments f;
    f.add("eSun Matte PLA Black", "GFL99", "PLA", "eSun", "#202020");
    f.add("eSun PLA+ Black", "GFL99", "PLA", "eSun", "#000000");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("eSUN", "PLA_Matte", "PLA", "060606"));

    REQUIRE(r.preset_name == "eSun Matte PLA Black");
}

TEST_CASE("A name match with no colour beats a colour match with the wrong name",
          "[FilamentMatcher]")
{
    // The generalized form of the defect: if the name rung ran only inside the
    // colour pass, the colourless matte profile would lose to the coloured PLA+.
    Filaments f;
    f.add("eSun PLA Matte Black", "GFL99", "PLA", "eSun", ""); // declares no colour
    f.add("eSun PLA+ Black", "GFL99", "PLA", "eSun", "#000000");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("eSUN", "PLA_Matte", "PLA", "060606"));

    REQUIRE(r.preset_name == "eSun PLA Matte Black");
}

TEST_CASE("A reported PLA matches a preset named PLA+", "[FilamentMatcher]")
{
    // Tokens split on non-alphanumerics, so "PLA+" yields [PLA] and a bare
    // reported "PLA" still narrows to it rather than falling through.
    Filaments f;
    f.add("Sunlu PLA+ White @Qidi X-Plus 4", "GFL99", "PLA", "Sunlu", "#FFFFFF");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("SUNLU", "PLA", "PLA", "FAFAFA"));

    REQUIRE(r.preset_name == "Sunlu PLA+ White @Qidi X-Plus 4");
}

TEST_CASE("Vendor matching ignores case", "[FilamentMatcher]")
{
    // The printer says "SUNLU", the preset says "Sunlu".
    Filaments f;
    f.add("Sunlu PLA White", "GFL99", "PLA", "Sunlu", "#FFFFFF");

    REQUIRE(FilamentMatcher::resolve(&f.coll, qidi_slot("SUNLU", "PLA", "PLA", "FFFFFF")).preset_name
            == "Sunlu PLA White");
}

TEST_CASE("Vendor matching bridges a separator difference", "[FilamentMatcher]")
{
    // Both sides run through sanitize_for_id(), so "Acme Inc" and "Acme-Inc"
    // collapse to the same token. Note this only works when both sides have a
    // separator: a reported "eSUN" does NOT reach a preset's "e-Sun", because
    // only the preset gains an underscore. Agents sanitize what the printer
    // reports, so the two sides normalize identically in practice.
    Filaments f;
    f.add("Acme Inc PLA White", "GFL99", "PLA", "Acme-Inc", "#FFFFFF");

    const auto r = FilamentMatcher::resolve(
        &f.coll, qidi_slot(FilamentMatcher::sanitize_for_id("Acme Inc"), "PLA", "PLA", "FFFFFF"));

    REQUIRE(r.preset_name == "Acme Inc PLA White");
}

// ---------------------------------------------------------------------------
// Colour is evidence, not a default.
// ---------------------------------------------------------------------------

TEST_CASE("A preset declaring no colour sits out the closest-colour rung", "[FilamentMatcher]")
{
    // Scoring a colourless preset as black would make it win a black spool
    // outright, and make every colourless preset tie so collection order
    // decided the winner. The coloured profile must take a black spool.
    Filaments f;
    f.add("Acme PLA Unspecified", "GFL99", "PLA", "Acme", "");
    f.add("Acme PLA Black", "GFL99", "PLA", "Acme", "#000000");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("Acme", "PLA", "PLA", "000000"));

    REQUIRE(r.preset_name == "Acme PLA Black");
}

TEST_CASE("A colourless preset is still reachable when nothing declares a colour",
          "[FilamentMatcher]")
{
    // Sitting out the colour rung must not mean being excluded entirely --
    // vendor profiles overwhelmingly declare no default_filament_colour.
    Filaments f;
    f.add("Acme PLA Unspecified", "GFL99", "PLA", "Acme", "");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("Acme", "PLA", "PLA", "123456"));

    REQUIRE(r.preset_name == "Acme PLA Unspecified");
}

TEST_CASE("Closest colour picks the nearest of several same-vendor profiles", "[FilamentMatcher]")
{
    // An exact colour is not required; the nearest wins.
    Filaments f;
    f.add("Acme PLA White", "GFL99", "PLA", "Acme", "#FFFFFF");
    f.add("Acme PLA Red", "GFL99", "PLA", "Acme", "#FF0000");
    f.add("Acme PLA Black", "GFL99", "PLA", "Acme", "#000000");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("Acme", "PLA", "PLA", "EE1111"));

    REQUIRE(r.preset_name == "Acme PLA Red");
}

// ---------------------------------------------------------------------------
// Which presets are eligible at all.
// ---------------------------------------------------------------------------

TEST_CASE("A derived preset is eligible for config matching", "[FilamentMatcher]")
{
    // Every GUI-created preset inherits from the profile it was saved off, so
    // restricting config matching to base presets excluded exactly the presets
    // that have no other way to be matched.
    Filaments f;
    f.add("Qidi Generic PLA", "GFL99", "PLA", "Qidi", "");
    f.add("Flashforge Red HS PLA @Qidi X-Plus 4 0.4 nozzle", "GFL99", "PLA", "Flashforge",
          "#800000", /*inherits=*/"Qidi Generic PLA");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("Flashforge", "PLA_HS", "PLA", "FF362D"));

    REQUIRE(r.preset_name == "Flashforge Red HS PLA @Qidi X-Plus 4 0.4 nozzle");
}

TEST_CASE("Invisible and incompatible presets are never matched", "[FilamentMatcher]")
{
    Filaments f;
    Preset &hidden = f.add("Acme PLA Hidden", "GFL99", "PLA", "Acme", "#FF0000");
    hidden.is_visible = false;
    Preset &incompatible = f.add("Acme PLA Incompatible", "GFL99", "PLA", "Acme", "#FF0000");
    incompatible.is_compatible = false;
    f.add("Acme PLA Visible", "GFL99", "PLA", "Acme", "#0000FF");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("Acme", "PLA", "PLA", "FF0000"));

    REQUIRE(r.preset_name == "Acme PLA Visible");
}

// ---------------------------------------------------------------------------
// The id levels: hand-authored filament_ids still win.
// ---------------------------------------------------------------------------

TEST_CASE("An authored vendor+filament id outranks a config-field match", "[FilamentMatcher]")
{
    Filaments f;
    f.add("Acme PLA By Config", "GFL99", "PLA", "Acme", "#FF0000");
    f.add("Acme PLA By Id", "Acme_PLA_Matte", "PLA", "SomeoneElse", "");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("Acme", "PLA_Matte", "PLA", "FF0000"));

    REQUIRE(r.preset_name == "Acme PLA By Id");
}

TEST_CASE("An authored id with a colour suffix picks the nearest colour", "[FilamentMatcher]")
{
    Filaments f;
    f.add("Acme PLA Red", "Acme_PLA_FF0000", "PLA", "Acme", "");
    f.add("Acme PLA Blue", "Acme_PLA_0000FF", "PLA", "Acme", "");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("Acme", "PLA", "PLA", "EE1111"));

    REQUIRE(r.preset_name == "Acme PLA Red");
}

TEST_CASE("A colour-suffixed id group never matches a longer name", "[FilamentMatcher]")
{
    // The suffix must be exactly six hex digits, so group "Acme_PLA" matches
    // "Acme_PLA_FF0000" but never "Acme_PLA_Matte_FF0000" -- otherwise a bare
    // "PLA" spool would be captured by a "PLA Matte" profile.
    //
    // The preset is given a different vendor and an unrelated name so that the
    // id group is the *only* rung that could reach it. A named result would
    // therefore mean the group matched; falling through leaves preset_name
    // empty, since the remaining rungs answer with a type's generic id.
    // The fallback preset is deliberately named without a "PLA" token, so the
    // name rung cannot claim it and level 4d stays the only route.
    Filaments f;
    f.add("Unrelated Profile", "Acme_PLA_Matte_FF0000", "PLA", "SomeoneElse", "");
    f.add_system("Basic Filament", "OGFL99", "PLA");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("Acme", "PLA", "PLA", "FF0000"));

    REQUIRE(r.preset_name.empty());
}

// ---------------------------------------------------------------------------
// The result must be able to name a preset, not just an id.
// ---------------------------------------------------------------------------

TEST_CASE("A matched preset is named even when its filament_id is shared", "[FilamentMatcher]")
{
    // Five presets share GFL99 here, as they do in the shipped profiles, so an
    // id alone cannot identify the match. sync_ams_list resolves the id by
    // first-match, which is why the preset name has to travel with it.
    Filaments f;
    add_esun_pla(f);

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("eSUN", "PLA_Matte", "PLA", "060606"));

    REQUIRE(r.filament_id == "GFL99");
    REQUIRE(r.preset_name == "eSun Matte PLA Black");
}

TEST_CASE("A generic fallback names no preset", "[FilamentMatcher]")
{
    // Rungs that answer with a type's generic id rather than one particular
    // preset must leave preset_name empty, so PresetBundle::sync_ams_list falls
    // back to looking the id up -- which is the right behaviour for an answer
    // that was never about one specific preset.
    // Named without a "PLA" token so the name rung cannot claim it: an unknown
    // vendor AND an unmatchable name leave the generic rung as the only answer.
    Filaments f;
    f.add_system("Basic Filament", "OGFL99", "PLA");

    const auto r = FilamentMatcher::resolve(&f.coll, qidi_slot("Nobody", "PLA", "PLA", "FF0000"));

    REQUIRE(r.preset_name.empty());
    REQUIRE(r.filament_id == "OGFL99");
}

TEST_CASE("Resolving without a preset collection still yields an id", "[FilamentMatcher]")
{
    FilamentMatchInput in = qidi_slot("Acme", "PLA", "PLA", "FF0000");
    in.vendor_type   = 7;
    in.filament_idx  = 52;

    const auto r = FilamentMatcher::resolve(nullptr, in);

    REQUIRE(r.preset_name.empty());
    REQUIRE(r.filament_id == "QD_0_7_52");
}

// ---------------------------------------------------------------------------
// Printers that report no filament name (Snapmaker, Moonraker).
// ---------------------------------------------------------------------------

TEST_CASE("Vendor and type still match when no filament name is reported", "[FilamentMatcher]")
{
    // Snapmaker reports vendor + type + colour and no product name. Those rungs
    // must behave exactly as before the name signal was added.
    Filaments f;
    f.add("eSun PLA-CF Black", "GFL98", "PLA-CF", "eSUN", "#000000");
    f.add("eSun PLA-CF White", "GFL98", "PLA-CF", "eSUN", "#FFFFFF");

    FilamentMatchInput in;
    in.vendor_name = "eSUN";
    in.tray_type   = "PLA-CF";
    in.color       = "F0F0F0FF"; // Snapmaker reports RRGGBBAA

    const auto r = FilamentMatcher::resolve(&f.coll, in);

    REQUIRE(r.preset_name == "eSun PLA-CF White");
}

TEST_CASE("A type-only report falls through to the generic for that type", "[FilamentMatcher]")
{
    // Moonraker reports only a material type: no vendor, no name, no colour.
    Filaments f;
    f.add_system("Generic PLA", "OGFL99", "PLA");
    f.add_system("Generic ABS", "OGFB99", "ABS");

    FilamentMatchInput in;
    in.tray_type = "PLA";

    const auto r = FilamentMatcher::resolve(&f.coll, in);

    REQUIRE(r.filament_id == "OGFL99");
}

// ---------------------------------------------------------------------------
// filament_type resolution, read out of the collection rather than hardcoded.
// ---------------------------------------------------------------------------

TEST_CASE("A composite filament type is not flattened to its base", "[FilamentMatcher]")
{
    // A substring ladder returned on the first hit, so "PLA-CF" became "PLA"
    // and the spool was then excluded from its own PLA-CF profiles by the type
    // gate. The most specific type present must win.
    Filaments f;
    f.add("Generic PLA", "OGFL99", "PLA", "", "");
    f.add("Generic PLA-CF", "OGFL98", "PLA-CF", "", "");

    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "PLA-CF") == "PLA-CF");
    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "PLA") == "PLA");
}

TEST_CASE("A finish word does not create a composite type", "[FilamentMatcher]")
{
    // "Matte" is a finish, not a material -- no such type exists, so the
    // reported "PLA Matte" is still a PLA.
    Filaments f;
    f.add("Generic PLA", "OGFL99", "PLA", "", "");
    f.add("Generic PLA-CF", "OGFL98", "PLA-CF", "", "");

    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "PLA Matte") == "PLA");
}

TEST_CASE("A composite degrades to its base when no composite profile exists",
          "[FilamentMatcher]")
{
    // A user with no CF profiles installed should still land on plain PLA
    // rather than nothing at all.
    Filaments f;
    f.add("Generic PLA", "OGFL99", "PLA", "", "");

    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "PLA-CF") == "PLA");
}

TEST_CASE("Multi-word and prefix-colliding types resolve to the right one", "[FilamentMatcher]")
{
    Filaments f;
    f.add("Generic PA", "OGFN99", "PA", "", "");
    f.add("Generic PA-CF", "OGFN98", "PA-CF", "", "");
    f.add("Generic PAHT-CF", "x1", "PAHT-CF", "", "");
    f.add("Generic ABS", "OGFB99", "ABS", "", "");
    f.add("Generic PC", "OGFC99", "PC", "", "");
    f.add("Generic PC-ABS-FR", "x2", "PC-ABS-FR", "", "");

    // "PAHT" is its own token, so PA-CF must not claim a PAHT-CF spool.
    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "PAHT-CF") == "PAHT-CF");
    // The ladder returned "ABS" here, on the first substring hit.
    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "PC-ABS-FR") == "PC-ABS-FR");
    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "PA-CF") == "PA-CF");
}

TEST_CASE("Nylon resolves to PA", "[FilamentMatcher]")
{
    Filaments f;
    f.add("Generic PA", "OGFN99", "PA", "", "");

    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "Nylon") == "PA");
}

TEST_CASE("An unrecognized material resolves to no type", "[FilamentMatcher]")
{
    // Empty is the signal for the caller to fall back rather than to invent one.
    Filaments f;
    f.add("Generic PLA", "OGFL99", "PLA", "", "");

    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "Unobtanium").empty());
    REQUIRE(FilamentMatcher::resolve_filament_type(f.coll, "").empty());
}

// ---------------------------------------------------------------------------
// sanitize_for_id, which agents use to build the reported name and vendor.
// ---------------------------------------------------------------------------

TEST_CASE("Names are sanitized into id-safe tokens", "[FilamentMatcher]")
{
    CHECK(FilamentMatcher::sanitize_for_id("Acme Inc") == "Acme_Inc");
    CHECK(FilamentMatcher::sanitize_for_id("PLA Plus") == "PLA_Plus");
    CHECK(FilamentMatcher::sanitize_for_id("PLA-AERO") == "PLA_AERO");
    CHECK(FilamentMatcher::sanitize_for_id("TPU-AERO 64D") == "TPU_AERO_64D");
    // Consecutive separators collapse and trailing ones are stripped.
    CHECK(FilamentMatcher::sanitize_for_id("  Acme -- Inc  ") == "Acme_Inc");
}
