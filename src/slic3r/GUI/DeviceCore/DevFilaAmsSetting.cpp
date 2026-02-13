#include "DevFilaAmsSetting.h"
#include "DevUtil.h"

namespace Slic3r {

void DevAmsSystemSetting::Reset()
{
    m_enable_detect_on_insert.reset();
    SetDetectOnPowerupEnabled(false);
    SetDetectRemainEnabled(false);
    SetAutoRefillEnabled(false);
}

void DevAmsSystemFirmwareSwitch::Reset()
{
    m_status.clear();
    m_current_firmware_run = DevAmsSystemFirmware();
    m_current_firmware_sel = DevAmsSystemFirmware();
    m_firmwares.clear();
}

void DevAmsSystemFirmwareSwitch::ParseFirmwareSwitch(const nlohmann::json& j)
{
    if (!m_ctrl_switching.CheckCanUpdateData(j.contains("upgrade") ? j["upgrade"] :j)) {
        return;
    }

    if (j.contains("print")) {
        const auto& print_jj = j["print"];
        if (print_jj.contains("upgrade_state")) {
            const auto& upgrade_jj = print_jj["upgrade_state"];
            if (upgrade_jj.contains("mc_for_ams_firmware")) {
                const auto& mc_for_ams_firmware_jj = upgrade_jj["mc_for_ams_firmware"];
                if (mc_for_ams_firmware_jj.contains("firmware")) {
                    m_firmwares.clear();
                    const auto& firmwares = mc_for_ams_firmware_jj["firmware"];
                    for (auto item : firmwares) {
                        DevAmsSystemFirmware firmware;
                        DevJsonValParser::ParseVal(item, "id", firmware.m_firmare_idx);
                        DevJsonValParser::ParseVal(item, "name", firmware.m_name);
                        DevJsonValParser::ParseVal(item, "version", firmware.m_version);
                        m_firmwares[firmware.m_firmare_idx] = firmware;
                    }
                }

                if (mc_for_ams_firmware_jj.contains("current_firmware_id")) {
                    int idx = DevJsonValParser::GetVal(mc_for_ams_firmware_jj, "current_firmware_id", -1);
                    if (m_firmwares.count(idx) != 0) {
                        m_current_firmware_sel = m_firmwares[idx];
                    } else {
                        m_current_firmware_sel = DevAmsSystemFirmware();
                    }
                }

                if (mc_for_ams_firmware_jj.contains("current_run_firmware_id")) {
                    auto idx = DevJsonValParser::GetVal(mc_for_ams_firmware_jj, "current_run_firmware_id", IDX_DC);
                    if (m_firmwares.count(idx) != 0) {
                        m_current_firmware_run = m_firmwares[idx];
                    } else {
                        m_current_firmware_run = DevAmsSystemFirmware();
                    }
                }

                DevJsonValParser::ParseVal(mc_for_ams_firmware_jj, "status", m_status);
            }
        }
    }
}

}