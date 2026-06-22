// orca-engine-cli — Phase 0.3 build canary.
//
// Links against orca-engine (the libslic3r shared library) with NO wxWidgets
// and drives a real slice end-to-end: build a cube, apply the default print
// config, run the FFF pipeline, export G-code. If this runs and emits a
// non-trivial G-code file, the engine is proven usable by a non-GUI consumer
// without any GUI layer present.

#include <libslic3r/Config.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libslic3r_version.h>

// Phase 0.4d — verify the typed surface compiles + runs from a non-GUI consumer.
#include <orca/Config.hpp>
#include <orca/ConfigKeys.hpp>
#include <orca/Session.hpp>
#include <orca/Slicer.hpp>
#include <orca/Export.hpp>
#include <orca/Events.hpp>
#include <orca/EventTypes.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <system_error>

using namespace Slic3r;

int main() {
    std::printf("orca-engine-cli: libslic3r %s\n", SLIC3R_VERSION);

    // 1) A 20mm cube — the smallest meaningful FFF job.
    Model       model;
    ModelObject *object = model.add_object();
    object->name = "canary_cube";
    object->add_volume(make_cube(20.0, 20.0, 20.0));
    object->add_instance();
    for (ModelObject *mo : model.objects)
        mo->ensure_on_bed();

    // 2) Default print configuration. Use absolute E distances so the stock
    //    layer_gcode validates (relative E would require a "G92 E0" reset that
    //    the bare default profile doesn't include).
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(1);
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(false));

    // Phase 0.4d typed surface — sanity-check read+write through orca::config
    // against a bare DynamicPrintConfig (Adhoc receiver, the dominant call
    // shape per the wave-1 catalog). If these don't round-trip the canary
    // fails fast and tells us the typed surface is broken.
    {
        using namespace orca::keys;
        auto lh = orca::config::get<layer_height>(config);
        if (!lh) {
            std::printf("orca-engine-cli: typed get<layer_height> returned nullopt\n");
            return 1;
        }
        orca::config::put<layer_height>(config, *lh + 0.01);
        auto lh2 = orca::config::get<layer_height>(config);
        if (!lh2 || std::abs(*lh2 - (*lh + 0.01)) > 1e-9) {
            std::printf("orca-engine-cli: typed layer_height write+read did not round-trip\n");
            return 1;
        }
        // Restore the original so the slice below uses the canonical value.
        orca::config::put<layer_height>(config, *lh);

        auto noz = orca::config::get_vec<nozzle_diameter>(config);
        if (!noz || noz->empty()) {
            std::printf("orca-engine-cli: typed get_vec<nozzle_diameter> empty\n");
            return 1;
        }
        std::printf("orca-engine-cli: typed surface OK (layer_height=%.3f, nozzle_diameter[0]=%.3f)\n",
                    *lh, (*noz)[0]);
    }

    // 3) FFF pipeline.
    Print print;
    for (ModelObject *mo : model.objects)
        print.auto_assign_extruders(mo);
    print.apply(model, config);

    StringObjectException err = print.validate();
    if (!err.string.empty()) {
        std::printf("orca-engine-cli: validate FAILED: %s\n", err.string.c_str());
        return 1;
    }

    print.set_status_silent();
    print.process();

    // 4) Verify the slice produced real toolpaths. process() runs the full FFF
    //    pipeline (perimeters, infill, support, G-code generation) — a populated
    //    layer set is proof the engine sliced end-to-end without any GUI layer.
    if (print.objects().empty()) {
        std::printf("orca-engine-cli: slice produced no print objects\n");
        return 1;
    }
    const std::size_t layers = print.objects().front()->layer_count();
    if (layers < 50) {
        std::printf("orca-engine-cli: slice produced too few layers (%zu)\n", layers);
        return 1;
    }

    // 5) Export G-code (Phase 0.4c). Exporting a *bare* full_print_config()
    //    used to crash in append_full_config → ConfigOptionEnumsGeneric::
    //    serialize(): some Orca enum options ship without an enum-names map in
    //    their ConfigDef, and serializing the whole config as G-code comments
    //    dereferenced the null keys_map. Fixed in libslic3r/Config.hpp by
    //    falling back to the integer storage when keys_map is null. This export
    //    is the regression guard for that fix.
    GCodeProcessorResult gcode_result;
    // Write canary G-code to CWD, not /tmp — predictable for the user invoking
    // the binary; lets `cd build && orca-engine-cli` keep its output local.
    const std::filesystem::path out_path =
        std::filesystem::current_path() / "orca_engine_canary.gcode";
    std::error_code rm_ec;
    std::filesystem::remove(out_path, rm_ec);

    const std::string written = print.export_gcode(out_path.string(), &gcode_result, nullptr);

    std::error_code sz_ec;
    const auto written_size = std::filesystem::file_size(written.empty() ? out_path : std::filesystem::path(written), sz_ec);
    if (sz_ec || written_size < 1024) {
        std::printf("orca-engine-cli: export produced no/too-small G-code (%s)\n",
                    sz_ec ? sz_ec.message().c_str() : "size < 1KB");
        return 1;
    }

    std::printf("orca-engine-cli: engine OK — sliced 20mm cube into %zu layers, exported %ju bytes of G-code\n",
                layers, static_cast<std::uintmax_t>(written_size));

    // 6) Engine service path (Phase 0.4c) — drive the SAME slice through the
    //    orca::Slicer/Exporter services via the explicit-input overload (no
    //    PresetBundle attached). Proves the async service API works headless.
    {
        auto session = orca::Session::create();

        // Phase 0.5 — subscribe to the engine event bus and assert the
        // Slicer/Exporter publish typed lifecycle events. Subscribers fire on
        // the worker thread, so use atomics. This is the 0.5 regression guard:
        // if the engine stops publishing, the canary fails fast.
        std::atomic<int>  progress_events{0};
        std::atomic<bool> got_finished{false};
        std::atomic<bool> finished_success{false};
        std::atomic<bool> export_finished_ok{false};
        session->events().subscribe<orca::SlicingProgress>(
            [&](const orca::SlicingProgress&) { ++progress_events; });
        session->events().subscribe<orca::SlicingFinished>(
            [&](const orca::SlicingFinished& e) { got_finished = true; finished_success = e.success; });
        session->events().subscribe<orca::ExportFinished>(
            [&](const orca::ExportFinished& e) { export_finished_ok = e.success; });

        orca::SliceParams sp;            // FFF, plate 0 (defaults)
        auto h = session->slicer().request_slice(sp, model, config);
        if (!h) {
            std::printf("orca-engine-cli: service request_slice failed: %s\n",
                        h.error().message.c_str());
            return 1;
        }
        const orca::SliceHandle handle = h.value();

        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::seconds(120);
        orca::SliceStatus st;
        for (;;) {
            st = session->slicer().status(handle);
            if (st.state == orca::SliceState::Completed ||
                st.state == orca::SliceState::Failed ||
                st.state == orca::SliceState::Cancelled)
                break;
            if (clock::now() > deadline) {
                std::printf("orca-engine-cli: service slice timed out\n");
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (st.state != orca::SliceState::Completed) {
            std::printf("orca-engine-cli: service slice not completed (state=%d, err=%s)\n",
                        static_cast<int>(st.state), st.error.c_str());
            return 1;
        }

        // Phase 0.5 — the bus must have delivered progress + a successful finish.
        if (progress_events.load() == 0) {
            std::printf("orca-engine-cli: bus delivered no SlicingProgress events\n");
            return 1;
        }
        if (!got_finished.load() || !finished_success.load()) {
            std::printf("orca-engine-cli: bus did not deliver a successful SlicingFinished\n");
            return 1;
        }

        const std::filesystem::path svc_out =
            std::filesystem::current_path() / "orca_engine_service.gcode";
        std::error_code rm2;
        std::filesystem::remove(svc_out, rm2);

        auto ex = session->exporter().export_gcode({svc_out});
        if (!ex) {
            std::printf("orca-engine-cli: service export_gcode failed: %s\n",
                        ex.error().message.c_str());
            return 1;
        }
        std::error_code sz2;
        const auto svc_size = std::filesystem::file_size(svc_out, sz2);
        if (sz2 || svc_size < 1024) {
            std::printf("orca-engine-cli: service export too small/missing\n");
            return 1;
        }
        if (!export_finished_ok.load()) {
            std::printf("orca-engine-cli: bus did not deliver a successful ExportFinished\n");
            return 1;
        }
        std::printf("orca-engine-cli: service OK — sliced+exported via Slicer/Exporter (%ju bytes), "
                    "bus delivered %d progress + finished + export events\n",
                    static_cast<std::uintmax_t>(svc_size), progress_events.load());
    }

    return 0;
}
