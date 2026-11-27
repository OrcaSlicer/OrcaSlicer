#include <nlohmann/json.hpp>
#include <string>
#include "DevLamp.h"

// TODO: remove this include
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/OpcpCommands.hpp"

using namespace nlohmann;

namespace Slic3r
{


static std::string _light_effect_str(DevLamp::LIGHT_EFFECT effect)
{
    switch (effect)
    {
    case Slic3r::DevLamp::LIGHT_EFFECT_ON:
        return "on";
    case Slic3r::DevLamp::LIGHT_EFFECT_OFF:
        return "off";
    case Slic3r::DevLamp::LIGHT_EFFECT_FLASHING:
        return "flashing";
    default:
        return "unknown";
    }
    return "unknown";
}

void DevLamp::CtrlSetChamberLight(LIGHT_EFFECT effect)
{
    // copied from others, TODO CHECK
    command_set_chamber_light(effect);
    command_set_chamber_light2(effect);
}

int DevLamp::command_set_chamber_light(LIGHT_EFFECT effect, int on_time, int off_time, int loops, int interval)
{
    json j = opcp::system::ledctrl(MachineObject::m_sequence_id++, "chamber_light", _light_effect_str(effect), on_time, off_time, loops, interval);
    return m_owner->publish_json(j);
}

int DevLamp::command_set_chamber_light2(LIGHT_EFFECT effect, int on_time, int off_time, int loops, int interval)
{
    json j = opcp::system::ledctrl(MachineObject::m_sequence_id++, "chamber_light2", _light_effect_str(effect), on_time, off_time, loops, interval);
    return m_owner->publish_json(j);
}

}