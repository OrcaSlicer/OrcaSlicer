/**
 * @file DevUtilBackend.h
 * @brief Provides common static utility methods for backend (preset/slicing).
 */

#pragma once
#include "DevDefs.h"
#include "DevFilaSystem.h"

#include "libslic3r/MultiNozzleUtils.hpp"
#include <optional>
#include <string>

namespace Slic3r
{
namespace GUI { class Plater; }

class DevUtilBackend
{
public:
    DevUtilBackend() = delete;

public:

    // for rack: the slicer's per-filament -> logical-nozzle grouping for the current plate, read off
    // the post-slice GCodeProcessorResult (plater->background_process().get_current_gcode_result()).
    // Returns nullptr when there is no plater / no current result.
    static std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> GetNozzleGroupResult(Slic3r::GUI::Plater* plater);

    // for filament preset
    static std::optional<DevFilamentDryingPreset> GetFilamentDryingPreset(const std::string& fila_id);
};

}; // namespace Slic3r
