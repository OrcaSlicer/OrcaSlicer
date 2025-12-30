#include <nlohmann/json.hpp>
#include "DevExtruderSystem.h"

#include "slic3r/GUI/DeviceManager.hpp"


using namespace nlohmann;

namespace Slic3r
{
    int DevExtderSystem::CtrlRetrySwitching()
    {
        if (m_owner) { return m_owner->command_ams_control("resume"); }
        return -1;
    }

    int DevExtderSystem::CtrlQuitSwitching()
    {
        if (m_owner) { return m_owner->command_ams_control("abort"); }
        return -1;
    }
}