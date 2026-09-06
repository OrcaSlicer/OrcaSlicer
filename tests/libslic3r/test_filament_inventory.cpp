#include <catch2/catch_test_macros.hpp>
#include "libslic3r/FilamentInventory.hpp"
#include <nlohmann/json.hpp>
using namespace Slic3r;
using json = nlohmann::json;

TEST_CASE("Inventory v2 round-trips ids, kinds, and next_id", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools.resize(2);
    inv.tools[0] = { {1, "#FF0000", "PLA"}, {5, "#FFFF00", "PLA"} }; // loaded red + swappable yellow
    inv.tools[1] = { {2, "#FFFFFF", "PETG"} };                       // loaded white
    inv.next_id = 6;

    FilamentInventory back = FilamentInventory::deserialize(inv.serialize(), 2);
    REQUIRE(back.next_id == 6);
    REQUIRE(back.tools.size() == 2);
    REQUIRE(back.tools[0].size() == 2);
    REQUIRE(back.tools[0][0].id == 1);
    REQUIRE(back.tools[0][0].color == "#FF0000");
    REQUIRE(back.tools[0][0].kind == PhysicalFilament::Kind::Manual);
    REQUIRE(back.tools[0][1].id == 5);
    REQUIRE(back.tools[0][1].color == "#FFFF00");
    REQUIRE(back.tools[1].size() == 1);
    REQUIRE(back.tools[1][0].id == 2);
    REQUIRE(back.tools[1][0].type == "PETG");
}

TEST_CASE("Deserialize dedupes colliding v2 ids and floors next_id above the max", "[FilamentInventory]") {
    // Two slots claim the same id 1 -- the later one must be renumbered, not silently shadowed
    // (find()/tool_of() key on id, so a collision would make one entry unreachable).
    std::string v2 = R"({"next_id":2,"tools":[[{"id":1,"color":"#FF0000","type":"PLA"}],[{"id":1,"color":"#FFFFFF","type":"PETG"}]]})";
    FilamentInventory inv = FilamentInventory::deserialize(v2, 2);
    REQUIRE(inv.tools[0][0].id == 1);
    REQUIRE(inv.tools[1][0].id != 1);
    REQUIRE(inv.tools[1][0].id > 0);
    REQUIRE(inv.tools[0][0].color == "#FF0000");   // data preserved, not dropped
    REQUIRE(inv.tools[1][0].color == "#FFFFFF");
    REQUIRE(inv.next_id > inv.tools[1][0].id);
    REQUIRE(inv.next_id > 1);
}

TEST_CASE("Deserialize clamps a stale next_id above every id already in use", "[FilamentInventory]") {
    std::string v2 = R"({"next_id":1,"tools":[[{"id":7,"color":"#FF0000","type":"PLA"}]]})";
    FilamentInventory inv = FilamentInventory::deserialize(v2, 1);
    REQUIRE(inv.tools[0][0].id == 7);
    REQUIRE(inv.next_id > 7);
}

TEST_CASE("Deserialize assigns a fresh id to a non-empty v2 slot missing its id", "[FilamentInventory]") {
    std::string v2 = R"({"next_id":1,"tools":[[{"color":"#FF0000","type":"PLA"}]]})";
    FilamentInventory inv = FilamentInventory::deserialize(v2, 1);
    REQUIRE(inv.tools[0][0].id > 0);
    REQUIRE(inv.next_id > inv.tools[0][0].id);
}

TEST_CASE("Deserialize tolerates a v2 object missing the tools key", "[FilamentInventory]") {
    FilamentInventory inv = FilamentInventory::deserialize(R"({"next_id":5})", 3);
    REQUIRE(inv.tools.size() == 3);
    for (const auto& t : inv.tools) {
        REQUIRE(t.size() == 1);
        REQUIRE(t[0].empty());
    }
    REQUIRE(inv.next_id == 1);
}

TEST_CASE("Inventory tolerates junk and sizes to tool_count", "[FilamentInventory]") {
    FilamentInventory junk = FilamentInventory::deserialize("not json{", 3);
    REQUIRE(junk.tools.size() == 3);
    for (const auto& t : junk.tools) {
        REQUIRE(t.size() == 1);
        REQUIRE(t[0].empty());
    }
    REQUIRE(junk.next_id == 1);
}

// Orca (exactly one mapping suggestor): match_filaments_to_
// inventory (the GUI dialog's own matcher) is retired -- compute_physical_map_proposal now
// delegates matching to auto_map_filaments, same as the slice-time auto-mapper. The color/type/
// loaded-preference/merge coverage the retired matcher had is exercised via compute_physical_map_
// proposal's own tests below (same behavior, single underlying implementation now) and via the
// [AutoMap]-tagged auto_map_filaments tests further down (nearest-RGB, family-compatible,
// loaded-vs-swappable, vendor tiebreak, merge). The one signal the retired matcher had that
// auto_map_filaments didn't is exact-preset trust -- ported below, see "Auto-map trusts an exact
// preset match over color/type" and "...even when the slot's type looks incompatible".

TEST_CASE("find and tool_of look up physical filaments by id", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}, {5, "#FFFF00", "PLA"}}, {{2, "#FFFFFF", "PETG"}}};
    const PhysicalFilament* pf = inv.find(5);
    REQUIRE(pf != nullptr);
    REQUIRE(pf->color == "#FFFF00");
    REQUIRE(inv.tool_of(5) == 0);
    REQUIRE(inv.tool_of(2) == 1);
    REQUIRE(inv.find(99) == nullptr);
    REQUIRE(inv.tool_of(99) == -1);
    REQUIRE(inv.find(0) == nullptr);
}

TEST_CASE("Proposal keeps a confirmed stored id that still resolves to a non-empty physical filament", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#00FF00", "PLA"}}};
    std::vector<std::string> colors = {"#0000FF"}; // would not auto-match either slot
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored(1, 0);
    stored[0] = 2; // filament id 1 (0-based index 0) stored as physical id 2
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, stored, {}, inv, /*stored_map_confirmed=*/true);
    REQUIRE(res == std::vector<int>{2});
}

TEST_CASE("Proposal re-matches when the stored id points at a slot the editor cleared", "[FilamentInventory]") {
    // A cleared slot keeps its live id but drops its content, so
    // find(id) succeeds while empty() is true -- that must still fall through to auto-match,
    // not be trusted as a valid stored target.
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "", ""}}}; // tool 1's slot 0 was cleared
    std::vector<std::string> colors = {"#FE0101"}; // near-red, should auto-match id 1
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored(1, 2); // stale reference to the now-cleared slot
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, stored, {}, inv, /*stored_map_confirmed=*/true);
    REQUIRE(res == std::vector<int>{1}); // re-matched, not the stale cleared id
}

TEST_CASE("Proposal re-matches when the stored id no longer exists in the inventory", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}};
    std::vector<std::string> colors = {"#FE0101"};
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored(1, 99); // id no longer present (e.g. removed via the editor)
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, stored, {}, inv, /*stored_map_confirmed=*/true);
    REQUIRE(res == std::vector<int>{1});
}

TEST_CASE("Proposal ignores the stored map entirely on an unconfirmed (fresh) plate", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#00FF00", "PLA"}}};
    std::vector<std::string> colors = {"#FF0000"}; // matches id 1 exactly
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored(1, 2); // present and valid, but the plate was never actually confirmed
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, stored, {}, inv, /*stored_map_confirmed=*/false);
    REQUIRE(res == std::vector<int>{1}); // auto-match wins, stored id 2 is not trusted
}

TEST_CASE("Proposal falls back to no-match when auto-match also fails", "[FilamentInventory]") {
    // Every slot is genuinely empty here (not the inventory_all_unset/bootstrap case below --
    // matcher still runs and legitimately finds nothing compatible).
    FilamentInventory inv;
    inv.tools = {{{1, "", ""}}, {{2, "", ""}}}; // ids present, but nothing recorded
    std::vector<std::string> colors = {"#FF0000"};
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, {0}, {}, inv, false);
    REQUIRE(res == std::vector<int>{0});
}

TEST_CASE("inventory_all_unset is true only when every slot on every tool is empty", "[FilamentInventory]") {
    FilamentInventory empty_inv; empty_inv.tools.resize(2);
    REQUIRE(inventory_all_unset(empty_inv));

    FilamentInventory loaded_inv; loaded_inv.tools = {{{1, "#FF0000", "PLA"}}, {}};
    REQUIRE_FALSE(inventory_all_unset(loaded_inv));

    // The hazard this exists to catch: tool 0's loaded slot (index 0) is empty, but a swappable
    // slot on the same tool (index 1) still carries real data -- a tool-level/loaded-only check
    // would misreport this as "all unset".
    FilamentInventory swap_only_inv;
    swap_only_inv.tools = {{{0, "", ""}, {5, "#00FF00", "PETG"}}};
    REQUIRE_FALSE(inventory_all_unset(swap_only_inv));
}

TEST_CASE("Proposal reproposes the stored tool (not a physical id) on a bootstrap reopen", "[FilamentInventory]") {
    // Regression: a plate confirmed via bootstrap mode (inventory was entirely empty when first
    // mapped) with the "record as loaded" offer left unchecked keeps an all-0 physical map
    // forever -- there was never anything to record it against. Reopening such a plate must
    // still honor its confirmed tool-level filament_map, not silently reset every row.
    FilamentInventory inv; inv.tools.resize(2); // still entirely empty -- bootstrap
    std::vector<std::string> colors = {"#FF0000", "#00FF00"};
    std::vector<std::string> types  = {"PLA", "PLA"};
    std::vector<int> plate = {1, 2}; // two plate filaments, 1-based ids
    std::vector<int> stored_physical(2, 0); // never recorded -- see the scenario above
    std::vector<int> stored_filament = {2, 1}; // filament 1 -> tool 2, filament 2 -> tool 1
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, stored_physical, stored_filament, inv, /*stored_map_confirmed=*/true);
    REQUIRE(res == std::vector<int>{-2, -1}); // sentinel-encoded tool picks, not physical ids
}

TEST_CASE("Bootstrap reopen ignores the stored tool when the plate was never confirmed", "[FilamentInventory]") {
    FilamentInventory inv; inv.tools.resize(2);
    std::vector<std::string> colors = {"#FF0000"};
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> stored_filament = {2};
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, {0}, stored_filament, inv, /*stored_map_confirmed=*/false);
    REQUIRE(res == std::vector<int>{0});
}

TEST_CASE("ensure_ids mints ids for non-empty slots only and floors next_id", "[FilamentInventory]") {
    FilamentInventory inv;
    // Tool 1: loaded slot filled but never given an id (fresh-inventory editor save);
    // tool 2: loaded slot already owns id 7; tool 2 also has an id-less swappable and an
    // untouched (empty) trailing slot.
    inv.tools = {{{0, "#FF0000", "PLA"}},
                 {{7, "#00FF00", "PETG"}, {0, "#0000FF", "PLA"}, {0, "", ""}}};
    inv.next_id = 3; // stale: below the highest id in use (7)

    inv.ensure_ids();

    // Existing id kept; the two id-less non-empty slots got fresh ids above every id in use.
    CHECK(inv.tools[1][0].id == 7);
    CHECK(inv.tools[0][0].id == 8);
    CHECK(inv.tools[1][1].id == 9);
    // The empty slot carries no data to reference, so it stays unminted.
    CHECK(inv.tools[1][2].id == 0);
    REQUIRE(inv.next_id == 10);
}

TEST_CASE("ensure_ids zeroes a cleared slot's stale id", "[FilamentInventory]") {
    // Regression: FilamentInventoryEditor::on_ok never resets Row::id (sync-clear and the manual
    // Clear button only blank color/type/preset), so a slot that used to hold a real filament and
    // was then cleared kept its old id. find()/tool_of() key on id without checking empty(), so
    // that stale id let a plate's old filament_physical_map entry (or a durable SlotAssignment)
    // keep resolving to the cleared slot -- e.g. the mapping dialog's picker re-offering a tool
    // as if it still held the filament that used to be there. ensure_ids is the single choke
    // point every GUI write path already calls before saving, so it canonicalizes id 0 there.
    FilamentInventory inv;
    inv.tools = {{{7, "", ""}}}; // tool 1's loaded slot was cleared but kept id 7
    inv.next_id = 8;

    inv.ensure_ids();

    CHECK(inv.tools[0][0].id == 0);
    CHECK(inv.find(7) == nullptr);
    CHECK(inv.tool_of(7) == -1);
}

TEST_CASE("Ids survive a save/load round-trip when ensure_ids ran before saving", "[FilamentInventory]") {
    // Regression: the Physical Filaments editor's first-ever save wrote loaded rows with id 0
    // (only rows *added* that session were minted). deserialize's reconcile pass then renumbered
    // them on the next launch, so ids churned across restarts and every plate
    // filament_physical_map entry pointing at them went stale. With ensure_ids applied before
    // serialize, a reload must hand back exactly the ids that were saved.
    FilamentInventory inv;
    inv.tools = {{{0, "#E01B24", "PLA"}},
                 {{0, "#FFFFFF", "PLA"}},
                 {{0, "#33D17A", "PLA"}, {1, "#9141AC", ""}}}; // the added row was minted (id 1)
    inv.next_id = 2;

    inv.ensure_ids();
    FilamentInventory reloaded = FilamentInventory::deserialize(inv.serialize(), inv.tools.size());

    for (size_t t = 0; t < inv.tools.size(); ++t)
        for (size_t s = 0; s < inv.tools[t].size(); ++s)
            CHECK(reloaded.tools[t][s].id == inv.tools[t][s].id);
    REQUIRE(reloaded.next_id == inv.next_id);
}

TEST_CASE("Slot preset field round-trips and is optional on read", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}};
    inv.tools[0][0].preset = "Snapmaker PLA SnapSpeed @U1";
    inv.next_id = 2;
    FilamentInventory back = FilamentInventory::deserialize(inv.serialize(), 1);
    REQUIRE(back.tools[0][0].preset == "Snapmaker PLA SnapSpeed @U1");

    // v2 string (no preset field) reads back with an empty preset.
    std::string v2 = R"({"next_id":2,"tools":[[{"id":1,"color":"#FF0000","type":"PLA","kind":"manual"}]]})";
    FilamentInventory old = FilamentInventory::deserialize(v2, 1);
    REQUIRE(old.tools[0][0].preset.empty());
    REQUIRE(old.tools[0][0].id == 1);
}

TEST_CASE("A slot with only a preset recorded is not empty", "[FilamentInventory]") {
    PhysicalFilament pf;
    pf.preset = "Generic PLA";
    REQUIRE_FALSE(pf.empty());
}

TEST_CASE("FilamentInventories round-trips inventories keyed by printer preset", "[FilamentInventory]") {
    FilamentInventories store;
    FilamentInventory& inv = store.for_preset("Snapmaker U1 (0.4 nozzle)", 4);
    inv.tools[0][0] = {1, "#FF0000", "PLA"};
    inv.tools[0][0].preset = "Snapmaker PLA SnapSpeed @U1";
    inv.next_id = 2;

    FilamentInventories back = FilamentInventories::deserialize(store.serialize());
    REQUIRE(back.by_preset.count("Snapmaker U1 (0.4 nozzle)") == 1);
    const FilamentInventory& back_inv = back.by_preset.at("Snapmaker U1 (0.4 nozzle)");
    CHECK(back_inv.tools[0][0].preset == "Snapmaker PLA SnapSpeed @U1");
    CHECK(back_inv.tools[0][0].color == "#FF0000");
    CHECK_FALSE(back.parse_error);
}

TEST_CASE("FilamentInventories::serialize writes the versioned envelope", "[FilamentInventory]") {
    FilamentInventories store;
    store.for_preset("My Printer", 1).tools[0][0] = {1, "#FF0000", "PLA"};
    json j = json::parse(store.serialize());
    REQUIRE(j["version"] == 1);
    REQUIRE(j["presets"].is_object());
    REQUIRE(j["presets"].contains("My Printer"));
}

TEST_CASE("FilamentInventories::deserialize loads a legacy bare-object store", "[FilamentInventory]") {
    // Pre-envelope shape: no "version"/"presets" wrapper, top level keyed directly by preset name.
    std::string legacy = R"({"My Printer":{"next_id":2,"tools":[[{"id":1,"color":"#FF0000","type":"PLA"}]]}})";
    FilamentInventories store = FilamentInventories::deserialize(legacy);
    CHECK_FALSE(store.parse_error);
    REQUIRE(store.by_preset.count("My Printer") == 1);
    CHECK(store.by_preset.at("My Printer").tools[0][0].color == "#FF0000");
}

TEST_CASE("FilamentInventories::deserialize loads a legacy store with presets literally named version/presets", "[FilamentInventory]") {
    // Both named entries are themselves inventory OBJECTS (as any real preset's value always is),
    // so j["version"] is never an integer here and the whole store correctly reads as legacy.
    std::string legacy = R"({
        "version": {"next_id":2,"tools":[[{"id":1,"color":"#FF0000","type":"PLA"}]]},
        "presets": {"next_id":3,"tools":[[{"id":2,"color":"#00FF00","type":"PETG"}]]}
    })";
    FilamentInventories store = FilamentInventories::deserialize(legacy);
    CHECK_FALSE(store.parse_error);
    REQUIRE(store.by_preset.count("version") == 1);
    REQUIRE(store.by_preset.count("presets") == 1);
    CHECK(store.by_preset.at("version").tools[0][0].color == "#FF0000");
    CHECK(store.by_preset.at("presets").tools[0][0].color == "#00FF00");
}

TEST_CASE("FilamentInventories::deserialize reports a parse failure without fabricating data", "[FilamentInventory]") {
    FilamentInventories junk = FilamentInventories::deserialize("not json{");
    CHECK(junk.parse_error);
    CHECK(junk.by_preset.empty());

    FilamentInventories not_object = FilamentInventories::deserialize("[1,2,3]");
    CHECK(not_object.parse_error);
    CHECK(not_object.by_preset.empty());
}

TEST_CASE("for_preset creates an empty, padded inventory on first use", "[FilamentInventory]") {
    FilamentInventories store;
    FilamentInventory& inv = store.for_preset("Brand New Printer", 3);
    REQUIRE(inv.tools.size() == 3);
    for (const auto& tool : inv.tools)
        for (const auto& pf : tool)
            CHECK(pf.empty());
    CHECK(store.by_preset.size() == 1);
}

TEST_CASE("for_preset is idempotent and pads without truncating on later calls", "[FilamentInventory]") {
    FilamentInventories store;
    store.for_preset("My Printer", 2).tools[0][0] = {1, "#0000FF", "ABS"};
    REQUIRE(store.by_preset.size() == 1);

    // A later call (e.g. a fresh dialog open) returns the SAME entry, padded up if tool_count grew.
    FilamentInventory& inv = store.for_preset("My Printer", 4);
    CHECK(inv.tools[0][0].color == "#0000FF");
    REQUIRE(inv.tools.size() == 4);
    CHECK(store.by_preset.size() == 1); // still one entry, not duplicated
}

TEST_CASE("ensure_tool_count pads and never truncates", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}};
    inv.ensure_tool_count(4);
    REQUIRE(inv.tools.size() == 4);
    CHECK(inv.tools[0][0].color == "#FF0000");
    REQUIRE(inv.tools[3].size() == 1); // slot 0 present
    inv.ensure_tool_count(2);
    REQUIRE(inv.tools.size() == 4); // never truncates
}

TEST_CASE("apply_synced_loaded_slot overwrites only the target tool's loaded slot", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}, {2, "#00FF00", "PETG"}}, // tool 1: loaded + one swap row
                 {{3, "#0000FF", "PLA"}}};                        // tool 2: loaded only
    inv.next_id = 4;

    // A fresh sync report replaces tool 1's loaded slot; its swap row and tool 2 are untouched.
    // apply_synced_loaded_slot is a verbatim overwrite -- callers (slot_from_row) decide what id
    // to carry over; here the caller explicitly keeps the slot's existing id (1).
    PhysicalFilament synced;
    synced.id    = 1;
    synced.color = "#FFFF00";
    synced.type  = "PETG";
    inv.apply_synced_loaded_slot(0, synced);
    CHECK(inv.tools[0][0].color == "#FFFF00");
    CHECK(inv.tools[0][0].type == "PETG");
    CHECK(inv.tools[0][0].id == 1);
    CHECK(inv.tools[0][1].color == "#00FF00"); // swap row untouched
    CHECK(inv.tools[1][0].color == "#0000FF"); // other tool untouched

    // The printer reporting a tool's tray as empty is represented by a default-constructed slot
    // (id 0) -- it's the caller's job (via ensure_ids, exercised in the next test) to reconcile
    // that against whatever id the slot used to carry.
    inv.apply_synced_loaded_slot(1, PhysicalFilament{});
    CHECK(inv.tools[1][0].empty());
    CHECK(inv.tools[1][0].id == 0);
}

TEST_CASE("apply_synced_loaded_slot grows tools for an out-of-range tool index", "[FilamentInventory]") {
    FilamentInventory inv;
    PhysicalFilament synced;
    synced.color = "#123456";
    inv.apply_synced_loaded_slot(2, synced);
    REQUIRE(inv.tools.size() == 3);
    CHECK(inv.tools[2][0].color == "#123456");
    CHECK(inv.tools[0][0].empty());
    CHECK(inv.tools[1][0].empty());
}

TEST_CASE("A synced-then-cleared tool round-trips as canonically empty after ensure_ids", "[FilamentInventory]") {
    // End-to-end of the write-through seam: sync applies a real filament (id-less, as a freshly
    // reported tray would be), ensure_ids mints it; a later sync reports the same tool as empty,
    // ensure_ids must zero that id right back out.
    FilamentInventory inv;
    inv.tools = {{{0, "", ""}}};

    PhysicalFilament loaded;
    loaded.color = "#AABBCC";
    loaded.type  = "PLA";
    inv.apply_synced_loaded_slot(0, loaded);
    inv.ensure_ids();
    int minted_id = inv.tools[0][0].id;
    REQUIRE(minted_id > 0);

    inv.apply_synced_loaded_slot(0, PhysicalFilament{}); // printer now reports the tray empty
    inv.ensure_ids();
    CHECK(inv.tools[0][0].empty());
    CHECK(inv.tools[0][0].id == 0);
    CHECK(inv.find(minted_id) == nullptr);
}

TEST_CASE("Proposal reproposes a confirmed pick of an empty tool as a tool sentinel", "[FilamentInventory]") {
    // Mixed mode: the inventory has real records on some tools,
    // but the user confirmed routing a filament to a tool with NOTHING recorded (physical id 0,
    // tool set in filament_map). Reopening must keep that pick (sentinel-encoded -(tool)),
    // not silently auto-match it away.
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{0, "", ""}}}; // tool 1 recorded; tool 2 empty
    std::vector<std::string> colors = {"#FF0000", "#0000FF"};
    std::vector<std::string> types  = {"PLA", "PLA"};
    std::vector<int> plate = {1, 2};
    std::vector<int> stored_physical = {1, 0};  // filament 2 confirmed onto the EMPTY tool 2
    std::vector<int> stored_filament = {1, 2};
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate,
                                                          stored_physical, stored_filament, inv,
                                                          /*stored_map_confirmed=*/true);
    REQUIRE(res.size() == 2);
    CHECK(res[0] == 1);   // stored real id kept
    CHECK(res[1] == -2);  // empty-tool pick preserved as sentinel
}

TEST_CASE("Proposal does not sentinel a stored empty-tool pick once that tool gains a record", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#0000FF", "PLA"}}}; // tool 2 now has a recorded filament
    std::vector<int> res = compute_physical_map_proposal({"#FF0000", "#0000FF"}, {"PLA", "PLA"}, {}, {},
                                                          {1, 2}, {1, 0}, {1, 2}, inv,
                                                          /*stored_map_confirmed=*/true);
    REQUIRE(res.size() == 2);
    CHECK(res[1] == 2); // auto-match takes over: the recorded filament is the better proposal
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

// Durable per-(physical device x project) slot assignments.
// dump_slot_assignments/load_slot_assignments are the (de)serialization pair for the
// device-key -> assignment-list map persisted on the Model as JSON.

TEST_CASE("Slot assignments round-trip through dump and load", "[FilamentInventory][SlotAssignment]") {
    std::map<std::string, std::vector<SlotAssignment>> devices;
    devices["My Printer"] = { {0, 1, 0}, {1, 2, 0} };
    devices["Snapmaker U1 (0.4 nozzle) - garage"] = { {2, 0, 0} };

    std::string json = dump_slot_assignments(devices);
    std::map<std::string, std::vector<SlotAssignment>> back = load_slot_assignments(json);

    REQUIRE(back.size() == 2);
    REQUIRE(back.count("My Printer") == 1);
    REQUIRE(back["My Printer"].size() == 2);
    CHECK(back["My Printer"][0].filament_idx == 0);
    CHECK(back["My Printer"][0].tool == 1);
    CHECK(back["My Printer"][0].slot == 0);
    CHECK(back["My Printer"][1].filament_idx == 1);
    CHECK(back["My Printer"][1].tool == 2);
    REQUIRE(back.count("Snapmaker U1 (0.4 nozzle) - garage") == 1);
    CHECK(back["Snapmaker U1 (0.4 nozzle) - garage"][0].filament_idx == 2);
}

TEST_CASE("An empty slot-assignments string loads to an empty map", "[FilamentInventory][SlotAssignment]") {
    std::map<std::string, std::vector<SlotAssignment>> back = load_slot_assignments("");
    CHECK(back.empty());
}

TEST_CASE("Malformed slot-assignments JSON loads to an empty map without throwing", "[FilamentInventory][SlotAssignment]") {
    std::map<std::string, std::vector<SlotAssignment>> back;
    REQUIRE_NOTHROW(back = load_slot_assignments("{not json"));
    CHECK(back.empty());

    REQUIRE_NOTHROW(back = load_slot_assignments("[1,2,3]"));
    CHECK(back.empty());

    REQUIRE_NOTHROW(back = load_slot_assignments("\"just a string\""));
    CHECK(back.empty());
}

TEST_CASE("Unknown device keys are preserved through a load/dump cycle", "[FilamentInventory][SlotAssignment]") {
    // A device this build has never heard of (e.g. from a newer Orca or a machine unplugged
    // right now) must survive a load -> dump round-trip untouched, so re-saving a project never
    // silently drops another device's assignments.
    std::string json = R"({"Future Printer":[{"filament":4,"tool":1,"slot":0}]})";
    std::map<std::string, std::vector<SlotAssignment>> loaded = load_slot_assignments(json);
    REQUIRE(loaded.count("Future Printer") == 1);

    std::string dumped = dump_slot_assignments(loaded);
    std::map<std::string, std::vector<SlotAssignment>> back = load_slot_assignments(dumped);
    REQUIRE(back.count("Future Printer") == 1);
    REQUIRE(back["Future Printer"].size() == 1);
    CHECK(back["Future Printer"][0].filament_idx == 4);
    CHECK(back["Future Printer"][0].tool == 1);
}

TEST_CASE("Multiple devices in one blob load independently", "[FilamentInventory][SlotAssignment]") {
    std::string json = R"({
        "Printer A": [{"filament":0,"tool":0,"slot":0}],
        "Printer B": [{"filament":0,"tool":3,"slot":0},{"filament":1,"tool":1,"slot":0}]
    })";
    std::map<std::string, std::vector<SlotAssignment>> back = load_slot_assignments(json);
    REQUIRE(back.size() == 2);
    CHECK(back["Printer A"].size() == 1);
    CHECK(back["Printer B"].size() == 2);
}

// Slice-time auto-mapper. auto_map_filaments is the pure
// matcher: assignment (if still valid) wins, else family-compatible nearest-color match with a
// vendor tiebreak, else unmatched.

TEST_CASE("Auto-map honors a valid stored assignment over a better color match", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#000000", "PLA"}}}; // tool 0 exact color match
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "Generic"}};
    std::vector<SlotAssignment> assignments = {{0, 1, 0}}; // filament 0 explicitly routed to tool 1 (worse color)
    AutoMapResult res = auto_map_filaments(project, inv, assignments);
    REQUIRE(res.filament_map.size() == 1);
    CHECK(res.filament_map[0] == 2);       // 1-based tool 2 (assignment.tool == 1)
    CHECK(res.physical_map[0] == 2);       // physical id of tool 1's slot 0
    CHECK(res.unmatched.empty());
}

TEST_CASE("Auto-map falls through to matching when the stored assignment's tool no longer exists", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}}; // only one tool now
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "Generic"}};
    std::vector<SlotAssignment> assignments = {{0, 5, 0}}; // stale: tool 5 is out of range
    AutoMapResult res = auto_map_filaments(project, inv, assignments);
    REQUIRE(res.filament_map.size() == 1);
    CHECK(res.filament_map[0] == 1);
    CHECK(res.physical_map[0] == 1);
    CHECK(res.unmatched.empty());
}

TEST_CASE("Auto-map falls through to matching when the stored assignment's slot no longer exists", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}}; // tool 0 has only slot 0
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "Generic"}};
    std::vector<SlotAssignment> assignments = {{0, 0, 3}}; // stale: slot 3 does not exist on tool 0
    AutoMapResult res = auto_map_filaments(project, inv, assignments);
    CHECK(res.filament_map[0] == 1);
    CHECK(res.physical_map[0] == 1);
    CHECK(res.unmatched.empty());
}

TEST_CASE("Auto-map falls through to matching when the stored assignment's slot is empty", "[FilamentInventory][AutoMap]") {
    // Real Snapmaker U1 bug: an assignment was written while tool 1 still had a filament, then a
    // printer sync cleared that tool's loaded slot (see FilamentInventory::apply_synced_loaded_
    // slot). The assignment's tool/slot indices are still in range, so it must not be honored
    // outright -- an empty slot has nothing physically loaded, so the auto-mapper must fall
    // through to matching (which finds tool 0's exact match) instead of routing to the empty tool.
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{}}}; // tool 1's slot 0 is empty (id 0, no color/type/preset)
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "Generic"}};
    std::vector<SlotAssignment> assignments = {{0, 1, 0}}; // stale: tool 1 slot 0 is empty
    AutoMapResult res = auto_map_filaments(project, inv, assignments);
    REQUIRE(res.filament_map.size() == 1);
    CHECK(res.filament_map[0] == 1);   // falls through to the matcher, which finds tool 0
    CHECK(res.physical_map[0] == 1);
    CHECK(res.unmatched.empty());
}

TEST_CASE("Auto-map leaves a filament unmatched when the only stored assignment's slot is empty and nothing else matches", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{}}}; // one tool, its only slot is empty
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "Generic"}};
    std::vector<SlotAssignment> assignments = {{0, 0, 0}}; // stale: tool 0 slot 0 is empty
    AutoMapResult res = auto_map_filaments(project, inv, assignments);
    REQUIRE(res.unmatched == std::vector<int>{0});
    CHECK(res.physical_map[0] == -1);
}

TEST_CASE("Auto-map matcher never selects an empty slot even when it is the only slot", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{}}}; // one tool, its only slot is empty -- nothing physically present
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched == std::vector<int>{0});
    CHECK(res.physical_map[0] == -1);
}

TEST_CASE("Auto-map never family-matches an empty-type slot to a project filament with no type either", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", ""}}}; // slot has a color/id but no type recorded -- not empty()
    std::vector<ProjectFilamentInfo> project = {{"", "#FF0000", ""}}; // project type also unknown
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched == std::vector<int>{0}); // unknown-vs-unknown type must never be treated as compatible
    CHECK(res.physical_map[0] == -1);
}

TEST_CASE("Auto-map treats PLA and PLA+ as family-compatible", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA+"}}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched.empty());
    CHECK(res.filament_map[0] == 1);
    CHECK(res.physical_map[0] == 1);
}

TEST_CASE("Auto-map never matches PLA to PLA-CF", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA-CF"}}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched == std::vector<int>{0});
    CHECK(res.filament_map[0] == 1); // fallback default, caller decides what to do with it
    CHECK(res.physical_map[0] == -1);
}

TEST_CASE("Auto-map never matches PETG to PLA even at identical color", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}};
    std::vector<ProjectFilamentInfo> project = {{"PETG", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched == std::vector<int>{0});
}

TEST_CASE("Auto-map picks the nearest-RGB family-compatible candidate", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#FE0101", "PLA"}}, {{3, "#000000", "PLA"}}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FE0101", ""}}; // nearest is tool 1 (id 2), not the exact-but-farther tool 0
    AutoMapResult res = auto_map_filaments(project, inv, {});
    CHECK(res.filament_map[0] == 2);
    CHECK(res.physical_map[0] == 2);
}

TEST_CASE("Auto-map uses vendor to break an exact color tie", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#FF0000", "PLA"}}};
    inv.tools[1][0].preset = "Snapmaker PLA SnapSpeed"; // vendor token "Snapmaker"
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "Snapmaker"}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    CHECK(res.filament_map[0] == 2); // tool 1 wins on vendor despite identical color distance
    CHECK(res.physical_map[0] == 2);
}

// Regression guard: compute_physical_map_proposal (the dialog's proposal) used to build its
// ProjectFilamentInfo list inline and silently drop vendor, so it disagreed with auto_map_filaments
// (slice-time's core) on this exact scenario -- the dialog would propose tool 1 while a live-auto
// slice routed to tool 2. Both now go through the same build_project_filament_info, so this must
// resolve identically to "Auto-map uses vendor to break an exact color tie" above.
TEST_CASE("Proposal uses vendor to break an exact color tie, same as slice-time auto-map", "[FilamentInventory]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#FF0000", "PLA"}}};
    inv.tools[0][0].preset = "Generic PLA";
    inv.tools[1][0].preset = "Snapmaker PLA SnapSpeed"; // vendor token "Snapmaker"
    std::vector<std::string> colors  = {"#FF0000"};
    std::vector<std::string> types   = {"PLA"};
    std::vector<std::string> vendors = {"Snapmaker"};
    std::vector<int>         plate   = {1};
    std::vector<int> res = compute_physical_map_proposal(colors, types, vendors, {}, plate, {}, {}, inv,
                                                          /*stored_map_confirmed=*/false);
    REQUIRE(res.size() == 1);
    CHECK(res[0] == 2); // tool 2 wins on vendor, matching auto_map_filaments exactly
}

// Orca: ported from the retired match_filaments_to_inventory -- an exact preset match is
// the strongest signal the old GUI-only matcher trusted, carried forward into the single
// suggestor rather than silently dropped.
TEST_CASE("Auto-map trusts an exact preset match over a closer color match", "[FilamentInventory][AutoMap]") {
    // slot 1's color is deliberately kept within kMaxFamilyColorDistance of the project color
    // (#FF0000 vs #C80000: dr=255-200=55, 55*55=3025 < 10000) -- a preset match beyond
    // the cutoff falls through instead of winning outright, so this test's own point (preset still
    // outranks a closer but preset-less color match, PROVIDED it clears the cutoff) needs the
    // preset candidate to clear it. See "...falls through to the family/color tier" below for the
    // beyond-cutoff case.
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#C80000", "PLA"}}}; // slot 0: exact color, no preset; slot 1: a further-but-still-in-cutoff color...
    inv.tools[1][0].preset = "Snapmaker PLA SnapSpeed @U1";         // ...but exact preset
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed @U1"}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    CHECK(res.filament_map[0] == 2);
    CHECK(res.physical_map[0] == 2);
}

TEST_CASE("Auto-map's exact-preset tier ignores a type mismatch on the slot record", "[FilamentInventory][AutoMap]") {
    // A stale/miscategorized type on the slot must not veto an exact profile match.
    FilamentInventory inv;
    inv.tools = {{{1, "", "PETG"}}};
    inv.tools[0][0].preset = "Snapmaker PLA SnapSpeed @U1";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed @U1"}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    CHECK(res.filament_map[0] == 1);
    CHECK(res.physical_map[0] == 1);
}

TEST_CASE("Auto-map's exact-preset tier is color-aware for the U1 five-filament repro (fix15)", "[FilamentInventory][AutoMap]") {
    // Real-world bug report: a project with 5 filaments, all sharing preset "Snapmaker PLA
    // SnapSpeed" but 5 different colors (red/white/black/green/tan), against an inventory of
    // T1 blue PolyTerra PLA (no matching preset), T2 teal SnapSpeed PLA, T4 white SnapSpeed PLA
    // (T3 empty). Previously, auto_map_filaments sent ALL FIVE to T2 (first preset match found,
    // no color awareness, no cutoff at the preset tier). Before this fix: filament_map
    // == {2, 2, 2, 2, 2} for all five, unconditionally.
    //
    // Squared-RGB distances (dr^2+dg^2+db^2), teal = #008080 = (0,128,128), white spool =
    // #FFFFFF, blue PolyTerra = #0000FF:
    //   red   #FF0000 (255,0,0)   -> teal:  255^2+128^2+128^2 = 65025+16384+16384 =  97793
    //                              -> white: 0^2+255^2+255^2  =     0+65025+65025 = 130050
    //                              -> blue:  255^2+0+255^2    = 65025+0+65025     = 130050
    //   white #FFFFFF (255,255,255) -> teal: 255^2+127^2+127^2 = 65025+16129+16129 =  97283
    //                                -> white: 0                                    =      0
    //   black #000000 (0,0,0)     -> teal:  0+128^2+128^2     =     0+16384+16384 =  32768
    //                              -> white: 255^2*3           = 195075
    //                              -> blue:  255^2             =  65025
    //   green #00FF00 (0,255,0)   -> teal:  0+127^2+128^2     =     0+16129+16384 =  32513
    //                              -> white: 255^2+0+255^2    = 130050
    //                              -> blue:  255^2+255^2      = 130050
    //   tan   #D2B48C (210,180,140) -> teal: 210^2+52^2+12^2  = 44100+2704+144    =  46948
    //                                -> white: 45^2+75^2+115^2 = 2025+5625+13225   =  20875
    //                                -> blue: 210^2+180^2+115^2 = 44100+32400+13225 =  89725
    // kMaxFamilyColorDistance is 10000. Only white's distance to the white spool (0) clears it --
    // every other filament's nearest candidate (preset or family/color, teal/white/blue alike) is
    // over the cutoff, so white->T4 and the other four are UNMATCHED (the dialog then prompts for
    // confirmation instead of silently proposing a wrong color).
    FilamentInventory inv;
    inv.tools = {{{1, "#0000FF", "PolyTerra PLA"}},   // T1: no matching preset
                 {{2, "#008080", "PLA"}},             // T2: SnapSpeed preset, teal
                 {},                                  // T3: empty
                 {{4, "#FFFFFF", "PLA"}}};             // T4: SnapSpeed preset, white
    inv.tools[1][0].preset = "Snapmaker PLA SnapSpeed";
    inv.tools[3][0].preset = "Snapmaker PLA SnapSpeed";
    std::vector<ProjectFilamentInfo> project = {
        {"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed"}, // red
        {"PLA", "#FFFFFF", "", "Snapmaker PLA SnapSpeed"}, // white
        {"PLA", "#000000", "", "Snapmaker PLA SnapSpeed"}, // black
        {"PLA", "#00FF00", "", "Snapmaker PLA SnapSpeed"}, // green
        {"PLA", "#D2B48C", "", "Snapmaker PLA SnapSpeed"}, // tan
    };
    AutoMapResult res = auto_map_filaments(project, inv, {});
    CHECK(res.filament_map[1] == 4);   // white -> T4, the color-nearest preset match
    CHECK(res.physical_map[1] == 4);
    REQUIRE(res.unmatched == std::vector<int>{0, 2, 3, 4}); // red, black, green, tan: no in-cutoff candidate anywhere
}

TEST_CASE("Auto-map honors a durable assignment even over an exact-preset slot", "[FilamentInventory][AutoMap]") {
    // Ordering pin: assignment > exact-preset > family/color/vendor. A durable pick still wins
    // outright even when a different slot would have matched the project filament's preset exactly.
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#800000", "PLA"}}};
    inv.tools[1][0].preset = "Snapmaker PLA SnapSpeed @U1";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed @U1"}};
    std::vector<SlotAssignment> assignments = {{0, 0, 0}}; // filament 0 explicitly routed to tool 0
    AutoMapResult res = auto_map_filaments(project, inv, assignments);
    CHECK(res.filament_map[0] == 1);  // tool 0 (the assignment), not tool 1 (the exact preset)
    CHECK(res.physical_map[0] == 1);
}

TEST_CASE("Auto-map prefers a loaded slot over an equally-close swappable one", "[FilamentInventory][AutoMap]") {
    // Ported from the retired match_filaments_to_inventory, which had this same loaded-over-
    // swappable tie-break; promoted into auto_map_filaments's family/color tier since there is now
    // only one matcher.
    FilamentInventory inv;
    // Tool 0: nothing loaded, but a swappable red PLA (id 5). Tool 1: loaded red PLA (id 9).
    inv.tools = {{{0, "", ""}, {5, "#FF0000", "PLA"}}, {{9, "#FF0000", "PLA"}}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    CHECK(res.filament_map[0] == 2);  // tool 1's loaded slot wins over tool 0's swappable one
    CHECK(res.physical_map[0] == 9);
}

TEST_CASE("Auto-map leaves a filament unmatched rather than propose a wildly different color", "[FilamentInventory][AutoMap]") {
    // KEPT from the retired match_filaments_to_inventory (its ΔE cutoff), reimplemented as a
    // squared-RGB kMaxFamilyColorDistance in auto_map_filaments' family/color tier: silently
    // auto-proposing blue for a red filament just because it's the only same-type slot around is
    // the exact failure auto-mapping must not commit. Unmatched is the safe outcome -- the caller
    // (dialog or slice-time) then prompts instead of committing a wrong color.
    FilamentInventory inv;
    inv.tools = {{{1, "#0000FF", "PLA"}}}; // the only PLA slot, and it's blue
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}}; // project filament is red
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched == std::vector<int>{0});
    CHECK(res.physical_map[0] == -1);
}

TEST_CASE("Proposal leaves a row unproposed rather than a wildly different color, through the dialog path", "[FilamentInventory]") {
    // Same cutoff, exercised through compute_physical_map_proposal (the dialog's proposal/
    // Automatic-button path) rather than auto_map_filaments directly, since both now share one
    // matcher and this is the path the original bug report came through.
    FilamentInventory inv;
    inv.tools = {{{1, "#0000FF", "PLA"}}};
    std::vector<std::string> colors = {"#FF0000"};
    std::vector<std::string> types  = {"PLA"};
    std::vector<int> plate = {1};
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, {0}, {}, inv, /*stored_map_confirmed=*/false);
    REQUIRE(res == std::vector<int>{0});
}

TEST_CASE("Auto-map's color cutoff still lets an unknown-color slot qualify on type alone", "[FilamentInventory][AutoMap]") {
    // The cutoff only disqualifies a KNOWN, out-of-range color -- an unknown color on either side
    // (unparseable/absent) must still qualify when it's the only compatible candidate, same as
    // before the cutoff existed.
    FilamentInventory inv;
    inv.tools = {{{1, "", "PLA"}}}; // type matches, color unrecorded
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched.empty());
    CHECK(res.physical_map[0] == 1);
}

TEST_CASE("Auto-map still matches a moderately different shade of the same color family", "[FilamentInventory][AutoMap]") {
    // The cutoff must not overcorrect into rejecting ordinary batch-to-batch variation -- only a
    // genuinely different hue should require confirmation, not a darker/lighter version of the
    // same color. #FF0000 vs #AB0000 measures 7056 in this file's squared-RGB metric, comfortably
    // under kMaxFamilyColorDistance (10000).
    FilamentInventory inv;
    inv.tools = {{{1, "#AB0000", "PLA"}}}; // a noticeably darker red, still clearly "red"
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched.empty());
    CHECK(res.physical_map[0] == 1);
}

// Two-way pin for kMaxFamilyColorDistance's actual boundary (the moderate-shade test above only
// pins the calibration FLOOR -- it passes with or without the cutoff, since 7056 < 10000 either
// way). #FF0000 vs #9A0000: dr = 255-154 = 101, 101*101 = 10201, just over the 10000 cutoff. This
// must go RED if the cutoff is ever removed or its distance metric changes, unlike the moderate-
// shade test.
TEST_CASE("Auto-map's color cutoff disqualifies a shade just past the threshold", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#9A0000", "PLA"}}}; // squared-RGB distance to #FF0000 is 10201 (> 10000)
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched == std::vector<int>{0});
    CHECK(res.physical_map[0] == -1);
}

TEST_CASE("Auto-map's exact-preset tier falls through to unmatched when the color is a gross mismatch", "[FilamentInventory][AutoMap]") {
    // Supersedes "...wins regardless of how far the color is" -- that pinned an earlier
    // behavior where a preset match was trusted no matter the color. That was extended: a preset
    // match beyond kMaxFamilyColorDistance is no longer trusted outright; it falls through to the
    // family/color tier (itself cutoff-guarded). Here that's the very same slot, so it's
    // disqualified there too and the filament ends up unmatched -- same outcome as "Auto-map
    // leaves a filament unmatched rather than propose a wildly different color" above, which pins
    // the identical red-vs-blue distance with no preset involved at all.
    FilamentInventory inv;
    inv.tools = {{{1, "#0000FF", "PLA"}}};
    inv.tools[0][0].preset = "Snapmaker PLA SnapSpeed @U1";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed @U1"}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched == std::vector<int>{0});
    CHECK(res.physical_map[0] == -1);
}

TEST_CASE("Auto-map's exact-preset tier falls through to the family/color tier when beyond the cutoff and a family candidate is in range", "[FilamentInventory][AutoMap]") {
    // A preset match beyond the cutoff is treated as absent, not as a hard failure -- if a
    // family-compatible, cutoff-in-range candidate exists elsewhere, tier 3 still finds it.
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#0000FF", "PLA"}}}; // tool 0: no preset, exact color; tool 1: exact preset, blue (gross mismatch)
    inv.tools[1][0].preset = "Snapmaker PLA SnapSpeed @U1";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed @U1"}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched.empty());
    CHECK(res.filament_map[0] == 1);  // tool 0 (family/color tier), not tool 1 (the disqualified preset match)
    CHECK(res.physical_map[0] == 1);
}

TEST_CASE("Auto-map's exact-preset tier breaks a tie between two preset matches by nearest color", "[FilamentInventory][AutoMap]") {
    // With several slots sharing the same preset (e.g. one preset loaded into
    // several different-colored spools), the tier used to break ties by tool index alone -- a
    // white project filament could land on a teal spool just because it was scanned first. Now the
    // nearest color wins: #FFFFFF vs #008080 (teal) = 255^2+127^2+127^2 = 65025+16129+16129 =
    // 97283; #FFFFFF vs #F0F0F0 (near-white) = 15^2*3 = 675. Tool 1 (near-white) must win despite
    // tool 0 being scanned first.
    FilamentInventory inv;
    inv.tools = {{{1, "#008080", "PLA"}}, {{2, "#F0F0F0", "PLA"}}};
    inv.tools[0][0].preset = "Snapmaker PLA SnapSpeed";
    inv.tools[1][0].preset = "Snapmaker PLA SnapSpeed";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FFFFFF", "", "Snapmaker PLA SnapSpeed"}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    CHECK(res.filament_map[0] == 2);
    CHECK(res.physical_map[0] == 2);
}

TEST_CASE("Auto-map's durable assignment wins regardless of how far the color is", "[FilamentInventory][AutoMap]") {
    // Same principle as the exact-preset case above: a durable SlotAssignment is an explicit user
    // choice, resolved before the family/color tier even runs, so the color cutoff never
    // applies to it -- the same red-vs-blue pairing that would otherwise be unmatched still
    // resolves when the user has explicitly routed this filament to that tool.
    FilamentInventory inv;
    inv.tools = {{{1, "#0000FF", "PLA"}}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    std::vector<SlotAssignment> assignments = {{0, 0, 0}};
    AutoMapResult res = auto_map_filaments(project, inv, assignments);
    REQUIRE(res.unmatched.empty());
    CHECK(res.filament_map[0] == 1);
    CHECK(res.physical_map[0] == 1);
}

TEST_CASE("Proposal matches a PLA+ project filament to a plain PLA slot", "[FilamentInventory]") {
    // The bug that motivated this consolidation: the dialog's own matcher required an exact type
    // match (PLA+ vs PLA disqualified), while the slice-time auto-mapper already treated them as
    // family-compatible. Both paths now share auto_map_filaments, so the dialog's proposal (and,
    // by extension, its Automatic button) gets the same family match slice-time mapping always had.
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}};
    std::vector<std::string> colors = {"#FF0000"};
    std::vector<std::string> types  = {"PLA+"};
    std::vector<int> plate = {1};
    std::vector<int> res = compute_physical_map_proposal(colors, types, {}, {}, plate, {0}, {}, inv, /*stored_map_confirmed=*/false);
    REQUIRE(res == std::vector<int>{1});
}

TEST_CASE("Auto-map breaks a full tie (color and vendor) with the lowest tool index", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#FF0000", "PLA"}}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    CHECK(res.filament_map[0] == 1);
    CHECK(res.physical_map[0] == 1);
}

TEST_CASE("Auto-map surfaces unmatched filaments with a fallback tool of 1", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PETG"}}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}, {"PETG", "#FF0000", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched == std::vector<int>{0});
    CHECK(res.filament_map[0] == 1);   // fallback
    CHECK(res.physical_map[0] == -1);
    CHECK(res.filament_map[1] == 1);   // real match
    CHECK(res.physical_map[1] == 1);
}

TEST_CASE("Auto-map on an empty inventory marks every filament unmatched", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools.resize(2); // no physical filaments recorded
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}, {"PETG", "#00FF00", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched == std::vector<int>{0, 1});
    CHECK(res.filament_map == std::vector<int>{1, 1});
    CHECK(res.physical_map == std::vector<int>{-1, -1});
}

TEST_CASE("Auto-map allows two project filaments to merge onto the same tool", "[FilamentInventory][AutoMap]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", ""}, {"PLA", "#FE0101", ""}};
    AutoMapResult res = auto_map_filaments(project, inv, {});
    REQUIRE(res.unmatched.empty());
    CHECK(res.filament_map[0] == 1);
    CHECK(res.filament_map[1] == 1); // legal merge, not an error
    CHECK(res.physical_map[0] == 1);
    CHECK(res.physical_map[1] == 1);
}

TEST_CASE("An assignment entry with a missing field is skipped, not defaulted", "[FilamentInventory][SlotAssignment]") {
    // Missing/invalid fields must not silently coerce to 0 -- that would fabricate a plausible-
    // looking (filament 0, tool 0, slot 0) assignment out of corrupt data. Skip the entry instead.
    std::string json = R"({"My Printer":[{"filament":0,"tool":1},{"filament":-1,"tool":2,"slot":0},
                                       {"tool":1,"slot":0},{"filament":1,"tool":-2,"slot":0},
                                       {"filament":2,"tool":1,"slot":0}]})";
    std::map<std::string, std::vector<SlotAssignment>> back = load_slot_assignments(json);
    REQUIRE(back.count("My Printer") == 1);
    // Only the fully-valid, non-negative entry survives.
    REQUIRE(back["My Printer"].size() == 1);
    CHECK(back["My Printer"][0].filament_idx == 2);
    CHECK(back["My Printer"][0].tool == 1);
    CHECK(back["My Printer"][0].slot == 0);
}

// --- validate_manual_map (slice-time re-validation of a confirmed manual mapping) ---

TEST_CASE("Manual map validator flags a target tool that has since gone empty", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{}}}; // tool 1 loaded red PLA, tool 2 empty
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", ""}};
    // Confirmed onto tool 2 (physical id 2, which no longer exists -- the sync/edit that emptied
    // the tool also drops its old id).
    std::vector<int> filament_map = {2};
    std::vector<int> physical_map = {2};
    auto violations = validate_manual_map(project, filament_map, physical_map, inv);
    REQUIRE(violations.size() == 1);
    CHECK(violations[0].filament_index == 0);
    CHECK(violations[0].kind == ManualMapViolationKind::EmptyTarget);
}

TEST_CASE("Manual map validator flags a bootstrap-style pick (physical_map<=0) onto a now-empty tool", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory inv;
    inv.tools = {{{}}}; // single tool, empty
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", ""}};
    std::vector<int> filament_map = {1};
    std::vector<int> physical_map = {0}; // no physical pick recorded
    auto violations = validate_manual_map(project, filament_map, physical_map, inv);
    REQUIRE(violations.size() == 1);
    CHECK(violations[0].kind == ManualMapViolationKind::EmptyTarget);
}

TEST_CASE("Manual map validator flags a family mismatch against the live inventory", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PETG"}}}; // tool now holds PETG, not what was confirmed
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", ""}};
    std::vector<int> filament_map = {1};
    std::vector<int> physical_map = {1};
    auto violations = validate_manual_map(project, filament_map, physical_map, inv);
    REQUIRE(violations.size() == 1);
    CHECK(violations[0].kind == ManualMapViolationKind::FamilyMismatch);
}

TEST_CASE("Manual map validator flags a gross color mismatch using the shared cutoff", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#0000FF", "PLA"}}}; // blue tool
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", ""}}; // red project filament
    std::vector<int> filament_map = {1};
    std::vector<int> physical_map = {1};
    auto violations = validate_manual_map(project, filament_map, physical_map, inv);
    REQUIRE(violations.size() == 1);
    CHECK(violations[0].kind == ManualMapViolationKind::GrossColorMismatch);
}

TEST_CASE("Manual map validator flags a gross color mismatch even when the target's preset matches the project filament's", "[FilamentInventory][ManualMapValidate]") {
    // The same rule extends to the validator: a matching preset does not exempt a
    // MANUAL map from the color-mismatch confirmation either -- validate_manual_map never reads
    // preset at all, so a preset match here still gets flagged, same as it would with no preset.
    // This test documents that the auto-map and validator paths agree.
    FilamentInventory inv;
    inv.tools = {{{1, "#008080", "PLA"}}}; // teal spool, preset-matching
    inv.tools[0][0].preset = "Snapmaker PLA SnapSpeed";
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", "Snapmaker PLA SnapSpeed"}}; // red project filament
    std::vector<int> filament_map = {1};
    std::vector<int> physical_map = {1};
    auto violations = validate_manual_map(project, filament_map, physical_map, inv);
    REQUIRE(violations.size() == 1);
    CHECK(violations[0].kind == ManualMapViolationKind::GrossColorMismatch);
}

TEST_CASE("Manual map validator passes a clean map through untouched", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}}, {{2, "#00FF00", "PETG"}}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", ""}, {"PETG", "#00FF00", "", ""}};
    std::vector<int> filament_map = {1, 2};
    std::vector<int> physical_map = {1, 2};
    auto violations = validate_manual_map(project, filament_map, physical_map, inv);
    CHECK(violations.empty());
}

TEST_CASE("Manual map validator ignores an unknown color or type rather than flagging it", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory inv;
    PhysicalFilament pf;
    pf.id     = 1;
    pf.preset = "Generic PLA"; // preset known, but color/type not recorded -- not empty()
    inv.tools = {{pf}};
    std::vector<ProjectFilamentInfo> project = {{"PLA", "#FF0000", "", ""}};
    std::vector<int> filament_map = {1};
    std::vector<int> physical_map = {1};
    auto violations = validate_manual_map(project, filament_map, physical_map, inv);
    CHECK(violations.empty());
}

// --- inventory_confirmation_fingerprint ---

TEST_CASE("Inventory fingerprint is stable across calls on the same inventory", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory inv;
    inv.tools = {{{1, "#FF0000", "PLA"}, {5, "#FFFF00", "PLA"}}, {{2, "#FFFFFF", "PETG"}}};
    CHECK(inventory_confirmation_fingerprint(inv) == inventory_confirmation_fingerprint(inv));
}

TEST_CASE("Inventory fingerprint ignores id, only the confirmed content", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory a, b;
    a.tools = {{{1, "#FF0000", "PLA"}}};
    b.tools = {{{99, "#FF0000", "PLA"}}}; // same content, different id (e.g. re-numbered on save)
    CHECK(inventory_confirmation_fingerprint(a) == inventory_confirmation_fingerprint(b));
}

TEST_CASE("Inventory fingerprint changes when a slot's color changes", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory a, b;
    a.tools = {{{1, "#FF0000", "PLA"}}};
    b.tools = {{{1, "#00FF00", "PLA"}}};
    CHECK(inventory_confirmation_fingerprint(a) != inventory_confirmation_fingerprint(b));
}

TEST_CASE("Inventory fingerprint changes when a slot's type changes", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory a, b;
    a.tools = {{{1, "#FF0000", "PLA"}}};
    b.tools = {{{1, "#FF0000", "PETG"}}};
    CHECK(inventory_confirmation_fingerprint(a) != inventory_confirmation_fingerprint(b));
}

TEST_CASE("Inventory fingerprint changes when a tool goes from loaded to empty", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory a, b;
    a.tools = {{{1, "#FF0000", "PLA"}}};
    b.tools = {{{}}}; // same slot, now empty
    CHECK(inventory_confirmation_fingerprint(a) != inventory_confirmation_fingerprint(b));
}

TEST_CASE("Inventory fingerprint changes when the tool count changes", "[FilamentInventory][ManualMapValidate]") {
    FilamentInventory a, b;
    a.tools = {{{1, "#FF0000", "PLA"}}};
    b.tools = {{{1, "#FF0000", "PLA"}}, {{}}};
    CHECK(inventory_confirmation_fingerprint(a) != inventory_confirmation_fingerprint(b));
}

// --- confirmation metadata dump/load ---

TEST_CASE("Manual map confirmations round-trip device key, plate index, and fingerprint", "[FilamentInventory][ManualMapValidate]") {
    std::map<std::string, std::map<int, std::string>> by_device_plate;
    by_device_plate["My Printer"][0] = "deadbeef";
    by_device_plate["My Printer"][2] = "cafef00d";
    std::string dumped = dump_manual_map_confirmations(by_device_plate);
    auto back = load_manual_map_confirmations(dumped);
    REQUIRE(back.count("My Printer") == 1);
    REQUIRE(back["My Printer"].size() == 2);
    CHECK(back["My Printer"][0] == "deadbeef");
    CHECK(back["My Printer"][2] == "cafef00d");
}

TEST_CASE("Manual map confirmations tolerate malformed JSON", "[FilamentInventory][ManualMapValidate]") {
    CHECK(load_manual_map_confirmations("{not json").empty());
    CHECK(load_manual_map_confirmations("[1,2,3]").empty());
    CHECK(load_manual_map_confirmations("").empty());
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
    // Unknown on either side is not compatible -- callers gate that case themselves.
    CHECK_FALSE(type_compatible("", "PLA"));
    CHECK_FALSE(type_compatible("PLA", ""));
}
