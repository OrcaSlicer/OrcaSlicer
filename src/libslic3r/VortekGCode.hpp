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

// Vortek::GCode - H2C-specific gcode helpers: registers placeholder variables and writes filament start gcode.
class GCode {
public:
    // Registers H2C printer variables and tool selections in the placeholder parser.
    static void init(
        Slic3r::GCode& gcode,
        int initial_extruder_id,
        int initial_non_support_extruder_id,
        std::vector<int>& first_filaments,
        std::vector<int>& first_non_support_filaments
    );

    // Writes the parsed startup script for the initial filaments to the G-code file.
    static void write_filament_start(
        Slic3r::GCode& gcode,
        int initial_extruder_id,
        int initial_non_support_extruder_id,
        Slic3r::GCode::GCodeOutputStream& file,
        Slic3r::Print& print
    );

    static void update_placeholder_parser_with_variant_params(Slic3r::GCode& gcode);

    // Returns the logical filament slot count (reads filament_self_index from ori_full_print_config;
    // BBL compresses filament arrays to physical extruder count, so this recovers the original slot count).
    static size_t get_logical_filament_count(const Slic3r::GCode& gcode);

    // Returns filament_retract_length_nc for the given logical slot via ori_full_print_config
    // (bypasses VariantAwareConfig overlay where nullable options are not propagated).
    static float get_filament_retract_length_nc(const Slic3r::GCode& gcode, int filament_id);

    // Populates flush_volumetric_speeds, flush_temperatures, and filament_cooling_before_tower
    // with per-logical-slot arrays. skip_cooling zeros filament_cooling_before_tower on contact layers.
    static void apply_tcr_flush_config(
        const Slic3r::GCode& gcode,
        bool skip_cooling,
        Slic3r::DynamicConfig& config
    );

    // Builds a per-logical-slot vector of doubles for a filament config key
    // using variant-aware index resolution from ori_full_print_config.
    static std::vector<double> remap_floats_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments);

    // Same as remap_floats_by_filament_vortek but for int options; nil-sentinels (INT_MAX) become 0.
    static std::vector<int> remap_ints_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments);

    // Builds a per-logical-slot vector of ints for a nozzle config key using physical extruder mapping.
    static std::vector<int> remap_nozzle_ints_by_filament_vortek(const Slic3r::GCode& gcode, const std::string& key, size_t num_filaments);

    // Corrects scalar filament params in dyn_config (old/new filament temp, feedrate, retract) for H2C
    // multi-slot projects where m_config is compressed to 2 extruders and get_at() clamps incorrectly.
    static void patch_toolchange_dyn_config(
        Slic3r::DynamicConfig&    dyn_config,
        Slic3r::GCode&            gcode,
        int                        old_filament_id,
        int                        new_filament_id,
        float                      filament_area
    );

    // Wrapper for patch_toolchange_dyn_config that computes filament_area internally.
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
