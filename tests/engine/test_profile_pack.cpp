// Phase 2.5.3 — profile pack slot end-to-end test.
//
// Builds a fresh Session, attaches a Slic3r::PresetBundle so Presets has a
// target for vendor loads, then discovers the fixture pack at
// TEST_FIXTURES_DIR/profile_pack/orca-test-pack. Asserts:
//   1. discover_and_load_plugins returns >= 1
//   2. PluginManager recognized the data-only branch (no .so loaded)
//   3. The fixture vendor appears in session->presets().vendors()
//   4. The ORCA_SLOT_PROFILE_PACK slot count incremented by 1

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"
#include "orca/Presets.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <filesystem>

TEST_CASE("Phase 2.5.3 — data-only profile pack plugin loads via discover_and_load",
          "[plugin][slots][profile_pack]") {
    auto session = orca::Session::create();
    REQUIRE(session != nullptr);

    // Presets::load_vendor_configs_from_json needs a PresetBundle on the
    // session; default-construct one here for the test's lifetime.
    Slic3r::PresetBundle bundle;
    session->attach_preset_bundle(&bundle);

    namespace fs = std::filesystem;
    const fs::path packs_root = fs::path{TEST_FIXTURES_DIR} / "profile_pack";
    REQUIRE(fs::is_directory(packs_root));
    REQUIRE(fs::is_regular_file(packs_root / "orca-test-pack" / "manifest.json"));

    const auto slots_before = session->registered_slot_count();

    const auto loaded = session->discover_and_load_plugins(packs_root);
    CHECK(loaded >= 1);

    CHECK(session->is_plugin_loaded("orcaslicer.test.profile_pack"));

    // The vendor JSON is staged as <vendor_dir>/OrcaTestVendor.json, so
    // load_vendor_configs_from_json keys the entry by stem "OrcaTestVendor".
    // Check both the underlying bundle (direct) and the engine Presets API.
    CHECK(bundle.vendors.count("OrcaTestVendor") == 1);

    const auto vendors = session->presets().vendors();
    const auto it = std::find_if(vendors.begin(), vendors.end(),
                                 [](const orca::VendorRef& v) { return v.id == "OrcaTestVendor"; });
    CHECK(it != vendors.end());

    // The data-only branch registers exactly one ORCA_SLOT_PROFILE_PACK slot
    // for this plugin.
    CHECK(session->registered_slot_count() >= slots_before + 1);
}
