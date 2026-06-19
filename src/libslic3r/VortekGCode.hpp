// ============================================================================
// VortekGCode.hpp
//
// Implements the Vortek::GCode class to handle parser integration
// and filament start G-code blocks for Bambu Lab H2C multi-nozzle integrations.
// ============================================================================

#ifndef VORTEK_GCODE_HPP
#define VORTEK_GCODE_HPP

#include "GCode.hpp"

namespace Slic3r {
    class Print;
}

namespace Vortek {

/**
 * @class GCode
 * @brief Manages G-code parser context initialization and custom filament startup G-code.
 * 
 * Registers H2C-specific placeholder variables (e.g. `initial_nozzle_id`, `nozzle_diameter_at_nozzle_id`, 
 * `first_non_support_hotend`) inside the slicer's PlaceholderParser. Also outputs filament start G-code
 * macros containing specific tool/nozzle indexes to instruct the printer's firmware correctly.
 */
class GCode {
public:
    /**
     * @brief Registers H2C printer variables and tool selections in the G-code placeholder parser.
     * @param gcode Host GCode generator object.
     * @param initial_extruder_id The ID of the starting extruder.
     * @param initial_non_support_extruder_id The ID of the starting extruder used for model parts (non-support).
     * @param first_filaments List of first filaments to print per extruder.
     * @param first_non_support_filaments List of first non-support filaments to print per extruder.
     */
    static void init(
        Slic3r::GCode& gcode,
        int initial_extruder_id,
        int initial_non_support_extruder_id,
        std::vector<int>& first_filaments,
        std::vector<int>& first_non_support_filaments
    );

    /**
     * @brief Writes the parsed startup script for the initial filaments to the G-code file.
     * @param gcode Host GCode generator object.
     * @param initial_extruder_id Active startup extruder ID.
     * @param initial_non_support_extruder_id Active model extruder ID.
     * @param file Target output file stream.
     * @param print Host print job configuration.
     */
    static void write_filament_start(
        Slic3r::GCode& gcode,
        int initial_extruder_id,
        int initial_non_support_extruder_id,
        Slic3r::GCode::GCodeOutputStream& file,
        Slic3r::Print& print
    );

    static void update_placeholder_parser_with_variant_params(Slic3r::GCode& gcode);
};

} // namespace Vortek

#endif // VORTEK_GCODE_HPP
