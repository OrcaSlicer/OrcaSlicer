#include "FilamentSyncUtils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"
#include "libslic3r/Preset.hpp"

#include <nlohmann/json.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace Slic3r {

void FilamentSyncUtils::build_ams_payload(const std::string& dev_id,
                                          const std::string& model_id,
                                          int ams_count,
                                          int max_lane_index,
                                          const std::vector<AmsTrayData>& trays)
{
    // Look up MachineObject via DeviceManager
    auto* dev_manager = GUI::wxGetApp().getDeviceManager();
    if (!dev_manager) {
        return;
    }
    MachineObject* obj = dev_manager->get_my_machine(dev_id);
    if (!obj) {
        return;
    }

    // Build BBL-format JSON for DevFilaSystemParser::ParseV1_0
    nlohmann::json ams_json = nlohmann::json::object();
    nlohmann::json ams_array = nlohmann::json::array();

    // Calculate ams_exist_bits and tray_exist_bits
    unsigned long ams_exist_bits = 0;
    unsigned long tray_exist_bits = 0;

    for (int ams_id = 0; ams_id < ams_count; ++ams_id) {
        ams_exist_bits |= (1 << ams_id);

        nlohmann::json ams_unit = nlohmann::json::object();
        ams_unit["id"] = std::to_string(ams_id);
        ams_unit["info"] = "0002";  // AMS_LITE type

        nlohmann::json tray_array = nlohmann::json::array();
        int max_slot_in_this_ams = std::min(3, max_lane_index - ams_id * 4);
        for (int slot_id = 0; slot_id <= max_slot_in_this_ams; ++slot_id) {
            int slot_index = ams_id * 4 + slot_id;

            // Find tray with matching slot_index
            const AmsTrayData* tray = nullptr;
            for (const auto& t : trays) {
                if (t.slot_index == slot_index) {
                    tray = &t;
                    break;
                }
            }

            nlohmann::json tray_json = nlohmann::json::object();
            tray_json["id"] = std::to_string(slot_id);
            tray_json["tag_uid"] = "0000000000000000";

            if (tray && tray->has_filament) {
                tray_exist_bits |= (1 << slot_index);

                tray_json["tray_info_idx"] = tray->tray_info_idx;
                tray_json["tray_type"] = tray->tray_type;
                tray_json["tray_color"] = normalize_color(tray->tray_color);

                if (tray->bed_temp > 0) {
                    tray_json["bed_temp"] = std::to_string(tray->bed_temp);
                }
                if (tray->nozzle_temp > 0) {
                    tray_json["nozzle_temp_max"] = std::to_string(tray->nozzle_temp);
                }
            } else {
                tray_json["tray_info_idx"] = "";
                tray_json["tray_type"] = "";
                tray_json["tray_color"] = "00000000";
                tray_json["tray_slot_placeholder"] = "1";
            }

            tray_array.push_back(tray_json);
        }
        ams_unit["tray"] = tray_array;
        ams_array.push_back(ams_unit);
    }

    // Format as hex strings (matching BBL protocol)
    std::ostringstream ams_exist_ss;
    ams_exist_ss << std::hex << std::uppercase << ams_exist_bits;
    std::ostringstream tray_exist_ss;
    tray_exist_ss << std::hex << std::uppercase << tray_exist_bits;

    ams_json["ams"] = ams_array;
    ams_json["ams_exist_bits"] = ams_exist_ss.str();
    ams_json["tray_exist_bits"] = tray_exist_ss.str();

    // Wrap in the expected structure for ParseV1_0
    nlohmann::json print_json = nlohmann::json::object();
    print_json["ams"] = ams_json;

    // Call the parser to populate DevFilaSystem
    DevFilaSystemParser::ParseV1_0(print_json, obj, obj->GetFilaSystem().get(), false);
    BOOST_LOG_TRIVIAL(info) << "FilamentSyncUtils::build_ams_payload: Parsed " << trays.size() << " trays";

    // Set printer_type so update_sync_status() can match against the preset's printer type
    obj->printer_type = model_id;

    // Set push counters so is_info_ready() returns true for pull-mode agents
    if (obj->m_push_count == 0) {
        obj->m_push_count = 1;
    }
    if (obj->m_full_msg_count == 0) {
        obj->m_full_msg_count = 1;
    }
    obj->last_push_time = std::chrono::system_clock::now();

    // Set storage state — required for SelectMachineDialog to allow printing
    obj->GetStorage()->set_sdcard_state(DevStorage::HAS_SDCARD_NORMAL);

    // Populate module_vers so is_info_ready() passes the version check
    if (obj->module_vers.empty()) {
        DevFirmwareVersionInfo ota_info;
        ota_info.name = "ota";
        ota_info.sw_ver = "1.0.0";
        obj->module_vers.emplace("ota", ota_info);
    }
}

std::string FilamentSyncUtils::normalize_color(const std::string& color)
{
    std::string value = color;
    boost::trim(value);

    // Remove 0x or 0X prefix
    if (value.size() >= 2 && (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)) {
        value = value.substr(2);
    }
    // Remove # prefix
    if (!value.empty() && value[0] == '#') {
        value = value.substr(1);
    }

    // Extract only hex digits
    std::string normalized;
    for (char c : value) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }

    // If 6 hex digits, add FF alpha
    if (normalized.size() == 6) {
        normalized += "FF";
    }

    // Validate length
    if (normalized.size() != 8) {
        return "00000000";
    }

    return normalized;
}

std::string FilamentSyncUtils::map_filament_type_to_generic_id(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    // PLA variants
    if (upper == "PLA")           return "OGFL99";
    if (upper == "PLA-CF")        return "OGFL98";
    if (upper == "PLA SILK" || upper == "PLA-SILK") return "OGFL96";
    if (upper == "PLA HIGH SPEED" || upper == "PLA-HS" || upper == "PLA HS") return "OGFL95";

    // ABS/ASA variants
    if (upper == "ABS")           return "OGFB99";
    if (upper == "ASA")           return "OGFB98";

    // PETG/PET variants
    if (upper == "PETG" || upper == "PET") return "OGFG99";
    if (upper == "PCTG")          return "OGFG97";

    // PA/Nylon variants
    if (upper == "PA" || upper == "NYLON") return "OGFN99";
    if (upper == "PA-CF")         return "OGFN98";
    if (upper == "PPA" || upper == "PPA-CF") return "OGFN97";
    if (upper == "PPA-GF")        return "OGFN96";

    // PC variants
    if (upper == "PC")            return "OGFC99";

    // PP/PE variants
    if (upper == "PE")            return "OGFP99";
    if (upper == "PP")            return "OGFP97";

    // Support materials
    if (upper == "PVA")           return "OGFS99";
    if (upper == "HIPS")          return "OGFS98";
    if (upper == "BVOH")          return "OGFS97";

    // TPU variants
    if (upper == "TPU")           return "OGFU99";

    // Other materials
    if (upper == "EVA")           return "OGFR99";
    if (upper == "PHA")           return "OGFR98";
    if (upper == "COPE")          return "OGFLC99";
    if (upper == "SBS")           return "OFLSBS99";

    return UNKNOWN_FILAMENT_ID;
}

std::string FilamentSyncUtils::trim_and_upper(const std::string& input)
{
    std::string result = input;
    boost::trim(result);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

} // namespace Slic3r
