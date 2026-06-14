#ifndef VORTEK_PLATE_MAPPING_HPP
#define VORTEK_PLATE_MAPPING_HPP

#include <vector>
#include <string>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
    class Print;
    class DynamicPrintConfig;
    struct GCodeProcessorResult;
    class PresetBundle;
    class DynamicConfig;
    struct PlateData;
    namespace GUI {
        class PartPlate;
    }
}

namespace Vortek {

/**
 * @brief Vortek::PlateMapping handles mapping and synchronization between 
 * print configurations, loaded 3MF structures, and active plates.
 * 
 * Specifically, it handles custom layout configurations for H2C dual-nozzle
 * printers where filament-to-nozzle and filament-to-volume mappings
 * need to remain synchronized as filaments are added, deleted, or loaded.
 */
class PlateMapping {
public:
    /**
     * @brief Checks if the active print job is targeting a Bambu Lab H2C
     * printer configured with dual-nozzle system.
     */
    static bool is_h2c_multi_nozzle(const Slic3r::Print* print);

    /**
     * @brief Synchronizes filament maps and nozzle volume types on the active 
     * plate after background slicing completes.
     */
    static void sync_after_slicing(
        Slic3r::GUI::PartPlate* current_plate, 
        const Slic3r::Print* print, 
        Slic3r::PresetBundle& preset_bundle
    );

    /**
     * @brief Dynamic resizing of custom mapping vectors when filament counts change
     */
    static void handle_filament_count_changed(Slic3r::GUI::PartPlate* plate, int filament_count);
    static void handle_filament_added(Slic3r::GUI::PartPlate* plate);
    static void handle_filament_deleted(Slic3r::GUI::PartPlate* plate, int filament_id);
    static void clear_mappings(Slic3r::GUI::PartPlate* plate);

    /**
     * @brief Parses custom nozzle/volume mapping attributes from 3MF metadata
     * on project load, initializing LayeredNozzleGroupResult.
     */
    static void load_from_3mf_structure(
        Slic3r::GUI::PartPlate* plate,
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
     * For H2C dual-nozzle printers, fills plate_data->nozzles_info and
     * patches FilamentInfo::group_id in slice_filaments_info using the plate's
     * filament_nozzle_map/filament_volume_map/filament_maps.
     * This is the canonical source of nozzle metadata for slice_info.config,
     * bypassing the fragile nozzle_group_result pipeline from the slicing backend.
     *
     * For non-H2C printers this is a no-op.
     */
    static void patch_plate_data_for_export(
        Slic3r::PlateData* plate_data,
        const Slic3r::GUI::PartPlate* plate,
        const Slic3r::DynamicPrintConfig& config
    );
};

} // namespace Vortek

#endif // VORTEK_PLATE_MAPPING_HPP
