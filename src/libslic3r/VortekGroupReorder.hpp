#ifndef H2C_GROUP_REORDER_HPP
#define H2C_GROUP_REORDER_HPP

// ============================================================================
// VortekGroupReorder.hpp
//
// Forward declarations for functions extracted from ToolOrdering.cpp.
// GroupReorder namespace declarations stay in ToolOrdering.hpp (unchanged).
// This header only declares the two formerly-static helper functions that
// reorder_extruders_for_minimum_flush_volume() calls from ToolOrdering.cpp.
//
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:1270-1789
// ============================================================================

// NOTE: This header is included from within namespace Slic3r {} in ToolOrdering.cpp.
// Do NOT wrap contents in namespace Slic3r.

// H2C port: Refine filament-to-nozzle assignment using min-cost flow.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:1495-1631
MultiNozzleUtils::LayeredNozzleGroupResult refine_groups_by_Nozzle_State(
    const FilamentGroupContext&                          ctx,
    const MultiNozzleUtils::LayeredNozzleGroupResult&    group,
    const std::unordered_map<int, int>&                  nozzles_state);

// H2C port: Core dynamic GroupReorder — plan filament mapping per combo range.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:1633-1789
std::vector<FilamentPlanRes> plan_filament_mapping_and_order_by_combo_ranges(
    Print*                                             print,
    const FilamentGroupContext&                        ctx,
    const ToolOrdering::OrderingContext&               order_ctx,
    const FilamentMapMode                              mode,
    const std::vector<std::set<int>>&                  physical_unprintables,
    const std::vector<std::set<int>>&                  geometric_unprintables,
    const std::map<int, std::set<NozzleVolumeType>>&   unprintable_volumes,
    MultiNozzleUtils::NozzleStatusRecorder*            io_nozzle_status = nullptr);

#endif // H2C_GROUP_REORDER_HPP
