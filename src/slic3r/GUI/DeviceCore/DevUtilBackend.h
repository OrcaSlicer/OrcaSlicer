/**
 * @file DevUtilBackend.h
 * @brief Static helpers bridging the backend (slicer/preset) to the device GUI.
 *
 * Scope (H2C nozzle rack): exposes only GetNozzleGroupResult — the accessor the print-dispatch
 * nozzle-mapping V1 request and result-resolve helpers read. Related helpers (per-nozzle info
 * collection via the ExtruderNozzleInfos/NozzleDef type pair, and the drying-preset lookup) are
 * not implemented here and left to the consuming code.
 */

#pragma once

#include "libslic3r/MultiNozzleUtils.hpp"

#include <memory>

namespace Slic3r
{
namespace GUI { class Plater; }

class DevUtilBackend
{
public:
    DevUtilBackend() = delete;

    // for rack: the slicer's per-filament -> logical-nozzle grouping for the current plate, read off
    // the post-slice GCodeProcessorResult (plater->background_process().get_current_gcode_result()).
    // Returns nullptr when there is no plater / no current result.
    static std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> GetNozzleGroupResult(Slic3r::GUI::Plater* plater);
};

}; // namespace Slic3r
