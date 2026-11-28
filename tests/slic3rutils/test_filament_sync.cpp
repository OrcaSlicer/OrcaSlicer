#ifdef _WIN32
// DeviceManager.hpp includes COM headers whose byte typedef must precede std::byte.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "slic3r/Utils/FilamentSync.hpp"

#include "filament_test_collection.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include <wx/string.h>

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Slic3r;
using FilamentSync::AmsSlot;
using FilamentSync::SpoolIdentity;
using filament_test::ecc2_filaments;
using filament_test::FilamentTestCollection;
using filament_test::populate;
using filament_test::ScopedLatinCtype;

TEST_CASE("A six digit hex colour becomes an opaque upper case RGBA value", "[FilamentSync]")
{
    CHECK(FilamentSync::parse_hex_rgb("#000000") == std::optional<std::string>("000000FF"));
    CHECK(FilamentSync::parse_hex_rgb("#44f1ff") == std::optional<std::string>("44F1FFFF"));
}

TEST_CASE("Anything but #RRGGBB is not a colour", "[FilamentSync]")
{
    const std::string bad = GENERATE("", "#", "000000", "#00000", "#0000000", "#GG0000", " #000000", "0x000000", "#000000FF");
    CHECK_FALSE(FilamentSync::parse_hex_rgb(bad).has_value());
}

TEST_CASE("Four slots of one unit produce the AMS document ParseV1_0 reads", "[FilamentSync]")
{
    const auto doc  = FilamentSync::build_ams_print_json({{AmsSlot{true, "PLA", "000000FF", "OFkhhUS0", 230}, {}, {}, {}}});
    const auto& ams = doc.at("ams");
    CHECK(ams.at("ams_exist_bits") == "1");
    CHECK(ams.at("tray_exist_bits") == "1");
    REQUIRE(ams.at("ams").size() == 1);
    const auto& unit = ams.at("ams").at(0);
    CHECK(unit.at("id") == "0");
    CHECK(unit.at("info") == "0002");
    CHECK_FALSE(unit.contains("display_name"));
    const auto& trays = unit.at("tray");
    REQUIRE(trays.size() == 4);
    const auto& loaded = trays.at(0);
    CHECK(loaded.at("tray_info_idx") == "OFkhhUS0");
    CHECK(loaded.at("tray_type") == "PLA");
    CHECK(loaded.at("tray_color") == "000000FF");
    CHECK(loaded.at("cols") == nlohmann::json::array({"000000FF"}));
    CHECK(loaded.at("ctype") == 1);
    CHECK(loaded.at("nozzle_temp_max") == "230");
    CHECK_FALSE(loaded.contains("tray_slot_placeholder"));
    for (std::size_t i = 0; i < trays.size(); ++i) {
        CHECK(trays.at(i).at("id") == std::to_string(i));
        CHECK(trays.at(i).at("tag_uid") == "0000000000000000");
    }
    const auto& empty = trays.at(3);
    CHECK_FALSE(empty.contains("tray_slot_placeholder"));
    CHECK(empty.at("tray_color") == "00000000");
    CHECK(empty.at("tray_info_idx") == "");
    CHECK(empty.at("tray_type") == "");
    CHECK_FALSE(empty.contains("cols"));
    CHECK_FALSE(empty.contains("ctype"));
    CHECK_FALSE(empty.contains("nozzle_temp_max"));
}

TEST_CASE("A present slot without a filament id is listed without a placeholder", "[FilamentSync]")
{
    const std::string color          = GENERATE("", "336699FF");
    const std::string expected_color = color.empty() ? "00000000" : color;
    const auto doc                   = FilamentSync::build_ams_print_json({{AmsSlot{true, "PETG", color, "", 0}, {}, {}, {}}});
    CHECK(doc.at("ams").at("tray_exist_bits") == "1");
    const auto& tray = doc.at("ams").at("ams").at(0).at("tray").at(0);
    CHECK_FALSE(tray.contains("tray_slot_placeholder"));
    CHECK(tray.at("tray_info_idx") == "");
    CHECK(tray.at("tray_type") == "PETG");
    CHECK(tray.at("tray_color") == expected_color);
    CHECK(tray.at("cols") == nlohmann::json::array({expected_color}));
    CHECK(tray.at("ctype") == 1);
    CHECK_FALSE(tray.contains("nozzle_temp_max"));
}

TEST_CASE("Slots of a second unit set bits four to seven", "[FilamentSync]")
{
    std::array<AmsSlot, 4> first{};
    std::array<AmsSlot, 4> second{};
    second[2]      = {true, "PETG", "FFFFFFFF", "X", 0};
    const auto doc = FilamentSync::build_ams_print_json({first, second});
    CHECK(doc.at("ams").at("ams_exist_bits") == "3");
    CHECK(doc.at("ams").at("tray_exist_bits") == "40");
    CHECK(doc.at("ams").at("ams").at(1).at("id") == "1");
}

TEST_CASE("Seven units fill the masks ParseV1_0 reads and an eighth is rejected", "[FilamentSync]")
{
    std::array<AmsSlot, 4> full{};
    full.fill(AmsSlot{true, "PLA", "000000FF", "X", 0});
    const auto doc = FilamentSync::build_ams_print_json(std::vector<std::array<AmsSlot, 4>>(7, full));
    CHECK(doc.at("ams").at("ams_exist_bits") == "7F");
    CHECK(doc.at("ams").at("tray_exist_bits") == "FFFFFFF");
    CHECK_THROWS_AS(FilamentSync::build_ams_print_json(std::vector<std::array<AmsSlot, 4>>(8, full)), std::invalid_argument);
}

TEST_CASE("A single named AMS unit carries its display name", "[FilamentSync]")
{
    const auto doc = FilamentSync::build_ams_print_json({{}}, "CANVAS");
    CHECK(doc.at("ams").at("ams").at(0).at("display_name") == "CANVAS");
}

TEST_CASE("Multiple named AMS units carry consecutive display names starting at one", "[FilamentSync]")
{
    const auto doc    = FilamentSync::build_ams_print_json({{}, {}}, "CANVAS");
    const auto& units = doc.at("ams").at("ams");
    CHECK(units.at(0).at("display_name") == "CANVAS 1");
    CHECK(units.at(1).at("display_name") == "CANVAS 2");
}

TEST_CASE("ParseV1_0 reads four trays and their filament presence from the AMS document", "[FilamentSync]")
{
    AppConfig config;
    DeviceManager manager(nullptr, false, &config);
    BBLocalMachine machine;
    machine.dev_id     = "filament-sync-device";
    MachineObject* obj = manager.insert_local_device(machine, "lan", "free", "", "");
    REQUIRE(obj != nullptr);

    std::array<AmsSlot, 4> slots{};
    slots[1] = {true, "PETG", "336699FF", "", 0};
    slots[2] = {true, "PLA", "000000FF", "OFkhhUS0", 230};
    auto doc = FilamentSync::build_ams_print_json({slots}, "CANVAS");
    // ParseV1_0 resolves these fields through wxGetApp().preset_bundle, even for empty trays.
    for (auto& tray : doc.at("ams").at("ams").at(0).at("tray")) {
        tray.erase("tray_info_idx");
        tray.erase("tray_type");
    }
    const auto system = obj->GetFilaSystem();
    DevFilaSystemParser::ParseV1_0(doc, obj, system.get(), false);
    REQUIRE(system->HasAms());
    const auto* unit = system->GetAmsById("0");
    REQUIRE(unit != nullptr);
    CHECK(unit->GetDisplayName() == "CANVAS");
    CHECK(unit->GetTrays().size() == 4);
    const auto* loaded = unit->GetTray("2");
    REQUIRE(loaded != nullptr);
    CHECK(loaded->is_exists);
    CHECK_FALSE(loaded->is_slot_placeholder);
    const auto* unrecognised = unit->GetTray("1");
    REQUIRE(unrecognised != nullptr);
    CHECK(unrecognised->is_exists);
    CHECK_FALSE(unrecognised->is_slot_placeholder);
    const auto* empty = unit->GetTray("0");
    REQUIRE(empty != nullptr);
    CHECK_FALSE(empty->is_exists);
    CHECK_FALSE(empty->is_slot_placeholder);
}

TEST_CASE("An Elegoo Rapid PLA+ spool matches its ECC2 preset", "[FilamentSync]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);
    CHECK(FilamentSync::match_filament_preset(filaments, {"ELEGOO", "RAPID PLA+", "PLA"}) == "OFkhhUS0");
}

TEST_CASE("Preset matching folds the case of ASCII letters only, whatever the locale", "[FilamentSync]")
{
    const ScopedLatinCtype latin;
    FilamentTestCollection filaments;
    // Latin-1 e-acute and E-acute: the locale folds one onto the other, ASCII folding does not.
    const std::string lower(1, '\xE9');
    const std::string upper(1, '\xC9');
    const std::string eclat = "Elegoo " + lower + "clat PLA @ECC2";
    auto specs              = ecc2_filaments;
    specs.push_back({eclat.c_str(), "ECLAT", "PLA", "Elegoo", true});
    populate(filaments, specs);
    CHECK(FilamentSync::match_filament_preset(filaments, {"ELEGOO", "RAPID PLA+", "PLA"}) == "OFkhhUS0");
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", lower + "clat PLA", "PLA"}) == "ECLAT");
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", upper + "CLAT PLA", "PLA"}) == "OFvgE0Zh");
}

TEST_CASE("A plus in the product name keeps PLA+ apart from PLA", "[FilamentSync]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "PLA+", "PLA"}) == "OFBUIlZ7");
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "PLA", "PLA"}) == "OFvgE0Zh");
}

TEST_CASE("A multi word product matches regardless of case", "[FilamentSync]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);
    CHECK(FilamentSync::match_filament_preset(filaments, {"elegoo", "pla silk", "pla"}) == "SILK");
}

TEST_CASE("A vendor spool without a known product falls back to the vendor's base product", "[FilamentSync]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "Mystery PLA", "PLA"}) == "OFvgE0Zh");
}

TEST_CASE("An unknown vendor falls back to the collection's lookup by type", "[FilamentSync]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);
    const std::string by_type = filaments.filament_id_by_type("PLA");
    REQUIRE_FALSE(by_type.empty());
    CHECK(FilamentSync::match_filament_preset(filaments, {"Polymaker", "PolyTerra PLA", "PLA"}) == by_type);
}

TEST_CASE("A spool without a type matches no preset", "[FilamentSync]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "RAPID PLA+", ""}) == "");
}

TEST_CASE("A type with no visible preset matches no preset", "[FilamentSync]")
{
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "PC", "PC"}) == "");
}

TEST_CASE("A system preset beats a user copy with the same score", "[FilamentSync]")
{
    auto specs = ecc2_filaments;
    specs.push_back({"Elegoo Rapid PLA+ @ECC2 (mine)", "OFkhhUS0-user", "PLA", "Elegoo", false});
    // Sorts before the system preset, so only the system rule can pick the system one.
    specs.push_back({"Elegoo Rapid PLA+ @0 (mine)", "OFkhhUS0-early", "PLA", "Elegoo", false});
    FilamentTestCollection filaments;
    populate(filaments, specs);
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "Rapid PLA+", "PLA"}) == "OFkhhUS0");
}

TEST_CASE("A preset without a filament id is never matched", "[FilamentSync]")
{
    auto specs = ecc2_filaments;
    // Same words and also a system preset, and its name sorts first, so only the missing id can rule it out.
    specs.push_back({"Elegoo Rapid PLA+ @A", "", "PLA", "Elegoo", true});
    FilamentTestCollection filaments;
    populate(filaments, specs);
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "Rapid PLA+", "PLA"}) == "OFkhhUS0");
}

TEST_CASE("Hidden or incompatible presets are never matched", "[FilamentSync]")
{
    const bool hide = GENERATE(true, false);
    FilamentTestCollection filaments;
    populate(filaments, ecc2_filaments);
    Preset* rapid = filaments.find_preset("Elegoo Rapid PLA+ @ECC2");
    REQUIRE(rapid != nullptr);
    (hide ? rapid->is_visible : rapid->is_compatible) = false;
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "Rapid PLA+", "PLA"}) != "OFkhhUS0");
}

TEST_CASE("A preset without a filament vendor takes the vendor from its profile", "[FilamentSync]")
{
    FilamentTestCollection filaments;
    // Generic PLA is the type fallback, so only the profile vendor can pick Rapid PLA+.
    populate(filaments, {{"Rapid PLA+ @ECC2", "OFkhhUS0", "PLA", "", true}, {"Generic PLA @System", "GENPLA", "PLA", "Generic", true}});
    VendorProfile elegoo;
    elegoo.name   = "Elegoo";
    Preset* rapid = filaments.find_preset("Rapid PLA+ @ECC2");
    REQUIRE(rapid != nullptr);
    rapid->vendor = &elegoo;
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "Rapid PLA+", "PLA"}) == "OFkhhUS0");
}

TEST_CASE("A preset name keeps the vendor words its filament vendor does not name", "[FilamentSync]")
{
    FilamentTestCollection filaments;
    populate(filaments, {{"Elegoo Rapid PLA+ @ECC2", "OFkhhUS0", "PLA", "", true}, {"Elegoo PLA @ECC2", "OFvgE0Zh", "PLA", "Elegoo", true}});
    VendorProfile elegoo;
    elegoo.name   = "Elegoo";
    Preset* rapid = filaments.find_preset("Elegoo Rapid PLA+ @ECC2");
    REQUIRE(rapid != nullptr);
    rapid->vendor = &elegoo;
    CHECK(FilamentSync::match_filament_preset(filaments, {"Elegoo", "Rapid PLA+", "PLA"}) == "OFvgE0Zh");
}

TEST_CASE("An unbranded spool matches an unbranded preset of the same product", "[FilamentSync]")
{
    const bool in_vendor_profile = GENERATE(false, true);
    FilamentTestCollection filaments;
    // Generic PLA is the type fallback, so only the exact product can pick Rapid PLA+.
    populate(filaments, {{"Rapid PLA+", "RAPID-USER", "PLA", "", false}, {"Generic PLA @System", "GENPLA", "PLA", "Generic", true}});
    VendorProfile elegoo;
    elegoo.name   = "Elegoo";
    Preset* rapid = filaments.find_preset("Rapid PLA+");
    REQUIRE(rapid != nullptr);
    rapid->vendor = in_vendor_profile ? &elegoo : nullptr;
    CHECK(FilamentSync::match_filament_preset(filaments, {"", "Rapid PLA+", "PLA"}) == "RAPID-USER");
}
