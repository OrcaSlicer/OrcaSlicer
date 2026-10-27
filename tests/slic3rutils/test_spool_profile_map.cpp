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

TEST_CASE("A spool's name drops the words its group heading already says", "[SpoolProfileMap]")
{
    CHECK(spool_name_without_group("Elegoo PETG", "PETG") == "Elegoo");
    CHECK(spool_name_without_group("ELEGOO PLA MATTE", "Elegoo") == "PLA MATTE");
    CHECK(spool_name_without_group("Snapmaker PLA HIGH SPEED", "PLA HIGH SPEED") == "Snapmaker");
    CHECK(spool_name_without_group("eSun PLA+", "PLA") == "eSun PLA+");
    CHECK(spool_name_without_group("PETG", "PETG") == "PETG");
    CHECK(spool_name_without_group("Elegoo PETG", "") == "Elegoo PETG");
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

TEST_CASE("The store keeps its links and colors through a round trip", "[SpoolProfileMap]")
{
    SpoolStore store;
    spool_remember(store.links, "name:rapid|pla", "Elegoo PLA", "Rapid", "#FF0000,#00FF00");
    spool_remember_color(store.links, "name:rapid|pla", "#0000FF", "Elegoo PLA Blue");

    const auto loaded = spool_store_from_json(spool_store_to_json(store));

    REQUIRE(loaded.has_value());
    CHECK(spool_remembered_preset(loaded->links, "name:rapid|pla") == "Elegoo PLA");
    CHECK(spool_remembered_preset(loaded->links, "name:rapid|pla", "#0000FF") == "Elegoo PLA Blue");
}

TEST_CASE("A filament seen in a colour never answered for is new", "[SpoolProfileMap]")
{
    std::map<std::string, SpoolLink> links;
    CHECK(spool_is_new(links, "name:elegoo pla matte|pla matte", "#F9C2D5"));

    spool_remember_spool(links, "name:elegoo pla matte|pla matte", "Elegoo PLA Matte", "Elegoo PLA Matte", "#F9C2D5");

    CHECK_FALSE(spool_is_new(links, "name:elegoo pla matte|pla matte", "#F9C2D5"));
    CHECK(spool_is_new(links, "name:elegoo pla matte|pla matte", "#1E6FD9"));
    CHECK_FALSE(spool_is_new(links, "name:elegoo pla matte|pla matte", ""));

    spool_remember_spool(links, "name:elegoo pla matte|pla matte", "Elegoo PLA Matte", "Elegoo PLA Matte", "#1E6FD9");

    CHECK_FALSE(spool_is_new(links, "name:elegoo pla matte|pla matte", "#1E6FD9"));
}

TEST_CASE("A store saved with the settings it once held still loads its links", "[SpoolProfileMap]")
{
    const auto loaded = spool_store_from_json(
        R"({"version":2,"settings":{"enabled":true,"remember_generic":true},"links":{"name:rapid|pla":{"preset":"Elegoo PLA"}}})");

    REQUIRE(loaded.has_value());
    CHECK(spool_remembered_preset(loaded->links, "name:rapid|pla") == "Elegoo PLA");
}
