// ============================================================================
// VortekGroupReorder.hpp
//
// Implements the Vortek::GroupReorder class to handle H2C combo-range
// optimization and multi-nozzle tool ordering.
// ============================================================================

#ifndef H2C_GROUP_REORDER_HPP
#define H2C_GROUP_REORDER_HPP

#include "GCode/ToolOrdering.hpp"

namespace Slic3r {
    class Print;
}

namespace Vortek {

/**
 * @class GroupReorder
 * @brief Performs tool-ordering optimization and grouping for multi-nozzle extrusion.
 * 
 * Tool ordering in multi-nozzle prints is complex due to the physical constraint of nozzle-change operations
 * (e.g. rotating a hotend turret/carousel, shifting heads). It is crucial to group filament prints by layers and
 * combo ranges to minimize nozzle switches and purge volume.
 * 
 * This class provides helper methods to build ordering contexts, solve minimal-flush nozzle allocation matchings,
 * and reorder filament change sequences either statically or dynamically (combo-range-based).
 */
class GroupReorder {
public:
    /**
     * @brief Optimizes and matches nozzle configurations using bipartite matching (MCMF) based on current nozzle status/loaded filament.
     * @param ctx Current filament group context.
     * @param group Recommended static layered nozzle group matching.
     * @param nozzles_state Active/cached nozzle state matching.
     * @return Refined nozzle-to-filament mapping.
     */
    static Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult refine_groups_by_Nozzle_State(
        const Slic3r::FilamentGroupContext&                          ctx,
        const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult&    group,
        const std::unordered_map<int, int>&                          nozzles_state
    );

    /**
     * @brief Computes optimal filament mappings and sequences dynamically by breaking layers down into contiguous combo ranges.
     * @param print Host print job configuration.
     * @param ctx Current filament grouping context.
     * @param order_ctx Custom tool ordering settings.
     * @param mode Filament mapping mode (flush matching vs auto).
     * @param physical_unprintables Filament constraints based on physical extruder characteristics.
     * @param geometric_unprintables Filament constraints based on geometric limits.
     * @param unprintable_volumes Material compatibility limits.
     * @param io_nozzle_status Recorder storing active nozzle contents (updated on return).
     * @return Vector of mapping results per layer.
     */
    static std::vector<Slic3r::FilamentPlanRes> plan_filament_mapping_and_order_by_combo_ranges(
        Slic3r::Print*                                             print,
        const Slic3r::FilamentGroupContext&                        ctx,
        const Slic3r::ToolOrdering::OrderingContext&               order_ctx,
        const Slic3r::FilamentMapMode                              mode,
        const std::vector<std::set<int>>&                          physical_unprintables,
        const std::vector<std::set<int>>&                          geometric_unprintables,
        const std::map<int, std::set<Slic3r::NozzleVolumeType>>&   unprintable_volumes,
        Slic3r::MultiNozzleUtils::NozzleStatusRecorder*            io_nozzle_status = nullptr
    );

    /**
     * @brief Main entrance point called by ToolOrdering. Performs dynamic or static reordering of extruders and filaments.
     * @param tool_ordering The host ToolOrdering instance.
     * @param filament_lists Unique filaments used across the print.
     * @param filament_maps Active filament-to-nozzle index map.
     * @param maps_without_group Fallback filament mapping.
     * @param layer_filaments Lists of filaments used in each print layer.
     * @param nozzle_flush_mtx The flush matrix containing purge volumes per filament transition.
     * @param get_custom_seq Lambda function to query custom ordering sequences.
     * @param filament_sequences Output sequence of filaments to print per layer.
     * @param nozzle_nums Total number of nozzles.
     * @param map_mode active mapping mode.
     * @param support_multi_nozzle True if printer config enables multiple nozzles.
     */
    static void reorder_extruders(
        Slic3r::ToolOrdering& tool_ordering,
        const std::vector<unsigned int>& filament_lists,
        const std::vector<int>& filament_maps,
        const std::vector<int>& maps_without_group,
        const std::vector<std::vector<unsigned int>>& layer_filaments,
        const std::vector<Slic3r::FlushMatrix>& nozzle_flush_mtx,
        const std::function<bool(int, std::vector<int>&)>& get_custom_seq,
        std::vector<std::vector<unsigned int>>& filament_sequences,
        int nozzle_nums,
        Slic3r::FilamentMapMode map_mode,
        bool support_multi_nozzle
    );

    /**
     * @brief Assembles the FilamentGroupContext descriptor containing all filament, machine, and speed properties.
     */
    static Slic3r::FilamentGroupContext build_filament_group_context(
        Slic3r::Print*                                             print,
        const std::vector<std::vector<unsigned int>>&              layer_filaments,
        const std::vector<std::set<int>>&                          physical_unprintables,
        const std::vector<std::set<int>>&                          geometric_unprintables,
        const std::map<int, std::set<Slic3r::NozzleVolumeType>>&   unprintable_volumes,
        Slic3r::FilamentMapMode                                    mode,
        const std::unordered_map<int, int>&                        nozzle_status
    );

    /**
     * @brief Prepares per-nozzle transition matrix weighted with multipliers.
     */
    static std::vector<Slic3r::FlushMatrix> prepare_flush_matrices(const Slic3r::PrintConfig& print_config);

    /**
     * @brief Builds nozzle group descriptors from print configuration.
     */
    static std::vector<Slic3r::MultiNozzleUtils::NozzleGroupInfo> build_nozzle_groups(const Slic3r::PrintConfig &config, size_t extruder_nums);

    /**
     * @brief Assembles a default list of nozzle metadata descriptions.
     */
    static std::vector<Slic3r::MultiNozzleUtils::NozzleInfo> build_default_nozzle_list(const Slic3r::PrintConfig &config, size_t extruder_nums);
};

} // namespace Vortek

#endif // H2C_GROUP_REORDER_HPP
