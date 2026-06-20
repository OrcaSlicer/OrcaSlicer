#include "VortekNozzleState.hpp"
#include "Print.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

#include <boost/log/trivial.hpp>

namespace Vortek {

std::string NozzleState::normalize_color(const std::string& hex)
{
    std::string s = hex;

    // Strip leading '#'
    if (!s.empty() && s[0] == '#')
        s = s.substr(1);

    // Strip alpha channel: if 8 hex chars → take first 6 (RRGGBB from RRGGBBAA)
    if (s.size() == 8)
        s = s.substr(0, 6);

    // Lowercase
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    return s;
}

std::unordered_map<int, int> NozzleState::match_nozzle_colors_to_filaments(
    const std::unordered_map<int, std::string>& device_nozzle_colors,
    const std::vector<std::string>& preset_filament_colours)
{
    std::unordered_map<int, int> result;

    if (device_nozzle_colors.empty() || preset_filament_colours.empty())
        return result;

    // Pre-normalize preset colors
    std::vector<std::string> preset_normalized;
    preset_normalized.reserve(preset_filament_colours.size());
    for (const auto& c : preset_filament_colours)
        preset_normalized.push_back(normalize_color(c));

    for (const auto& [nozzle_id, device_color] : device_nozzle_colors) {
        if (device_color.empty())
            continue;

        std::string normalized_device = normalize_color(device_color);
        if (normalized_device.size() != 6)
            continue; // invalid color format

        // Exact match: find first preset with same normalized color
        for (size_t idx = 0; idx < preset_normalized.size(); ++idx) {
            if (preset_normalized[idx] == normalized_device) {
                result[nozzle_id] = static_cast<int>(idx);
                BOOST_LOG_TRIVIAL(info)
                    << "[H2C-NozzleState] nozzle " << nozzle_id
                    << " color " << device_color << " → filament " << idx
                    << " (preset: " << preset_filament_colours[idx] << ")";
                break;
            }
        }

        if (result.find(nozzle_id) == result.end()) {
            BOOST_LOG_TRIVIAL(info)
                << "[H2C-NozzleState] nozzle " << nozzle_id
                << " color " << device_color << " (" << normalized_device
                << ") — no preset match found, skipping";
        }
    }

    BOOST_LOG_TRIVIAL(info)
        << "[H2C-NozzleState] matched " << result.size()
        << " of " << device_nozzle_colors.size() << " nozzles to preset filaments";

    return result;
}

    return print->get_device_nozzle_status();
}

} // namespace Vortek
