// Phase 2.1.3 — pipeline observer + interceptor slot dispatch end-to-end test.
//
// Registers slot vtables directly through the Session's PluginRegistry
// (no .so plugin needed — that path is exercised by tests/engine/test_plugin_loader.cpp).
// Drives a real canary slice and asserts:
//   * the observer fires for each of the 9 pipeline steps, with the slice_handle
//     argument matching the handle returned by Slicer::request_slice;
//   * the interceptor's SKIP semantics let the slice complete;
//   * the interceptor's ABORT on BeforeSlice ends the slice as Cancelled.

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"
#include "orca/Slicer.hpp"
#include "orca/Export.hpp"
#include "orca/plugin_api.h"

// PluginRegistry lives in engine/src/ (engine-internal). The engine_tests
// CMakeLists adds ${CMAKE_SOURCE_DIR}/engine/src to the include path.
#include "PluginRegistry.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Config.hpp>          // for DynamicPrintConfig::full_print_config

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace {

constexpr auto kSliceTimeout = std::chrono::seconds(120);

// Per-step counters + last-seen handle captured by the observer trampoline.
// Atomics are required because dispatch happens on the slicing worker thread.
struct ObserverState {
    // Indexed by orca_pipeline_step_t (1..9); slot 0 unused.
    std::array<std::atomic<int>, 10> counts{};
    std::atomic<std::uint64_t> last_handle{0};
};

extern "C" void observer_on_step(orca_pipeline_step_t step,
                                 std::uint64_t        handle,
                                 void*                user_data) {
    auto* s = static_cast<ObserverState*>(user_data);
    if (s == nullptr) return;
    if (step >= 1 && step <= 9)
        s->counts[static_cast<std::size_t>(step)].fetch_add(1, std::memory_order_relaxed);
    s->last_handle.store(handle, std::memory_order_relaxed);
}

// Interceptor that returns a configurable disposition for a configurable step.
struct InterceptorPolicy {
    orca_pipeline_step_t target_step       = ORCA_STEP_BEFORE_SLICE;
    orca_disposition_t   target_disposition = ORCA_DISPOSITION_PROCEED;
    std::atomic<int>     calls{0};
};

extern "C" orca_disposition_t interceptor_on_step(orca_pipeline_step_t step,
                                                  std::uint64_t        /*handle*/,
                                                  void*                user_data) {
    auto* p = static_cast<InterceptorPolicy*>(user_data);
    if (p == nullptr) return ORCA_DISPOSITION_PROCEED;
    p->calls.fetch_add(1, std::memory_order_relaxed);
    if (step == p->target_step)
        return p->target_disposition;
    return ORCA_DISPOSITION_PROCEED;
}

// Result of run_canary_slice — terminal state + handle the slicer returned.
struct CanaryResult {
    orca::SliceState  state  = orca::SliceState::NotStarted;
    orca::SliceHandle handle = 0;
};

// Build the standard 20mm-cube canary model + full default print config,
// then drive a slice via the explicit-input request_slice overload (same
// pattern as tests/engine/test_pipeline_events.cpp). Optionally also exports
// G-code so BeforeGCodeExport / AfterGCodeExport observer callbacks fire.
// `out_path_tag` makes the temp G-code file name unique per test case.
CanaryResult run_canary_slice(orca::Session& session,
                              const char*    out_path_tag,
                              bool           do_export) {
    Slic3r::Model model;
    auto* obj = model.add_object();
    obj->name = "slot_pipeline_cube";
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
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("Slice timed out waiting for terminal state");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    cr.state = st.state;

    if (do_export && cr.state == orca::SliceState::Completed) {
        namespace fs = std::filesystem;
        fs::path out_path = fs::path{TEST_TMP_DIR}
                          / (std::string("orca_slot_pipeline_") + out_path_tag + ".gcode");
        std::error_code rm_ec;
        fs::remove(out_path, rm_ec);

        auto exp = session.exporter().export_gcode({out_path});
        REQUIRE(exp.ok());

        fs::remove(out_path, rm_ec);
    }
    return cr;
}

} // namespace

TEST_CASE("Phase 2.1.3 — pipeline observer slot fires for each step",
          "[plugin][slots][pipeline]") {
    auto session = orca::Session::create();
    REQUIRE(session);

    ObserverState state;
    orca_slot_pipeline_observer_t vt{};
    vt.struct_size = sizeof(vt);
    vt.on_step     = &observer_on_step;

    auto& registry = session->plugin_registry();
    registry.set_current_plugin_id("test.observer");
    const auto slot_id = registry.add_slot(ORCA_SLOT_PIPELINE_OBSERVER,
                                           &vt, &state, /*priority=*/0);
    REQUIRE(slot_id != 0);
    registry.clear_current_plugin_id();

    auto result = run_canary_slice(*session, "observer", /*do_export=*/true);
    REQUIRE(result.state == orca::SliceState::Completed);

    // All 9 pipeline steps MUST have fired at least once. The exporter path
    // drives the last two (BeforeGCodeExport, AfterGCodeExport).
    for (int s = 1; s <= 9; ++s) {
        const int hits = state.counts[static_cast<std::size_t>(s)].load();
        INFO("pipeline step index " << s << " hit count = " << hits);
        CHECK(hits >= 1);
    }

    // The handle the observer saw must match what request_slice returned.
    CHECK(state.last_handle.load() == result.handle);

    // Clean up — the Session destructor would also drop the registry, but
    // being explicit documents the slot lifetime contract.
    auto removed = registry.remove_slot(slot_id);
    CHECK(removed.ok());
}

TEST_CASE("Phase 2.1.3 — pipeline interceptor slot SKIP and ABORT",
          "[plugin][slots][pipeline]") {
    SECTION("SKIP on BeforeWipeTower lets the slice complete") {
        auto session = orca::Session::create();
        REQUIRE(session);

        InterceptorPolicy policy;
        policy.target_step        = ORCA_STEP_BEFORE_WIPE_TOWER;
        policy.target_disposition = ORCA_DISPOSITION_SKIP;

        orca_slot_pipeline_interceptor_t vt{};
        vt.struct_size = sizeof(vt);
        vt.on_step     = &interceptor_on_step;

        auto& registry = session->plugin_registry();
        registry.set_current_plugin_id("test.interceptor.skip");
        const auto slot_id = registry.add_slot(ORCA_SLOT_PIPELINE_INTERCEPTOR,
                                               &vt, &policy);
        REQUIRE(slot_id != 0);
        registry.clear_current_plugin_id();

        // Single-extruder canary has no wipe tower, so SKIP here is a no-op
        // for the actual step body — but the interceptor IS still consulted
        // for every pipeline step, so calls > 0 either way and the slice
        // must run to completion.
        auto result = run_canary_slice(*session, "interceptor_skip",
                                       /*do_export=*/true);
        CHECK(result.state == orca::SliceState::Completed);
        CHECK(policy.calls.load() >= 1);

        auto removed = registry.remove_slot(slot_id);
        CHECK(removed.ok());
    }

    SECTION("ABORT on BeforeSlice ends the slice as Cancelled") {
        auto session = orca::Session::create();
        REQUIRE(session);

        InterceptorPolicy policy;
        policy.target_step        = ORCA_STEP_BEFORE_SLICE;
        policy.target_disposition = ORCA_DISPOSITION_ABORT;

        orca_slot_pipeline_interceptor_t vt{};
        vt.struct_size = sizeof(vt);
        vt.on_step     = &interceptor_on_step;

        auto& registry = session->plugin_registry();
        registry.set_current_plugin_id("test.interceptor.abort");
        const auto slot_id = registry.add_slot(ORCA_SLOT_PIPELINE_INTERCEPTOR,
                                               &vt, &policy);
        REQUIRE(slot_id != 0);
        registry.clear_current_plugin_id();

        // ABORT at BeforeSlice should raise CanceledException inside the
        // pipeline; engine/src/Slicer.cpp maps that to SliceState::Cancelled.
        // No export is attempted because the slice never completes.
        auto result = run_canary_slice(*session, "interceptor_abort",
                                       /*do_export=*/false);
        CHECK(result.state == orca::SliceState::Cancelled);
        CHECK(policy.calls.load() >= 1);

        auto removed = registry.remove_slot(slot_id);
        CHECK(removed.ok());
    }
}
