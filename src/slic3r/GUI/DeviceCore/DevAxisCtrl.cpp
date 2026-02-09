#include "DevAxis.h"

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

namespace Slic3r
{

int DevAxis::Ctrl_GoHome()
{
    if (m_is_support_mqtt_homing) {
        json j;
        j["print"]["command"] = "back_to_center";
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        return m_owner->publish_json(j);
    }

    // gcode command
    return m_owner->is_in_printing() ? m_owner->publish_gcode("G28 X\n") : m_owner->publish_gcode("G28 \n");
}

int DevAxis::Ctrl_Axis(std::string axis, double unit, double input_val, int speed)
{
    if (m_is_support_mqtt_axis_ctrl) {
        json j;
        j["print"]["command"] = "xyz_ctrl";
        j["print"]["axis"] = axis;
        j["print"]["dir"] = input_val > 0 ? 1 : -1;
        j["print"]["mode"] = (std::abs(input_val) >= 10) ? 1 : 0;
        j["print"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        return m_owner->publish_json(j);
    }

    double value = input_val;
    if (!IsArchCoreXY()) {
        if (axis.compare("Y") == 0 || axis.compare("Z") == 0) {
            value = -1.0 * input_val;
        }
    }

    char cmd[256];
    if (axis.compare("X") == 0 || axis.compare("Y") == 0 || axis.compare("Z") == 0) {
        sprintf(cmd, "M211 S \nM211 X1 Y1 Z1\nM1002 push_ref_mode\nG91 \nG1 %s%0.1f F%d\nM1002 pop_ref_mode\nM211 R\n", axis.c_str(), value * unit, speed);
    } else if (axis.compare("E") == 0) {
        sprintf(cmd, "M83 \nG0 %s%0.1f F%d\n", axis.c_str(), value * unit, speed);
    } else {
        return -1;
    }

    try {
        if (m_owner->get_agent()) {
            json j;
            j["axis_control"] = axis;
            m_owner->get_agent()->track_event("printer_control", j.dump());
        }
    } catch (...) {
    }

    return m_owner->publish_gcode(cmd);
}

} // namespace Slic3r