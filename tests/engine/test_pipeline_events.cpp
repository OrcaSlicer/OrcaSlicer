#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"
#include "orca/Slicer.hpp"
#include "orca/Export.hpp"
#include "orca/Events.hpp"
#include "orca/EventTypes.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Config.hpp>          // for DynamicPrintConfig::full_print_config

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace {

constexpr auto kSliceTimeout = std::chrono::seconds(120);

// Tiny holder for the 9 pipeline-event counters. Each counter is atomic because
// the events fire from the slicing worker thread.
struct PipelineHits {
    std::atomic<int> before_slice{0};
    std::atomic<int> after_perimeters{0};
    std::atomic<int> after_infill{0};
    std::atomic<int> after_ironing{0};
    std::atomic<int> after_supports{0};
    std::atomic<int> before_wipe_tower{0};
    std::atomic<int> after_skirt_brim{0};
    std::atomic<int> before_gcode_export{0};
    std::atomic<int> after_gcode_export{0};
};

} // namespace

TEST_CASE("Service slice fires all 9 pipeline-stage events", "[engine][events][pipeline]") {
    auto session = orca::Session::create();
    REQUIRE(session);

    PipelineHits hits;

    // Subscribe before the slice starts.
    auto& bus = session->events();
    auto id_bs = bus.subscribe<orca::BeforeSlice>(
        [&](const orca::BeforeSlice&) { hits.before_slice.fetch_add(1); });
    auto id_ap = bus.subscribe<orca::AfterPerimeters>(
        [&](const orca::AfterPerimeters&) { hits.after_perimeters.fetch_add(1); });
    auto id_ai = bus.subscribe<orca::AfterInfill>(
        [&](const orca::AfterInfill&) { hits.after_infill.fetch_add(1); });
    auto id_ar = bus.subscribe<orca::AfterIroning>(
        [&](const orca::AfterIroning&) { hits.after_ironing.fetch_add(1); });
    auto id_as = bus.subscribe<orca::AfterSupports>(
        [&](const orca::AfterSupports&) { hits.after_supports.fetch_add(1); });
    auto id_bwt = bus.subscribe<orca::BeforeWipeTower>(
        [&](const orca::BeforeWipeTower&) { hits.before_wipe_tower.fetch_add(1); });
    auto id_asb = bus.subscribe<orca::AfterSkirtBrim>(
        [&](const orca::AfterSkirtBrim&) { hits.after_skirt_brim.fetch_add(1); });
    auto id_bge = bus.subscribe<orca::BeforeGCodeExport>(
        [&](const orca::BeforeGCodeExport&) { hits.before_gcode_export.fetch_add(1); });
    auto id_age = bus.subscribe<orca::AfterGCodeExport>(
        [&](const orca::AfterGCodeExport&) { hits.after_gcode_export.fetch_add(1); });

    // ---- Build the 20mm-cube canary model + config (mirrors engine/cli/main.cpp). ----
    Slic3r::Model model;
    auto* obj = model.add_object();
    obj->name = "pipeline_events_cube";
    obj->add_volume(Slic3r::make_cube(20.0, 20.0, 20.0));
    obj->add_instance();
    for (auto* mo : model.objects) mo->ensure_on_bed();

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_num_extruders(1);
    config.set_key_value("use_relative_e_distances", new Slic3r::ConfigOptionBool(false));

    // ---- Drive the slice through orca::Slicer's explicit-input overload. ----
    orca::SliceParams sp;
    auto h = session->slicer().request_slice(sp, model, config);
    REQUIRE(h.ok());

    auto handle = h.value();
    auto deadline = std::chrono::steady_clock::now() + kSliceTimeout;
    orca::SliceStatus st;
    for (;;) {
        st = session->slicer().status(handle);
        if (st.state == orca::SliceState::Completed ||
            st.state == orca::SliceState::Failed    ||
            st.state == orca::SliceState::Cancelled)
            break;
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("Slice timed out waiting for terminal state");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(st.state == orca::SliceState::Completed);

    // ---- Export through the engine Exporter so BeforeGCodeExport / AfterGCodeExport fire. ----
    namespace fs = std::filesystem;
    fs::path out_path = fs::path{TEST_TMP_DIR} / "orca_pipeline_events_test.gcode";
    std::error_code rm_ec;
    fs::remove(out_path, rm_ec);

    auto exp = session->exporter().export_gcode({out_path});
    REQUIRE(exp.ok());

    // ---- Assertions. All 9 events MUST have fired at least once. ----
    REQUIRE(hits.before_slice.load()         >= 1);
    REQUIRE(hits.after_perimeters.load()     >= 1);
    REQUIRE(hits.after_infill.load()         >= 1);
    REQUIRE(hits.after_ironing.load()        >= 1);
    REQUIRE(hits.after_supports.load()       >= 1);
    REQUIRE(hits.before_wipe_tower.load()    >= 1);
    REQUIRE(hits.after_skirt_brim.load()     >= 1);
    REQUIRE(hits.before_gcode_export.load()  >= 1);
    REQUIRE(hits.after_gcode_export.load()   >= 1);

    // Unsubscribe cleanly.
    bus.unsubscribe(id_bs);
    bus.unsubscribe(id_ap);
    bus.unsubscribe(id_ai);
    bus.unsubscribe(id_ar);
    bus.unsubscribe(id_as);
    bus.unsubscribe(id_bwt);
    bus.unsubscribe(id_asb);
    bus.unsubscribe(id_bge);
    bus.unsubscribe(id_age);

    // Clean up the file.
    fs::remove(out_path, rm_ec);
}
