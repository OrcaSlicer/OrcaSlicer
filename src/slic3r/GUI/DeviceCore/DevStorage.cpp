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
        if (print_json.contains("sdcard")) {
            if (print_json["sdcard"].get<bool>())
                system->m_sdcard_state = DevStorage::SdcardState::HAS_SDCARD_NORMAL;
            else
                system->m_sdcard_state = DevStorage::SdcardState::NO_SDCARD;
        } else {
            system->m_sdcard_state = DevStorage::SdcardState::NO_SDCARD;
        }
    }
}


} // namespace Slic3r