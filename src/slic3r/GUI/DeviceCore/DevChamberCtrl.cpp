#include "DevChamber.h"
#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r {

int DevChamber::CtrlSetChamberTemp(int temp) 
{
    json j;
    j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["print"]["command"]     = "set_ctt";
    j["print"]["ctt_val"]     = temp;

    return m_owner->publish_json(j, 1);
}

} // namespace Slic3r