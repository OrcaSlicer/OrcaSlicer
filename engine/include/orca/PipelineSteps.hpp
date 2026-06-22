#pragma once

#include <cstdint>

namespace orca {

// Mirrors orca_pipeline_step_t in <orca/plugin_api.h>. Kept value-stable so
// static_cast across the boundary in engine/src/Slicer.cpp is well-defined.
enum class PipelineStep : std::uint32_t {
    BeforeSlice       = 1,
    AfterPerimeters   = 2,
    AfterInfill       = 3,
    AfterIroning      = 4,
    AfterSupports     = 5,
    BeforeWipeTower   = 6,
    AfterSkirtBrim    = 7,
    BeforeGCodeExport = 8,
    AfterGCodeExport  = 9,
};

// Mirrors orca_disposition_t. Same value-stability promise.
enum class PipelineDisposition : std::uint32_t {
    Proceed = 0,
    Skip    = 1,
    Abort   = 2,
};

} // namespace orca
