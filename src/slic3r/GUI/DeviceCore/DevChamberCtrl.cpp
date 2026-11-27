#include "DevChamber.h"
#include <nlohmann/json.hpp>
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/OpcpCommands.hpp"
#include <string>

using json = nlohmann::json;

namespace Slic3r {

int DevChamber::CtrlSetChamberTemp(int temp) 
{
    json j = opcp::print::set_chamber_temp(MachineObject::m_sequence_id++, temp);

    return m_owner->publish_json(j, 1);
}

} // namespace Slic3r