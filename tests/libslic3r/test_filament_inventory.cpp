#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/FilamentInventory.hpp"
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
using namespace Slic3r;
using json = nlohmann::json;

TEST_CASE("Inventory round-trips every slot field, next_id and the changer dialect", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.slots.resize(3);
    inv.slots[0] = PhysicalFilament{1, "#FF0000", "PLA", "Snapmaker PLA SnapSpeed @U1"};
    inv.slots[0].name = "lane1"; inv.slots[0].unit = "Turtle_1"; inv.slots[0].unit_label = "Left Turtle";
    inv.slots[0].head = "extruder1"; inv.slots[0].slot = 3; inv.slots[0].extruder = 2; inv.slots[0].virtual_tool = 5;
    inv.slots[1] = PhysicalFilament{2, "#FFFFFF", "PETG"};
    inv.next_id = 6;
    inv.dialect = "afc";
    const FilamentInventory back = FilamentInventory::deserialize(inv.serialize(), 1);
    REQUIRE(back.slots.size() == 3);
    CHECK(back.next_id == 6);
    CHECK(back.dialect == "afc");
    const PhysicalFilament& s0 = back.slots[0];
    CHECK(s0.id == 1);
    CHECK(s0.color == "#FF0000");
    CHECK(s0.type == "PLA");
    CHECK(s0.preset == "Snapmaker PLA SnapSpeed @U1");
    CHECK(s0.name == "lane1");
    CHECK(s0.unit == "Turtle_1");
    CHECK(s0.unit_label == "Left Turtle");
    CHECK(s0.head == "extruder1");
    CHECK(s0.slot == 3);
    CHECK(s0.extruder == 2);
    CHECK(s0.virtual_tool == 5);
    CHECK(back.slots[1].id == 2);
    CHECK(back.slots[1].type == "PETG");
    CHECK(back.slots[1].name.empty());
    CHECK(back.slots[1].unit.empty());
    CHECK(back.slots[2].empty());
}

TEST_CASE("An inventory written before the newer fields existed reads them as unset", "[FilamentInventory]") {
    const FilamentInventory old = FilamentInventory::deserialize(R"({"next_id":2,"tools":[[{"id":1,"color":"#FF0000","type":"PLA","kind":"manual"}]]})", 1);
    CHECK(old.slots[0].id == 1);
    CHECK(old.slots[0].preset.empty());
    CHECK(old.slots[0].name.empty());
    CHECK(old.dialect.empty());
}

TEST_CASE("A per-tool inventory record flattens to its loaded slots", "[FilamentInventory]") {
    const std::string v2 = R"({"next_id":4,"tools":[[{"id":1,"color":"#FF0000","type":"PLA","preset":"Generic PLA","kind":"manual"},
        {"id":3,"color":"#0000FF","type":"ABS","preset":"","kind":"manual"}],[{"id":2,"color":"#00FF00","type":"PETG","preset":"","kind":"manual","name":"e2"}]]})";
    const FilamentInventory inv = FilamentInventory::deserialize(v2, 4);
    REQUIRE(inv.slots.size() == 4); // padded to the fallback count
    CHECK(inv.slots[0].id == 1);
    CHECK(inv.slots[0].extruder == 0);
    CHECK(inv.slots[1].name == "e2");
    CHECK(inv.slots[1].extruder == 1);
    CHECK(inv.slots[3].empty());
    CHECK(inv.find(3) == nullptr);
}

TEST_CASE("Deserialize dedupes colliding v2 ids and floors next_id above the max", "[FilamentInventory]") {
    std::string v2 = R"({"next_id":2,"tools":[[{"id":1,"color":"#FF0000","type":"PLA"}],[{"id":1,"color":"#FFFFFF","type":"PETG"}]]})";
    FilamentInventory inv = FilamentInventory::deserialize(v2, 2);
    REQUIRE(inv.slots[0].id == 1);
    REQUIRE(inv.slots[1].id != 1);
    REQUIRE(inv.slots[1].id > 0);
    REQUIRE(inv.slots[0].color == "#FF0000");   // data preserved, not dropped
    REQUIRE(inv.slots[1].color == "#FFFFFF");
    REQUIRE(inv.next_id > inv.slots[1].id);
    REQUIRE(inv.next_id > 1);
}

TEST_CASE("Deserialize clamps a stale next_id above every id already in use", "[FilamentInventory]") {
    std::string v2 = R"({"next_id":1,"tools":[[{"id":7,"color":"#FF0000","type":"PLA"}]]})";
    FilamentInventory inv = FilamentInventory::deserialize(v2, 1);
    REQUIRE(inv.slots[0].id == 7);
    REQUIRE(inv.next_id > 7);
}

TEST_CASE("Deserialize assigns a fresh id to a non-empty v2 slot missing its id", "[FilamentInventory]") {
    std::string v2 = R"({"next_id":1,"tools":[[{"color":"#FF0000","type":"PLA"}]]})";
    FilamentInventory inv = FilamentInventory::deserialize(v2, 1);
    REQUIRE(inv.slots[0].id > 0);
    REQUIRE(inv.next_id > inv.slots[0].id);
}

TEST_CASE("Deserialize falls back to empty slots for junk or an object without slots", "[FilamentInventory]") {
    FilamentInventory inv = FilamentInventory::deserialize(GENERATE(as<std::string>{}, "not json{", R"({"next_id":5})"), 3);
    REQUIRE(inv.slots.size() == 3);
    for (const auto& pf : inv.slots)
        REQUIRE(pf.empty());
    REQUIRE(inv.next_id == 1);
}

TEST_CASE("find looks up physical filaments by id", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}, {5, "#FFFF00", "PLA"}, {2, "#FFFFFF", "PETG"}};
    const PhysicalFilament* pf = inv.find(5);
    REQUIRE(pf != nullptr);
    REQUIRE(pf->color == "#FFFF00");
    REQUIRE(inv.find(99) == nullptr);
    REQUIRE(inv.find(0) == nullptr);
}

TEST_CASE("ensure_ids mints ids for non-empty slots only and floors next_id", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.slots = {{0, "#FF0000", "PLA"}, {7, "#00FF00", "PETG"}, {0, "#0000FF", "PLA"}, {0, "", ""}};
    inv.next_id = 3; // stale: below the highest id in use (7)

    inv.ensure_ids();

    CHECK(inv.slots[1].id == 7);
    CHECK(inv.slots[0].id == 8);
    CHECK(inv.slots[2].id == 9);
    CHECK(inv.slots[3].id == 0);
    REQUIRE(inv.next_id == 10);
}

TEST_CASE("ensure_ids zeroes a cleared slot's stale id", "[FilamentInventory]") {
    // find() ignores empty(), so a stale id would keep resolving to the cleared slot.
    FilamentInventory inv;
    inv.slots = {{7, "", ""}};
    inv.next_id = 8;

    inv.ensure_ids();

    CHECK(inv.slots[0].id == 0);
    CHECK(inv.find(7) == nullptr);
}

TEST_CASE("Ids survive a save/load round-trip when ensure_ids ran before saving", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.slots = {{0, "#E01B24", "PLA"}, {0, "#FFFFFF", "PLA"}, {0, "#33D17A", "PLA"}, {1, "#9141AC", ""}};
    inv.next_id = 2;

    inv.ensure_ids();
    FilamentInventory reloaded = FilamentInventory::deserialize(inv.serialize(), inv.slots.size());

    for (size_t t = 0; t < inv.slots.size(); ++t)
        CHECK(reloaded.slots[t].id == inv.slots[t].id);
    REQUIRE(reloaded.next_id == inv.next_id);
}

TEST_CASE("A slot with only a preset recorded is not empty", "[FilamentInventory]") {
    PhysicalFilament pf;
    pf.preset = "Generic PLA";
    REQUIRE_FALSE(pf.empty());
}

TEST_CASE("FilamentInventories round-trips inventories keyed by printer preset", "[FilamentInventory]") {
    FilamentInventories store;
    FilamentInventory& inv = store.for_preset("Snapmaker U1 (0.4 nozzle)", 4);
    inv.slots[0] = {1, "#FF0000", "PLA"};
    inv.slots[0].preset = "Snapmaker PLA SnapSpeed @U1";
    inv.next_id = 2;

    FilamentInventories back = FilamentInventories::deserialize(store.serialize());
    REQUIRE(back.by_preset.count("Snapmaker U1 (0.4 nozzle)") == 1);
    const FilamentInventory& back_inv = back.by_preset.at("Snapmaker U1 (0.4 nozzle)");
    CHECK(back_inv.slots[0].preset == "Snapmaker PLA SnapSpeed @U1");
    CHECK(back_inv.slots[0].color == "#FF0000");
    CHECK_FALSE(back.parse_error);
}

TEST_CASE("FilamentInventories::serialize writes the versioned envelope", "[FilamentInventory]") {
    FilamentInventories store;
    store.for_preset("My Printer", 1).slots[0] = {1, "#FF0000", "PLA"};
    json j = json::parse(store.serialize());
    REQUIRE(j["version"] == 1);
    REQUIRE(j["presets"].is_object());
    REQUIRE(j["presets"].contains("My Printer"));
}

TEST_CASE("FilamentInventories::deserialize loads a legacy bare-object store", "[FilamentInventory]") {
    std::string legacy = R"({"My Printer":{"next_id":2,"tools":[[{"id":1,"color":"#FF0000","type":"PLA"}]]}})";
    FilamentInventories store = FilamentInventories::deserialize(legacy);
    CHECK_FALSE(store.parse_error);
    REQUIRE(store.by_preset.count("My Printer") == 1);
    CHECK(store.by_preset.at("My Printer").slots[0].color == "#FF0000");
}

TEST_CASE("FilamentInventories::deserialize loads a legacy store with presets literally named version/presets", "[FilamentInventory]") {
    // Legacy values are objects, so j["version"] is never an integer here.
    std::string legacy = R"({
        "version": {"next_id":2,"tools":[[{"id":1,"color":"#FF0000","type":"PLA"}]]},
        "presets": {"next_id":3,"tools":[[{"id":2,"color":"#00FF00","type":"PETG"}]]}
    })";
    FilamentInventories store = FilamentInventories::deserialize(legacy);
    CHECK_FALSE(store.parse_error);
    REQUIRE(store.by_preset.count("version") == 1);
    REQUIRE(store.by_preset.count("presets") == 1);
    CHECK(store.by_preset.at("version").slots[0].color == "#FF0000");
    CHECK(store.by_preset.at("presets").slots[0].color == "#00FF00");
}

TEST_CASE("FilamentInventories::deserialize reports a parse failure without fabricating data", "[FilamentInventory]") {
    FilamentInventories junk = FilamentInventories::deserialize("not json{");
    CHECK(junk.parse_error);
    CHECK(junk.by_preset.empty());

    FilamentInventories not_object = FilamentInventories::deserialize("[1,2,3]");
    CHECK(not_object.parse_error);
    CHECK(not_object.by_preset.empty());
}

TEST_CASE("for_preset creates an empty, padded inventory on first use and pads without truncating later", "[FilamentInventory]") {
    FilamentInventories store;
    FilamentInventory& inv = store.for_preset("Brand New Printer", 3);
    REQUIRE(inv.slots.size() == 3);
    for (const auto& pf : inv.slots)
        CHECK(pf.empty());
    inv.slots[0] = {1, "#0000FF", "ABS"};

    CHECK(store.for_preset("Brand New Printer", 4).slots.size() == 4);
    CHECK(store.for_preset("Brand New Printer", 2).slots.size() == 4);
    CHECK(store.by_preset.at("Brand New Printer").slots[0].color == "#0000FF");
    CHECK(store.by_preset.size() == 1);
}

TEST_CASE("A synced-then-cleared tool round-trips as canonically empty after ensure_ids", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.slots = {{0, "", ""}};

    PhysicalFilament loaded;
    loaded.color = "#AABBCC";
    loaded.type  = "PLA";
    inv.slots[0] = loaded;
    inv.ensure_ids();
    int minted_id = inv.slots[0].id;
    REQUIRE(minted_id > 0);

    inv.slots[0] = PhysicalFilament{}; // printer now reports the tray empty
    inv.ensure_ids();
    CHECK(inv.slots[0].empty());
    CHECK(inv.slots[0].id == 0);
    CHECK(inv.find(minted_id) == nullptr);
}

TEST_CASE("derive_filament_subtype strips vendor and type tokens from a display name", "[FilamentInventory]") {
    CHECK(derive_filament_subtype("Snapmaker PLA SnapSpeed", "Snapmaker", "PLA") == "SnapSpeed");
    CHECK(derive_filament_subtype("PolyTerra PLA", "Polymaker", "PLA") == "PolyTerra");
    CHECK(derive_filament_subtype("Generic PLA", "Generic", "PLA") == "");
    CHECK(derive_filament_subtype("Snapmaker PLA Matte", "Snapmaker", "PLA") == "Matte");
    // Case-insensitive token matching; remainder keeps its original casing and order.
    CHECK(derive_filament_subtype("SNAPMAKER pla SnapSpeed", "Snapmaker", "PLA") == "SnapSpeed");
    // A dashed type variant is one token: not stripped by its base type.
    CHECK(derive_filament_subtype("Generic PLA-CF", "Generic", "PLA-CF") == "");
    CHECK(derive_filament_subtype("", "Generic", "PLA") == "");
}
