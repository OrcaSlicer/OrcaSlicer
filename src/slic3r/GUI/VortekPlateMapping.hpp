#ifndef VORTEK_PLATE_MAPPING_HPP
#define VORTEK_PLATE_MAPPING_HPP

#include <vector>
#include <string>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
    class Print;
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
};

} // namespace Vortek

#endif // VORTEK_PLATE_MAPPING_HPP
