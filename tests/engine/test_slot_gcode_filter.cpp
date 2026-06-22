// Phase 2.2.3 — gcode filter slot end-to-end test.
//
// Registers a single gcode_filter vtable via Session::plugin_registry(),
// drives a real canary slice + export, then reads the exported file and asserts
// the filter's marker line is present. Verifies the filter slot is invoked
// during Print::export_gcode and that its in-place mutation survives back to
// the caller.

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"
#include "orca/Slicer.hpp"
#include "orca/Export.hpp"
#include "orca/plugin_api.h"
#include "orca/c_api.h"

// PluginRegistry lives in engine/src/ (engine-internal). The engine_tests
// CMakeLists adds ${CMAKE_SOURCE_DIR}/engine/src to the include path.
#include "PluginRegistry.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Config.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

namespace {

constexpr auto kSliceTimeout = std::chrono::seconds(120);
constexpr const char* kMarker = "; HELLO FROM ORCA PLUGIN";

// State captured by the filter trampoline. Atomics because the filter runs on
// the slicing worker thread.
struct FilterState {
    std::atomic<int>          invocations{0};
    std::atomic<bool>         saw_path{false};
    std::atomic<bool>         wrote_marker{false};
    std::atomic<std::uint64_t> last_handle{0};
};

// Append the marker line to the file at `gcode_path`. The contract from
// plugin_api.h: mutate the file in place, return ORCA_OK on success, any other
// code aborts the export.
extern "C" orca_error_code_t append_marker_filter(const char*   gcode_path,
                                                  std::uint64_t slice_handle,
                                                  void*         user_data) {
    auto* s = static_cast<FilterState*>(user_data);
    if (s == nullptr) return ORCA_ERR_INVALID_ARGUMENT;
    s->invocations.fetch_add(1, std::memory_order_relaxed);
    s->last_handle.store(slice_handle, std::memory_order_relaxed);
    if (gcode_path != nullptr && *gcode_path != '\0')
        s->saw_path.store(true, std::memory_order_relaxed);
    else
        return ORCA_ERR_INVALID_ARGUMENT;

    std::ofstream out(gcode_path, std::ios::app);
    if (!out) return ORCA_ERR_IO;
    out << '\n' << kMarker << '\n';
    if (!out) return ORCA_ERR_IO;
    out.close();
    if (!out) return ORCA_ERR_IO;

    s->wrote_marker.store(true, std::memory_order_relaxed);
    return ORCA_OK;
}

// Outcome of the canary slice + export driver. Mirrors the pattern in
// tests/engine/test_slot_pipeline.cpp::run_canary_slice but ALSO returns the
// output path so the test can read back the produced G-code file. The file is
// NOT removed here — the caller owns cleanup.
struct CanaryResult {
    orca::SliceState      state       = orca::SliceState::NotStarted;
    orca::SliceHandle     handle      = 0;
    bool                  exported    = false;
    std::filesystem::path output_path;
};

CanaryResult run_canary_slice(orca::Session& session, const char* out_path_tag) {
    Slic3r::Model model;
    auto* obj = model.add_object();
    obj->name = "slot_gcode_filter_cube";
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
                       / (std::string("orca_slot_gcode_filter_") + out_path_tag + ".gcode");
        std::error_code rm_ec;
        fs::remove(cr.output_path, rm_ec);

        auto exp = session.exporter().export_gcode({cr.output_path});
        REQUIRE(exp.ok());
        cr.exported = true;
    }
    return cr;
}

} // namespace

TEST_CASE("Phase 2.2.3 — gcode filter slot appends marker to exported file",
          "[plugin][slots][gcode_filter]") {
    auto session = orca::Session::create();
    REQUIRE(session);

    FilterState state;
    orca_slot_gcode_filter_t vt{};
    vt.struct_size = sizeof(vt);
    vt.filter      = &append_marker_filter;

    auto& registry = session->plugin_registry();
    registry.set_current_plugin_id("test.gcode_filter");
    const auto slot_id = registry.add_slot(ORCA_SLOT_GCODE_FILTER,
                                           &vt, &state, /*priority=*/0);
    REQUIRE(slot_id != 0);
    registry.clear_current_plugin_id();

    const auto result = run_canary_slice(*session, "marker");
    REQUIRE(result.state == orca::SliceState::Completed);
    REQUIRE(result.exported);

    // Filter must have been invoked, must have received a non-empty path,
    // and must have successfully written its marker.
    CHECK(state.invocations.load() >= 1);
    CHECK(state.saw_path.load());
    CHECK(state.wrote_marker.load());

    // Read the exported G-code file and assert the marker line survived.
    std::ifstream in(result.output_path);
    REQUIRE(in);
    const std::string contents{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
    CHECK(contents.find(kMarker) != std::string::npos);

    // Clean up the temp file and the slot registration. The Session
    // destructor would also drop the registry, but being explicit documents
    // the slot lifetime contract.
    std::error_code rm_ec;
    std::filesystem::remove(result.output_path, rm_ec);

    auto removed = registry.remove_slot(slot_id);
    CHECK(removed.ok());
}
