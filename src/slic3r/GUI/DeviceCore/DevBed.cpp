#include "DevBed.h"
#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r {

void DevBed::ParseV1_0(const json &print_json, DevBed *system)
{
    if (print_json.contains("bed_temper")) {
        if (print_json["bed_temper"].is_number()) { system->bed_temp = print_json["bed_temper"].get<float>(); }
    }
    if (print_json.contains("bed_target_temper")) {
        if (print_json["bed_target_temper"].is_number()) { system->bed_temp_target = print_json["bed_target_temper"].get<float>(); }
    }
}

void DevBed::ParseV2_0(const json &print_json, DevBed *system)
{
    if (print_json.contains("bed_temp") && print_json["bed_temp"].is_number()) {
        int bed_temp_bits = print_json["bed_temp"].get<int>();
        system->bed_temp        = system->m_owner->get_flag_bits(bed_temp_bits, 0, 16);
        system->bed_temp_target = system->m_owner->get_flag_bits(bed_temp_bits, 16, 16);
    }
}

} // namespace Slic3r