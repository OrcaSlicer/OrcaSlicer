// ============================================================================
// VortekPreCooling.hpp
//
// Implements the Vortek::PreCooling class to handle temperature scheduling,
// configuration loading, pre-scanning, and command injection for Bambu Lab H2C
// multi-nozzle systems.
// ============================================================================

#ifndef VORTEK_PRE_COOLING_HPP
#define VORTEK_PRE_COOLING_HPP

#include <vector>
#include <string>
#include <map>
#include <set>
#include <functional>
#include <memory>

#include "GCode/GCodeProcessor.hpp"

namespace Slic3r {
    class PrintConfig;
}

namespace Vortek {

/**
 * @class PreCooling
 * @brief Coordinates nozzle pre-heating and pre-cooling for the H2C multi-nozzle system.
 * 
 * In multi-nozzle and dual-extruder systems, keeping nozzles at printing temperature
 * when they are inactive causes filament oozing, degradation, and print defects. Conversely,
 * heating up a cold nozzle takes time and can slow down the print.
 * 
 * The PreCooling class scans the generated G-code before writing it, identifies long
 * periods of inactivity for each nozzle (free blocks), and schedules pre-cooling and
 * pre-heating commands (M104/M109/M632) at the optimal timestamps based on physical heating/cooling rates.
 */
class PreCooling {
public:
    /**
     * @struct ExtruderFreeBlock
     * @brief Represents a continuous interval in the G-code where an extruder/nozzle is not printing.
     */
    struct ExtruderFreeBlock {
        unsigned int free_lower_gcode_id;    ///< The starting G-code line ID where the extruder becomes free.
        unsigned int free_upper_gcode_id;    ///< The ending G-code line ID where the extruder must be ready to print again.
        unsigned int partial_free_lower_id;  ///< The start of the post-extrusion period (partial free).
        unsigned int partial_free_upper_id;  ///< The end of the post-extrusion period (partial free).
        int last_filament_id;                ///< ID of the filament loaded before this free block.
        int next_filament_id;                ///< ID of the filament to be loaded after this free block.
        int last_nozzle_id;                  ///< ID of the nozzle used before this free block.
        int next_nozzle_id;                  ///< ID of the nozzle to be used after this free block.
        int extruder_id;                     ///< The physical extruder ID associated with this block.
        bool ignore_cooling_before_tower = false; ///< If true, bypass cooling operations before entering the wipe tower.
    };

    /**
     * @brief Constructs a PreCooling manager with all physical and logical constraints of the printer.
     */
    PreCooling(
        const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>& moves_,
        const std::vector<std::string>& filament_types_,
        const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result_,
        const std::vector<int>& filament_nozzle_temps_,
        const std::vector<int>& filament_nozzle_temps_initial_layer_,
        const std::vector<int>& physical_extruder_map_,
        int valid_machine_id_,
        float inject_time_threshold_,
        bool handle_hotend_as_extruder_,
        bool has_filament_switcher_,
        const std::vector<int>& pre_cooling_temp_,
        const std::vector<double>& cooling_rate_,
        const std::vector<double>& heating_rate_,
        const std::vector<std::pair<unsigned int, unsigned int>>& skippable_blocks_,
        const std::vector<int>& extruder_max_nozzle_count_,
        const std::vector<double>& filament_preheat_temperature_delta_,
        const std::vector<double>& filament_max_temperature_drop_when_ec_,
        unsigned int machine_start_gcode_end_id_,
        unsigned int machine_end_gcode_start_id_,
        const std::vector<Slic3r::ExtruderType>& extruder_types_,
        const std::vector<double>& nozzle_diameter_
    );

    /**
     * @brief Scans all registered free blocks and inserts pre-cooling and pre-heating operations.
     * @param inserted_operation_lines Map where the generated G-code command sequences are inserted at specific line IDs.
     */
    void process_pre_cooling_and_heating(Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap& inserted_operation_lines);
    
    /**
     * @brief Decides whether to build free blocks based on filament usage blocks or extruder usage blocks.
     */
    void build_extruder_free_blocks(
        const std::vector<Slic3r::ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks,
        const std::vector<Slic3r::ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks
    );

    /**
     * @brief Helper to load pre-heating parameters and configurations from PrintConfig into the processor.
     * @param config The global print configuration bundle.
     * @param filament_count Number of active filaments.
     * @param processor Target GCodeProcessor instance.
     */
    static void apply_config(const Slic3r::PrintConfig& config, size_t filament_count, Slic3r::GCodeProcessor& processor);
    
    /**
     * @brief Performs a pre-scan over the generated G-code file to identify filament/nozzle changes and calculate timing blocks.
     * @param processor The active GCodeProcessor instance.
     * @param f File descriptor of the temporary G-code file.
     * @return Map of line numbers to lists of G-code strings to inject.
     */
    static Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap run_pre_scan(Slic3r::GCodeProcessor& processor, FILE* f);
    
    /**
     * @brief Iterates over the pre-scan map and writes the generated M104/M109/M632 lines into the output stream.
     * @param precooling_iter Iterator pointing to the current position in the pre-scan map.
     * @param precooling_inserted_lines The pre-scan map containing all lines to inject.
     * @param enable_pre_heating Flag indicating if preheating/precooling is enabled.
     * @param line_id The current line ID being written.
     * @param append_line_fn Callback function to output G-code strings.
     */
    static void inject_lines(
        Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap::iterator& precooling_iter,
        const Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap& precooling_inserted_lines,
        bool enable_pre_heating,
        unsigned int line_id,
        std::function<void(const std::string&)> append_line_fn
    );

private:
    /**
     * @brief Calculates and inserts the pre-cool and pre-heat commands for a specific free block.
     */
    void inject_cooling_heating_command(
        Slic3r::GCodeProcessor::TimeProcessor::InsertedLinesMap& inserted_operation_lines,
        const ExtruderFreeBlock& block,
        float curr_temp,
        float target_temp,
        bool pre_cooling,
        bool pre_heating
    );
    
    /**
     * @brief Assembles free blocks from filament usage structures (used for single-extruder multi-nozzle H2C setup).
     */
    void build_by_filament_blocks(const std::vector<Slic3r::ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks);
    
    /**
     * @brief Assembles free blocks from extruder usage structures (used for multi-extruder multi-nozzle H2C setup).
     */
    void build_by_extruder_blocks(const std::vector<Slic3r::ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks);

    std::vector<ExtruderFreeBlock> m_extruder_free_blocks; ///< Internal list of all calculated free blocks.
    const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>& moves; ///< Reference to the trajectory/vertex log of the print.
    [[maybe_unused]] const std::vector<std::string>& filament_types; ///< Types of loaded filaments (e.g. PLA, PETG, TPU).
    const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result; ///< Current nozzle mapping configurations.
    std::vector<int> filament_nozzle_temps; ///< Printing temperature per filament.
    std::vector<int> filament_nozzle_temps_initial_layer; ///< First layer printing temperature per filament.
    std::vector<int> physical_extruder_map; ///< Mapping of logical extruders to physical hardware tool index.
    int valid_machine_id; ///< Index of active machine statistics (time estimation model).
    float inject_time_threshold; ///< Minimum free time threshold (seconds) required to trigger pre-cooling/pre-heating.
    bool handle_hotend_as_extruder; ///< Control flag for temperature command target selection.
    bool has_filament_switcher; ///< True if the machine uses a filament track switcher (FTS).
    std::vector<int> filament_pre_cooling_temps; ///< Standby/cool temperature per filament.
    std::vector<double> cooling_rate; ///< Cooling speed (C/s) per extruder.
    std::vector<double> heating_rate; ///< Heating speed (C/s) per extruder.
    std::vector<std::pair<unsigned int, unsigned int>> skippable_blocks; ///< Blocks of G-code lines where commands cannot be injected.
    std::vector<int> extruder_max_nozzle_count; ///< Maximum number of nozzles configured per extruder.
    std::vector<double> filament_preheat_temperature_delta; ///< Delta temperature to subtract for early preheating.
    std::vector<double> filament_max_temperature_drop_when_ec; ///< Maximum allowable temperature drop when changing extruders.
    unsigned int machine_start_gcode_end_id; ///< End line ID of the machine start G-code.
    unsigned int machine_end_gcode_start_id; ///< Start line ID of the machine end G-code.
    std::vector<Slic3r::ExtruderType> extruder_types; ///< Types of extruders (e.g. standard vs independent).
    std::vector<double> nozzle_diameter; ///< Nozzle diameter sizes per extruder.
};

} // namespace Vortek

#endif // VORTEK_PRE_COOLING_HPP
