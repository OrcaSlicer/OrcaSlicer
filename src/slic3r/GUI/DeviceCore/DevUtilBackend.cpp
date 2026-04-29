#include "DevUtilBackend.h"

#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/lexical_cast.hpp>

namespace Slic3r {

MultiNozzleUtils::NozzleInfo DevUtilBackend::GetNozzleInfo(const DevNozzle& dev_nozzle)
{
    MultiNozzleUtils::NozzleInfo info;
    // Use boost::lexical_cast-safe format ("0.4" not "0.4 mm") to match slicing NozzleInfo.
    info.diameter    = boost::lexical_cast<std::string>(dev_nozzle.GetNozzleDiameter());
    info.volume_type = DevNozzle::ToNozzleVolumeType(dev_nozzle.GetNozzleFlowType());
    info.extruder_id = dev_nozzle.GetLogicExtruderId();
    return info;
}

std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> DevUtilBackend::GetNozzleGroupResult(Slic3r::GUI::Plater* plater)
{
    // Orca stores nozzle_group_result as std::optional<MultiNozzleGroupResult> (value type)
    // in GCodeProcessorResult, whereas BBL uses shared_ptr<NozzleGroupResultBase>.
    // Wrap the value in a shared_ptr so callers get the same interface.
    if (plater) {
        auto* gcode_result = plater->background_process().get_current_gcode_result();
        if (gcode_result && gcode_result->nozzle_group_result.has_value()) {
            return std::make_shared<MultiNozzleUtils::NozzleGroupResultBase>(
                gcode_result->nozzle_group_result.value());
        }
    }
    return nullptr;
}

std::unordered_map<NozzleDef, int> DevUtilBackend::CollectNozzleInfo(MultiNozzleUtils::NozzleGroupResultBase* nozzle_group_res, int logic_ext_id)
{
    std::unordered_map<NozzleDef, int> need_nozzle_map;
    if (!nozzle_group_res)
        return need_nozzle_map;

    for (auto& slicing_nozzle : nozzle_group_res->get_used_nozzles_in_extruder(logic_ext_id)) {
        try {
            NozzleDef data;
            data.nozzle_diameter = boost::lexical_cast<float>(slicing_nozzle.diameter);
            data.nozzle_flow_type = DevNozzle::ToNozzleFlowType(slicing_nozzle.volume_type);
            need_nozzle_map[data]++;
        } catch (const std::exception& e) {
            assert(0);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " exception: " << e.what();
        }
    }
    return need_nozzle_map;
}

std::optional<DevFilamentDryingPreset> DevUtilBackend::GetFilamentDryingPreset(const std::string& /*fila_id*/)
{
    return std::nullopt;
}

} // namespace Slic3r
