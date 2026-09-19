#pragma once

#include <string>
#include <vector>

namespace Slic3r {

class MachineObject;

/**
 * AmsTrayData - Normalized filament tray information from any printer.
 *
 * This struct represents a single slot/tray in a multi-material system
 * (AMS, CANVAS, CFS, etc.) in a protocol-agnostic format. Agents populate
 * these from printer-specific APIs, then pass them to build_ams_payload()
 * to feed OrcaSlicer's filament UI.
 */
struct AmsTrayData {
    int         slot_index = 0;      ///< 0-based global slot index
    bool        has_filament = false; ///< Whether filament is loaded in this slot
    std::string tray_type;           ///< Material type (e.g., "PLA", "ASA", "PETG")
    std::string tray_color;          ///< Raw color (#RRGGBB, 0xRRGGBB, or RRGGBBAA)
    std::string tray_info_idx;       ///< OrcaSlicer filament preset/setting ID (optional)
    int         bed_temp = 0;        ///< Recommended bed temperature (optional)
    int         nozzle_temp = 0;     ///< Recommended/max nozzle temperature (optional)
};

/**
 * FilamentSyncUtils - Shared utilities for filament sync across printer agents.
 *
 * Provides the common logic to convert normalized tray data into the format
 * OrcaSlicer's DevFilaSystem expects, regardless of which printer protocol
 * was used to obtain it.
 */
class FilamentSyncUtils {
public:
    /**
     * Build the AMS JSON payload and populate the MachineObject's DevFilaSystem.
     *
     * Converts the agent-neutral AmsTrayData vector into the BBL-format JSON that
     * DevFilaSystemParser::ParseV1_0 expects, then calls the parser to populate
     * the machine's filament system state.
     *
     * @param dev_id       Device ID to look up the MachineObject
     * @param model_id     Printer model ID (set on MachineObject for sync matching)
     * @param ams_count    Number of AMS/CANVAS units
     * @param max_lane_index  Highest slot index in the tray vector
     * @param trays        Vector of tray data from the printer
     */
    static void build_ams_payload(const std::string& dev_id,
                                  const std::string& model_id,
                                  int ams_count,
                                  int max_lane_index,
                                  const std::vector<AmsTrayData>& trays);

    /**
     * Normalize a color string to RRGGBBAA uppercase hex format.
     * Handles #RRGGBB, 0xRRGGBB, RRGGBB, and RRGGBBAA inputs.
     * Returns "00000000" for invalid input.
     */
    static std::string normalize_color(const std::string& color);

    /**
     * Map a filament type string to the OrcaFilamentLibrary generic preset ID.
     * Input is case-insensitive and trimmed.
     */
    static std::string map_filament_type_to_generic_id(const std::string& filament_type);

    /**
     * Trim whitespace and convert to uppercase.
     */
    static std::string trim_and_upper(const std::string& input);
};

} // namespace Slic3r
