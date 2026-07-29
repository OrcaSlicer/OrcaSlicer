#include <catch2/catch_all.hpp>

#include "libslic3r/MaterialType.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;

TEST_CASE("Every material type name is unique", "[MaterialType]") {
    // find() returns the first match, so a duplicated name silently shadows the later entry and makes
    // the data ambiguous. Guard the whole table (this would have caught the duplicated "BVOH").
    std::set<std::string> seen;
    for (const MaterialTypeInfo& info : MaterialType::all()) {
        INFO("duplicate material type: " << info.name);
        CHECK(seen.insert(info.name).second);
    }
}

TEST_CASE("Every material type has a sane temperature range", "[MaterialType]") {
    for (const MaterialTypeInfo& info : MaterialType::all()) {
        INFO("material type: " << info.name);
        CHECK(info.min_temp <= info.max_temp);
        CHECK(info.chamber_min_temp <= info.chamber_max_temp);
    }
}

TEST_CASE("find() returns the matching material or nullptr", "[MaterialType]") {
    const MaterialTypeInfo* pla = MaterialType::find("PLA");
    REQUIRE(pla != nullptr);
    CHECK(pla->name == "PLA");

    CHECK(MaterialType::find("Unobtanium") == nullptr);
}

TEST_CASE("get_temperature_range() reports known types and falls back for unknown ones", "[MaterialType]") {
    SECTION("known type returns its configured range") {
        int min_temp = 0, max_temp = 0;
        REQUIRE(MaterialType::get_temperature_range("PLA", min_temp, max_temp));
        CHECK(min_temp == 180);
        CHECK(max_temp == 240);
    }

    SECTION("unknown type returns false and leaves generic defaults") {
        int min_temp = 0, max_temp = 0;
        REQUIRE_FALSE(MaterialType::get_temperature_range("Unobtanium", min_temp, max_temp));
        // The generic fallback is still written to the out-params.
        CHECK(min_temp <= max_temp);
    }
}

TEST_CASE("base_materials() resolves variants to their family and falls back to the type", "[MaterialType]") {
    // A carbon-fibre variant reports its base family.
    CHECK(MaterialType::base_materials("PLA-CF") == std::vector<std::string>{"PLA"});
    // A blend reports every family it belongs to.
    CHECK(MaterialType::base_materials("PC-ABS") == std::vector<std::string>{"PC", "ABS"});
    // A plain type with no configured family falls back to itself.
    CHECK(MaterialType::base_materials("PLA") == std::vector<std::string>{"PLA"});
    // An unknown type also falls back to itself.
    CHECK(MaterialType::base_materials("Unobtanium") == std::vector<std::string>{"Unobtanium"});
}

TEST_CASE("compatibility() resolves adhesion at the base-material level", "[MaterialType]") {
    using MC = MaterialCompatibility;

    SECTION("a material always bonds with itself") {
        CHECK(MaterialType::compatibility("PLA", "PLA") == MC::Compatible);
        // Even a soluble material (incompatible with everything) still bonds with itself.
        CHECK(MaterialType::compatibility("PVA", "PVA") == MC::Compatible);
    }

    SECTION("a shared base family is compatible") {
        // ASA and ABS-CF both resolve to the ABS family.
        CHECK(MaterialType::compatibility("ASA", "ABS") == MC::Compatible);
        CHECK(MaterialType::compatibility("ABS-CF", "ASA-GF") == MC::Compatible);
    }

    SECTION("an explicitly listed cross-family bond is compatible") {
        CHECK(MaterialType::compatibility("ABS", "PC") == MC::Compatible);
    }

    SECTION("an explicit incompatibility wins") {
        CHECK(MaterialType::compatibility("PLA", "PETG") == MC::Incompatible);
        // Variants resolve to their family before the rule is applied.
        CHECK(MaterialType::compatibility("ABS-CF", "PLA") == MC::Incompatible);
        // A blend is incompatible when any of its families conflicts (ABS vs PLA here).
        CHECK(MaterialType::compatibility("PC-ABS", "PLA") == MC::Incompatible);
    }

    SECTION("soluble support materials bond with nothing else (\"*\" wildcard)") {
        CHECK(MaterialType::compatibility("PVA", "PLA") == MC::Incompatible);
        CHECK(MaterialType::compatibility("BVOH", "ABS") == MC::Incompatible);
        CHECK(MaterialType::compatibility("PP", "ABS") == MC::Incompatible);
    }

    SECTION("no rule either way is unknown") {
        CHECK(MaterialType::compatibility("PET", "TPU") == MC::Unknown);
    }
}

TEST_CASE("compatibility() is symmetric", "[MaterialType]") {
    const auto pairs = {
        std::make_pair("ABS", "PC"),
        std::make_pair("PLA", "PETG"),
        std::make_pair("PVA", "ABS"),
        std::make_pair("PET", "TPU"),
        std::make_pair("PC-ABS", "PLA"),
    };
    for (const auto& [a, b] : pairs) {
        INFO("pair: " << a << " / " << b);
        CHECK(MaterialType::compatibility(a, b) == MaterialType::compatibility(b, a));
    }
}
