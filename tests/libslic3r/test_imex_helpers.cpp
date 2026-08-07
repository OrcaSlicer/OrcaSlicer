#include <catch2/catch_all.hpp>

#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

static ConfigOptionInts make_pem(std::vector<int> v) {
    ConfigOptionInts o;
    o.values = std::move(v);
    return o;
}

TEST_CASE("effective_physical_extruder_map - authored map wins", "[IMEX]") {
    // An AFC manifold on a 7-extruder machine: logical 0-3 all feed physical 0. Not derivable,
    // so it must be honoured verbatim.
    auto explicit_pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    auto out = effective_physical_extruder_map(&explicit_pem, 7);
    REQUIRE(out.values == std::vector<int>{0, 0, 0, 0, 1, 2, 3});
}

TEST_CASE("effective_physical_extruder_map - a permutation is honoured", "[IMEX]") {
    // Shipping BBL dual-nozzle profiles author {1,0}: a slicer/firmware numbering swap, not a
    // sharing map. Deriving over it would silently renumber both extruders.
    auto explicit_pem = make_pem({1, 0});
    auto out = effective_physical_extruder_map(&explicit_pem, 2);
    REQUIRE(out.values == std::vector<int>{1, 0});
}

TEST_CASE("effective_physical_extruder_map - unauthored derives the identity", "[IMEX]") {
    // The PrintConfig default is a single element, which is not an authored map on a 2-extruder
    // machine. Identity is what upstream itself falls back to.
    auto default_pem = make_pem({0});
    auto out = effective_physical_extruder_map(&default_pem, 2);
    REQUIRE(out.values == std::vector<int>{0, 1});
}

TEST_CASE("effective_physical_extruder_map - result is always one entry per extruder", "[IMEX]") {
    // The defining property: consumers index this map by logical extruder and size their own
    // arrays from nozzle_diameter, so a shorter map is an out-of-bounds read in several of them.
    for (int n : { 1, 2, 4, 7 }) {
        DYNAMIC_SECTION("nozzle count " << n) {
            REQUIRE((int) effective_physical_extruder_map(nullptr, n).values.size() == n);
            auto stale = make_pem({0});   // wrong length: must not be mistaken for authored
            REQUIRE((int) effective_physical_extruder_map(&stale, n).values.size() == n);
        }
    }
}

TEST_CASE("effective_physical_extruder_map - degenerate nozzle count still yields a usable map", "[IMEX]") {
    // Never hand back an empty map: several consumers index it as a raw vector.
    REQUIRE(effective_physical_extruder_map(nullptr, 0).values == std::vector<int>{0});
}

TEST_CASE("effective_physical_extruder_map - IDEX ghost-color regression guard", "[IMEX]") {
    // Dual extruder, no authored map. Before the GUI fix this produced a black ghost on T1
    // because first_filament_for_physical_head({0}, 1) == -1.
    auto default_pem = make_pem({0});
    auto pem         = effective_physical_extruder_map(&default_pem, 2);
    REQUIRE(first_filament_for_physical_head(pem, 0) == 0);
    REQUIRE(first_filament_for_physical_head(pem, 1) == 1); // no longer -1
}

TEST_CASE("imex_pem_tool_for - non-parallel mode returns -1", "[IMEX]") {
    // Empty mode string (not in IMEX) and "primary" (IMEX present but not parallel) both
    // short-circuit to bare-firmware PA/temp emission.
    auto pem = make_pem({0, 1, 2});
    REQUIRE(imex_pem_tool_for(0, "",        pem) == -1);
    REQUIRE(imex_pem_tool_for(1, "primary", pem) == -1);
    REQUIRE(imex_pem_tool_for(2, "primary", pem) == -1);
}

TEST_CASE("imex_pem_tool_for - parallel mode with empty pem returns -1", "[IMEX]") {
    // Defense-in-depth for exotic profiles where pem never gets populated. get_at would
    // throw on an empty pem; the helper must return -1 instead so the writer emits bare.
    ConfigOptionInts empty_pem;
    REQUIRE(imex_pem_tool_for(0, "copy_mode",   empty_pem) == -1);
    REQUIRE(imex_pem_tool_for(3, "mirror_mode", empty_pem) == -1);
}

TEST_CASE("imex_pem_tool_for - identity pem routes filament to itself", "[IMEX]") {
    // IDEX with printer_extruder_id = [1, 2] auto-derives pem = [0, 1]; non-MMU is identity.
    auto pem = make_pem({0, 1});
    REQUIRE(imex_pem_tool_for(0, "copy_mode", pem) == 0);
    REQUIRE(imex_pem_tool_for(1, "copy_mode", pem) == 1);
}

TEST_CASE("imex_pem_tool_for - MMU collapse routes multiple logical slots to one physical", "[IMEX]") {
    // 7-slot printer: first four slots share physical extruder 0 (a 4-lane MMU),
    // slots 4/5/6 are independent on 1/2/3. Filament index is the logical slot;
    // the helper returns the physical extruder carrying it.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    REQUIRE(imex_pem_tool_for(0, "copy_mode", pem) == 0);
    REQUIRE(imex_pem_tool_for(3, "copy_mode", pem) == 0); // MMU collapse: T3 logical → T0 physical
    REQUIRE(imex_pem_tool_for(6, "copy_mode", pem) == 3);
}

TEST_CASE("imex_suppresses_bare_toolchange - non-IMEX printer never suppresses", "[IMEX]") {
    REQUIRE_FALSE(imex_suppresses_bare_toolchange("", 0));
    REQUIRE_FALSE(imex_suppresses_bare_toolchange("", 1));
    REQUIRE_FALSE(imex_suppresses_bare_toolchange("", 99));
}

TEST_CASE("imex_suppresses_bare_toolchange - Primary mode never suppresses (preserves AFC lane swap)", "[IMEX]") {
    REQUIRE_FALSE(imex_suppresses_bare_toolchange("primary", 0));
    REQUIRE_FALSE(imex_suppresses_bare_toolchange("primary", 1));
    REQUIRE_FALSE(imex_suppresses_bare_toolchange("primary", 50));
}

TEST_CASE("imex_suppresses_bare_toolchange - parallel mode suppresses at print-start", "[IMEX]") {
    // count == 0 in the single-extruder path (which never increments) and == 1 in
    // the long multi-extruder path's first call after the increment both represent
    // the print-start initial-tool select. Either should suppress so the user's
    // mode_gcode-emitted T<n> isn't duplicated.
    REQUIRE(imex_suppresses_bare_toolchange("copy_mode",   0));
    REQUIRE(imex_suppresses_bare_toolchange("copy_mode",   1));
    REQUIRE(imex_suppresses_bare_toolchange("mirror_mode", 0));
    REQUIRE(imex_suppresses_bare_toolchange("iq-copy",     1));
}

TEST_CASE("imex_suppresses_bare_toolchange - parallel mode allows mid-print toolchange", "[IMEX]") {
    // Mid-print toolchanges (count > 1) emit normally. Print::validate() blocks
    // multi-color setups where mid-print T<n> wouldn't make sense; the only
    // remaining case is IQEX with 2+ tools active on the primary gantry, where
    // the firmware handles the slaved gantry automatically.
    REQUIRE_FALSE(imex_suppresses_bare_toolchange("copy_mode",   2));
    REQUIRE_FALSE(imex_suppresses_bare_toolchange("mirror_mode", 5));
    REQUIRE_FALSE(imex_suppresses_bare_toolchange("backup_mode", 100));
}

TEST_CASE("imex_multicolor_block_reason - non-IMEX prints never block", "[IMEX]") {
    auto pem = make_pem({0, 1, 2, 3});
    REQUIRE(imex_multicolor_block_reason("",        "0:P,1:C", 2, {0, 1}, pem).empty());
    REQUIRE(imex_multicolor_block_reason("primary", "0:P,1:C", 2, {0, 1}, pem).empty());
}

TEST_CASE("imex_multicolor_block_reason - single-color prints never block", "[IMEX]") {
    auto pem = make_pem({0, 1, 2, 3});
    REQUIRE(imex_multicolor_block_reason("copy",   "0:P,1:C,2:M,3:M", 2, {0}, pem).empty());
    REQUIRE(imex_multicolor_block_reason("mirror", "0:P,1:C,2:M,3:M", 2, {},  pem).empty());
}

TEST_CASE("imex_multicolor_block_reason - IDEX (1 tool per gantry) blocks multi-color", "[IMEX]") {
    // Two physical heads, each on its own gantry: tools_per_gantry=1, primary=T0
    // on gantry 0, copy=T1 on gantry 1. Primary's gantry can never carry a Span
    // partner (only 1 tool slot), so multicolor in this mode is never declarable.
    auto pem = make_pem({0, 1});
    const std::string reason = imex_multicolor_block_reason("copy", "0:P,1:C", 1, {0, 1}, pem);
    REQUIRE_FALSE(reason.empty());
    REQUIRE_THAT(reason, Catch::Matchers::ContainsSubstring("Span tool"));
}

TEST_CASE("imex_multicolor_block_reason - single-gantry IMEX mode blocks multi-color", "[IMEX]") {
    // 2x2 IQEX, mode "0:P,1:C" — T0 and T1 both sit on gantry 0 (since
    // tools_per_gantry=2). The mode label says "copy" but there's no second
    // gantry being copied to — this is a confused configuration. Multi-color
    // here is just a regular multi-tool single-gantry print, not an IMEX
    // parallel print. Block before the slicer wastes effort emitting parallel-
    // print firmware setup that doesn't apply.
    auto pem = make_pem({0, 1, 2, 3});
    const std::string reason = imex_multicolor_block_reason("copy", "0:P,1:C", 2, {0, 1}, pem);
    REQUIRE_FALSE(reason.empty());
    REQUIRE_THAT(reason, Catch::Matchers::ContainsSubstring("single gantry"));
}

TEST_CASE("imex_multicolor_block_reason - IQEX 2-tool-active mode blocks multi-color", "[IMEX]") {
    // 2x2 IQEX, mode has T0 primary + T2 copy (one tool per gantry, different
    // gantries). No Span on primary's gantry → multicolor partner not declared. Block.
    auto pem = make_pem({0, 1, 2, 3});
    const std::string reason = imex_multicolor_block_reason("copy", "0:P,2:C", 2, {0, 2}, pem);
    REQUIRE_FALSE(reason.empty());
    REQUIRE_THAT(reason, Catch::Matchers::ContainsSubstring("Span tool"));
}

TEST_CASE("imex_multicolor_block_reason - IQEX 4-tool independent copies block multi-color", "[IMEX]") {
    // 2x2 IQEX, T0:P,T1:C,T2:M,T3:M — user's real-world 4-independent-copies job.
    // No Span on primary's gantry: T1 is an independent copy, not a multicolor
    // partner, so multicolor here would be incoherent (T1 prints its own object,
    // it can't sync color changes with T0). Block.
    auto pem = make_pem({0, 1, 2, 3});
    const std::string reason = imex_multicolor_block_reason("copy", "0:P,1:C,2:M,3:M", 2, {0, 1}, pem);
    REQUIRE_FALSE(reason.empty());
    REQUIRE_THAT(reason, Catch::Matchers::ContainsSubstring("Span tool"));
}

TEST_CASE("imex_multicolor_block_reason - IQEX paired-gantry multicolor allowed with Span", "[IMEX]") {
    // 2x2 IQEX, T0:P,T1:S,T2:M,T3:M — Span on T1 declares the within-gantry multicolor
    // partner; T2/T3 mirror with column-pairing T2↔T0, T3↔T1. The slaved gantry can
    // follow the primary's mid-print T0↔T1 toolchange because both colors live on the
    // same gantry. Allowed.
    auto pem = make_pem({0, 1, 2, 3});
    REQUIRE(imex_multicolor_block_reason("copy", "0:P,1:S,2:M,3:M", 2, {0, 1}, pem).empty());
}

TEST_CASE("imex_multicolor_block_reason - MMU lane sharing blocks multi-color", "[IMEX]") {
    // pem maps both filament 0 and filament 1 to the same physical head 0 — that's
    // an MMU/AFC manifold. IMEX parallel modes can't slave the secondary gantry
    // through an MMU lane swap, so block.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    const std::string reason = imex_multicolor_block_reason("copy", "0:P,1:C,2:M,3:M", 2, {0, 1}, pem);
    REQUIRE_FALSE(reason.empty());
    REQUIRE_THAT(reason, Catch::Matchers::ContainsSubstring("physical extruder map"));
}

TEST_CASE("imex_multicolor_block_reason - mode without primary blocks", "[IMEX]") {
    auto pem = make_pem({0, 1, 2, 3});
    const std::string reason = imex_multicolor_block_reason("copy", "1:C,2:M", 2, {0, 1}, pem);
    REQUIRE_FALSE(reason.empty());
    REQUIRE_THAT(reason, Catch::Matchers::ContainsSubstring("primary tool"));
}

TEST_CASE("first_filament_for_physical_head - identity pem", "[IMEX]") {
    auto pem = make_pem({0, 1, 2, 3});
    REQUIRE(first_filament_for_physical_head(pem, 0) == 0);
    REQUIRE(first_filament_for_physical_head(pem, 1) == 1);
    REQUIRE(first_filament_for_physical_head(pem, 2) == 2);
    REQUIRE(first_filament_for_physical_head(pem, 3) == 3);
    REQUIRE(first_filament_for_physical_head(pem, 4) == -1);
}

TEST_CASE("first_filament_for_physical_head - AFC routing", "[IMEX]") {
    // User's IQEX: 4 AFC lanes on T0, direct extruders on T1, T2, T3
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    REQUIRE(first_filament_for_physical_head(pem, 0) == 0); // first of T0's lanes
    REQUIRE(first_filament_for_physical_head(pem, 1) == 4);
    REQUIRE(first_filament_for_physical_head(pem, 2) == 5);
    REQUIRE(first_filament_for_physical_head(pem, 3) == 6);
    REQUIRE(first_filament_for_physical_head(pem, 5) == -1); // no head 5
}

TEST_CASE("first_filament_for_physical_head - empty pem", "[IMEX]") {
    ConfigOptionInts pem;
    REQUIRE(first_filament_for_physical_head(pem, 0) == 0);
    REQUIRE(first_filament_for_physical_head(pem, 1) == -1);
}

TEST_CASE("has_mmu - pure IDEX", "[IMEX]") {
    REQUIRE_FALSE(has_mmu(make_pem({0, 1})));
    REQUIRE_FALSE(has_mmu(make_pem({0, 1, 2, 3})));
}

TEST_CASE("has_mmu - MMU on one head", "[IMEX]") {
    REQUIRE(has_mmu(make_pem({0, 0, 0, 0, 1, 2, 3}))); // user's IQEX
    REQUIRE(has_mmu(make_pem({0, 0}))); // tiny MMU
}

TEST_CASE("has_mmu - MMU on second head", "[IMEX]") {
    // Hypothetical future: direct extruder on T0, MMU on T1
    REQUIRE(has_mmu(make_pem({0, 1, 1, 1})));
}

TEST_CASE("has_mmu - empty / single-entry pem", "[IMEX]") {
    REQUIRE_FALSE(has_mmu(make_pem({})));
    REQUIRE_FALSE(has_mmu(make_pem({0})));
}

TEST_CASE("imex_primary_logical_from_objects - AFC primary picks the object's slot", "[IMEX]") {
    // User's Neo XP 0.6: pem maps slots 0-3 to physical 0 (4-lane AFC manifold),
    // slots 4-6 to physicals 1/2/3.  Object assigned to 1-based slot 3 = 0-based 2.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    REQUIRE(imex_primary_logical_from_objects({3}, pem, 0) == 2);
}

TEST_CASE("imex_primary_logical_from_objects - multi-color AFC primary returns first match", "[IMEX]") {
    // Two objects on the AFC manifold (slots 0 and 2 in 1-based = slots 0 and 2 in
    // 0-based wait that's wrong let me redo).  Two objects: 1-based slots 1 and 3
    // (= 0-based 0 and 2).  Both route to physical 0 via pem.  First in input wins.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    REQUIRE(imex_primary_logical_from_objects({1, 3}, pem, 0) == 0); // first match
    REQUIRE(imex_primary_logical_from_objects({3, 1}, pem, 0) == 2); // order matters
}

TEST_CASE("imex_primary_logical_from_objects - direct extruder primary unambiguous", "[IMEX]") {
    // Object on 1-based slot 5 = 0-based 4 (direct extruder T1 in the user's layout).
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    REQUIRE(imex_primary_logical_from_objects({5}, pem, 1) == 4);
}

TEST_CASE("imex_primary_logical_from_objects - no object routed to primary returns -1", "[IMEX]") {
    // Object on 1-based slot 5 (= physical 1) but primary_physical is 0.  No object
    // on the plate routes to T0 — caller should fall back / treat as missing.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    REQUIRE(imex_primary_logical_from_objects({5}, pem, 0) == -1);
}

TEST_CASE("imex_primary_logical_from_objects - empty inputs", "[IMEX]") {
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    REQUIRE(imex_primary_logical_from_objects({}, pem, 0) == -1);  // no objects
    ConfigOptionInts empty_pem;
    REQUIRE(imex_primary_logical_from_objects({1, 3}, empty_pem, 0) == -1); // empty pem
}

TEST_CASE("imex_secondary_logical_slots - copy mode skips primary, falls back to first-routed", "[IMEX]") {
    // User's setup: copy mode active = [0, 1] (T0 primary, T1 copy), no plate map override.
    // Secondary T1 should resolve to first slot whose pem is 1 = slot 4.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    auto out = imex_secondary_logical_slots({0, 1}, /*primary*/0, /*plate_map*/{}, pem);
    REQUIRE(out == std::vector<int>{4});
}

TEST_CASE("imex_secondary_logical_slots - IQEX 4-mode enumerates all secondaries", "[IMEX]") {
    // iq-copy / iq-mirror: active = [0, 1, 2, 3], primary = 0.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    auto out = imex_secondary_logical_slots({0, 1, 2, 3}, /*primary*/0, /*plate_map*/{}, pem);
    REQUIRE(out == std::vector<int>{4, 5, 6}); // first slot for each physical 1, 2, 3
}

TEST_CASE("imex_secondary_logical_slots - per-plate override wins for secondary", "[IMEX]") {
    // User picks slot 6 (1-based) for T1 via the IMEX ghost picker.  plate_map[1] = 6.
    // resolve_filament_for_head should subtract 1: 0-based slot 5.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    std::map<int, int> plate_map{{1, 6}};
    auto out = imex_secondary_logical_slots({0, 1}, /*primary*/0, plate_map, pem);
    REQUIRE(out == std::vector<int>{5});
}

TEST_CASE("imex_secondary_logical_slots - drops unrouted physicals, deduplicates", "[IMEX]") {
    // pem only has 2 entries (slot 0 -> phys 0, slot 1 -> phys 1).  active includes
    // a phys 2 that has no logical → should be dropped.  Also: same pem has both
    // slots routing to the same physical to test dedup.
    auto pem = make_pem({0, 1});
    auto out = imex_secondary_logical_slots({0, 1, 2}, /*primary*/0, {}, pem);
    REQUIRE(out == std::vector<int>{1}); // phys 2 unrouted, primary skipped, only phys 1's slot 1 left

    // Dedup: two physicals resolving to the same logical (via plate_map override).
    std::map<int, int> dup_map{{1, 1}, {2, 1}}; // both T1 and T2 → 1-based slot 1 = 0-based 0
    auto pem2 = make_pem({0, 0, 0});
    auto out2 = imex_secondary_logical_slots({0, 1, 2}, /*primary*/0, dup_map, pem2);
    REQUIRE(out2 == std::vector<int>{0}); // both secondaries point at slot 0; only emitted once
}

TEST_CASE("imex_secondary_logical_slots - only-primary-active returns empty", "[IMEX]") {
    // Primary mode (just T0 active) → no secondaries.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    auto out = imex_secondary_logical_slots({0}, /*primary*/0, {}, pem);
    REQUIRE(out.empty());
}

TEST_CASE("parse_imex_head_filament_map - round-trip", "[IMEX]") {
    auto m = parse_imex_head_filament_map("0:3,4:5");
    REQUIRE(m.size() == 2);
    REQUIRE(m[0] == 3);
    REQUIRE(m[4] == 5);
}

TEST_CASE("parse_imex_head_filament_map - whitespace + empty tokens", "[IMEX]") {
    auto m = parse_imex_head_filament_map("  0 : 3 , , 4:5  ");
    REQUIRE(m.size() == 2);
    REQUIRE(m[0] == 3);
    REQUIRE(m[4] == 5);
}

TEST_CASE("parse_imex_head_filament_map - empty string", "[IMEX]") {
    REQUIRE(parse_imex_head_filament_map("").empty());
}

TEST_CASE("resolve_filament_for_head - override wins", "[IMEX]") {
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    std::map<int,int> plate_map{{0, 3}}; // 1-based slot 3 = 0-based logical 2
    REQUIRE(resolve_filament_for_head(plate_map, pem, 0) == 2);
}

TEST_CASE("resolve_filament_for_head - fallback when unset", "[IMEX]") {
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    std::map<int,int> plate_map{}; // empty
    REQUIRE(resolve_filament_for_head(plate_map, pem, 0) == 0); // first pem-routed
    REQUIRE(resolve_filament_for_head(plate_map, pem, 1) == 4);
}

TEST_CASE("resolve_filament_for_head - no routing for head", "[IMEX]") {
    auto pem = make_pem({0, 1}); // no head 2
    std::map<int,int> plate_map{};
    REQUIRE(resolve_filament_for_head(plate_map, pem, 2) == -1);
}

TEST_CASE("imex_primary_tool_for_mode - role marker authoritative", "[IMEX]") {
    // Tab.cpp enforces one Primary per mode; position is not semantically meaningful.
    REQUIRE(imex_primary_tool_for_mode("0:P,1:C,2:C") == 0);
    REQUIRE(imex_primary_tool_for_mode("1:C,0:P,2:M") == 0);
    REQUIRE(imex_primary_tool_for_mode("1:C,2:M,3:P") == 3);
}

TEST_CASE("imex_primary_tool_for_mode - backwards-compat plain index", "[IMEX]") {
    REQUIRE(imex_primary_tool_for_mode("0") == 0);
    REQUIRE(imex_primary_tool_for_mode("2") == 2);
    // Bare index wins when no :P marker is present; first bare index takes primary.
    REQUIRE(imex_primary_tool_for_mode("1,2,3") == 1);
}

TEST_CASE("imex_primary_tool_for_mode - explicit :P beats bare index", "[IMEX]") {
    // Mixed serialization: role marker must dominate over bare-index fallback.
    REQUIRE(imex_primary_tool_for_mode("1,2:P,3") == 2);
}

TEST_CASE("imex_primary_tool_for_mode - empty + malformed", "[IMEX]") {
    REQUIRE(imex_primary_tool_for_mode("") == -1);
    REQUIRE(imex_primary_tool_for_mode(",,") == -1);
    REQUIRE(imex_primary_tool_for_mode("abc:P") == -1); // idx not parseable
    REQUIRE(imex_primary_tool_for_mode("-1:P") == -1);   // negative idx rejected
}

TEST_CASE("imex_primary_tool_for_mode - whitespace tolerant", "[IMEX]") {
    REQUIRE(imex_primary_tool_for_mode("  0 : P , 1 : C ") == 0);
}

TEST_CASE("has_non_primary_mmu - MMU on primary only", "[IMEX]") {
    // User's Neo XP 0.6: 4 AFC lanes on T0, singles on T4/T5/T6.
    // Primary = 0 → no other head has MMU.
    auto pem = make_pem({0, 0, 0, 0, 4, 5, 6});
    REQUIRE_FALSE(has_non_primary_mmu(pem, 0));
}

TEST_CASE("has_non_primary_mmu - MMU on secondary", "[IMEX]") {
    // Direct extruder on T0, MMU on T1 → non-primary MMU present.
    auto pem = make_pem({0, 1, 1, 1});
    REQUIRE(has_non_primary_mmu(pem, 0));
}

TEST_CASE("has_non_primary_mmu - pure IDEX", "[IMEX]") {
    REQUIRE_FALSE(has_non_primary_mmu(make_pem({0, 1}), 0));
    REQUIRE_FALSE(has_non_primary_mmu(make_pem({0, 1, 2, 3}), 0));
}

TEST_CASE("has_non_primary_mmu - secondary single-lane", "[IMEX]") {
    // Primary is T4 (single filament); T0 has MMU.
    auto pem = make_pem({0, 0, 0, 0, 4});
    REQUIRE(has_non_primary_mmu(pem, 4));
}

TEST_CASE("has_non_primary_mmu - empty / single-entry pem", "[IMEX]") {
    REQUIRE_FALSE(has_non_primary_mmu(make_pem({}), 0));
    REQUIRE_FALSE(has_non_primary_mmu(make_pem({0}), 0));
}

TEST_CASE("imex_head_transform - copy mode is pure translation", "[IMEX]") {
    const Vec2d offset{120.0, 0.0};
    Transform3d xf = imex_head_transform(0, 1, ImexRole::Copy, offset, Vec2d::Zero(), ImexMirrorAxis::X);
    const Vec3d in{10.0, 20.0, 30.0};
    const Vec3d out = xf * in;
    REQUIRE_THAT(out.x(), WithinAbs(130.0, 1e-9));
    REQUIRE_THAT(out.y(), WithinAbs(20.0,  1e-9));
    REQUIRE_THAT(out.z(), WithinAbs(30.0,  1e-9));
}

TEST_CASE("imex_head_transform - mirror at origin places ghost at gantry offset", "[IMEX]") {
    // Primary instance at origin: ghost origin lands at the gantry offset (Copy-style),
    // and applying mirror flips geometry about origin (= primary's translation).
    const Vec2d offset{120.0, 0.0};
    Transform3d xf = imex_head_transform(0, 1, ImexRole::Mirror, offset, Vec2d::Zero(), ImexMirrorAxis::X);
    const Vec3d mapped = xf * Vec3d::Zero();
    REQUIRE_THAT(mapped.x(), WithinAbs(120.0, 1e-9));
    REQUIRE_THAT(mapped.y(), WithinAbs(0.0,   1e-9));
    REQUIRE_THAT(mapped.z(), WithinAbs(0.0,   1e-9));
}

TEST_CASE("imex_head_transform - mirror lands ghost at reflected position in target zone", "[IMEX]") {
    // User's example: primary zone is 100x100 centered at (50, 50), primary at (80, 20),
    // gantry offset (100, 0) places the mirror zone centered at (150, 50). Ghost origin
    // should land at the reflection of the primary through the zone-boundary plane
    // (x = 100), i.e. world-space (120, 20, 0) — NOT the Copy-style (180, 20, 0).
    const Vec2d offset{100.0, 0.0};
    const Vec2d primary_zone_center{50.0, 50.0};
    Transform3d xf = imex_head_transform(0, 1, ImexRole::Mirror, offset, primary_zone_center, ImexMirrorAxis::X);

    const Vec3d primary{80.0, 20.0, 0.0};
    const Vec3d ghost_origin = xf * primary;
    REQUIRE_THAT(ghost_origin.x(), WithinAbs(120.0, 1e-9));
    REQUIRE_THAT(ghost_origin.y(), WithinAbs(20.0,  1e-9));

    // Model point at primary + (+5 X) lands 5 LEFT of ghost origin (geometry still flipped).
    const Vec3d mapped = xf * (primary + Vec3d(5.0, 0.0, 0.0));
    REQUIRE_THAT(mapped.x(), WithinAbs(115.0, 1e-9));
    REQUIRE_THAT(mapped.y(), WithinAbs(20.0,  1e-9));
}

TEST_CASE("imex_head_transform - mirror reflects primary drag motion", "[IMEX]") {
    // Dragging the primary must reflect the ghost across the zone-boundary plane:
    // primary +X → ghost -X (mirrored), primary +Y → ghost +Y (1:1). Without this the
    // ghost stops being a true mirror once the primary moves.
    const Vec2d offset{120.0, 0.0};
    const Vec2d primary_zone_center{60.0, 50.0};
    Transform3d xf = imex_head_transform(0, 1, ImexRole::Mirror, offset, primary_zone_center, ImexMirrorAxis::X);

    const Vec3d p0{10.0, 20.0, 0.0};
    const Vec3d p1{40.0, 15.0, 0.0};
    const Vec3d ghost0 = xf * p0;
    const Vec3d ghost1 = xf * p1;
    const Vec3d ghost_delta   = ghost1 - ghost0;
    const Vec3d primary_delta = p1 - p0;

    REQUIRE_THAT(ghost_delta.x(), WithinAbs(-primary_delta.x(), 1e-9)); // X inverted
    REQUIRE_THAT(ghost_delta.y(), WithinAbs( primary_delta.y(), 1e-9)); // Y 1:1
    REQUIRE_THAT(ghost_delta.z(), WithinAbs( primary_delta.z(), 1e-9));
}

TEST_CASE("imex_head_transform - mirror reflects model point across primary origin", "[IMEX]") {
    // Primary at origin, offset +X. Model point at +5 X lands 5 left of ghost origin.
    // Matches the pre-refactor semantics for the special case primary_origin = 0.
    const Vec2d offset{120.0, 0.0};
    Transform3d xf = imex_head_transform(0, 1, ImexRole::Mirror, offset, Vec2d::Zero(), ImexMirrorAxis::X);
    const Vec3d in{5.0, 7.0, 0.0};
    const Vec3d out = xf * in;
    REQUIRE_THAT(out.x(), WithinAbs(115.0, 1e-9));
    REQUIRE_THAT(out.y(), WithinAbs(7.0,   1e-9));
}

TEST_CASE("imex_head_transform - mirror axis comes from the caller, not the offset direction", "[IMEX]") {
    // The reflection plane normal is caller-supplied, never inferred from
    // gantry_offset.normalized(). A diagonal target (different column AND different gantry)
    // has offset components on both axes, so the vector alone cannot pick an axis.
    const Vec2d offset{0.0, 80.0};
    const Vec3d in{3.0, 10.0, 0.0};

    const Vec3d as_x = imex_head_transform(0, 1, ImexRole::Mirror, offset, Vec2d::Zero(),
                                           ImexMirrorAxis::X) * in;
    REQUIRE_THAT(as_x.x(), WithinAbs(-3.0, 1e-9));  // X flipped about origin
    REQUIRE_THAT(as_x.y(), WithinAbs(90.0, 1e-9));  // Y translated by gantry, unflipped

    const Vec3d as_y = imex_head_transform(0, 1, ImexRole::Mirror, offset, Vec2d::Zero(),
                                           ImexMirrorAxis::Y) * in;
    REQUIRE_THAT(as_y.x(), WithinAbs(3.0,  1e-9));  // X tracks 1:1
    REQUIRE_THAT(as_y.y(), WithinAbs(70.0, 1e-9));  // Y flipped about origin, then translated
}

TEST_CASE("imex_head_transform - cross-gantry mirror reflects Y and tracks X", "[IMEX]") {
    // Two-gantry machine: gantry 1's zone sits in FRONT of the primary's, stacked along Y.
    // The part that comes off it is a Y-reflection of the tool directly behind it, so the
    // mirror plane is the horizontal boundary between the two row strips.
    // Primary zone centered (50,150), target (50,50) → boundary at y = 100.
    const Vec2d offset{0.0, -100.0};
    const Vec2d primary_zone_center{50.0, 150.0};
    Transform3d xf = imex_head_transform(0, 2, ImexRole::Mirror, offset, primary_zone_center,
                                         ImexMirrorAxis::Y);

    // Primary at (20,130) reflects through y=100 to (20,70): X unchanged, Y mirrored.
    const Vec3d primary{20.0, 130.0, 0.0};
    const Vec3d ghost_origin = xf * primary;
    REQUIRE_THAT(ghost_origin.x(), WithinAbs(20.0, 1e-9));
    REQUIRE_THAT(ghost_origin.y(), WithinAbs(70.0, 1e-9));

    // Geometry is flipped in Y: a model point +5 in Y lands 5 BELOW the ghost origin.
    const Vec3d mapped = xf * (primary + Vec3d(0.0, 5.0, 0.0));
    REQUIRE_THAT(mapped.y(), WithinAbs(65.0, 1e-9));

    // Drag: primary +X → ghost +X (1:1), primary +Y → ghost -Y (mirrored).
    const Vec3d d = (xf * (primary + Vec3d(7.0, 3.0, 0.0))) - ghost_origin;
    REQUIRE_THAT(d.x(), WithinAbs( 7.0, 1e-9));
    REQUIRE_THAT(d.y(), WithinAbs(-3.0, 1e-9));
}

TEST_CASE("imex_head_transform - diagonal cross-gantry mirror translates X, reflects Y", "[IMEX]") {
    // T3 on a 2x2: different column AND different gantry. It mirrors the tool directly
    // behind it (T1), so it is a Y-reflection translated into its own column — NOT a
    // double flip, which would compose to a 180° rotation and print an unmirrored part.
    const Vec2d offset{200.0, -100.0};
    const Vec2d primary_zone_center{100.0, 150.0};
    Transform3d xf = imex_head_transform(0, 3, ImexRole::Mirror, offset, primary_zone_center,
                                         ImexMirrorAxis::Y);

    const Vec3d primary{40.0, 130.0, 0.0};
    const Vec3d ghost = xf * primary;
    REQUIRE_THAT(ghost.x(), WithinAbs(240.0, 1e-9));  // 40 + 200: translated, not flipped
    REQUIRE_THAT(ghost.y(), WithinAbs(70.0,  1e-9));  // reflected through y=100
}

TEST_CASE("imex_head_transform - mirror is a reflection, not a rotation, on both axes", "[IMEX]") {
    // det = -1 means chirality flips: an asymmetric part comes off the mirror tool as a
    // true mirror image. A 180° rotation (diag(-1,-1,1)) has det = +1 and would print the
    // primary's part merely turned around — a different physical result.
    const Vec2d offset{120.0, -80.0};
    for (ImexMirrorAxis axis : {ImexMirrorAxis::X, ImexMirrorAxis::Y}) {
        Transform3d xf = imex_head_transform(0, 1, ImexRole::Mirror, offset, Vec2d{10.0, 20.0}, axis);
        REQUIRE_THAT(xf.linear().determinant(), WithinAbs(-1.0, 1e-9));
    }
}

TEST_CASE("imex_head_transform - primary is identity", "[IMEX]") {
    const Vec2d offset{120.0, 30.0};
    Transform3d xf = imex_head_transform(0, 0, ImexRole::Primary, offset, Vec2d::Zero(), ImexMirrorAxis::X);
    REQUIRE(xf.isApprox(Transform3d::Identity()));
}

TEST_CASE("imex_head_transform - mirror with zero offset is identity", "[IMEX]") {
    const Vec2d offset{0.0, 0.0};
    Transform3d xf = imex_head_transform(0, 1, ImexRole::Mirror, offset, Vec2d::Zero(), ImexMirrorAxis::X);
    REQUIRE(xf.isApprox(Transform3d::Identity()));
}

TEST_CASE("imex_head_transform - mirror on 2x2 off-row target reflects Y, not X", "[IMEX]") {
    // Supersedes an earlier test that asserted the diagonal target "flips X only". T3 is on
    // the OTHER gantry, so it mirrors the tool directly in front of/behind it — a Y
    // reflection — and merely translates in X into its own column. Reflecting X here is what
    // stacked T2 and T3 on the same marker position and mirrored the ghosts on the wrong axis.
    const Vec2d offset{100.0, 100.0};
    const ImexMirrorAxis axis = imex_mirror_axis_for(/*primary=*/0, /*target=*/3, /*tpg=*/2);
    REQUIRE(axis == ImexMirrorAxis::Y);

    Transform3d xf = imex_head_transform(0, 3, ImexRole::Mirror, offset, Vec2d::Zero(), axis);

    // Primary origin still lands on the target zone origin.
    const Vec3d mapped = xf * Vec3d::Zero();
    REQUIRE_THAT(mapped.x(), WithinAbs(100.0, 1e-9));
    REQUIRE_THAT(mapped.y(), WithinAbs(100.0, 1e-9));
    REQUIRE_THAT(mapped.z(), WithinAbs(0.0,   1e-9));

    // A model point +5 X / +7 Y from primary: X translates 1:1, Y is flipped.
    const Vec3d out = xf * Vec3d{5.0, 7.0, 0.0};
    REQUIRE_THAT(out.x(), WithinAbs(105.0, 1e-9));   // 100 + 5, translated
    REQUIRE_THAT(out.y(), WithinAbs(93.0,  1e-9));   // 100 - 7, reflected
}

TEST_CASE("imex_mirror_axis_for - axis follows the gantry row", "[IMEX]") {
    // 2x2: T0/T1 on gantry 0, T2/T3 on gantry 1.
    REQUIRE(imex_mirror_axis_for(0, 1, 2) == ImexMirrorAxis::X);  // same gantry, beside it
    REQUIRE(imex_mirror_axis_for(0, 2, 2) == ImexMirrorAxis::Y);  // other gantry, in front
    REQUIRE(imex_mirror_axis_for(0, 3, 2) == ImexMirrorAxis::Y);  // other gantry, diagonal
    REQUIRE(imex_mirror_axis_for(2, 3, 2) == ImexMirrorAxis::X);  // primary on gantry 1

    // Single-gantry IDEX (all 4 tools on one gantry, tpg=4): every tool shares the primary's
    // gantry, so mirrors stay on X. This is the pre-existing behavior and must not change.
    REQUIRE(imex_mirror_axis_for(0, 1, 4) == ImexMirrorAxis::X);
    REQUIRE(imex_mirror_axis_for(0, 3, 4) == ImexMirrorAxis::X);

    // tools_per_gantry = 1: every tool is its own gantry, so any secondary is cross-gantry.
    REQUIRE(imex_mirror_axis_for(0, 1, 1) == ImexMirrorAxis::Y);

    // Degenerate tools_per_gantry clamps to 1 rather than dividing by zero.
    REQUIRE(imex_mirror_axis_for(0, 1, 0)  == ImexMirrorAxis::Y);
    REQUIRE(imex_mirror_axis_for(0, 1, -3) == ImexMirrorAxis::Y);
    REQUIRE(imex_mirror_axis_for(0, 0, 0)  == ImexMirrorAxis::X);
}

TEST_CASE("resolve_filament_for_head - no routing returns -1 (ghost color fallback)", "[IMEX]") {
    // User's IQEX pem: T0 has 4 AFC lanes, T1/T2/T3 direct. T5 is unrouted.
    auto pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    std::map<int,int> no_override;
    REQUIRE(resolve_filament_for_head(no_override, pem, 5) == -1);

    // Plate override for an unrouted head still resolves (user's explicit choice wins).
    std::map<int,int> override_on_5 = {{5, 7}};
    REQUIRE(resolve_filament_for_head(override_on_5, pem, 5) == 6);
}

TEST_CASE("parse_imex_active_tools - Span role parsed from S suffix", "[IMEX]") {
    auto out = parse_imex_active_tools("0:P,1:S,2:M,3:M");
    REQUIRE(out.size() == 4);
    REQUIRE(out[0].first == 0);
    REQUIRE(out[0].second == ImexRole::Primary);
    REQUIRE(out[1].first == 1);
    REQUIRE(out[1].second == ImexRole::Span);
    REQUIRE(out[2].first == 2);
    REQUIRE(out[2].second == ImexRole::Mirror);
    REQUIRE(out[3].first == 3);
    REQUIRE(out[3].second == ImexRole::Mirror);
}

TEST_CASE("parse_imex_active_tools - Span suffix whitespace tolerant", "[IMEX]") {
    auto out = parse_imex_active_tools("  0 : P , 1 : S ");
    REQUIRE(out.size() == 2);
    REQUIRE(out[1].second == ImexRole::Span);
}

TEST_CASE("group_imex_active_tools_by_gantry - paired-gantry mc-mirror aggregates", "[IMEX]") {
    // 2x2 IQEX, primary T0, T1 declared Span (multicolor partner on primary's gantry),
    // T2/T3 mirror with column-pairing T2↔T0 and T3↔T1.
    auto g = group_imex_active_tools_by_gantry("0:P,1:S,2:M,3:M", 2);
    REQUIRE(g.primary_phys == 0);
    REQUIRE(g.primary_gantry == 0);
    REQUIRE(g.span_on_primary);
    REQUIRE(g.groups.size() == 2);

    REQUIRE(g.groups[0].gantry_index == 0);
    REQUIRE_FALSE(g.groups[0].aggregate);  // primary's gantry never aggregates
    REQUIRE(g.groups[0].tools.size() == 2);

    REQUIRE(g.groups[1].gantry_index == 1);
    REQUIRE(g.groups[1].aggregate);
    REQUIRE(g.groups[1].representative_phys == 2);  // column-paired to primary T0
    REQUIRE(g.groups[1].representative_role == ImexRole::Mirror);
}

TEST_CASE("group_imex_active_tools_by_gantry - 4 independent copies (no Span) stay per-tool", "[IMEX]") {
    // User's real-world 4-copy job. Same active_tools shape as the mc-mirror case but no
    // Span marker → each tool keeps its own ghost + zone. This is the disambiguation that
    // motivates the Span tile state.
    auto g = group_imex_active_tools_by_gantry("0:P,1:C,2:M,3:M", 2);
    REQUIRE_FALSE(g.span_on_primary);
    REQUIRE(g.groups.size() == 2);
    REQUIRE_FALSE(g.groups[0].aggregate);
    REQUIRE_FALSE(g.groups[1].aggregate);
}

TEST_CASE("group_imex_active_tools_by_gantry - non-primary gantry with single tool stays per-tool", "[IMEX]") {
    // 2-tool mirror on 2x2 IQEX (T0 primary, T2 mirror) — even with Span elsewhere on
    // primary's gantry, a 1-tool non-primary gantry has nothing to aggregate.
    auto g = group_imex_active_tools_by_gantry("0:P,1:S,2:M", 2);
    REQUIRE(g.span_on_primary);
    REQUIRE(g.groups.size() == 2);
    REQUIRE(g.groups[1].gantry_index == 1);
    REQUIRE(g.groups[1].tools.size() == 1);
    REQUIRE_FALSE(g.groups[1].aggregate);
}

TEST_CASE("group_imex_active_tools_by_gantry - mixed-role non-primary gantry falls back", "[IMEX]") {
    // T2:C, T3:M on the same non-primary gantry — user explicitly authored two distinct
    // topologies for that gantry. Aggregation would lose information; stay per-tool.
    auto g = group_imex_active_tools_by_gantry("0:P,1:S,2:C,3:M", 2);
    REQUIRE(g.span_on_primary);
    REQUIRE(g.groups.size() == 2);
    REQUIRE_FALSE(g.groups[1].aggregate);
}

TEST_CASE("group_imex_active_tools_by_gantry - IDEX (tpg=1) never aggregates", "[IMEX]") {
    // IDEX has 1 tool per gantry by definition — primary's gantry has no Span partner,
    // and non-primary gantries each have 1 tool. Aggregation never triggers.
    auto g = group_imex_active_tools_by_gantry("0:P,1:M", 1);
    REQUIRE_FALSE(g.span_on_primary);
    REQUIRE(g.groups.size() == 2);
    REQUIRE_FALSE(g.groups[0].aggregate);
    REQUIRE_FALSE(g.groups[1].aggregate);
}

TEST_CASE("group_imex_active_tools_by_gantry - empty / no primary returns empty grouping", "[IMEX]") {
    auto g = group_imex_active_tools_by_gantry("", 2);
    REQUIRE(g.primary_phys == -1);
    REQUIRE(g.groups.empty());
}

TEST_CASE("group_imex_active_tools_by_gantry - column pairing picks correct representative", "[IMEX]") {
    // Primary at T1 (col=1, gantry=0). On gantry 1, the column-pair is T3 (col=1, gantry=1).
    // Representative for gantry 1 must be T3, not T2 — drives mirror geometry through
    // the column-paired tool's role.
    auto g = group_imex_active_tools_by_gantry("0:S,1:P,2:M,3:M", 2);
    REQUIRE(g.primary_phys == 1);
    REQUIRE(g.span_on_primary);
    REQUIRE(g.groups.size() == 2);
    REQUIRE(g.groups[1].gantry_index == 1);
    REQUIRE(g.groups[1].representative_phys == 3);  // column-paired to primary T1
}

// -----------------------------------------------------------------------------
// compute_imex_slice_offset — center-origin shift for firmware-managed printers
// -----------------------------------------------------------------------------
// Centered on the plate-local zone at (10,5)..(110,75) → center (60, 40).
static const BoundingBoxf kPrimaryZoneCenteredAt60_40{
    Vec2d(10.0, 5.0), Vec2d(110.0, 75.0)};

TEST_CASE("compute_imex_slice_offset - flag off returns zero regardless of mode", "[IMEX]") {
    auto z = compute_imex_slice_offset(false, "copy",    kPrimaryZoneCenteredAt60_40);
    REQUIRE_THAT(z.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(0.0, 1e-9));

    z = compute_imex_slice_offset(false, "mirror",  kPrimaryZoneCenteredAt60_40);
    REQUIRE_THAT(z.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(0.0, 1e-9));

    z = compute_imex_slice_offset(false, "primary", kPrimaryZoneCenteredAt60_40);
    REQUIRE_THAT(z.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("compute_imex_slice_offset - primary mode returns zero even with flag on", "[IMEX]") {
    auto z = compute_imex_slice_offset(true, "primary", kPrimaryZoneCenteredAt60_40);
    REQUIRE_THAT(z.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("compute_imex_slice_offset - empty mode returns zero even with flag on", "[IMEX]") {
    auto z = compute_imex_slice_offset(true, "", kPrimaryZoneCenteredAt60_40);
    REQUIRE_THAT(z.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("compute_imex_slice_offset - empty zone box returns zero even with flag on and copy mode", "[IMEX]") {
    auto z = compute_imex_slice_offset(true, "copy", std::nullopt);
    REQUIRE_THAT(z.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("compute_imex_slice_offset - flag on + copy mode returns primary zone center", "[IMEX]") {
    auto z = compute_imex_slice_offset(true, "copy", kPrimaryZoneCenteredAt60_40);
    REQUIRE_THAT(z.x(), WithinAbs(60.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(40.0, 1e-9));
}

TEST_CASE("compute_imex_slice_offset - flag on + mirror mode returns primary zone center", "[IMEX]") {
    auto z = compute_imex_slice_offset(true, "mirror", kPrimaryZoneCenteredAt60_40);
    REQUIRE_THAT(z.x(), WithinAbs(60.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(40.0, 1e-9));
}

TEST_CASE("compute_imex_slice_offset - user-defined non-primary mode names trigger shift", "[IMEX]") {
    // Mode names are user-defined; anything not "primary" or "" should activate.
    auto z = compute_imex_slice_offset(true, "iq-copy", kPrimaryZoneCenteredAt60_40);
    REQUIRE_THAT(z.x(), WithinAbs(60.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(40.0, 1e-9));
}

TEST_CASE("compute_imex_slice_offset - center value tracks zone box position", "[IMEX]") {
    // Right half of a center-origin bed: (0,-75)..(160,75) → center (80, 0).
    BoundingBoxf right_half{Vec2d(0.0, -75.0), Vec2d(160.0, 75.0)};
    auto z = compute_imex_slice_offset(true, "copy", right_half);
    REQUIRE_THAT(z.x(), WithinAbs(80.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(0.0, 1e-9));

    // Left half of a center-origin bed: (-160,-75)..(0,75) → center (-80, 0).
    BoundingBoxf left_half{Vec2d(-160.0, -75.0), Vec2d(0.0, 75.0)};
    z = compute_imex_slice_offset(true, "mirror", left_half);
    REQUIRE_THAT(z.x(), WithinAbs(-80.0, 1e-9));
    REQUIRE_THAT(z.y(), WithinAbs(0.0, 1e-9));
}


// ---------------------------------------------------------------------------
// imex_hull_violates_zones
//
// The single predicate behind both the object and prime-tower placement checks.
// Zones are unscaled mm (BoundingBoxf3); hulls are scaled Clipper coords, which is
// what ModelInstance::convex_hull_2d() and PartPlate::imex_wipe_tower_hull() return.
// ---------------------------------------------------------------------------

// Axis-aligned rectangle in SCALED coords, from unscaled mm corners.
static Polygon scaled_rect(double x0, double y0, double x1, double y1) {
    Polygon p;
    p.points = {Point(scaled(x0), scaled(y0)), Point(scaled(x1), scaled(y0)),
                Point(scaled(x1), scaled(y1)), Point(scaled(x0), scaled(y1))};
    return p;
}

// Zone in UNSCALED mm, as PartPlate stores them.
static BoundingBoxf3 zone(double x0, double y0, double x1, double y1) {
    return BoundingBoxf3(Vec3d(x0, y0, 0.0), Vec3d(x1, y1, 1.0));
}

TEST_CASE("imex_hull_violates_zones - empty hull never violates", "[IMEX]") {
    // The tower helper returns an empty Polygon for "no tower"; that must not block.
    std::vector<BoundingBoxf3> zones{zone(0, 0, 100, 100)};
    REQUIRE_FALSE(imex_hull_violates_zones(zones, Polygon()));
}

TEST_CASE("imex_hull_violates_zones - empty zone list never violates", "[IMEX]") {
    // Non-IMEX printers have no zones at all and must be unaffected.
    REQUIRE_FALSE(imex_hull_violates_zones({}, scaled_rect(10, 10, 20, 20)));
}

TEST_CASE("imex_hull_violates_zones - hull fully inside a zone violates", "[IMEX]") {
    std::vector<BoundingBoxf3> zones{zone(0, 0, 100, 100)};
    REQUIRE(imex_hull_violates_zones(zones, scaled_rect(10, 10, 20, 20)));
}

TEST_CASE("imex_hull_violates_zones - hull straddling a zone edge violates", "[IMEX]") {
    // The dragged-tower case: partly in the primary zone, partly in the reserved one.
    std::vector<BoundingBoxf3> zones{zone(100, 0, 200, 100)};
    REQUIRE(imex_hull_violates_zones(zones, scaled_rect(90, 10, 110, 20)));
}

TEST_CASE("imex_hull_violates_zones - hull enclosing a zone violates", "[IMEX]") {
    // A small collision strip swallowed by a large hull still overlaps by area.
    std::vector<BoundingBoxf3> zones{zone(45, 45, 55, 55)};
    REQUIRE(imex_hull_violates_zones(zones, scaled_rect(0, 0, 100, 100)));
}

TEST_CASE("imex_hull_violates_zones - disjoint hull does not violate", "[IMEX]") {
    std::vector<BoundingBoxf3> zones{zone(100, 0, 200, 100)};
    REQUIRE_FALSE(imex_hull_violates_zones(zones, scaled_rect(0, 0, 50, 50)));
}

TEST_CASE("imex_hull_violates_zones - edge-flush hull does not violate", "[IMEX]") {
    // Documented area-based semantics: Clipper returns nothing for shapes sharing only
    // an edge, so a hull butted exactly against a zone boundary is legal. This matches
    // the long-standing object behaviour and is what lets a tower sit flush against the
    // primary-zone edge. Pinned because a switch to a touch-based test would silently
    // start blocking placements users currently rely on.
    std::vector<BoundingBoxf3> zones{zone(100, 0, 200, 100)};
    REQUIRE_FALSE(imex_hull_violates_zones(zones, scaled_rect(0, 0, 100, 100)));
}

TEST_CASE("imex_hull_violates_zones - violating any one of several zones is enough", "[IMEX]") {
    std::vector<BoundingBoxf3> zones{zone(0, 0, 10, 10), zone(20, 20, 30, 30), zone(40, 40, 50, 50)};
    // Overlaps only the third.
    REQUIRE(imex_hull_violates_zones(zones, scaled_rect(45, 45, 60, 60)));
    // Overlaps none of the three (sits in the gaps between them).
    REQUIRE_FALSE(imex_hull_violates_zones(zones, scaled_rect(12, 12, 18, 18)));
}
