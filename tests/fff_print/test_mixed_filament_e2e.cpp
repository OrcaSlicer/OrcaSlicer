// Tests for mixed-filament project-config round-trip persistence.
//
// The full-3MF (store_bbs_3mf / load_bbs_3mf) round-trip requires a heavy
// PlateDataPtrs + Model + PresetBundle setup that is not yet scaffolded in
// fff_print tests.  We therefore exercise the *persistence layer* directly:
//
//   MixedFilamentManager::serialize_custom_entries()
//     → stored in project_config["mixed_filament_definitions"]
//     → PresetBundle::sync_mixed_filaments_to_config()   (mirrors store path)
//     → PresetBundle::sync_mixed_filaments_from_config() (mirrors load path)
//
// This is the code path that bbs_3mf.cpp invokes when writing/reading the
// project config block, so a regression here would break 3MF persistence.
//
// TODO: full-pipeline E2E slice test (T0/T1 in G-code, dithering_local_z_mode
//       sublayer assertions) requires the full Print::process() scaffolding;
//       defer to a follow-up once a minimal PrintObject fixture exists.

#include <catch2/catch_all.hpp>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace Slic3r;

// ---------------------------------------------------------------------------
// Helper: build a two-filament PresetBundle with colours set.
// ---------------------------------------------------------------------------
namespace {

static PresetBundle make_bundle_2(const std::string &col_a = "#FF0000",
                                  const std::string &col_b = "#0000FF")
{
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {col_a, col_b};
    return bundle;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test 1 — plain auto-generated round-trip
//   Build bundle → sync to config string → reload in fresh bundle → compare
// ---------------------------------------------------------------------------
TEST_CASE("Mixed filament 3MF round-trip: auto-generated entries survive serialize/load cycle",
          "[MixedFilamentRoundTrip]")
{
    PresetBundle origin = make_bundle_2();
    origin.sync_mixed_filaments_from_config();

    const auto &orig_mixed = origin.mixed_filaments.mixed_filaments();
    REQUIRE(!orig_mixed.empty());

    // Capture the stable_id and component IDs of the first enabled entry.
    const MixedFilament *first = nullptr;
    for (const auto &mf : orig_mixed) {
        if (mf.enabled && !mf.deleted) { first = &mf; break; }
    }
    REQUIRE(first != nullptr);
    const uint64_t     orig_stable_id   = first->stable_id;
    const unsigned int orig_component_a = first->component_a;
    const unsigned int orig_component_b = first->component_b;

    // Sync to config (mirrors what store_bbs_3mf does).
    origin.sync_mixed_filaments_to_config();
    const std::string serialized = origin.project_config.opt_string("mixed_filament_definitions");
    // Auto-generated entries are NOT stored in the custom definitions string —
    // they are rebuilt by auto_generate() on load.  The string will be empty
    // unless a custom entry was added.  We just verify the round-trip doesn't
    // drop auto-generated rows on re-sync.

    // Reload into a fresh bundle with the same colours.
    PresetBundle loaded = make_bundle_2();
    loaded.project_config.option<ConfigOptionString>("mixed_filament_definitions")->value = serialized;
    loaded.sync_mixed_filaments_from_config();

    const auto &load_mixed = loaded.mixed_filaments.mixed_filaments();
    REQUIRE(load_mixed.size() == orig_mixed.size());

    // Find the matching entry by component pair.
    const MixedFilament *reloaded = nullptr;
    for (const auto &mf : load_mixed) {
        if (mf.component_a == orig_component_a && mf.component_b == orig_component_b) {
            reloaded = &mf;
            break;
        }
    }
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->stable_id == orig_stable_id);
}

// ---------------------------------------------------------------------------
// Test 2 — custom entry round-trip
//   2 physical + 1 custom mixed entry: ratio_a, ratio_b, mix_b_percent, stable_id
// ---------------------------------------------------------------------------
TEST_CASE("Mixed filament 3MF round-trip: custom entry count, components, ratio, stable_id",
          "[MixedFilamentRoundTrip]")
{
    const std::vector<std::string> colors = {"#FF0000", "#00FF00"};

    MixedFilamentManager mgr;
    // Add two custom entries to exercise the multi-entry path.
    mgr.add_custom_filament(1, 2, 25, colors);
    mgr.add_custom_filament(1, 2, 75, colors);

    const auto &entries = mgr.mixed_filaments();
    REQUIRE(entries.size() == 2);

    const uint64_t stable_id_0 = entries[0].stable_id;
    const uint64_t stable_id_1 = entries[1].stable_id;
    CHECK(stable_id_0 != stable_id_1);

    // Serialize and reload.
    const std::string serialized = mgr.serialize_custom_entries();
    REQUIRE(!serialized.empty());

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);

    const auto &reloaded = loaded.mixed_filaments();
    REQUIRE(reloaded.size() == 2);

    // Order must be preserved.
    CHECK(reloaded[0].component_a  == 1);
    CHECK(reloaded[0].component_b  == 2);
    CHECK(reloaded[0].mix_b_percent == 25);
    CHECK(reloaded[0].stable_id    == stable_id_0);

    CHECK(reloaded[1].component_a  == 1);
    CHECK(reloaded[1].component_b  == 2);
    CHECK(reloaded[1].mix_b_percent == 75);
    CHECK(reloaded[1].stable_id    == stable_id_1);
}

// ---------------------------------------------------------------------------
// Test 3 — PresetBundle project_config string path (mirrors 3MF store+load)
//   Verify that sync_mixed_filaments_to_config + sync_mixed_filaments_from_config
//   preserves a custom entry end-to-end through the project_config string.
// ---------------------------------------------------------------------------
TEST_CASE("Mixed filament 3MF round-trip: PresetBundle project_config string path",
          "[MixedFilamentRoundTrip]")
{
    PresetBundle origin = make_bundle_2("#FFFF00", "#FF00FF");
    origin.sync_mixed_filaments_from_config();

    // Add a custom entry on top of auto-generated ones.
    const auto &colors = origin.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    origin.mixed_filaments.add_custom_filament(1, 2, 40, colors);

    const size_t num_entries_before = origin.mixed_filaments.mixed_filaments().size();

    // Count custom entries in origin.
    size_t custom_count_before = 0;
    uint64_t custom_stable_id = 0;
    for (const auto &mf : origin.mixed_filaments.mixed_filaments()) {
        if (mf.custom && mf.enabled && !mf.deleted) {
            ++custom_count_before;
            custom_stable_id = mf.stable_id;
        }
    }
    REQUIRE(custom_count_before == 1);

    // Sync to config string — mirrors bbs_3mf store path.
    origin.sync_mixed_filaments_to_config();
    const std::string defs = origin.project_config.opt_string("mixed_filament_definitions");
    REQUIRE(!defs.empty());

    // Reload — mirrors bbs_3mf load path.
    PresetBundle loaded = make_bundle_2("#FFFF00", "#FF00FF");
    loaded.project_config.option<ConfigOptionString>("mixed_filament_definitions")->value = defs;
    loaded.sync_mixed_filaments_from_config();

    const auto &reloaded = loaded.mixed_filaments.mixed_filaments();
    REQUIRE(reloaded.size() == num_entries_before);

    // Custom entry must survive with its stable_id intact.
    size_t custom_count_after = 0;
    uint64_t reloaded_stable_id = 0;
    for (const auto &mf : reloaded) {
        if (mf.custom && mf.enabled && !mf.deleted) {
            ++custom_count_after;
            reloaded_stable_id = mf.stable_id;
        }
    }
    CHECK(custom_count_after == custom_count_before);
    CHECK(reloaded_stable_id == custom_stable_id);
}

// ---------------------------------------------------------------------------
// Test 4 — total_filaments counts physical + mixed correctly after round-trip
// ---------------------------------------------------------------------------
TEST_CASE("Mixed filament 3MF round-trip: total_filaments count is stable after reload",
          "[MixedFilamentRoundTrip]")
{
    PresetBundle origin = make_bundle_2();
    origin.sync_mixed_filaments_from_config();

    const size_t num_physical = origin.filament_presets.size();
    const size_t total_before = origin.mixed_filaments.total_filaments(num_physical);
    // For 2 physical filaments C(2,2)=1 virtual → total must be 3.
    REQUIRE(total_before == 3u);

    origin.sync_mixed_filaments_to_config();
    const std::string defs = origin.project_config.opt_string("mixed_filament_definitions");

    PresetBundle loaded = make_bundle_2();
    loaded.project_config.option<ConfigOptionString>("mixed_filament_definitions")->value = defs;
    loaded.sync_mixed_filaments_from_config();

    const size_t total_after = loaded.mixed_filaments.total_filaments(loaded.filament_presets.size());
    CHECK(total_after == total_before);
}
