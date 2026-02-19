/**
 * @file DevUtilBackend.h
 * @brief Provides common static utility methods for backend (preset/slicing).
 *
 * This class offers a collection of static helper functions such as string manipulation,
 * file operations, and other frequently used utilities.
 */

#pragma once
#include "DevDefs.h"
#include "DevNozzleSystem.h"
#include "DevFilaSystem.h"

#include "libslic3r/MultiNozzleUtils.hpp"
#include <unordered_map>

 // Forward declarations
namespace Slic3r
{
class MachineObject;

namespace GUI
{
class Plater;
}
}; // namespace Slic3r::GUI

namespace Slic3r
{

class DevUtilBackend
{
public:
    DevUtilBackend() = delete;

public:
    static MultiNozzleUtils::NozzleInfo GetNozzleInfo(const DevNozzle& dev_nozzle);

    // for rack
    static std::optional<MultiNozzleUtils::MultiNozzleGroupResult> GetNozzleGroupResult(Slic3r::GUI::Plater* plater);
    static std::unordered_map<NozzleDef, int> CollectNozzleInfo(MultiNozzleUtils::MultiNozzleGroupResult* nozzle_group_res, int logic_ext_id);

    // for filament preset
    static std::optional<DevFilamentDryingPreset> GetFilamentDryingPreset(const std::string& fila_id);
};

}; // namespace Slic3r