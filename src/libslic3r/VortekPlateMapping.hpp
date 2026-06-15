#ifndef VORTEK_PLATE_MAPPING_HPP
#define VORTEK_PLATE_MAPPING_HPP

#include <vector>
#include <string>
#include <unordered_set>
#include "PrintConfig.hpp"

namespace Slic3r {
    class Print;
    class DynamicPrintConfig;
    class PrintConfig;
    struct GCodeProcessorResult;
    class PresetBundle;
    class DynamicConfig;
    struct PlateData;
}

namespace Vortek {

/**
 * @brief Result of loading nozzle/volume maps from 3MF structure.
 * Returned by load_from_3mf_structure(); caller applies onto PartPlate.
 */
struct LoadMappingResult {
    std::vector<int> filament_nozzle_map;
    std::vector<int> filament_volume_map;
};

/**
 * @brief Vortek::PlateMapping handles mapping and synchronization between
 * print configurations, loaded 3MF structures, and active plates.
 *
 * Handles custom layout configurations for H2C dual-nozzle printers where
 * filament-to-nozzle and filament-to-volume mappings need to remain
 * synchronized as filaments are added, deleted, or loaded.
 *
 * NOTE: This class operates on DynamicPrintConfig and primitive vectors,
 * not on GUI types. The GUI layer (PartPlate) passes its config/maps here.
 */
class PlateMapping {
public:
    /**
     * @brief Checks if the active print job is targeting a Bambu Lab H2C
     * printer configured with dual-nozzle system.
     */
    static bool is_h2c_multi_nozzle(const Slic3r::Print* print);

    /**
     * @brief Synchronizes filament maps and nozzle volume types after
     * background slicing completes.
     *
     * @param plate_config       Mutable plate config (caller passes plate->config())
     * @param filament_map_mode  Current real filament map mode of the plate
     * @param print              Completed Print with slicing results
     * @param preset_bundle      Mutable preset bundle for project config sync
     */
    static void sync_after_slicing(
        Slic3r::DynamicPrintConfig& plate_config,
        Slic3r::FilamentMapMode filament_map_mode,
        const Slic3r::Print* print,
        Slic3r::PresetBundle& preset_bundle
    );

    /**
     * @brief Dynamic resizing of custom mapping vectors when filament counts change.
     * All take DynamicPrintConfig* (the plate's m_config).
     */
    static void handle_filament_count_changed(Slic3r::DynamicPrintConfig* config, int filament_count);
    static void handle_filament_added(Slic3r::DynamicPrintConfig* config);
    static void handle_filament_deleted(Slic3r::DynamicPrintConfig* config, int filament_id);
    static void clear_mappings(Slic3r::DynamicPrintConfig* config);

    /**
     * @brief Parses custom nozzle/volume mapping attributes from 3MF metadata
     * on project load, initializing LayeredNozzleGroupResult.
     *
     * Returns parsed maps in LoadMappingResult. Caller applies them
     * to PartPlate via set_filament_nozzle_maps() / set_filament_volume_maps().
     */
    static LoadMappingResult load_from_3mf_structure(
        const Slic3r::PlateData* plate_data,
        int filament_count,
        Slic3r::GCodeProcessorResult* gcode_result
    );

    /**
     * @brief Ensures project configuration maps have matching size for the
     * active filament count when starting file loading.
     */
    static void sync_project_config_on_load(Slic3r::DynamicConfig& proj_cfg, int filament_count);

    /**
     * @brief Patches the full DynamicPrintConfig before 3MF export.
     * For H2C printers: ensures has_filament_switcher = true (FTS flag).
     */
    static void patch_export_config(Slic3r::DynamicPrintConfig& cfg);

    /**
     * @brief Patches PlateData for 3MF export with correct H2C nozzle group info.
     *
     * @param plate_data            Mutable plate export data
     * @param filament_nozzle_map   Per-filament nozzle assignments (from plate)
     * @param filament_volume_map   Per-filament volume type (from plate)
     * @param filament_maps         Per-filament extruder assignment (from plate)
     * @param config                Full print config
     * @param print                 Print object (optional, for LNGR access)
     */
    static void patch_plate_data_for_export(
        Slic3r::PlateData* plate_data,
        const std::vector<int>& filament_nozzle_map,
        const std::vector<int>& filament_volume_map,
        const std::vector<int>& filament_maps,
        const Slic3r::DynamicPrintConfig& config,
        const Slic3r::Print* print = nullptr
    );

    /**
     * @brief H2C Vortek: Handles H2C filament map preservation, validation,
     * and automatic recalculation/invalidation when presets/properties change.
     */
    static void handle_h2c_mapping_apply(
        Slic3r::Print* print,
        Slic3r::DynamicPrintConfig& new_full_config,
        const Slic3r::DynamicPrintConfig& old_full_config
    );

    /**
     * @brief H2C Vortek: Handles H2C print diff suppression. Removes mapped parameters
     * from print_diff_set to prevent unnecessary invalidation while syncing values.
     */
    static void handle_h2c_print_diff(
        Slic3r::Print* print,
        Slic3r::PrintConfig& config,
        Slic3r::DynamicPrintConfig& full_print_config,
        const Slic3r::DynamicPrintConfig& new_full_config,
        std::unordered_set<std::string>& print_diff_set
    );
};

} // namespace Vortek

#endif // VORTEK_PLATE_MAPPING_HPP
