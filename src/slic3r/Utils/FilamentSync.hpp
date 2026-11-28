#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class PresetCollection;
}

namespace Slic3r::FilamentSync {

// "#RRGGBB" in either case becomes "RRGGBBFF" in upper case; anything else is nullopt.
std::optional<std::string> parse_hex_rgb(const std::string& color);

struct AmsSlot
{
    bool present = false;
    std::string type;
    std::string color_rgba;  // RRGGBBAA, or empty for an unknown colour.
    std::string filament_id; // Empty when no filament preset matches.
    int nozzle_temp_max = 0;
};

// At most 7 units: DevFilaSystemParser::ParseV1_0 parses the masks with std::stol and shifts them as int.
constexpr std::size_t max_ams_units = 7;

// A single unit uses unit_name as its display_name. Multiple units append a space and their one-based number.
// An empty unit_name omits display_name so the parser uses its default. Throws std::invalid_argument past max_ams_units.
nlohmann::json build_ams_print_json(const std::vector<std::array<AmsSlot, 4>>& units, const std::string& unit_name = {});

// What a printer reports about a loaded spool.
struct SpoolIdentity
{
    std::string vendor;
    std::string product;
    std::string type;
};

// The filament_id of the visible, compatible preset that best matches the spool, or empty when none does.
std::string match_filament_preset(const PresetCollection& filaments, const SpoolIdentity& spool);

} // namespace Slic3r::FilamentSync
