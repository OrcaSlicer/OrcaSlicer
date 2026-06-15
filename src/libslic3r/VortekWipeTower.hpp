// ============================================================================
// VortekWipeTower.hpp
//
// Implements the Vortek::WipeTower class to delegate WipeTower setups
// and nozzle change generation for Bambu Lab H2C multi-nozzle integrations.
// ============================================================================

#ifndef VORTEK_WIPE_TOWER_HPP
#define VORTEK_WIPE_TOWER_HPP

#include "GCode/WipeTower.hpp"
#include <string>
#include <vector>
#include <functional>

namespace Slic3r {
    class PrintConfig;
    class Print;
    class DynamicConfig;
}

namespace Vortek {

struct FilamentChangeTimeResult {
    bool  performed = false;
    float extra_time = 0.f;
    bool  extruder_unloaded = false;
    bool  flush_filament_changed = false;
};

/**
 * @class WipeTower
 * @brief Extends WipeTower generation for multi-nozzle configurations (Bambu Lab H2C).
 * 
 * Handles settings initialization, extruder-nozzle-change perimeter widths, 
 * G-code generation for tool changes, and checks if ramming is needed based on layer
 * status and multi-nozzle groups.
 */
class WipeTower {
public:
    /**
     * @brief Checks if the active printer configuration corresponds to the H2C multi-nozzle printer.
     */
    static bool is_h2c_printer(const Slic3r::Print* print);
    static bool is_h2c_printer(const Slic3r::PrintConfig& config);
    static bool is_h2c_printer(const Slic3r::DynamicConfig& config);
    static bool is_h2c_printer(const std::string& printer_model);

    /**
     * @brief Estimates time cost of H2C specific filament change.
     */
    static FilamentChangeTimeResult calculate_filament_change_time(
        const std::string& printer_model,
        int new_extruder_id,
        int next_filament_id,
        int old_filament_in_extruder,
        int old_filament_in_nozzle,
        bool filament_in_nozzle_change,
        bool nozzle_in_extruder_change,
        const std::vector<unsigned char>& m_filament_id,
        const std::function<float(size_t)>& get_filament_unload_time,
        const std::function<float(size_t)>& get_filament_load_time
    );

    /**
     * @brief Initializes the WipeTower parameters from the PrintConfig.
     * @param tower Host WipeTower instance to configure.
     * @param config The print configuration package.
     */
    static void init_ctor(Slic3r::WipeTower& tower, const Slic3r::PrintConfig& config);
    
    /**
     * @brief Computes perimeter widths specifically for nozzle change lines based on the active nozzle diameter.
     * @param tower Host WipeTower instance.
     * @param nozzle_diameter The diameter (e.g. 0.4, 0.6) of the active nozzle.
     */
    static void init_set_extruder(Slic3r::WipeTower& tower, float nozzle_diameter);
    
    /**
     * @brief Generates G-code instructions and paths for a nozzle/filament change sequence inside the wipe tower.
     * @param tower Host WipeTower instance.
     * @param old_filament_id ID of the filament being replaced.
     * @param new_filament_id ID of the filament being loaded.
     * @return NozzleChangeResult containing G-code block, travel paths, and coordinates.
     */
    static Slic3r::WipeTower::NozzleChangeResult nozzle_change(
        Slic3r::WipeTower& tower,
        int old_filament_id,
        int new_filament_id
    );
    
    /**
     * @brief Determines whether filament ramming is required before toolchange.
     * @param tower Host WipeTower instance.
     * @param filament_id_1 Active filament.
     * @param filament_id_2 Next filament to switch to.
     * @param layer_id The current layer index.
     * @return True if ramming is required.
     */
    static bool is_need_ramming(const Slic3r::WipeTower& tower, int filament_id_1, int filament_id_2, int layer_id);
    
    /**
     * @brief Checks if two filaments map to the same physical extruder unit.
     */
    static bool is_same_extruder(const Slic3r::WipeTower& tower, int filament_id_1, int filament_id_2, int layer_id);
    
    /**
     * @brief Checks if two filaments map to the same physical nozzle.
     */
    static bool is_same_nozzle(const Slic3r::WipeTower& tower, int filament_id_1, int filament_id_2, int layer_id);

    /**
     * @brief Initializes NozzleStatusRecorder with the starting filament for each nozzle.
     */
    static void initialize_nozzle_status(
        Slic3r::MultiNozzleUtils::NozzleStatusRecorder& recorder,
        const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult& group_result,
        const Slic3r::Print* print = nullptr
    );

    /**
     * @brief Overrides prime volumes to 0 if the target nozzle already contains the requested filament.
     */
    static void adjust_prime_volumes(
        int prev_nozzle_filament,
        int new_filament_id,
        float& wipe_volume_ec,
        float& wipe_volume_nc
    );
};

} // namespace Vortek

#endif // VORTEK_WIPE_TOWER_HPP
