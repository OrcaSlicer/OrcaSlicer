#include "DevUtilBackend.h"

#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r
{

std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> DevUtilBackend::GetNozzleGroupResult(Slic3r::GUI::Plater* plater)
{
    if (plater && plater->background_process().get_current_gcode_result()) {
        return plater->background_process().get_current_gcode_result()->nozzle_group_result;
    }

    return nullptr;
}

}; // namespace Slic3r
