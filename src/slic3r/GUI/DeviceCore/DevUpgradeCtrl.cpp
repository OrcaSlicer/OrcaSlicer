#include "DevUpgrade.h"

#include <nlohmann/json.hpp>
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/OpcpCommands.hpp"
#include <string>
#include "slic3r/GUI/DeviceCore/DevFirmware.h"

using json = nlohmann::json;

namespace Slic3r {

int DevUpgrade::CtrlUpgradeConfirm()
{
    json j = opcp::upgrade::upgrade_confirm(MachineObject::m_sequence_id++);
    return m_owner->publish_json(j);
}

int DevUpgrade::CtrlUpgradeConsistencyConfirm()
{
    json j = opcp::upgrade::consistency_confirm(MachineObject::m_sequence_id++);
    return m_owner->publish_json(j);
}

int DevUpgrade::CtrlUpgradeFirmware(FirmwareInfo info)
{
    json j = opcp::upgrade::start(MachineObject::m_sequence_id++, info.url, info.module_type, info.version);

    return m_owner->publish_json(j);
}

int DevUpgrade::CtrlUpgradeModule(std::string url, std::string module_type, std::string version)
{
    json j = opcp::upgrade::start(MachineObject::m_sequence_id++, url, module_type, version);

    return m_owner->publish_json(j);
}

} // namespace Slic3r