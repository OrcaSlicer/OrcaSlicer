#include "DevUpgrade.h"
#include "DevUtil.h"

#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r {

int DevUpgrade::CtrlUpgradeConfirm()
{
    json j;
    j["upgrade"]["command"]     = "upgrade_confirm";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["src_id"]      = 1; // 1 for slicer
    return m_owner->publish_json(j);
}

int DevUpgrade::CtrlUpgradeConsistencyConfirm()
{
    json j;
    j["upgrade"]["command"]     = "consistency_confirm";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["src_id"]      = 1; // 1 for slicer
    return m_owner->publish_json(j);
}

int DevUpgrade::CtrlUpgradeFirmware(FirmwareInfo info)
{
    std::string version     = info.version;
    std::string dst_url     = info.url;
    std::string module_name = info.module_type;

    json j;
    j["upgrade"]["command"]     = "start";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["url"]         = info.url;
    j["upgrade"]["module"]      = info.module_type;
    j["upgrade"]["version"]     = info.version;
    j["upgrade"]["src_id"]      = 1;

    return m_owner->publish_json(j);
}

int DevUpgrade::CtrlUpgradeModule(std::string url, std::string module_type, std::string version)
{
    json j;
    j["upgrade"]["command"]     = "start";
    j["upgrade"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["upgrade"]["url"]         = url;
    j["upgrade"]["module"]      = module_type;
    j["upgrade"]["version"]     = version;
    j["upgrade"]["src_id"]      = 1;

    return m_owner->publish_json(j);
}

} // namespace Slic3r