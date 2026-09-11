#include "DevStorage.h"
#include "slic3r/GUI/DeviceManager.hpp"


namespace Slic3r {

DevStorage::SdcardState Slic3r::DevStorage::set_sdcard_state(int state)

{
    if (state < DevStorage::NO_SDCARD || state > DevStorage::SDCARD_STATE_NUM) {
        m_sdcard_state = DevStorage::NO_SDCARD;
    } else {
        m_sdcard_state = DevStorage::SdcardState(state);
    }
    return m_sdcard_state;
}

 void DevStorage::ParseV1_0(const json &print_json, DevStorage *system)
{
     if (system)
     {
        try {
            if (print_json.contains("sdcard")) {
                if (print_json["sdcard"].get<bool>())
                    system->m_sdcard_state = DevStorage::SdcardState::HAS_SDCARD_NORMAL;
                else
                    system->m_sdcard_state = DevStorage::SdcardState::NO_SDCARD;
            } else {
                system->m_sdcard_state = DevStorage::SdcardState::NO_SDCARD;
            }

            // parse timelapse storage space from cam push data
            if (print_json.contains("device") && print_json["device"].is_object()) {
                const auto& device_json = print_json["device"];
                if (device_json.contains("cam") && device_json["cam"].is_object()) {
                    const auto& cam_data = device_json["cam"];
                    if (cam_data.contains("tl_internal_free_kb"))
                        system->tl_internal_free_kb  = cam_data["tl_internal_free_kb"].get<int>();
                    if (cam_data.contains("tl_internal_total_kb"))
                        system->tl_internal_total_kb = cam_data["tl_internal_total_kb"].get<int>();
                    if (cam_data.contains("tl_external_free_kb"))
                        system->tl_external_free_kb  = cam_data["tl_external_free_kb"].get<int>();
                    if (cam_data.contains("tl_external_total_kb"))
                        system->tl_external_total_kb = cam_data["tl_external_total_kb"].get<int>();
                }
            }
        } catch (...) {}
     }
}

bool DevStorage::is_timelapse_storage_low(const std::string& storage) const
{
    const int THRESHOLD_KB = 20480; // Orca: 20MB (20480 KB; corrected mislabeled comment)
    if (storage == "internal")
        return tl_internal_free_kb >= 0 && tl_internal_free_kb < THRESHOLD_KB;
    else
        return tl_external_free_kb >= 0 && tl_external_free_kb < THRESHOLD_KB;
}

} // namespace Slic3r