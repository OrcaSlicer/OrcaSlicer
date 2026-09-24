#include <catch2/catch_all.hpp>

#include "libslic3r/MaterialType.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {
// The tables the database starts from, captured before main() runs - that is, before the test bootstrap
// replaces them with the ones shipped in resources/info. The drift test below compares the two.
struct BuiltinTables
{
    std::vector<MaterialTypeInfo>          types;
    std::vector<BaseMaterialCompatibility> base_compatibilities;
};

const BuiltinTables g_builtin_tables{MaterialType::all(), MaterialType::base_compatibilities()};

boost::filesystem::path data_file(const std::string& filename)
{
    return boost::filesystem::path(RESOURCES_DIR) / "info" / filename;
}
} // namespace

TEST_CASE("The shipped material data files are versioned and loadable", "[MaterialType]") {
    // The version is what tells an installation whether its copy of a table needs refreshing, so an
    // unparseable or missing one (which reads as 0.0.0) would silently freeze the data.
    for (const std::string& filename : {"material_types.json", "base_compatibilities.json"}) {
        INFO("data file: " << filename);
        REQUIRE(boost::filesystem::exists(data_file(filename)));
        CHECK(Semver::zero() < get_version_from_json(data_file(filename).string()));
    }

    // The shipped table is the full one, the built-in fallback only covers the common materials.
    CHECK(MaterialType::all().size() > g_builtin_tables.types.size());
    CHECK(MaterialType::find("PLA-CF") != nullptr);
}

TEST_CASE("The built-in fallback matches the shipped material data", "[MaterialType]") {
    // The fallback duplicates a handful of entries from the JSON; check they still agree, so the two
    // cannot drift apart when a material is edited.
    for (const MaterialTypeInfo& fallback : g_builtin_tables.types) {
        INFO("material type: " << fallback.name);
        const MaterialTypeInfo* shipped = MaterialType::find(fallback.name);
        REQUIRE(shipped != nullptr);
        CHECK(shipped->min_temp == fallback.min_temp);
        CHECK(shipped->max_temp == fallback.max_temp);
        CHECK(shipped->chamber_min_temp == fallback.chamber_min_temp);
        CHECK(shipped->chamber_max_temp == fallback.chamber_max_temp);
        CHECK_THAT(shipped->adhesion_coefficient, Catch::Matchers::WithinRel(fallback.adhesion_coefficient));
        CHECK_THAT(shipped->yield_strength, Catch::Matchers::WithinRel(fallback.yield_strength));
        CHECK_THAT(shipped->thermal_length, Catch::Matchers::WithinRel(fallback.thermal_length));
        CHECK(shipped->base_materials == fallback.base_materials);
    }

    // The adhesion rules are short enough that the fallback carries all of them.
    const std::vector<BaseMaterialCompatibility>& shipped = MaterialType::base_compatibilities();
    REQUIRE(shipped.size() == g_builtin_tables.base_compatibilities.size());
    for (size_t i = 0; i < shipped.size(); ++ i) {
        INFO("base material: " << g_builtin_tables.base_compatibilities[i].base_material);
        CHECK(shipped[i].base_material == g_builtin_tables.base_compatibilities[i].base_material);
        CHECK(shipped[i].compatible == g_builtin_tables.base_compatibilities[i].compatible);
        CHECK(shipped[i].incompatible == g_builtin_tables.base_compatibilities[i].incompatible);
    }
}

TEST_CASE("The filament type option lists every material type", "[MaterialType]") {
    // The value list is built during static initialisation, before the tables can be read from disk, so
    // it only matches the shipped data because refresh_material_type_config_defs() re-reads it after.
    const ConfigOptionDef* def = print_config_def.get("filament_type");
    REQUIRE(def != nullptr);

    std::vector<std::string> names;
    for (const MaterialTypeInfo& info : MaterialType::all())
        names.push_back(info.name);
    CHECK(def->enum_values == names);
}

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
