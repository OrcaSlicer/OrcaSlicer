#include <catch2/catch_all.hpp>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentCookbook.hpp"
#include "libslic3r/MixedFilamentOfd.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

namespace {

const char *k_fixture = R"JSON({
  "source": "fixture",
  "variants": [
    {
      "brand": "Polymaker",
      "filament": "PolyLite PLA",
      "variant": "Teal",
      "material": "PLA",
      "color_hex": "#008080"
    },
    {
      "brand": "Panchroma",
      "filament": "Matte PLA",
      "variant": "Sky Blue",
      "material": "PLA",
      "color_hex": "#1ac5fc"
    },
    {
      "brand": "Panchroma",
      "filament": "Dual Silk PLA",
      "variant": "Caribbean",
      "material": "PLA",
      "color_hex": ["#63AFBA", "#1A6B75"]
    }
  ]
})JSON";

} // namespace

TEST_CASE("ofd lookup fixture hex and missing name", "[spectrum_ofd]")
{
    const auto catalog = spectrum_ofd_parse(k_fixture);
    REQUIRE(catalog.size() == 3);

    const auto teal = spectrum_ofd_lookup(catalog, "Polymaker", "teal");
    REQUIRE(teal.size() == 1);
    CHECK(teal[0].brand == "Polymaker");
    CHECK(teal[0].filament == "PolyLite PLA");
    CHECK(spectrum_ofd_slot_hex(teal[0]) == "#008080");

    const auto panchroma = spectrum_ofd_lookup(catalog, "Panchroma", "sky");
    REQUIRE(panchroma.size() == 1);
    CHECK(spectrum_ofd_slot_hex(panchroma[0]) == "#1AC5FC");

    const auto missing = spectrum_ofd_lookup(catalog, "Panchroma", "no-such-spool");
    CHECK(missing.empty());

    const auto by_material = spectrum_ofd_lookup(catalog, "", "pla");
    REQUIRE(by_material.size() == 3);

    const auto brand_in_name = spectrum_ofd_lookup(catalog, "", "polymaker");
    REQUIRE(brand_in_name.size() == 1);
    CHECK(brand_in_name[0].variant == "Teal");
}

TEST_CASE("ofd array color_hex keeps all hexes", "[spectrum_ofd]")
{
    const auto catalog = spectrum_ofd_parse(k_fixture);
    const auto dual    = spectrum_ofd_lookup(catalog, "Panchroma", "caribbean");
    REQUIRE(dual.size() == 1);
    REQUIRE(dual[0].color_hexes.size() == 2);
    CHECK(spectrum_ofd_slot_hex(dual[0]) == "#63AFBA");
    CHECK(dual[0].color_hexes[1] == "#1A6B75");
}

TEST_CASE("ofd stamp helper slot override and force", "[spectrum_ofd]")
{
    std::vector<std::string> colour{"#111111", "#222222", "#333333"};
    std::vector<std::string> multi{"#111111", "#222222", "#333333"};
    std::vector<std::string> type{"1", "1", "1"};
    std::vector<char>        flags{0, 0, 0};

    REQUIRE(spectrum_ofd_stamp_slot(colour, multi, type, flags, 1, {"#aabbcc"}, false));
    CHECK(colour[0] == "#111111");
    CHECK(colour[1] == "#AABBCC");
    CHECK(colour[2] == "#333333");
    CHECK(multi[1] == "#AABBCC");
    CHECK(type[1] == "1");
    CHECK(flags[1] == 0);

    REQUIRE(spectrum_ofd_stamp_slot(colour, multi, type, flags, 0, {"#63AFBA", "#1A6B75"}, false));
    CHECK(colour[0] == "#63AFBA");
    CHECK(multi[0] == "#63AFBA #1A6B75");
    CHECK(type[0] == "1");
    CHECK(colour[1] == "#AABBCC");

    flags[1] = 1;
    REQUIRE_FALSE(spectrum_ofd_stamp_slot(colour, multi, type, flags, 1, {"#000000"}, false));
    CHECK(colour[1] == "#AABBCC");
    CHECK(flags[1] == 1);

    REQUIRE(spectrum_ofd_stamp_slot(colour, multi, type, flags, 1, {"#000000"}, true));
    CHECK(colour[1] == "#000000");
    CHECK(flags[1] == 0);

    REQUIRE_FALSE(spectrum_ofd_stamp_slot(colour, multi, type, flags, 2, {}, false));
    CHECK(colour[2] == "#333333");
}

TEST_CASE("ofd mix-seed cookbook append never clobber", "[spectrum_ofd]")
{
    const std::vector<std::string> four_hex{"#2F2E30", "#E6DDDB", "#DE1619", "#005AA2"};

    SECTION("empty + Yes → cookbook rows")
    {
        const auto added = spectrum_ofd_mix_seed_apply({}, 4, true, SpectrumMixSeedMode::Ask);
        REQUIRE_FALSE(added.empty());
        const auto cookbook = spectrum_cookbook_append({}, 4);
        REQUIRE(added.size() == cookbook.added.size());
        REQUIRE(spectrum_cookbook_same_recipe(added.front(), cookbook.added.front()));
    }

    SECTION("existing + Yes → keep existing, append missing")
    {
        MixedFilament custom;
        custom.component_a = 1;
        custom.component_b = 2;
        custom.ratio_a     = 3;
        custom.ratio_b     = 1;
        custom.enabled     = true;
        const auto out = spectrum_ofd_mix_seed_apply({custom}, 4, true, SpectrumMixSeedMode::Ask);
        REQUIRE(out.size() > 1);
        REQUIRE(spectrum_cookbook_same_recipe(out.front(), custom));
        size_t custom_count = 0;
        for (const MixedFilament &mf : out) {
            if (spectrum_cookbook_same_recipe(mf, custom))
                ++custom_count;
        }
        REQUIRE(custom_count == 1);
    }

    SECTION("Never → no rows")
    {
        const auto out = spectrum_ofd_mix_seed_apply({}, 4, true, SpectrumMixSeedMode::Never);
        REQUIRE(out.empty());
        CHECK(spectrum_ofd_mix_seed_decision({}, four_hex, SpectrumMixSeedMode::Never, false) ==
              SpectrumMixSeedDecision::Skip);
    }

    SECTION("disabled existing + Yes → keep disabled")
    {
        MixedFilament disabled;
        disabled.component_a = 1;
        disabled.component_b = 2;
        disabled.ratio_a     = 1;
        disabled.ratio_b     = 1;
        disabled.enabled     = false;
        const auto out = spectrum_ofd_mix_seed_apply({disabled}, 4, true, SpectrumMixSeedMode::Ask);
        REQUIRE(out.size() > 1);
        REQUIRE_FALSE(out.front().enabled);
        REQUIRE(spectrum_cookbook_same_recipe(out.front(), disabled));
        CHECK(spectrum_ofd_mix_seed_decision({disabled}, four_hex, SpectrumMixSeedMode::Ask, false) ==
              SpectrumMixSeedDecision::Prompt);

        MixedFilamentManager roundtrip;
        const std::string serialized = spectrum_ofd_serialize_mix_rows(out);
        REQUIRE_THAT(serialized, Catch::Matchers::ContainsSubstring(",0,"));
        roundtrip.load_definitions(serialized, true);
        REQUIRE(roundtrip.mixed_filaments().size() == out.size());
        REQUIRE_FALSE(roundtrip.mixed_filaments().front().enabled);
    }

    SECTION("decision Prompt / Append / Skip")
    {
        CHECK(spectrum_ofd_mix_seed_decision({}, four_hex, SpectrumMixSeedMode::Ask, false) ==
              SpectrumMixSeedDecision::Prompt);
        CHECK(spectrum_ofd_mix_seed_decision({}, four_hex, SpectrumMixSeedMode::Always, false) ==
              SpectrumMixSeedDecision::Append);
        CHECK(spectrum_ofd_mix_seed_decision({}, four_hex, SpectrumMixSeedMode::Ask, true) ==
              SpectrumMixSeedDecision::Skip);

        std::vector<std::string> missing = four_hex;
        missing[2].clear();
        CHECK(spectrum_ofd_mix_seed_decision({}, missing, SpectrumMixSeedMode::Ask, false) ==
              SpectrumMixSeedDecision::Skip);

        MixedFilament enabled;
        enabled.enabled = true;
        CHECK(spectrum_ofd_mix_seed_decision({enabled}, four_hex, SpectrumMixSeedMode::Ask, false) ==
              SpectrumMixSeedDecision::Skip);
        CHECK(spectrum_ofd_mix_seed_decision({enabled}, four_hex, SpectrumMixSeedMode::Always, false) ==
              SpectrumMixSeedDecision::Skip);
    }
}

TEST_CASE("ofd lookup from parse string with no network", "[spectrum_ofd]")
{
    CHECK(spectrum_ofd_parse("").empty());
    CHECK(spectrum_ofd_parse("not json {{{").empty());

    const auto ndjson = spectrum_ofd_parse(
        "{\"brand_name\":\"Acme\",\"filament_name\":\"PLA\",\"name\":\"Red\",\"color_hex\":\"#ff0000\"}\n"
        "{bad line}\n"
        "{\"brand\":{\"name\":\"NestedCo\"},\"filament\":{\"name\":\"PETG\"},\"variant\":{\"name\":\"Blue\"},"
        "\"color_hex\":\"#0000FF\"}\n");
    REQUIRE(ndjson.size() == 2);
    CHECK(ndjson[0].brand == "Acme");
    CHECK(ndjson[0].filament == "PLA");
    CHECK(ndjson[0].variant == "Red");
    CHECK(spectrum_ofd_slot_hex(ndjson[0]) == "#FF0000");
    CHECK(ndjson[1].brand == "NestedCo");
    CHECK(ndjson[1].filament == "PETG");
    CHECK(ndjson[1].variant == "Blue");

    const auto hit = spectrum_ofd_lookup(ndjson, "", "red");
    REQUIRE(hit.size() == 1);
    CHECK(spectrum_ofd_slot_hex(hit[0]) == "#FF0000");

    CHECK(spectrum_ofd_load_catalog("/no/such/ofd_seed.json", "/no/such/ofd_all.ndjson").empty());
    CHECK(spectrum_mix_seed_mode_from_string("") == SpectrumMixSeedMode::Ask);
    CHECK(spectrum_mix_seed_mode_from_string("ALWAYS") == SpectrumMixSeedMode::Always);
    CHECK(spectrum_mix_seed_mode_from_string("never") == SpectrumMixSeedMode::Never);
}
