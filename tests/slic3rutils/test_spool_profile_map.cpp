#include <catch2/catch_test_macros.hpp>
#include <map>
#include <string>
#include <vector>

#include "slic3r/Utils/SpoolProfileMap.hpp"

using namespace Slic3r;

namespace {

SpoolPresetCandidate candidate(const std::string& name, const std::string& material, const std::string& filament_id)
{
    SpoolPresetCandidate preset;
    preset.name        = name;
    preset.material    = material;
    preset.filament_id = filament_id;
    return preset;
}

} // namespace

TEST_CASE("A printer's report becomes a spool identity whatever the key spelling", "[SpoolProfileMap]")
{
    const SpoolIdentity spool = spool_identity_from_fields(2, true,
                                                           {{"Filament_Type", "PLA"}, {"brand", "Elegoo"}, {"Product", "Rapid"}, {"mood", "calm"}});

    CHECK(spool.slot_index == 2);
    CHECK(spool.material == "PLA");
    CHECK(spool.vendor == "Elegoo");
    CHECK(spool.name == "Rapid");
    REQUIRE(spool.extra.size() == 1);
    CHECK(spool.extra.front().first == "mood");
}

TEST_CASE("A spool described by its material alone is generic", "[SpoolProfileMap]")
{
    CHECK(spool_is_generic(spool_identity_from_fields(0, true, {{"material", "PLA"}})));
    CHECK_FALSE(spool_is_generic(spool_identity_from_fields(0, true, {{"material", "PLA"}, {"vendor", "Elegoo"}})));
    CHECK_FALSE(spool_is_generic(spool_identity_from_fields(0, true, {{"material", "PLA"}, {"uid", "A1B2C3"}})));
}

TEST_CASE("A multicolor spool keeps every color and its alpha as one variant", "[SpoolProfileMap]")
{
    const SpoolIdentity spool = spool_identity_from_fields(0, true, {{"colors", "#FF0000,#00FF0080"}});

    const std::string variant = spool_variant_color(spool);
    CHECK(variant == "#FF0000,#00FF0080");
    CHECK(spool_variant_colors(variant) == std::vector<std::string>{"#FF0000", "#00FF0080"});
}

TEST_CASE("A fully opaque alpha is dropped from a color", "[SpoolProfileMap]")
{
    CHECK(spool_normalize_color_alpha("FF0000FF") == "#FF0000");
    CHECK(spool_normalize_color_alpha("FF000080") == "#FF000080");
    CHECK(spool_normalize_color("FF000080") == "#FF0000");
}

TEST_CASE("The filament id a printer reports picks the profile", "[SpoolProfileMap]")
{
    SpoolIdentity spool;
    spool.has_filament = true;
    spool.material     = "PLA";
    spool.filament_id  = "GFL99";

    const std::vector<SpoolPresetCandidate> candidates{candidate("Elegoo PLA", "PLA", "GFL99"), candidate("Other PLA", "PLA", "GFA00")};

    CHECK(spool_choose_preset(candidates, spool, "") == "Elegoo PLA");
}

TEST_CASE("A remembered profile wins over the filament id", "[SpoolProfileMap]")
{
    SpoolIdentity spool;
    spool.has_filament = true;
    spool.material     = "PLA";
    spool.filament_id  = "GFL99";

    const std::vector<SpoolPresetCandidate> candidates{candidate("Elegoo PLA", "PLA", "GFL99"), candidate("Other PLA", "PLA", "GFA00")};

    CHECK(spool_choose_preset(candidates, spool, "Other PLA") == "Other PLA");
}

TEST_CASE("The store keeps its links, colors and settings through a round trip", "[SpoolProfileMap]")
{
    SpoolStore store;
    spool_remember(store.links, "name:rapid|pla", "Elegoo PLA", "Rapid", "#FF0000,#00FF00");
    spool_remember_color(store.links, "name:rapid|pla", "#0000FF", "Elegoo PLA Blue");
    store.settings.ask_only_new     = false;
    store.settings.remember_generic = true;

    const auto loaded = spool_store_from_json(spool_store_to_json(store));

    REQUIRE(loaded.has_value());
    CHECK(spool_remembered_preset(loaded->links, "name:rapid|pla") == "Elegoo PLA");
    CHECK(spool_remembered_preset(loaded->links, "name:rapid|pla", "#0000FF") == "Elegoo PLA Blue");
    CHECK_FALSE(spool_is_new(loaded->links, "name:rapid|pla", "#FF0000,#00FF00"));
    CHECK(spool_is_new(loaded->links, "name:rapid|pla", "#123456"));
    CHECK_FALSE(loaded->settings.ask_only_new);
    CHECK(loaded->settings.remember_generic);
    CHECK(loaded->settings.enabled);
}

TEST_CASE("A store written before the generic flag existed does not link generic spools", "[SpoolProfileMap]")
{
    const auto loaded = spool_store_from_json(R"({"version":2,"settings":{"enabled":true},"links":{}})");

    REQUIRE(loaded.has_value());
    CHECK_FALSE(loaded->settings.remember_generic);
}
