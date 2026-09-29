#pragma once
#include <nlohmann/json.hpp>
#include "slic3r/Utils/json_diff.hpp"

namespace Slic3r {

class MachineObject;

class DevAxis
{
public:
    static std::shared_ptr<DevAxis> Create(MachineObject *obj) { return std::shared_ptr<DevAxis>( new DevAxis(obj)); }
    virtual ~DevAxis() = default;

public:
    bool IsAxisAtHomeX() const { return m_home_flag == 0 ? true : (m_home_flag & 1) == 1; }
    bool IsAxisAtHomeY() const { return m_home_flag == 0 ? true : ((m_home_flag >> 1) & 1) == 1; }
    bool IsAxisAtHomeZ() const { return m_home_flag == 0 ? true : ((m_home_flag >> 2) & 1) == 1; }

    bool IsArchCoreXY() const;

public:
    void ParseAxis(const json &print_json);

    int Ctrl_GoHome();
    int Ctrl_Axis(std::string axis, double unit = 1.0f, double input_val = 1.0f, int speed = 3000); // xyz e

protected:
    DevAxis(MachineObject *obj) noexcept : m_owner(obj) {}

private:
    MachineObject *m_owner = nullptr;

    int  m_home_flag               = 0; // bit0:X, bit1:Y, bit2:Z
    bool m_is_support_mqtt_axis_ctrl = false;
    bool m_is_support_mqtt_homing    = false;
};

} // namespace Slic3r