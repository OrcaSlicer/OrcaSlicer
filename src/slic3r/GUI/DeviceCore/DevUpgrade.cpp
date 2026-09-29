#include "DevUpgrade.h"
#include "DevUtil.h"

#include "slic3r/GUI/DeviceManager.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r {

int DevUpgrade::GetUpgradeProgressInt() const
{
    try {
        return std::stoi(m_upgrade_progress);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << e.what();
    }

    return 0;
}

void DevUpgrade::ParseUpgrade_V1_0(const json &print_jj)
{
    if (print_jj.contains("upgrade_state")) {
        const auto &upgrade_jj = print_jj["upgrade_state"];

        // status and steps
        DevJsonValParser::ParseVal(upgrade_jj, "status", m_upgrade_status);
        DevJsonValParser::ParseVal(upgrade_jj, "progress", m_upgrade_progress);
        DevJsonValParser::ParseVal(upgrade_jj, "message", m_upgrade_message);
        DevJsonValParser::ParseVal(upgrade_jj, "module", m_upgrade_module);
        DevJsonValParser::ParseVal(upgrade_jj, "err_code", m_upgrade_err_code);

        // version requested
        DevJsonValParser::ParseVal(upgrade_jj, "new_version_state", m_upgrade_new_version_state);
        DevJsonValParser::ParseVal(upgrade_jj, "consistency_request", m_upgrade_consistency_request);
        DevJsonValParser::ParseVal(upgrade_jj, "force_upgrade", m_upgrade_force_upgrade);

        // version info
        DevJsonValParser::ParseVal(upgrade_jj, "ota_new_version_number", m_ota_new_version_number);

        // version list
        if (upgrade_jj.contains("new_ver_list")) {
            m_new_ver_list.clear();
            for (auto ver_item = upgrade_jj["new_ver_list"].begin(); ver_item != upgrade_jj["new_ver_list"].end(); ver_item++) {
                DevFirmwareVersionInfo ver_info;
                DevJsonValParser::ParseVal(*ver_item, "name", ver_info.name);
                DevJsonValParser::ParseVal(*ver_item, "cur_ver", ver_info.sw_ver);
                DevJsonValParser::ParseVal(*ver_item, "new_ver", ver_info.sw_new_ver);
                m_new_ver_list[ver_info.name] = ver_info;
                if (ver_info.name == "ota" && m_ota_new_version_number.empty()) { m_ota_new_version_number = ver_info.sw_new_ver; }
            }
        } else {
            m_new_ver_list.clear();
        }

        // try to parse display state
        ParseUpgradeDisplayState(upgrade_jj);
    }

    if (print_jj.contains("cfg")) {
        const std::string &cfg = print_jj["cfg"].get<std::string>();
        if (!cfg.empty()) { m_upgrade_force_upgrade = DevUtil::get_flag_bits(cfg, 2); }
    }
}

void DevUpgrade::ParseUpgradeDisplayState(const json &upgrade_state_jj)
{
    if (upgrade_state_jj.contains("dis_state")) {
        if ((int) m_upgrade_display_state != upgrade_state_jj["dis_state"].get<int>()) {
            DevJsonValParser::ParseVal(upgrade_state_jj, "dis_state", m_upgrade_display_state);

            // update the version after upgrade finished
            if (m_upgrade_display_state == DevFirmwareUpgradeState::UpgradingFinished) {
                Slic3r::GUI::wxGetApp().CallAfter([this] {
                    m_owner->command_get_version();
                });
            }

            // lan mode printer, hide upgrade avaliable state
            if (m_upgrade_display_state == DevFirmwareUpgradeState::UpgradingAvaliable &&
                m_owner->is_lan_mode_printer()) {
                m_upgrade_display_state = DevFirmwareUpgradeState::UpgradingUnavaliable;
            }
        }
    } else {
        // BBS compatibility with old version
        if (m_upgrade_status == "DOWNLOADING" ||
            m_upgrade_status == "FLASHING" ||
            m_upgrade_status == "UPGRADE_REQUEST" ||
            m_upgrade_status == "PRE_FLASH_START" ||
            m_upgrade_status == "PRE_FLASH_SUCCESS") {
            m_upgrade_display_state = DevFirmwareUpgradeState::UpgradingInProgress;
        } else if (m_upgrade_status == "UPGRADE_SUCCESS" ||
                   m_upgrade_status == "DOWNLOAD_FAIL" ||
                   m_upgrade_status == "FLASH_FAIL" ||
                   m_upgrade_status == "PRE_FLASH_FAIL" ||
                   m_upgrade_status == "UPGRADE_FAIL") {
            m_upgrade_display_state = DevFirmwareUpgradeState::UpgradingFinished;
        } else {
            if (HasNewVersion()) {
                m_upgrade_display_state = DevFirmwareUpgradeState::UpgradingAvaliable;
            } else {
                m_upgrade_display_state = DevFirmwareUpgradeState::UpgradingUnavaliable;
            }
        }
    }
}

#define UpgradeNoError 0
#define UpgradeDownloadFailed -1
#define UpgradeVerfifyFailed -2
#define UpgradeFlashFailed -3
#define UpgradePrinting -4

wxString DevUpgrade::GetUpgradeErrCodeStr() const
{
    switch (m_upgrade_err_code) {
    case UpgradeNoError: return _L("Update successful.");
    case UpgradeDownloadFailed: return _L("Downloading failed.");
    case UpgradeVerfifyFailed: return _L("Verification failed.");
    case UpgradeFlashFailed: return _L("Update failed.");
    case UpgradePrinting: return _L("Update failed.");
    default: return _L("Update failed.");
    }

    return "";
}

} // namespace Slic3r