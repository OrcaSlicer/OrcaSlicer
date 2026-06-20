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

    /**
     * @brief Returns the true logical filament slot count for this print.
     *
     * BBL/OrcaSlicer compresses filament arrays to the number of physical extruders
     * (e.g. 2 for H2C).  G-code templates, however, index arrays by logical slot
     * (e.g. 0-6 for a 7-slot project).  This function reads the maximum value of
     * `filament_self_index` from ori_full_print_config — which records the logical
     * slot count before any per-extruder compression — and falls back to the
     * compressed count only if the key is absent.
     */
    static size_t get_logical_filament_count(const Slic3r::GCode& gcode);

    /**
     * @brief Returns filament_retract_length_nc for the given logical filament slot.
     *
     * Reads via ori_full_print_config to bypass the VariantAwareConfig overlay
     * limitation: nullable options are not propagated to base FullPrintConfig fields,
     * so m_config.filament_retract_length_nc.get_at() returns the default (10 mm).
     */
    static float get_filament_retract_length_nc(const Slic3r::GCode& gcode, int filament_id);

    /**
     * @brief Populates flush_volumetric_speeds, flush_temperatures, and
     *        filament_cooling_before_tower in `config` with per-logical-slot arrays.
     *
     * Replaces the BBL-default approach of building 2-element arrays (one per
     * physical extruder) which causes PlaceholderParser to return 0 for template
     * accesses like {flush_volumetric_speeds[5]} in a 7-slot project.
     *
     * @param skip_cooling  Pass (tcr.is_contact || layer_index == 0) to zero
     *                      filament_cooling_before_tower for the contact layer.
     */
    static void apply_tcr_flush_config(
        const Slic3r::GCode& gcode,
        bool skip_cooling,
        Slic3r::DynamicConfig& config
    );

    /**
     * @brief Builds a per-logical-slot vector of doubles for a filament config key.
     *
     * Reads from ori_full_print_config using variant-aware index resolution so the
     * returned vector is always num_filaments long and correctly indexed by logical
     * slot, independent of the VariantAwareConfig overlay/compression state.
     */
    static std::vector<double> remap_floats_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments);

    /**
     * @brief Builds a per-logical-slot vector of ints for a filament config key.
     *
     * Same as remap_floats_by_filament_vortek but for integer options.
     * ConfigOptionIntsNullable nil-sentinels (INT_MAX) are treated as 0.
     */
    static std::vector<int> remap_ints_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments);

    /**
     * @brief Builds a per-logical-slot vector of ints for a nozzle config key.
     *
     * Maps printer/nozzle config options to logical filament slots using
     * physical extruder mapping.
     */
    static std::vector<int> remap_nozzle_ints_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments);

    /**
     * @brief Corrects scalar filament parameters in dyn_config for H2C multi-slot projects.
     *
     * GCode::set_extruder() computes old_filament_temp, old_filament_e_feedrate,
     * new_filament_e_feedrate, and filament_retract_length_nc from m_config which
     * is compressed to the physical extruder count (2 for H2C). For logical filament
     * slots >1, get_at() clamps to the last physical extruder entry, producing wrong
     * temperatures and speeds. This method re-derives those scalars from
     * ori_full_print_config via the variant-aware remap and overwrites the bad
     * entries in dyn_config before placeholder_parser_process() runs.
     *
     * @param dyn_config    The DynamicConfig to patch (already populated by set_extruder).
     * @param gcode         Host GCode generator (provides m_print, m_config, etc).
     * @param old_filament_id  Logical slot index of the outgoing filament (-1 if none).
     * @param new_filament_id  Logical slot index of the incoming filament.
     * @param filament_area    Cross-section area of the filament wire (mm²).
     */
    static void patch_toolchange_dyn_config(
        Slic3r::DynamicConfig&    dyn_config,
        Slic3r::GCode&            gcode,
        int                        old_filament_id,
        int                        new_filament_id,
        float                      filament_area
    );

    /**
     * @brief Helper wrapper for WipeTowerIntegration::append_tcr.
     * Calculates filament_area internally and invokes patch_toolchange_dyn_config.
     */
    static void patch_toolchange_dyn_config_wt(
        Slic3r::DynamicConfig&    dyn_config,
        Slic3r::GCode&            gcode,
        int                        old_filament_id,
        int                        new_filament_id
    );

private:
    static int get_original_filament_index(const Slic3r::GCode& gcode, int filament_id);
};

} // namespace Vortek

#endif // VORTEK_GCODE_HPP
