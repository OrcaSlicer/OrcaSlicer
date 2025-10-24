#include <nlohmann/json.hpp>
#include "DevCtrl.h"

// TODO: remove this include
#include "DevUtil.h"
#include "slic3r/GUI/DeviceManager.hpp"

using namespace nlohmann;

namespace Slic3r
{
    int DevCtrl::command_select_extruder(int id)
    {
        json j;
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        j["print"]["command"] = "select_extruder";
        j["print"]["extruder_index"] = id;
        int rtn = m_obj->publish_json(j, 1);
        if (rtn == 0)
        {
            m_obj->targ_nozzle_id_from_pc = id;
        }

        return rtn;
    }
}