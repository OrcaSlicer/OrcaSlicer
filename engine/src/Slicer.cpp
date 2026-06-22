// Slicer — engine-side PrintService (Phase 0.4c).
//
// Async FFF slice service. request_slice() launches the libslic3r FFF
// pipeline (auto_assign_extruders -> apply -> validate -> process) on a worker
// thread over an owned Model copy and an owned config. Progress is published
// via the Print status callback (which runs on the worker thread). The
// completed Print and its GCodeProcessorResult are consumed by the Exporter.
//
// The proven low-level sequence is mirrored from engine/cli/main.cpp.

#include "orca/Slicer.hpp"
#include "orca/Session.hpp"
#include "orca/Events.hpp"
#include "orca/EventTypes.hpp"
#include "orca/PipelineSteps.hpp"
#include "orca/plugin_api.h"
#include "PluginRegistry.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/PrintBase.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libslic3r/PresetBundle.hpp>

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace orca {

struct Slicer::Impl {
    Session* session = nullptr;

    mutable std::mutex   mtx;   // guards status, handle, busy, print, active_print
    std::thread          worker;
    std::atomic<bool>    cancel_requested{false};

    SliceHandle next_handle    = 1;
    SliceHandle current_handle = 0;
    bool        busy           = false;

    SliceStatus status;

    std::unique_ptr<Slic3r::Print> print;          // completed print, consumed by Exporter
    Slic3r::Print*                 active_print = nullptr;  // live print during process()

    Slic3r::GCodeProcessorResult   gcode_result;
    Slic3r::DynamicPrintConfig     owned_config;   // owned for the duration of the slice

    void join_worker() {
        if (worker.joinable())
            worker.join();
    }
};

Slicer::Slicer() : impl_(std::make_unique<Impl>()) {}

Slicer::~Slicer() {
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->cancel_requested = true;
        if (impl_->active_print)
            impl_->active_print->cancel();
    }
    impl_->join_worker();
}

void Slicer::bind_session(Session* session) {
    impl_->session = session;
}

Result<SliceHandle> Slicer::request_slice(SliceParams params) {
    if (params.technology != SliceTechnology::FFF)
        return err<SliceHandle>(ErrorCode::Unsupported,
                                "Slicer::request_slice — only FFF is supported");

    if (impl_->session == nullptr)
        return err<SliceHandle>(ErrorCode::InvalidState,
                                "Slicer::request_slice — no Session bound");
    if (!impl_->session->project().has_model())
        return err<SliceHandle>(ErrorCode::InvalidState,
                                "Slicer::request_slice — no Model attached to project");
    if (!impl_->session->presets().has_bundle())
        return err<SliceHandle>(ErrorCode::InvalidState,
                                "Slicer::request_slice — no PresetBundle attached");

    const Slic3r::Model&       model  = impl_->session->project().raw();
    Slic3r::DynamicPrintConfig config = impl_->session->presets().raw().full_config();
    return request_slice(params, model, config);
}

Result<SliceHandle> Slicer::request_slice(SliceParams                       params,
                                          const Slic3r::Model&              model,
                                          const Slic3r::DynamicPrintConfig& config) {
    if (params.technology != SliceTechnology::FFF)
        return err<SliceHandle>(ErrorCode::Unsupported,
                                "Slicer::request_slice — only FFF is supported");

    std::lock_guard<std::mutex> lk(impl_->mtx);

    if (impl_->busy)
        return err<SliceHandle>(ErrorCode::InvalidState,
                                "Slicer::request_slice — a slice is already running");

    // Reap any finished-but-not-joined worker before launching a new one.
    impl_->join_worker();

    const SliceHandle handle = impl_->next_handle++;
    impl_->current_handle     = handle;
    impl_->busy               = true;
    impl_->cancel_requested   = false;
    impl_->status             = SliceStatus{SliceState::Queued, 0.0f, "queued", {}};

    Slic3r::Model work_model = model;
    impl_->owned_config      = config;

    impl_->worker = std::thread([this, wm = std::move(work_model)]() mutable {
        this->run_slice(std::move(wm));
    });

    return ok<SliceHandle>(handle);
}

void Slicer::run_slice(Slic3r::Model work_model) {
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->status.state    = SliceState::Running;
        impl_->status.progress = 0.0f;
        impl_->status.message  = "running";
    }

    auto print = std::make_unique<Slic3r::Print>();
    print->restart();

    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->active_print = print.get();
    }

    // The status callback runs ON THE WORKER THREAD as process() advances —
    // it must lock the Impl mutex (short critical section) to publish progress.
    print->set_status_callback([this](const Slic3r::PrintBase::SlicingStatus& s) {
        float       progress;
        std::string message;
        SliceHandle handle;
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->status.progress = static_cast<float>(s.percent) / 100.0f;
            if (!s.text.empty())
                impl_->status.message = s.text;
            progress = impl_->status.progress;
            message  = impl_->status.message;
            handle   = impl_->current_handle;
        }
        if (impl_->session)
            impl_->session->events().publish<SlicingProgress>({handle, progress, message});
    });

    // Wire the typed event sink into the Print so libslic3r-side publishes
    // (Phase 1.4.2 inserts inside Print::process / Print::export_gcode) fan
    // out onto this session's bus tagged with the current slice handle.
    if (impl_->session != nullptr) {
        print->set_events_sink(&impl_->session->events(), impl_->current_handle);
    }

    // --- Phase 2.1.2d: snapshot pipeline observer + interceptor slots into the
    // Print's std::function hooks. Snapshot at slice-start so plugins that
    // (un)register mid-slice don't perturb the in-flight slice — same pattern as
    // orca::Events subscriber dispatch. ---
    if (impl_->session != nullptr) {
        auto& registry = impl_->session->plugin_registry();
        const std::uint64_t slice_handle = impl_->current_handle;

        auto observers = registry.snapshot(ORCA_SLOT_PIPELINE_OBSERVER);
        if (!observers.empty()) {
            print->set_step_observer(
                [snap = std::move(observers), slice_handle](orca::PipelineStep step) {
                    const auto c_step = static_cast<orca_pipeline_step_t>(step);
                    for (const auto& e : snap) {
                        const auto* vt = static_cast<const orca_slot_pipeline_observer_t*>(e.vtable);
                        if (vt && vt->on_step)
                            vt->on_step(c_step, slice_handle, e.user_data);
                    }
                });
        }

        auto interceptors = registry.snapshot(ORCA_SLOT_PIPELINE_INTERCEPTOR);
        if (!interceptors.empty()) {
            print->set_step_interceptor(
                [snap = std::move(interceptors), slice_handle](orca::PipelineStep step) {
                    const auto c_step = static_cast<orca_pipeline_step_t>(step);
                    orca::PipelineDisposition fold = orca::PipelineDisposition::Proceed;
                    for (const auto& e : snap) {
                        const auto* vt = static_cast<const orca_slot_pipeline_interceptor_t*>(e.vtable);
                        if (!vt || !vt->on_step)
                            continue;
                        const auto d_c = vt->on_step(c_step, slice_handle, e.user_data);
                        const auto d   = static_cast<orca::PipelineDisposition>(d_c);
                        if (d == orca::PipelineDisposition::Abort) {
                            fold = orca::PipelineDisposition::Abort;        // sticky-Abort wins
                        } else if (d == orca::PipelineDisposition::Skip
                                   && fold == orca::PipelineDisposition::Proceed) {
                            fold = orca::PipelineDisposition::Skip;          // any-Skip beats Proceed
                        }
                    }
                    return fold;
                });
        }

        // Phase 2.2.2: snapshot the gcode filter slot. Runs after export_gcode
        // writes the final G-code to disk; any non-OK rc aborts the export and
        // propagates as an exception out of Print::export_gcode.
        auto filters = registry.snapshot(ORCA_SLOT_GCODE_FILTER);
        if (!filters.empty()) {
            print->set_gcode_filter_callback(
                [snap = std::move(filters), slice_handle](const std::string& path) -> orca_error_code_t {
                    for (const auto& e : snap) {
                        const auto* vt = static_cast<const orca_slot_gcode_filter_t*>(e.vtable);
                        if (!vt || !vt->filter)
                            continue;
                        const auto rc = vt->filter(path.c_str(), slice_handle, e.user_data);
                        if (rc != ORCA_OK)
                            return rc;
                    }
                    return ORCA_OK;
                });
        }

        // Phase 2.3.2: snapshot the placeholder provider slot. Fires once at the top of
        // Print::export_gcode, BEFORE GCode::do_export captures the parser. Each provider
        // in the chain may push variables via the host's placeholder_set_{string,int,float}
        // vtable entries. Any non-OK rc aborts the export.
        auto providers = registry.snapshot(ORCA_SLOT_PLACEHOLDER_PROVIDER);
        if (!providers.empty()) {
            print->set_placeholder_provider_callback(
                [snap = std::move(providers)](std::uint64_t slice_handle) -> orca_error_code_t {
                    for (const auto& e : snap) {
                        const auto* vt = static_cast<const orca_slot_placeholder_provider_t*>(e.vtable);
                        if (!vt || !vt->on_provide)
                            continue;
                        const auto rc = vt->on_provide(slice_handle, e.user_data);
                        if (rc != ORCA_OK)
                            return rc;
                    }
                    return ORCA_OK;
                });
        }
    }

    try {
        for (Slic3r::ModelObject* mo : work_model.objects)
            print->auto_assign_extruders(mo);

        print->apply(work_model, impl_->owned_config);

        if (impl_->cancel_requested.load())
            print->cancel();

        Slic3r::StringObjectException verr = print->validate();
        if (!verr.string.empty()) {
            {
                std::lock_guard<std::mutex> lk(impl_->mtx);
                impl_->status.state   = SliceState::Failed;
                impl_->status.message = "validation failed";
                impl_->status.error   = verr.string;
            }
            finish_worker(false, nullptr);
            return;
        }

        print->process();

        if (print->objects().empty()) {
            {
                std::lock_guard<std::mutex> lk(impl_->mtx);
                impl_->status.state   = SliceState::Failed;
                impl_->status.message = "slice produced no print objects";
                impl_->status.error   = "no print objects";
            }
            finish_worker(false, nullptr);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->status.state    = SliceState::Completed;
            impl_->status.progress = 1.0f;
            impl_->status.message  = "completed";
            impl_->status.error.clear();
        }
        finish_worker(true, std::move(print));
    } catch (const Slic3r::CanceledException&) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->status.state   = SliceState::Cancelled;
            impl_->status.message = "cancelled";
        }
        finish_worker(false, nullptr);
    } catch (const std::exception& ex) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->status.state   = SliceState::Failed;
            impl_->status.message = "slice failed";
            impl_->status.error   = ex.what();
        }
        finish_worker(false, nullptr);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            impl_->status.state   = SliceState::Failed;
            impl_->status.message = "slice failed";
            impl_->status.error   = "unknown error";
        }
        finish_worker(false, nullptr);
    }
}

void Slicer::finish_worker(bool store_print, std::unique_ptr<Slic3r::Print> print) {
    SliceHandle handle;
    bool        success;
    std::string error;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->active_print = nullptr;
        if (store_print)
            impl_->print = std::move(print);
        else
            impl_->print.reset();
        impl_->busy = false;
        handle  = impl_->current_handle;
        success = impl_->status.state == SliceState::Completed;
        error   = impl_->status.error;
    }
    if (impl_->session)
        impl_->session->events().publish<SlicingFinished>({handle, success, error});
}

void Slicer::cancel(SliceHandle handle) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (handle != impl_->current_handle || !impl_->busy)
        return;
    impl_->cancel_requested = true;
    if (impl_->active_print)
        impl_->active_print->cancel();
}

SliceStatus Slicer::status(SliceHandle handle) const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (handle != impl_->current_handle)
        return SliceStatus{};
    return impl_->status;
}

bool Slicer::is_busy() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->busy;
}

void Slicer::cancel_all() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->cancel_requested = true;
    if (impl_->active_print)
        impl_->active_print->cancel();
}

Slic3r::Print* Slicer::completed_fff_print() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->print.get();
}

Slic3r::GCodeProcessorResult* Slicer::gcode_result() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return &impl_->gcode_result;
}

} // namespace orca
