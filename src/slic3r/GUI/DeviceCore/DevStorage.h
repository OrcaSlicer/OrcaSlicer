#pragma once
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"

namespace Slic3r {

class MachineObject;


class DevStorage
{
public:
    DevStorage(MachineObject *obj) : m_owner(obj){}

public:

    enum SdcardState :  int {
        NO_SDCARD = 0,
        HAS_SDCARD_NORMAL = 1,
        HAS_SDCARD_ABNORMAL = 2,
        HAS_SDCARD_READONLY = 3,
        SDCARD_STATE_NUM = 4
    };

    /* sdcard */
    SdcardState get_sdcard_state() const { return  m_sdcard_state; };
    SdcardState set_sdcard_state(int state);

      static void ParseV1_0(const json &print_json, DevStorage *system);

private:
    MachineObject *m_owner;
    SdcardState    m_sdcard_state  { NO_SDCARD };
};

} // namespace Slic3r