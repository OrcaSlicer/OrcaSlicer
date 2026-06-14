#ifndef VORTEK_NOZZLE_STATE_HPP
#define VORTEK_NOZZLE_STATE_HPP

#include <unordered_map>
#include <vector>
#include <string>

namespace Slic3r {
    class Print;
}

namespace Vortek {

/**
 * @brief Bridge between physical device nozzle state and slicer backend.
 *
 * Resolves which preset filament_id is loaded in each physical nozzle
 * by matching device-reported colors to preset filament_colour entries.
 *
 * Lifecycle:
 *   GUI (Plater) calls match_nozzle_colors_to_filaments() → stores in Print member
 *   Backend (ToolOrdering) calls resolve_for_print() → reads from Print member
 */
class NozzleState {
public:
    /**
     * @brief Color-match device nozzle colors to preset filament slots.
     *
     * @param device_nozzle_colors Map of physical_nozzle_id → hex color string
     *        from DevNozzle::GetFilamentColor() (RRGGBBAA or RRGGBB)
     * @param preset_filament_colours Ordered preset colors from PrintConfig::filament_colour
     *        (format: "#RRGGBB")
     * @return Map of nozzle_id → filament_idx (0-based). Only matched entries included.
     */
    static std::unordered_map<int, int> match_nozzle_colors_to_filaments(
        const std::unordered_map<int, std::string>& device_nozzle_colors,
        const std::vector<std::string>& preset_filament_colours
    );

    /**
     * @brief Read device nozzle status from Print and return nozzle→filament map.
     * Returns empty map if offline or no status stored.
     */
    static std::unordered_map<int, int> resolve_for_print(const Slic3r::Print* print);

private:
    /**
     * @brief Normalize hex color: strip '#', strip alpha, lowercase.
     * "FF0000FF" → "ff0000", "#FF0000" → "ff0000"
     */
    static std::string normalize_color(const std::string& hex);
};

} // namespace Vortek

#endif // VORTEK_NOZZLE_STATE_HPP
