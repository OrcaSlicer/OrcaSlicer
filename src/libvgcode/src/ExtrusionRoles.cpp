///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "ExtrusionRoles.hpp"

namespace libvgcode {

void ExtrusionRoles::add(EGCodeExtrusionRole role, const std::array<float, TIME_MODES_COUNT>& times)
{
    auto role_it = m_items.find(role);
    if (role_it == m_items.end())
        role_it = m_items.insert(std::make_pair(role, Item())).first;

    for (std::size_t i = 0; i < TIME_MODES_COUNT; ++i) {
        role_it->second.times[i] += times[i];
    }
}

std::vector<EGCodeExtrusionRole> ExtrusionRoles::get_roles() const
{
    std::vector<EGCodeExtrusionRole> ret;
    ret.reserve(m_items.size());
    for (const auto& [role, item] : m_items) {
        ret.emplace_back(role);
    }
    return ret;
}

float ExtrusionRoles::get_time(EGCodeExtrusionRole role, ETimeMode mode) const
{
    const auto role_it = m_items.find(role);
    if (role_it == m_items.end())
        return 0.0f;

    return (mode < ETimeMode::COUNT) ? role_it->second.times[static_cast<std::size_t>(mode)] : 0.0f;
}

} // namespace libvgcode
