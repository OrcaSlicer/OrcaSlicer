// Phase 2.3.3 — placeholder provider slot end-to-end test.
//
// Registers ORCA_SLOT_PLACEHOLDER_PROVIDER directly through the Session's
// PluginRegistry (no .so plugin needed — that path is exercised by
// tests/engine/test_plugin_loader.cpp). The provider's on_provide callback
// uses the engine-internal orca::placeholder_tls::current() bridge to inject
// a string variable into the active Print::placeholder_parser().
//
// Slot vtables are NOT given a host pointer at dispatch time, so the host
// thunks in engine/src/PluginManager.cpp aren't reachable from a directly-
// registered slot. Reaching the parser via the thread-local bridge bypasses
// the host-vtable thunks entirely while still proving:
//   * the slot snapshot in engine/src/Slicer.cpp installed the chained
//     callback on Print::set_placeholder_provider_callback;
//   * Print::export_gcode invoked the chain inside the RAII Scope guard so
//     the parser was visible via TLS at on_provide time;
//   * PlaceholderParser::set on the published parser succeeds — i.e. the
//     thread-local plumbing is wired to the right object.

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"
#include "orca/Slicer.hpp"
#include "orca/Export.hpp"
#include "orca/plugin_api.h"
#include "orca/c_api.h"
#include "orca/PlaceholderProvider.hpp"

// PluginRegistry lives in engine/src/ (engine-internal). The engine_tests
// CMakeLists adds ${CMAKE_SOURCE_DIR}/engine/src to the include path.
#include "PluginRegistry.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Config.hpp>
#include <libslic3r/PlaceholderParser.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

namespace {

constexpr auto kSliceTimeout = std::chrono::seconds(120);
constexpr const char* kVarName  = "orca_test_var";
constexpr const char* kVarValue = "plugin-value";

// State captured by the provider trampoline. Atomics because the provider runs
// on the slicing worker thread.
struct ProviderState {
    std::atomic<int>           invocations{0};
    std::atomic<bool>          saw_parser{false};
    std::atomic<bool>          set_succeeded{false};
    std::atomic<std::uint64_t> last_handle{0};
};

extern "C" orca_error_code_t inject_test_var_provider(std::uint64_t slice_handle,
                                                      void*         user_data) {
    auto* s = static_cast<ProviderState*>(user_data);
    if (s == nullptr) return ORCA_ERR_INVALID_ARGUMENT;
    s->invocations.fetch_add(1, std::memory_order_relaxed);
    s->last_handle.store(slice_handle, std::memory_order_relaxed);

    Slic3r::PlaceholderParser* pp = orca::placeholder_tls::current();
    if (pp == nullptr) {
        // TLS guard wasn't installed by Print::export_gcode — engine wiring
        // is broken. Surface as a non-OK rc so the slice fails loudly.
        return ORCA_ERR_INVALID_ARGUMENT;
    }
    s->saw_parser.store(true, std::memory_order_relaxed);

    try {
        pp->set(std::string{kVarName}, std::string{kVarValue});
        s->set_succeeded.store(true, std::memory_order_relaxed);
        return ORCA_OK;
    } catch (...) {
        return ORCA_ERR_UNKNOWN;
    }
}

// Outcome of run_canary_slice — terminal state + handle + (optionally) the
// export path the test should clean up.
struct CanaryResult {
    orca::SliceState      state    = orca::SliceState::NotStarted;
    orca::SliceHandle     handle   = 0;
    bool                  exported = false;
    std::filesystem::path output_path;
};

// Local 20mm-cube canary harness. Mirrors run_canary_slice in
// tests/engine/test_slot_gcode_filter.cpp; that helper sits in an anonymous
// namespace and is not reusable here.
CanaryResult run_canary_slice(orca::Session& session, const char* out_path_tag) {
    Slic3r::Model model;
    auto* obj = model.add_object();
    obj->name = "slot_placeholder_provider_cube";
    obj->add_volume(Slic3r::make_cube(20.0, 20.0, 20.0));
    obj->add_instance();
    for (auto* mo : model.objects) mo->ensure_on_bed();

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(1);
    config.set_key_value("use_relative_e_distances", new Slic3r::ConfigOptionBool(false));

    orca::SliceParams sp;
    auto h = session.slicer().request_slice(sp, model, config);
    REQUIRE(h.ok());

    CanaryResult cr;
    cr.handle = h.value();

    auto deadline = std::chrono::steady_clock::now() + kSliceTimeout;
    orca::SliceStatus st;
    for (;;) {
        st = session.slicer().status(cr.handle);
        if (st.state == orca::SliceState::Completed ||
            st.state == orca::SliceState::Failed    ||
            st.state == orca::SliceState::Cancelled)
            break;
        if (std::chrono::steady_clock::now() > deadline)
            FAIL("Slice timed out waiting for terminal state");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    cr.state = st.state;

    if (cr.state == orca::SliceState::Completed) {
        namespace fs = std::filesystem;
        cr.output_path = fs::path{TEST_TMP_DIR}
                       / (std::string("orca_slot_placeholder_provider_") + out_path_tag + ".gcode");
        std::error_code rm_ec;
        fs::remove(cr.output_path, rm_ec);

        auto exp = session.exporter().export_gcode({cr.output_path});
        REQUIRE(exp.ok());
        cr.exported = true;
    }
    return cr;
}

} // namespace

TEST_CASE("Phase 2.3.3 — placeholder provider slot injects via thread-local parser",
          "[plugin][slots][placeholder]") {
    auto session = orca::Session::create();
    REQUIRE(session);

    ProviderState state;
    orca_slot_placeholder_provider_t vt{};
    vt.struct_size = sizeof(vt);
    vt.on_provide  = &inject_test_var_provider;

    auto& registry = session->plugin_registry();
    registry.set_current_plugin_id("test.placeholder_provider");
    const auto slot_id = registry.add_slot(ORCA_SLOT_PLACEHOLDER_PROVIDER,
                                           &vt, &state, /*priority=*/0);
    REQUIRE(slot_id != 0);
    registry.clear_current_plugin_id();

    const auto result = run_canary_slice(*session, "inject");
    REQUIRE(result.state == orca::SliceState::Completed);
    REQUIRE(result.exported);

    // Dispatch wiring asserts:
    //   * provider fired exactly once during Print::export_gcode;
    //   * the slice_handle argument matches the handle the slicer returned;
    //   * TLS bridge had a non-null parser pointer when on_provide ran;
    //   * PlaceholderParser::set on the published parser succeeded.
    CHECK(state.invocations.load() == 1);
    CHECK(state.last_handle.load() == result.handle);
    CHECK(state.saw_parser.load());
    CHECK(state.set_succeeded.load());

    // Clean up the temp G-code file and the slot registration. Session's
    // destructor would drop the registry, but being explicit documents the
    // slot lifetime contract.
    std::error_code rm_ec;
    std::filesystem::remove(result.output_path, rm_ec);

    auto removed = registry.remove_slot(slot_id);
    CHECK(removed.ok());
}
