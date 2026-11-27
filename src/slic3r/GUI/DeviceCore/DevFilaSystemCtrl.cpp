#include <nlohmann/json.hpp>
#include <string>
#include "DevFilaSystem.h"

#include "slic3r/GUI/DeviceManager.hpp"// TODO: remove this include
#include "slic3r/Utils/OpcpCommands.hpp"

using namespace nlohmann;
namespace Slic3r
{

int DevFilaSystem::CtrlAmsReset() const
{
    json jj_command = opcp::print::ams_reset(MachineObject::m_sequence_id++);
    return m_owner->publish_json(jj_command);
}

int DevFilaSystem::CtrlAmsStartDryingHour(int ams_id,
                                          std::string filament_type,
                                          int tag_temp,
                                          int tag_duration_hour,
                                          bool rotate_tray,
                                          int cooling_temp,
                                          bool close_power_conflict) const
{
    json jj_command = opcp::print::ams_filament_drying(MachineObject::m_sequence_id++, ams_id, static_cast<int>(DevAms::DryCtrlMode::OnTime), filament_type, tag_temp,
                                                       tag_duration_hour, rotate_tray, cooling_temp, close_power_conflict);
    return m_owner->publish_json(jj_command);
}

int DevFilaSystem::CtrlAmsStopDrying(int ams_id) const
{
    json jj_command = opcp::print::ams_filament_drying(MachineObject::m_sequence_id++, ams_id, static_cast<int>(DevAms::DryCtrlMode::Off), "", 0, 0, false, 0, false);
    return m_owner->publish_json(jj_command);
}

}