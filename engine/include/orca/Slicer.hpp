#pragma once

#include "Result.hpp"

#include <cstdint>
#include <memory>
#include <string>

// Forward declarations of libslic3r types accepted by the explicit-input
// slice overload. The engine owns the Print built from them; callers keep
// ownership of the Model/config they pass.
namespace Slic3r {
class Model;
class DynamicPrintConfig;
class Print;
class GCodeProcessorResult;
} // namespace Slic3r

namespace orca {

class Session;

using SliceHandle = std::uint64_t;

enum class SliceTechnology {
    FFF,
    SLA,
};

enum class SliceState {
    NotStarted,
    Queued,
    Running,
    Completed,
    Cancelled,
    Failed,
};

struct SliceParams {
    int             plate_id   = 0;
    SliceTechnology technology = SliceTechnology::FFF;
};

struct SliceStatus {
    SliceState  state    = SliceState::NotStarted;
    float       progress = 0.0f;
    std::string message;
    std::string error;
};

class Slicer {
public:
    ~Slicer();

    Slicer(const Slicer&)            = delete;
    Slicer& operator=(const Slicer&) = delete;
    Slicer(Slicer&&)                 = delete;
    Slicer& operator=(Slicer&&)      = delete;

    // Slice the Session's attached Model with the Presets full_config.
    // Requires a Session with both a Model (project) and a PresetBundle
    // (presets) attached.
    Result<SliceHandle> request_slice(SliceParams params);

    // Slice explicit inputs without touching Session state — the headless /
    // plugin path. The engine builds and owns the Print from the given Model
    // and config (which the caller continues to own). The completed Print is
    // exported by Exporter::export_gcode just like the Session path.
    Result<SliceHandle> request_slice(SliceParams                       params,
                                      const Slic3r::Model&              model,
                                      const Slic3r::DynamicPrintConfig& config);

    void                cancel(SliceHandle handle);
    SliceStatus         status(SliceHandle handle) const;

    bool                is_busy() const;
    void                cancel_all();

private:
    friend class Session;
    friend class Exporter;
    Slicer();

    // Set by Session::create() so request_slice() can reach project()/presets().
    void bind_session(Session* session);

    // Worker entry — runs the FFF pipeline on a worker thread over an owned
    // Model copy. Defined in Slicer.cpp.
    void run_slice(Slic3r::Model model);
    void finish_worker(bool store_print, std::unique_ptr<Slic3r::Print> print);

    // The completed FFF Print + its G-code result, consumed by the Exporter.
    // Null until a successful FFF slice has completed.
    Slic3r::Print*                completed_fff_print() const;
    Slic3r::GCodeProcessorResult* gcode_result() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace orca
