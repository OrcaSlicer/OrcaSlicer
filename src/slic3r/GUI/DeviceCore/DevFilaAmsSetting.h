#pragma once
#include <optional>
#include <nlohmann/json.hpp>
#include "DevCtrl.h"

namespace Slic3r
{

class DevFilaSystem;
class DevAmsSystemSetting
{
public:
    DevAmsSystemSetting(DevFilaSystem* owner) : m_owner(owner) {};

public:
    // getters
    std::optional<bool> IsDetectOnInsertEnabled() const { return m_enable_detect_on_insert; };
    bool IsDetectOnPowerupEnabled() const { return m_enable_detect_on_powerup; }
    bool IsDetectRemainEnabled() const { return m_enable_detect_remain; }
    bool IsAutoRefillEnabled() const { return m_enable_auto_refill; }

    // setters
    void Reset();
    void SetDetectOnInsertEnabled(bool enable) { m_enable_detect_on_insert = enable; }
    void SetDetectOnPowerupEnabled(bool enable) { m_enable_detect_on_powerup = enable; }
    void SetDetectRemainEnabled(bool enable) { m_enable_detect_remain = enable; }
    void SetAutoRefillEnabled(bool enable) { m_enable_auto_refill = enable; }

private:
    DevFilaSystem* m_owner = nullptr;

    std::optional<bool> m_enable_detect_on_insert = false;
    bool m_enable_detect_on_powerup = false;
    bool m_enable_detect_remain = false;
    bool m_enable_auto_refill = false;
};

class DevAmsSystemFirmwareSwitch
{
public:
    enum DevAmsSystemIdx : int
    {
        IDX_DC = -1,
        IDX_LITE = 0,
        IDX_AMS_AMS2_AMSHT = 1,
    };

    struct DevAmsSystemFirmware
    {
        DevAmsSystemIdx m_firmare_idx = IDX_DC;
        std::string     m_name;
        std::string     m_version;

    public:
        bool operator==(const DevAmsSystemFirmware& o) const
        {
            return (m_firmare_idx == o.m_firmare_idx) &&
                   (m_name == o.m_name) &&
                   (m_version == o.m_version);
        };
    };

public:
    static std::shared_ptr<DevAmsSystemFirmwareSwitch> Create(DevFilaSystem* owner)
    {
        return std::shared_ptr<DevAmsSystemFirmwareSwitch>(new DevAmsSystemFirmwareSwitch(owner));
    };

protected:
    DevAmsSystemFirmwareSwitch(DevFilaSystem* owner) : m_owner(owner) {};

public:
    DevFilaSystem* GetFilaSystem() const { return m_owner; };
    bool SupportSwitchFirmware() const { return !m_firmwares.empty();};

    DevAmsSystemIdx GetCurrentFirmwareIdxSel() const { return m_current_firmware_sel.m_firmare_idx; };
    DevAmsSystemIdx GetCurrentFirmwareIdxRun() const { return m_current_firmware_run.m_firmare_idx; };
    std::unordered_map<int, DevAmsSystemFirmware> GetSuppotedFirmwares() const { return m_firmwares;};

    bool IsSwitching() const { return m_status == "SWITCHING";};
    bool IsIdle() const { return m_status == "IDLE";};

    // commands
    int CrtlSwitchFirmware(int firmware_idx);

    // setters
    void Reset();
    void ParseFirmwareSwitch(const nlohmann::json& j);

private:
    DevFilaSystem* m_owner = nullptr;

    std::string m_status;

    DevAmsSystemFirmware m_current_firmware_run;
    DevAmsSystemFirmware m_current_firmware_sel;
    std::unordered_map<int, DevAmsSystemFirmware> m_firmwares;

    DevCtrlInfo m_ctrl_switching;
};

}// namespace Slic3r