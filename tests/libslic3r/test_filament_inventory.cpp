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

TEST_CASE("Proposal auto-matches each plate filament to a physical id", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}, {2, "#00FF00", "PLA"}};
    std::vector<std::string> colors = {"#00FF00", "#FE0101"};
    std::vector<std::string> types  = {"PLA", "PLA"};
    std::vector<int> plate = {2, 1};
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, inv);
    REQUIRE(res == std::vector<int>{1, 2});
}

TEST_CASE("inventory_all_unset is true only when every slot on every tool is empty", "[FilamentInventory]") {
    FilamentInventory empty_inv; empty_inv.slots.resize(2);
    REQUIRE(inventory_all_unset(empty_inv));

    FilamentInventory loaded_inv; loaded_inv.slots = {{1, "#FF0000", "PLA"}, {}};
    REQUIRE_FALSE(inventory_all_unset(loaded_inv));
    // A slot with an id but nothing recorded is still unset.
    FilamentInventory id_only; id_only.slots = {{7, "", ""}};
    REQUIRE(inventory_all_unset(id_only));
}

TEST_CASE("Proposal leaves every row unassigned for an all-unset inventory", "[FilamentInventory]") {
    FilamentInventory inv; inv.slots.resize(2);
    std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    std::vector<std::string> types  = {"PLA", "PLA"};
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, {1, 2}, inv);
    REQUIRE(res == std::vector<int>{0, 0});
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

TEST_CASE("Auto-map matcher never selects an empty slot even when it is the only slot", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{-1});
}

TEST_CASE("Auto-map never family-matches an empty-type slot to a project filament with no type either", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", ""}}; // color but no type: not empty()
    std::vector<ProjectFilamentInfo> project = {{"", "#FF0000", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{-1});
}

TEST_CASE("Auto-map treats PLA and PLA+ as family-compatible", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA+"}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{1});
}

TEST_CASE("Auto-map picks the nearest-RGB family-compatible candidate", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}, {2, "#FE0101", "PLA"}, {3, "#000000", "PLA"}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FE0101", ""}}; // nearest is tool 1 (id 2), not the exact-but-farther tool 0
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{2});
}

TEST_CASE("Auto-map uses vendor to break an exact color tie", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}, {2, "#FF0000", "PLA"}};
    inv.slots[1].preset = "Snapmaker PLA SnapSpeed"; // vendor token "Snapmaker"
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "Snapmaker"}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{2});  // tool 1 wins on vendor despite identical color distance
}

TEST_CASE("Proposal uses vendor to break an exact color tie", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}, {2, "#FF0000", "PLA"}};
    inv.slots[0].preset = "Generic PLA";
    inv.slots[1].preset = "Snapmaker PLA SnapSpeed"; // vendor token "Snapmaker"
    std::vector<std::string> colors  = {"#FF0000"};
    std::vector<std::string> types   = {"PLA"};
    std::vector<std::string> vendors = {"Snapmaker"};
    std::vector<int>         plate   = {1};
    std::vector<int> res = compute_physical_map_proposal(colors, types, vendors, {}, plate, inv);
    REQUIRE(res.size() == 1);
    CHECK(res[0] == 2);
}

TEST_CASE("Auto-map trusts an exact preset match over a closer color match", "[FilamentInventory][AutoMap]") {
    // Slot 1 stays within the color cutoff (squared distance 3025).
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}, {2, "#C80000", "PLA"}}; // slot 0: exact color, no preset
    inv.slots[1].preset = "Snapmaker PLA SnapSpeed @U1";         // slot 1: exact preset
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed @U1"}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{2});
}

TEST_CASE("Auto-map's exact-preset tier ignores a type mismatch on the slot record", "[FilamentInventory][AutoMap]") {
    // A stale/miscategorized type on the slot must not veto an exact profile match.
    FilamentInventory inv;
    inv.slots = {{1, "", "PETG"}};
    inv.slots[0].preset = "Snapmaker PLA SnapSpeed @U1";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed @U1"}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{1});
}

TEST_CASE("Auto-map's exact-preset tier is color-aware for the U1 five-filament repro", "[FilamentInventory][AutoMap]") {
    // Five filaments share one preset in different colors. Only white is within the cutoff of a
    // same-preset slot (T4); every other candidate is over 10000 away, so the rest stay unmatched.
    FilamentInventory inv;
    inv.slots = {{1, "#0000FF", "PolyTerra PLA"},   // T1: no matching preset
                 {2, "#008080", "PLA"},             // T2: SnapSpeed preset, teal
                 {},                                // T3: empty
                 {4, "#FFFFFF", "PLA"}};            // T4: SnapSpeed preset, white
    inv.slots[1].preset = "Snapmaker PLA SnapSpeed";
    inv.slots[3].preset = "Snapmaker PLA SnapSpeed";
    std::vector<ProjectFilamentInfo> project = {
        {"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed"}, // red
        {"PLA", "#FFFFFF", "", "Snapmaker PLA SnapSpeed"}, // white
        {"PLA", "#000000", "", "Snapmaker PLA SnapSpeed"}, // black
        {"PLA", "#00FF00", "", "Snapmaker PLA SnapSpeed"}, // green
        {"PLA", "#D2B48C", "", "Snapmaker PLA SnapSpeed"}, // tan
    };
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{-1, 4, -1, -1, -1}); // white -> T4
}


TEST_CASE("Auto-map leaves a filament unmatched rather than propose a wildly different color", "[FilamentInventory][AutoMap]") {
    // Unmatched lets the caller prompt instead of committing a wrong color.
    FilamentInventory inv;
    inv.slots = {{1, "#0000FF", "PLA"}}; // the only PLA slot, and it's blue
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}}; // project filament is red
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{-1});
}

TEST_CASE("Proposal leaves a row unproposed rather than a wildly different color, through the dialog path", "[FilamentInventory]") {
    // Same cutoff through the dialog's proposal path.
    FilamentInventory inv;
    inv.slots = {{1, "#0000FF", "PLA"}};
    std::vector<std::string> colors = {"#FF0000"};
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, inv);
    REQUIRE(res == std::vector<int>{0});
}

TEST_CASE("Auto-map's color cutoff still lets an unknown-color slot qualify on type alone", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "", "PLA"}}; // type matches, color unrecorded
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{1});
}

TEST_CASE("Auto-map still matches a moderately different shade of the same color family", "[FilamentInventory][AutoMap]") {
    // #FF0000 vs #AB0000 is 7056, under the 10000 cutoff.
    FilamentInventory inv;
    inv.slots = {{1, "#AB0000", "PLA"}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{1});
}

// #FF0000 vs #9A0000 is 10201, just over the 10000 cutoff; fails if the cutoff is removed.
TEST_CASE("Auto-map's color cutoff disqualifies a shade just past the threshold", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "#9A0000", "PLA"}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{-1});
}

TEST_CASE("Auto-map's exact-preset tier falls through to unmatched when the color is a gross mismatch", "[FilamentInventory][AutoMap]") {
    // Beyond the cutoff a preset match falls through to the family/color tier, which rejects the
    // same slot.
    FilamentInventory inv;
    inv.slots = {{1, "#0000FF", "PLA"}};
    inv.slots[0].preset = "Snapmaker PLA SnapSpeed @U1";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed @U1"}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{-1});
}

TEST_CASE("Auto-map's exact-preset tier falls through to the family/color tier when beyond the cutoff and a family candidate is in range", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}, {2, "#0000FF", "PLA"}}; // tool 0: exact color; tool 1: exact preset, blue
    inv.slots[1].preset = "Snapmaker PLA SnapSpeed @U1";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed @U1"}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{1});  // tool 0 (family/color tier), not tool 1 (the disqualified preset match)
}

TEST_CASE("Auto-map's exact-preset tier breaks a tie between two preset matches by nearest color", "[FilamentInventory][AutoMap]") {
    // #FFFFFF is 97283 from teal and 675 from near-white, so slot 1 wins despite being scanned second.
    FilamentInventory inv;
    inv.slots = {{1, "#008080", "PLA"}, {2, "#F0F0F0", "PLA"}};
    inv.slots[0].preset = "Snapmaker PLA SnapSpeed";
    inv.slots[1].preset = "Snapmaker PLA SnapSpeed";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FFFFFF", "", "Snapmaker PLA SnapSpeed"}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{2});
}

TEST_CASE("Proposal matches a PLA+ project filament to a plain PLA slot", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}};
    std::vector<std::string> colors = {"#FF0000"};
    std::vector<std::string> types  = {"PLA+"};
    std::vector<int> plate = {1};
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, inv);
    REQUIRE(res == std::vector<int>{1});
}

TEST_CASE("Auto-map breaks a full tie (color and vendor) with the lowest tool index", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}, {2, "#FF0000", "PLA"}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{1});
}

TEST_CASE("Auto-map leaves only the filament with no compatible slot unresolved", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PETG"}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}, {"PETG", "#FF0000", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{-1, 1});
}

TEST_CASE("Auto-map on an empty inventory marks every filament unmatched", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots.resize(2); // no physical filaments recorded
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}, {"PETG", "#00FF00", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{-1, -1});
}

TEST_CASE("Auto-map allows two project filaments to merge onto the same tool", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.slots = {{1, "#FF0000", "PLA"}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}, {"PLA", "#FE0101", ""}};
    CHECK(auto_map_filaments(project, inv) == std::vector<int>{1, 1});
}

TEST_CASE("type_compatible matches exact types and small families only", "[FilamentInventory]") {
    // Exact, case/whitespace-insensitive.
    CHECK(type_compatible("PLA", "pla"));
    CHECK(type_compatible(" PETG ", "PETG"));
    // Family table: PLA/PLA+ and PETG/PETG+.
    CHECK(type_compatible("PLA", "PLA+"));
    CHECK(type_compatible("PETG+", "PETG"));
    // Distinct chemistries never match, however similar.
    CHECK_FALSE(type_compatible("PCTG", "PETG"));
    CHECK_FALSE(type_compatible("PLA", "ABS"));
    // Filled variants are their own types.
    CHECK_FALSE(type_compatible("PLA-CF", "PLA"));
    // Unknown on either side is not compatible.
    CHECK_FALSE(type_compatible("", "PLA"));
    CHECK_FALSE(type_compatible("PLA", ""));
}
