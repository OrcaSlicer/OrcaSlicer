#include "DevFilaAmsSetting.h"
#include "DevFilaSystem.h"

#include <nlohmann/json.hpp>
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/PrinterSequenceIds.hpp"
#include <string>
#include "slic3r/GUI/DeviceCore/DevCtrl.h"

using json = nlohmann::json;

namespace Slic3r {
    int DevAmsSystemFirmwareSwitch::CrtlSwitchFirmware(int firmware_idx)
    {
        if (!m_owner) {
            return -1;
        }

        MachineObject* obj_ =  m_owner->GetOwner();

        json command_json;
        command_json["upgrade"]["command"] = "mc_for_ams_firmware_upgrade";
        const int sequence_id = obj_->m_sequence_id++;
        command_json["upgrade"]["sequence_id"] = std::to_string(sequence_id);
        command_json["upgrade"]["src_id"] = 1;// 1-Studio
        command_json["upgrade"]["id"] = firmware_idx;

        int rtn = obj_->publish_json(command_json);
        if (rtn == 0) {
            m_status = "SWITCHING";
            m_ctrl_switching = DevCtrlInfo(obj_, sequence_id, command_json, 3, 1);
        }

        return rtn;
    };
}