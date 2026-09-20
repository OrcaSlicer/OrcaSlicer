#include "DevAxis.h"
#include "DevUtil.h"

#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r
{

void DevAxis::ParseAxis(const json& print_json)
{
    DevJsonValParser::ParseVal(print_json, "home_flag", m_home_flag);

    if (print_json.contains("fun")) {
        const std::string& fun = print_json["fun"].get<std::string>();
        m_is_support_mqtt_homing = DevUtil::get_flag_bits(fun, 32);
        m_is_support_mqtt_axis_ctrl = DevUtil::get_flag_bits(fun, 38);
    }
}

bool DevAxis::IsArchCoreXY() const
{
    return m_owner->get_printer_arch() == PrinterArch::ARCH_CORE_XY;
}

} // namespace Slic3r