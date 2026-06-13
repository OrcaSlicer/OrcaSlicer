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

class PlateMapping {
public:
    static bool is_h2c_multi_nozzle(const Slic3r::Print* print);

    static void sync_after_slicing(
        Slic3r::GUI::PartPlate* current_plate, 
        const Slic3r::Print* print, 
        Slic3r::PresetBundle& preset_bundle
    );

    static void handle_filament_count_changed(Slic3r::GUI::PartPlate* plate, int filament_count);
    static void handle_filament_added(Slic3r::GUI::PartPlate* plate);
    static void handle_filament_deleted(Slic3r::GUI::PartPlate* plate, int filament_id);
    static void clear_mappings(Slic3r::GUI::PartPlate* plate);

    static void load_from_3mf_structure(
        Slic3r::GUI::PartPlate* plate,
        const Slic3r::PlateData* plate_data,
        int filament_count,
        Slic3r::GCodeProcessorResult* gcode_result
    );

    static void sync_project_config_on_load(Slic3r::DynamicConfig& proj_cfg, int filament_count);
};

} // namespace Vortek

#endif // VORTEK_PLATE_MAPPING_HPP
