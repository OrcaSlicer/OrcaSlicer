#pragma once
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"

#include "DevDefs.h"
#include "DevFirmware.h"

namespace Slic3r {

class MachineObject;
class DevUpgrade
{
public:
    static std::shared_ptr<DevUpgrade> Create(MachineObject *owner) { return std::shared_ptr<DevUpgrade>(new DevUpgrade(owner)); }

public:
    // upgrade status
    bool                    IsUpgrading() const { return m_upgrade_display_state == DevFirmwareUpgradeState::UpgradingInProgress; }
    bool                    IsUpgradeAvaliable() const { return m_upgrade_display_state == DevFirmwareUpgradeState::UpgradingAvaliable; }
    DevFirmwareUpgradeState GetUpgradeState() const { return m_upgrade_display_state; }
    std::string             GetUpgradeStatusStr() const { return m_upgrade_status; }

    // upgrade request
    bool IsUpgradeConsistencyRequest() const { return m_upgrade_consistency_request; }
    bool IsUpgradeForceUpgrade() const { return m_upgrade_force_upgrade; }

    bool                                          HasNewVersion() const { return m_upgrade_new_version_state == 1; }
    std::string                                   GetOtaNewVersion() const { return m_ota_new_version_number; }
    std::map<std::string, DevFirmwareVersionInfo> GetNewVersionList() const { return m_new_ver_list; }

    // upgrading
    std::string GetUpgradeModuleStr() const { return m_upgrade_module; }
    std::string GetUpgradeProgressStr() const { return m_upgrade_progress; }
    int         GetUpgradeProgressInt() const;
    std::string GetUpgradeMessageStr() const { return m_upgrade_message; }
    wxString    GetUpgradeErrCodeStr() const;

public:
    // ctrls
    int CtrlUpgradeConfirm();
    int CtrlUpgradeConsistencyConfirm();
    int CtrlUpgradeFirmware(FirmwareInfo info);
    int CtrlUpgradeModule(std::string url, std::string module_type, std::string version);

    // parser
    void ParseUpgrade_V1_0(const json &print_jj);
    void ParseUpgradeDisplayState(const json &upgrade_state_jj);

protected:
    DevUpgrade(MachineObject *owner) : m_owner(owner) {}

private:
    MachineObject *m_owner = nullptr;

    int  m_upgrade_new_version_state   = 0; // 0: invalid version, 1: new version available, 2: no new version
    bool m_upgrade_consistency_request = false;
    bool m_upgrade_force_upgrade       = false;

    std::string                                   m_ota_new_version_number;
    std::map<std::string, DevFirmwareVersionInfo> m_new_ver_list; // some protcols have version list
    DevFirmwareUpgradeState                       m_upgrade_display_state = DevFirmwareUpgradeState::DC;

    int         m_upgrade_err_code;
    std::string m_upgrade_status;
    std::string m_upgrade_progress;
    std::string m_upgrade_message;
    std::string m_upgrade_module;
};

#if 0 /*TODO*/
class DevUpgradeGetVersionInfo
{
public:
    DevUpgradeGetVersionInfo(MachineObject* obj) : m_owner(obj) {} ;

public:
    bool IsEmpty() const { m_module_version_map.empty();}

    DevFirmwareVersionInfo GetAirPumpVersionInfo() const { return m_air_pump_version_info; }
    DevFirmwareVersionInfo GetLaserVersionInfo() const { return m_laser_version_info; }
    DevFirmwareVersionInfo GetCuttingModuleVersionInfo() const { return m_cutting_module_version_info; }
    DevFirmwareVersionInfo GetExtinguishVersionInfo() const { return m_extinguish_version_info; }
    DevFirmwareVersionInfo GetRotaryVersionInfo() const { return m_rotary_version_info; }
    std::map<std::string, DevFirmwareVersionInfo> GetModuleVersionInfoMap() const { return m_module_version_map; }

public:
    int CtrlGetVersion(bool with_retry = true);

    void ParseGetVersion(const json &print_jj);

private:
    MachineObject *m_owner;

    // some version info
    DevFirmwareVersionInfo                        m_air_pump_version_info;
    DevFirmwareVersionInfo                        m_laser_version_info;
    DevFirmwareVersionInfo                        m_cutting_module_version_info;
    DevFirmwareVersionInfo                        m_extinguish_version_info;
    DevFirmwareVersionInfo                        m_rotary_version_info;
    std::map<std::string, DevFirmwareVersionInfo> m_module_version_map;

    // ctrl retry
    int m_retry_count = 0;
    int m_max_retry   = 3;
};
#endif

} // namespace Slic3r